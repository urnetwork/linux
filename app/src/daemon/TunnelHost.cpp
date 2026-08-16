// SPDX-License-Identifier: MPL-2.0
#include "TunnelHost.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include <gio/gio.h>

#include "NetworkSpaceConfig.hpp"
#include "daemon/DaemonLog.hpp"

namespace urnw {
namespace {

// Persisted device identity, file-per-part like the Windows TunnelController
// (client_key_seed.bin / provide_cert.pem / provide_key.pem).
constexpr const char* kClientKeySeedFile = "client_key_seed.bin";
constexpr const char* kProvideCertFile = "provide_cert.pem";
constexpr const char* kProvideKeyFile = "provide_key.pem";

// How often the main-loop reaper runs. It does three jobs, all of which have
// to happen somewhere the session objects can safely be destroyed: notice a
// dead IoLoop, arm the kill switch after an UNEXPECTED drop, and enforce the
// orphan timeout.
constexpr guint kReaperIntervalSeconds = 1;

// How often the reaper asks the kernel whether our table is still there.
// nftables has no tamper callback, so a poll is the entire mitigation for a
// foreign `nft flush ruleset` — and on Fedora/Bazzite the shipped
// /etc/sysconfig/nftables.conf BEGINS with `flush ruleset`, so a
// `systemctl restart nftables` destroys `table inet urnetwork` silently.
// Every poll is one `nft list table` fork/exec, so it is deliberately NOT on
// every 1 s tick: 5 s bounds the tamper window at 5 s for ~17k execs a day
// instead of 86k.
constexpr int kFilterVerifyIntervalSeconds = 5;
// A Verify/re-install that keeps failing (nft uninstalled underneath us) must
// not turn the journal into a 5 s heartbeat. Log the first failure, then one
// in this many.
constexpr int kFilterVerifyLogEvery = 12;  // ~once a minute
// The live egress witness costs one nft read + one socket + four datagrams and
// no network round trip, so it is cheap — but it does emit packets, so it does
// not belong on every tick either. 30 s means two consecutive failures cap the
// exposure at ~60 s. The comparison that sets the bar: the incident this
// exists for ran for forty minutes.
constexpr int kEgressWitnessIntervalSeconds = 30;

int64_t UnixMillis() { return g_get_real_time() / 1000; }
int64_t MonotonicSeconds() { return g_get_monotonic_time() / G_USEC_PER_SEC; }

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

// Reports failure to the caller rather than only to stderr: a half-written
// identity must be detected, not left to the emptiness guard on the next read.
bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  bool ok = true;
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::fprintf(stderr, "[tunnel] could not open %s for writing\n", path.c_str());
      return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.flush();
    ok = static_cast<bool>(f);
  }
  if (!ok) {
    std::fprintf(stderr, "[tunnel] could not write %s\n", path.c_str());
    return false;
  }
  // key material: owner-only, on top of the root-owned 0700 state dir
  ::chmod(path.c_str(), 0600);
  return true;
}

// The interface name the NEXT tun will get, taken from the same default
// TunnelConfig RunStart builds — never a second literal "urnet0". The
// Connecting ruleset needs it before the interface exists, which is legal
// precisely because the permits match with oifname/iifname (a per-packet
// string match) and not oif/iif (resolved to an index at load time).
const std::string& PlannedTunName() {
  static const std::string kName = TunnelConfig().name;
  return kName;
}

// The file-local PortFromHostPort that used to live here is GONE. It parsed
// with std::atoi, so "127.0.0.1:notaport" yielded 0 and the published rpc_port
// silently stayed at the SDK default while the listener was somewhere else —
// a mismatch the GUI had no way to detect. Both halves now call
// ctl::RpcPortFromHostPort, which is the same function the shared validator
// uses, so the reply's rpc_port is a trustworthy cross-check.

}  // namespace

TunnelHost::TunnelHost(std::string storageRoot) : storageRoot_(std::move(storageRoot)) {
  cgroup_ = SelfCgroupV2();
  if (const char* env = std::getenv("URNETWORK_ALLOW_UNPROTECTED_EGRESS");
      env != nullptr && *env != '\0' && std::string(env) != "0") {
    allowUnprotectedEgress_ = true;
  }
  reaperId_ = g_timeout_add_seconds(kReaperIntervalSeconds, &TunnelHost::OnReaperTick, this);
  // systemd-resolved forgets every per-link setting across a restart, and a
  // DHCP search domain can beat our `~.` default-route domain afterwards
  // (docs/linux_agent_help.md R5). Watching the bus name is the cheap,
  // event-driven way to notice; the re-push itself runs on the main loop.
  resolvedWatchId_ = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.freedesktop.resolve1",
                                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                                      &TunnelHost::OnResolvedAppeared, nullptr, this, nullptr);
}

void TunnelHost::AdoptArmedFloor() {
  std::scoped_lock lock(opMutex_);
  filter_.AdoptArmedFloor();
  // Publish it, so the very FIRST status read after a crash-restart tells the
  // truth instead of the reassuring lie. Armed, not Connected: there is no
  // tunnel — the floor is all there is.
  killSwitchRequested_.store(true);
  status_.kill_switch = ctl::KillSwitchState::Armed;
  status_.kill_switch_detail.clear();
}

TunnelHost::~TunnelHost() {
  if (reaperId_ != 0) {
    g_source_remove(reaperId_);
    reaperId_ = 0;
  }
  if (resolvedWatchId_ != 0) {
    g_bus_unwatch_name(resolvedWatchId_);
    resolvedWatchId_ = 0;
  }
  Stop("daemon_shutdown");
}

// ---- key material ----------------------------------------------------------

bool TunnelHost::HasStoredKeyMaterial() const {
  return !ReadFileBytes(storageRoot_ + "/" + kClientKeySeedFile).empty();
}

std::optional<urnet::DeviceLocalKeyMaterial> TunnelHost::LoadKeyMaterial() const {
  const auto seed = ReadFileBytes(storageRoot_ + "/" + kClientKeySeedFile);
  const auto cert = ReadFileBytes(storageRoot_ + "/" + kProvideCertFile);
  const auto key = ReadFileBytes(storageRoot_ + "/" + kProvideKeyFile);
  if (seed.empty() || cert.empty() || key.empty()) return std::nullopt;
  return urnet::newDeviceLocalKeyMaterial(
      seed.data(), static_cast<int32_t>(seed.size()), cert.data(),
      static_cast<int32_t>(cert.size()), key.data(), static_cast<int32_t>(key.size()));
}

bool TunnelHost::PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km) const {
  bool ok = WriteFileBytes(storageRoot_ + "/" + kClientKeySeedFile, km.getClientKeySeed());
  ok = WriteFileBytes(storageRoot_ + "/" + kProvideCertFile, km.getProvideTlsCertificatePem()) &&
       ok;
  ok = WriteFileBytes(storageRoot_ + "/" + kProvideKeyFile, km.getProvideTlsPrivateKeyPem()) && ok;
  return ok;
}

// ---- published status ------------------------------------------------------

// ORDERING HAZARD, and it has bitten twice. This writes two fields that other
// paths REPLACE or CLEAR: StopInternalLocked(<non-empty reason>) clears both,
// and RunStart's catch block re-publishes its own. Calling it and then calling
// anything that tears down therefore tells the user nothing. Publish LAST, or
// keep a copy and re-publish after (RunStart's keptError/keptCode), or use
// StopUnsafeSessionLocked, which owns the whole ordering.
void TunnelHost::PublishError(const std::string& message, const std::string& code) {
  std::scoped_lock lock(statusMutex_);
  status_.error = message;
  status_.error_code = code;
}

ctl::StatusReply TunnelHost::Status() const {
  std::scoped_lock lock(statusMutex_);
  ctl::StatusReply s = status_;
  s.owner_connected = ownerConnected_.load();
  return s;
}

bool TunnelHost::TunnelUp() const {
  std::scoped_lock lock(statusMutex_);
  return status_.tunnel_state == ctl::TunnelState::Up;
}

bool TunnelHost::CanAdopt(const ctl::StartTunnelRequest& config) const {
  std::scoped_lock lock(statusMutex_);
  if (status_.tunnel_state != ctl::TunnelState::Up) return false;
  if (activeConfig_.instance_id.empty()) return false;
  return activeConfig_.instance_id == config.instance_id &&
         activeConfig_.by_jwt == config.by_jwt &&
         activeConfig_.network_space_json == config.network_space_json &&
         activeConfig_.rpc_server_pem == config.rpc_server_pem &&
         activeConfig_.rpc_client_cert_pem == config.rpc_client_cert_pem &&
         activeConfig_.rpc_listen_hostport == config.rpc_listen_hostport;
}

void TunnelHost::SetOwnerConnected(bool connected) {
  const bool previous = ownerConnected_.exchange(connected);
  if (previous && !connected) {
    ownerLostMonotonicSeconds_ = MonotonicSeconds();
  } else if (connected) {
    ownerLostMonotonicSeconds_ = 0;
  }
}

// ---- the nftables floor ----------------------------------------------------

FilterConfig TunnelHost::FilterConfigForLocked(FilterState state, bool floor) const {
  FilterConfig cfg;
  cfg.state = state;
  cfg.floor = floor;
  cfg.cgroup = cgroup_;
  cfg.block_ipv6 = true;  // leak prevention is not a preference (§6.3)
  cfg.block_offtunnel_dns = false;
  if (state == FilterState::Connecting || state == FilterState::Connected) {
    // By NAME, and set even while the interface does not exist yet: that is
    // what removes the blackhole window between the capture routes landing and
    // the swap to Connected.
    cfg.tun_name = tunnel_ ? tunnel_->name() : PlannedTunName();
  }
  if (tunnel_ != nullptr) {
    // THE OFF-TUNNEL DNS FLOOR, which before this line never installed in ANY
    // state: DeriveFilter (Tunnel.cpp) refuses to close :53 unless at least one
    // tunnel resolver survived inet_pton, and nothing in the daemon ever filled
    // tunnel_resolvers — so the field was empty on every single Apply and the
    // whole off-tunnel DNS block was dead code. Tunnel::resolvers() is what was
    // ACTUALLY handed to systemd-resolved (not what the device asked for), so
    // the pinned permit and the DNS override can never name different servers.
    cfg.tunnel_resolvers = tunnel_->resolvers();
    // Only close the off-tunnel DNS ports when DNS actually landed on the
    // tunnel. Blocking them when the override failed would leave the machine
    // unable to resolve at all; the honest alternative is to report
    // dns_applied=false loudly, which Status() does.
    cfg.block_offtunnel_dns = tunnel_->report().dns_applied;
  }
  if (state == FilterState::Connecting && floor) {
    // A FLOORED bring-up is a reconnect made from Armed, and on a
    // systemd-resolved host the SDK's own lookup leaves resolved's cgroup, not
    // ours: without this bounded permit the reconnect cannot resolve and the
    // user is stuck in Armed forever. Emitted by the builder only for this
    // exact case (state == Connecting && floor), and only for cgroups that
    // exist.
    cfg.dns_helper_cgroups = DnsHelperCgroupsV2();
  }
  return cfg;
}

bool TunnelHost::InstallFilterLocked(FilterState state, bool floor, std::string* error) {
  const FilterConfig cfg = FilterConfigForLocked(state, floor);
  const bool ok = filter_.Apply(cfg, error);

  // What is IN FORCE, never what was asked for.
  const bool floorInForce = ok && filter_.floorInstalled();
  ctl::KillSwitchState published = ctl::KillSwitchState::Off;
  if (!ok && floor) {
    published = ctl::KillSwitchState::Failed;
  } else if (floorInForce) {
    published = (state == FilterState::Connected) ? ctl::KillSwitchState::Connected
                                                  : ctl::KillSwitchState::Armed;
  }
  // A teardown that FAILED publishes nothing: the old value (Connected/Armed)
  // is closer to the truth than "off", because whatever we failed to delete is
  // very probably still in the kernel filtering this machine. The retry below
  // is what corrects it.
  if (state != FilterState::Off || ok) {
    std::scoped_lock lock(statusMutex_);
    status_.kill_switch = published;
    status_.kill_switch_detail =
        (published == ctl::KillSwitchState::Failed) ? filter_.lastError() : std::string();
    // True for every installed state, because it is true in every installed
    // state: Connecting, Armed and Connected all carry the v6 fail-closed
    // rules. Reporting it only for Connected told an armed-but-disconnected
    // user that v6 was flowing while it was being dropped.
    status_.ipv6_blocked = ok && state != FilterState::Off && cfg.block_ipv6;
  }

  if (state == FilterState::Off) {
    // A failed teardown is NOT a completed one. Remember it so the reaper
    // retries; the filter cannot remember it for us (see filterRemovalPending_).
    filterRemovalPending_.store(!ok);
    if (!ok) {
      DaemonLogf(
          "[tunnel] ERROR: the firewall teardown did not complete: %s. This machine may still "
          "be filtered by URnetwork. It will be retried every %ds; to lift it by hand run: %s\n",
          error != nullptr && !error->empty() ? error->c_str() : filter_.lastError().c_str(),
          static_cast<int>(kReaperIntervalSeconds), NetFilter::RecoveryCommand());
    }
  } else if (ok) {
    // We own a table again, and it is the one we intended.
    filterRemovalPending_.store(false);
  }
  if (floorInForce) {
    // Into the ring as well as the journal: the ring is what the GUI's log tail
    // shows, so the way out is on a surface a blocked user can actually reach.
    DaemonLogf(
        "[tunnel] the kill-switch block floor is in force (%s). If this daemon dies while armed "
        "the machine stays blocked; recover with: %s\n",
        ToString(state), NetFilter::RecoveryCommand());
  }
  return ok;
}

bool TunnelHost::ApplyFilterLocked(FilterState next, std::string* error) {
  // THE one decision site. FloorForTransition (Tunnel.hpp) is pure and is the
  // documented authority; it needs the state we are coming FROM, which is why
  // it cannot live at the call sites.
  const bool floor = FloorForTransition(filter_.state(), next, killSwitchRequested_.load());
  return InstallFilterLocked(next, floor, error);
}

bool TunnelHost::ReinstallFilterLocked(std::string* error) {
  const FilterState state = filter_.state();
  if (state == FilterState::Off) return true;  // nothing is claimed
  return InstallFilterLocked(state, filter_.floorInstalled(), error);
}

// ---- start / stop ----------------------------------------------------------

void TunnelHost::JoinWorker() {
  if (worker_.joinable()) worker_.join();
}

ctl::StatusReply TunnelHost::Start(const ctl::StartTunnelRequest& config) {
  if (busy_.load()) {
    // NOT a restart. The client's own receive timeout used to turn this case
    // into StopInternal() + a full rebuild, i.e. a restart loop.
    ctl::StatusReply s = Status();
    s.error = "a tunnel start is already in progress";
    s.error_code = ctl::kCodeStartInProgress;
    return s;
  }
  JoinWorker();  // a previous worker that has already finished
  stopRequested_.store(false);
  busy_.store(true);
  killSwitchRequested_.store(config.kill_switch);
  {
    std::scoped_lock lock(statusMutex_);
    status_.tunnel_state = ctl::TunnelState::Starting;
    status_.error.clear();
    status_.error_code.clear();
    status_.stop_reason.clear();
    status_.routes_installed = false;
    status_.egress_protected = false;
    status_.dns_applied = false;
    status_.dns_detail.clear();
    status_.tunnel_interface.clear();
    status_.up_since_millis = 0;
    status_.rpc_port = 0;
    // Cleared wherever rpc_port is cleared: a stale true would tell a polling
    // client that the NEXT session's listener is already pinned to material it
    // has not sent yet.
    status_.rpc_pinned = false;
    status_.client_id.clear();
  }
  if (config.async) {
    worker_ = std::thread(&TunnelHost::RunStart, this, config);
    return Status();  // tunnel_state=starting; the client polls
  }
  RunStart(config);
  return Status();
}

void TunnelHost::RunStart(ctl::StartTunnelRequest config) {
  {
    std::scoped_lock lock(opMutex_);
    // What was in force BEFORE this attempt, captured before anything can
    // change it. It decides where a FAILED start lands: back on the armed floor
    // it interrupted, or on a completely clean machine. (StopInternalLocked
    // with an empty reason deliberately does not touch the filter, so this
    // stays true across the line below.)
    const bool entryFloor = filter_.floorInstalled();
    // Idempotent restart, but NOT a "stop": no stop_reason is recorded for a
    // teardown that exists only to make room for this start.
    StopInternalLocked(std::string());
    const uint64_t generation = sessionGeneration_.fetch_add(1) + 1;
    ioLoopDied_.store(false);

    try {
      // --- 0) re-validate the request, before anything is built -------------
      // ControlServer already ran this, but Start() is a public entry point
      // and this process is root: re-check rather than trust the caller.
      // Deliberately the FIRST thing in the bring-up, so a request that cannot
      // produce a pinned rpc listener never costs a DeviceLocal construction
      // (a network round trip) or an nftables transaction.
      //
      // rpc_listen_hostport is the one that matters most: unvalidated, it lets
      // a control-socket peer choose where ROOT binds the device RPC —
      // "0.0.0.0:12025" would expose it to the whole LAN.
      if (const auto invalid = ctl::ValidateStartTunnelRequest(config)) {
        PublishError(invalid->message,
                     invalid->code == nullptr ? ctl::kCodeRpcPinRequired : invalid->code);
        throw std::runtime_error(invalid->message);
      }

      // --- 1) egress self-exclusion FIRST -----------------------------------
      // The exclusion has to be in force before a single capture route lands,
      // or the daemon's own SDK sockets (jwt refresh, window enumeration, DoH,
      // contract waits, the provider transports) fall into our own tun and the
      // control plane starves — or, as measured on 2026-08-15, loops.
      //
      // 1a. THE MECHANISM: mark this process's sockets AT CREATION. This must
      //     happen before step 3 builds the DeviceLocal, because that is what
      //     creates the SDK's sockets, and a mark that arrives after connect()
      //     is too late to matter — the route (and with it the source address)
      //     is already chosen. urnet::setEgressInterfaceIndex cannot do this
      //     job: connect:egress_other.go makes applyEgressInterface an 11-line
      //     `return nil` off Windows, and the SDK ABI exposes no dialer
      //     Control hook, so the mark has to be applied from outside the
      //     process. A cgroup-bpf sock_create program is the one hook that
      //     reaches Go's sockets without touching the vendored SDK.
      //
      //     A failure here is NOT fatal on its own: the nftables chain below
      //     is a genuine belt for sockets that already exist, and some hosts
      //     have no CONFIG_CGROUP_BPF. What is fatal is failing the packet
      //     witness in Tunnel::Configure, which is what decides whether this
      //     tunnel is allowed to exist.
      {
        std::string markerError;
        if (egressMarker_.Attach(cgroup_, kEgressMark, &markerError)) {
          DaemonLogf("[tunnel] egress: %s\n", egressMarker_.detail().c_str());
        } else {
          DaemonLogf(
              "[tunnel] WARNING: this daemon's own sockets could NOT be marked at creation "
              "(%s). Falling back to the nftables mark chain alone, which cannot repair a "
              "source address connect() has already chosen. The packet witness in the tun "
              "bring-up decides whether that is good enough; it usually is not.\n",
              markerError.c_str());
        }
      }

      // 1b. THE BELT: the nftables cgroup mark chain, plus the ruleset that
      //     carries the packet witness's counters.
      const bool egressPossible = cgroup_.valid && !FindTool("nft").empty();
      if (!egressPossible && !allowUnprotectedEgress_) {
        const std::string why =
            !cgroup_.valid
                ? "this system is not running the cgroup v2 unified hierarchy, so the "
                  "daemon's own sockets cannot be marked"
                : "nftables (nft) is not installed";
        throw std::runtime_error(
            std::string("refusing to start: the daemon's own traffic would be captured by "
                        "its own tunnel (") +
            why + ")");
      }
      if (egressPossible) {
        std::string filterError;
        // The floor is NOT hardcoded here any more. FloorForTransition answers
        // it, and it answers differently for the two bring-ups that used to be
        // treated as one:
        //   * from Off (a first connect): NO floor, because a failed start must
        //     never leave a clean machine cut off the network.
        //   * from Armed (a reconnect after an unexpected drop): the floor
        //     STAYS, because that window is the whole reason the kill switch
        //     exists. The bounded DNS-helper permit rides along so the
        //     reconnect can still resolve.
        // Hardcoding false meant the second case silently lifted the kill
        // switch for the length of every reconnect attempt.
        if (!ApplyFilterLocked(FilterState::Connecting, &filterError)) {
          throw std::runtime_error("could not install the egress-exclusion ruleset: " +
                                   filterError);
        }
      }

      // --- 2) network space (daemon-owned storage) --------------------------
      // The GUI's active space rides in on start_tunnel (windows parity): the
      // DeviceLocal must live in the SAME network as the jwt it registers, or
      // a custom-server session would sync against production. An absent or
      // broken json falls back to the compiled-in default — silence means
      // production, never a surprise server.
      if (!spaceManager_) {
        spaceManager_ = urnet::newNetworkSpaceManager(storageRoot_ + "/sdk");
      }
      networkSpace_.reset();
      if (!config.network_space_json.empty()) {
        try {
          networkSpace_ = spaceManager_->importNetworkSpaceFromJson(config.network_space_json);
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[tunnel] import network space failed (using default): %s\n",
                       e.what());
        }
      }
      if (!networkSpace_) networkSpace_ = BuildUrNetworkSpace(*spaceManager_);
      if (stopRequested_.load()) throw std::runtime_error("start cancelled");

      // --- 3) DeviceLocal, with NO listener of its own -----------------------
      // enable_rpc is FALSE here, and that is the fix for a real hole rather
      // than a style choice. The SDK's `enable_rpc=true` constructor builds its
      // OWN rpc manager immediately (device_local.go: `if settings.EnableRpc {
      // deviceLocal.deviceLocalRpcManager = newDeviceLocalRpcManagerWithDefaults
      // (...) }`), and that default manager binds 127.0.0.1:12025 as PLAIN ws
      // with no server cert and no client pinning. Every local process able to
      // open a TCP socket — including one the control socket's SO_PEERCRED +
      // `urnetwork` group check exists to refuse — could drive this ROOT device
      // for the whole window between construction and setRpcServer below.
      //
      // With enable_rpc=false the constructor creates no listener at all, and
      // DeviceLocal.SetRpcServer() (device_local.go) creates the manager
      // unconditionally from the pinned material — it does not consult
      // EnableRpc — so the ONLY listener this device ever has is the mTLS one
      // installed at step 4. The single other thing enable_rpc=false changes is
      // newSecurityPolicyMonitor(ctx, device, settings.Verbose), which returns
      // nil immediately because DefaultDeviceLocalSettings sets Verbose=false.
      const std::string appVersion =
          config.app_version.empty() ? kUrAppVersionFallback : config.app_version;
      const bool hadStoredMaterial = HasStoredKeyMaterial();
      bool restoreFailed = false;
      if (auto km = LoadKeyMaterial()) {
        try {
          device_ = urnet::newDeviceLocalWithKeyMaterial(
              *networkSpace_, config.by_jwt, UrDeviceDescription(), UrDeviceSpec(), appVersion,
              config.instance_id, /*enable_rpc=*/false, *km);
        } catch (const std::exception& e) {
          restoreFailed = true;
          std::fprintf(stderr, "[tunnel] restore device key material failed: %s\n", e.what());
        }
      }
      if (!device_) {
        device_ = urnet::newDeviceLocalWithDefaults(*networkSpace_, config.by_jwt,
                                                    UrDeviceDescription(), UrDeviceSpec(),
                                                    appVersion, config.instance_id,
                                                    /*enable_rpc=*/false);
        // Persist ONLY when nothing was stored. Overwriting after a FAILED
        // restore silently rotates this device's provider identity — peers
        // stop recognising it and its reputation is gone — for what may be a
        // transient failure. The stored identity is left intact so a later,
        // healthy start can still use it; this session runs on an ephemeral
        // one and says so at error level.
        if (!hadStoredMaterial) {
          try {
            if (auto km = device_->getKeyMaterial(); km && !km.isEmpty()) {
              if (!PersistKeyMaterial(km)) {
                std::fprintf(stderr,
                             "[tunnel] ERROR: the new device identity could not be saved; the "
                             "next start will register a different device\n");
              }
            }
          } catch (const std::exception& e) {
            std::fprintf(stderr, "[tunnel] persist device key material failed: %s\n", e.what());
          }
        } else if (restoreFailed) {
          std::fprintf(stderr,
                       "[tunnel] ERROR: the stored device identity could not be restored. This "
                       "session runs on a TEMPORARY identity and the stored one has been left "
                       "untouched; provider reputation is not lost, but it is not in use "
                       "either. Fix or remove %s to resolve this.\n",
                       storageRoot_.c_str());
        }
      }

      // --- 4) device-RPC mTLS pinning (windows TunnelController parity) -----
      // MANDATORY. Both halves pin the SAME generated material; the SDK
      // compares raw certificates for equality, so no "defaults" path can
      // produce a matching pair across two processes with separate storage
      // roots. There is no unpinned branch left, and as of step 3 there is no
      // unpinned WINDOW either: the SDK's built-in default listener has no
      // client pinning at all, so every local process able to open a TCP socket
      // to it — including one the control socket's SO_PEERCRED + `urnetwork`
      // group check would refuse — could drive this root DeviceLocal, and with
      // enable_rpc=true it was already listening by the time control reached
      // this line. This call is now the FIRST thing that binds the port.
      //
      // The triple was validated at step 0, so this port is guaranteed
      // non-zero and the old `int rpcPort = ctl::kDeviceRpcPort;` fallback is
      // gone: reporting a port nothing is listening on is a fiction the client
      // cross-checks against, so it must not be possible to produce one.
      const int rpcPort = ctl::RpcPortFromHostPort(config.rpc_listen_hostport);
      bool rpcPinned = false;
      try {
        device_->setRpcServer(config.rpc_server_pem, config.rpc_client_cert_pem,
                              config.rpc_listen_hostport);
        rpcPinned = true;
      } catch (const std::exception& e) {
        // Publish BEFORE rethrowing. Without this the throw fell through to
        // the generic catch below and was labelled kCodeTunOpenFailed, so an
        // mTLS bind failure reached the user as "could not open or configure
        // the tun device" — exactly the class of lie the code table exists to
        // prevent. The realistic cause is the port already being held.
        PublishError(std::string("the local control connection could not be secured on ") +
                         config.rpc_listen_hostport + ": " + e.what(),
                     ctl::kCodeRpcListenFailed);
        throw;
      }
      std::fprintf(stderr, "[tunnel] device rpc pinned on %s\n",
                   config.rpc_listen_hostport.c_str());

      // A set_provide that arrived while the tunnel was down applies now. (The
      // GUI also restores its persisted mode over the device RPC right after
      // start; the last writer wins, and both come from the same stored value.)
      std::string provideMode;
      {
        std::scoped_lock lock(statusMutex_);
        provideMode = pendingProvideMode_;
      }
      if (!provideMode.empty()) {
        device_->setProvideControlMode(provideMode);
      }
      if (stopRequested_.load()) throw std::runtime_error("start cancelled");

      // --- 5) the tun (address/dns from the device) -------------------------
      TunnelConfig cfg;
      cfg.local_addr = device_->tunnelLocalAddress();
      if (!IsIpv4Address(cfg.local_addr)) {
        if (!cfg.local_addr.empty()) {
          std::fprintf(stderr, "[tunnel] the device reported an unusable tunnel address '%s'\n",
                       cfg.local_addr.c_str());
        }
        cfg.local_addr = "169.254.2.1";
      }
      // dns from the device: the dns settings' unencrypted local servers when
      // set, otherwise the distinct plain-DNS UpgradeMux mask. Always plain
      // :53, never OS-level encrypted DNS: the mux performs the
      // unencrypted-DNS -> DoH upgrade in-tunnel. The tunnel is ipv4-only.
      if (auto dns = device_->tunnelDnsAddressesIpv4(); dns && !dns->empty()) {
        cfg.dns_servers = *dns;
      } else {
        // Keep the exceptional fallback coupled to the SDK's separately tested
        // URnetwork-owned UpgradeMux identity.
        cfg.dns_servers = {urnet::getDefaultTunnelDnsAddressIpv4()};
      }
      cfg.require_egress_protection = !allowUnprotectedEgress_;

      TunnelError tunError;
      tunnel_ = Tunnel::Open(cfg, &tunError);
      if (!tunnel_) {
        PublishError(tunError.message.empty() ? "could not open or configure the tun device"
                                              : tunError.message,
                     tunError.code.empty() ? ctl::kCodeTunOpenFailed : tunError.code);
        throw std::runtime_error(tunError.message);
      }
      {
        const TunnelReport& report = tunnel_->report();
        std::scoped_lock lock(statusMutex_);
        status_.routes_installed = report.routes_installed;
        status_.egress_protected = report.egress_protected;
        status_.dns_applied = report.dns_applied;
        status_.dns_detail = report.dns_detail;
        status_.tunnel_interface = report.interface;
      }
      if (killSwitchRequested_.load() && !tunnel_->report().dns_applied) {
        // The kill switch closes off-tunnel :53. Coming up with the DNS
        // override not in force would leave the machine unable to resolve at
        // all, which is a worse outcome than refusing with a reason.
        PublishError("the kill switch needs DNS on the tunnel, and it could not be applied: " +
                         tunnel_->report().dns_detail,
                     ctl::kCodeDnsApplyFailed);
        throw std::runtime_error("dns could not be applied");
      }
      if (stopRequested_.load()) throw std::runtime_error("start cancelled");

      // --- 6) hand the tun fd to the SDK's fd loop --------------------------
      // The flag the callback sets is OWNED BY A shared_ptr the callback holds,
      // not by this object: the callback can outlive both the loop's handle and
      // (on shutdown) this TunnelHost, and it must never write through a
      // dangling pointer. Retirement below waits on exactly this flag.
      ioLoopFinished_ = std::make_shared<std::atomic<bool>>(false);
      ioLoop_ = urnet::newIoLoop(*device_, tunnel_->fd(),
                                 [this, generation, finished = ioLoopFinished_] {
        // SDK THREAD. Publish only: the teardown (and arming the kill switch
        // on an unexpected drop) happens on the reaper, on the main loop.
        finished->store(true);  // FIRST, and unconditionally: this is what
                                // makes retiring the handle safe, and it must
                                // happen even for our own Stop.
        if (sessionGeneration_.load() != generation) return;  // our own Stop
        ioLoopDied_.store(true);
        std::fprintf(stderr, "[tunnel] io loop finished\n");
      });
      device_->setTunnelStarted(true);

      // --- 7) the leak floor, now that the tun exists -----------------------
      std::string filterError;
      const bool wantKillSwitch = killSwitchRequested_.load();
      if (!ApplyFilterLocked(FilterState::Connected, &filterError)) {
        if (wantKillSwitch) {
          PublishError("the kill switch could not be installed: " + filterError,
                       ctl::kCodeKillSwitchFailed);
          throw std::runtime_error(filterError);
        }
        // Without the kill switch this is still a leak (v6 and off-tunnel
        // DNS), so it is reported, not swallowed.
        std::fprintf(stderr, "[tunnel] WARNING: leak floor not installed: %s\n",
                     filterError.c_str());
        PublishError("connected, but the IPv6 and DNS leak floor could not be installed: " +
                         filterError,
                     ctl::kCodeKillSwitchFailed);
      }

      // --- 7b) WITNESS THE GENERATION THAT ACTUALLY GOVERNS THIS SESSION ----
      // The witness in step 5 ran against the CONNECTING ruleset. The apply
      // immediately above emits `add table` / `delete table` / `table {...}` —
      // the atomic swap — which destroys that table and resets every counter
      // in it. Before this block existed, the only measurement of the egress
      // exclusion belonged to a ruleset that was thrown away seconds later,
      // and the ruleset the tunnel actually ran under was never measured at
      // all. That is how a tunnel ran forty minutes and 3.38 Tb while
      // reporting egress_protected = true.
      //
      // The io loop IS live by now, so a failure here is a teardown rather
      // than a refusal — but it is still a teardown, immediately, on four
      // datagrams, instead of whenever a human happens to look.
      if (!allowUnprotectedEgress_) {
        TunnelError witnessError;
        if (!tunnel_->VerifyEgressWitness("connected", &witnessError)) {
          PublishError(witnessError.message, ctl::kCodeEgressUnprotected);
          throw std::runtime_error(witnessError.message);
        }
        {
          const TunnelReport& report = tunnel_->report();
          std::scoped_lock lock(statusMutex_);
          status_.egress_protected = report.egress_protected;
        }
      }

      {
        std::scoped_lock lock(statusMutex_);
        status_.tunnel_state = ctl::TunnelState::Up;
        status_.rpc_port = rpcPort;
        // Published in the SAME block that flips the state to Up, so a client
        // polling `status` on the async path can never observe Up with a stale
        // rpc_pinned. rpcPinned can only be true here — setRpcServer rethrows
        // otherwise — but it is carried rather than hardcoded so the fact
        // stays tied to the call that established it.
        status_.rpc_pinned = rpcPinned;
        status_.up_since_millis = UnixMillis();
        activeConfig_ = config;  // what a later start_tunnel is compared against
        try {
          status_.client_id = device_->getClientId();
        } catch (const std::exception&) {
          // status must never throw across the wire
        }
      }
      std::fprintf(stderr, "[tunnel] up (client=%s rpc=127.0.0.1:%d routes=%d egress=%d dns=%d)\n",
                   Status().client_id.c_str(), rpcPort,
                   tunnel_->report().routes_installed ? 1 : 0,
                   tunnel_->report().egress_protected ? 1 : 0,
                   tunnel_->report().dns_applied ? 1 : 0);
    } catch (const std::exception& e) {
      {
        std::scoped_lock lock(statusMutex_);
        if (status_.error.empty()) status_.error = e.what();
        if (status_.error_code.empty()) status_.error_code = ctl::kCodeTunOpenFailed;
        status_.stop_reason = "start_failed";
      }
      std::fprintf(stderr, "[tunnel] start failed: %s\n", Status().error.c_str());
      // Retryable: tear down every partially-created resource so a failed
      // attempt cannot leave an IoLoop or a half-configured tun behind. The
      // error/error_code published above survive it.
      const std::string keptError = Status().error;
      const std::string keptCode = Status().error_code;
      StopInternalLocked(std::string());

      // AND THE FIREWALL, which this path used to walk straight past.
      // StopInternalLocked(<empty reason>) deliberately does not touch the
      // filter (it is also the "make room for this start" path), so the
      // Connecting ruleset installed at step 1 — which blocks ALL global IPv6
      // machine-wide — survived every failed start: no tunnel, no UI signal,
      // and nothing that would ever remove it short of a reboot.
      //
      // Where it lands depends only on where the attempt came FROM:
      //   * it interrupted an ARMED machine and the switch is still on -> go
      //     back to Armed. Falling to Off here would lift the kill switch
      //     BECAUSE the reconnect failed, which is the one moment it must not
      //     lift.
      //   * anything else -> remove the table completely. A start that failed
      //     from a clean machine must leave the machine exactly as it found it.
      const bool restoreArmed = entryFloor && killSwitchRequested_.load();
      {
        std::string filterError;
        if (restoreArmed) {
          // FloorForTransition returns true for Armed structurally, so this
          // cannot come back without a floor.
          if (!ApplyFilterLocked(FilterState::Armed, &filterError)) {
            DaemonLogf(
                "[tunnel] the start failed and the armed floor could not be restored: %s\n",
                filterError.c_str());
          } else {
            DaemonLogf(
                "[tunnel] the start failed; this machine stays blocked because the kill switch "
                "is armed. Disconnect in the app to lift it, or run: %s\n",
                NetFilter::RecoveryCommand());
          }
        } else if (!ApplyFilterLocked(FilterState::Off, &filterError)) {
          // InstallFilterLocked has already logged the recovery command and
          // set filterRemovalPending_, so the reaper keeps retrying.
          std::fprintf(stderr, "[tunnel] start failed and the ruleset could not be removed: %s\n",
                       filterError.c_str());
        }
      }
      {
        std::scoped_lock lock(statusMutex_);
        status_.tunnel_state = ctl::TunnelState::Error;
        status_.error = keptError;
        status_.error_code = keptCode;
        status_.stop_reason = "start_failed";
        if (!restoreArmed && keptCode == ctl::kCodeKillSwitchFailed) {
          // The teardown above published kill_switch=Off, which is true of the
          // machine but not of the request: the user asked for the switch and
          // it could not be installed. KillSwitchState::Failed exists exactly
          // so this is never rendered as Off.
          status_.kill_switch = ctl::KillSwitchState::Failed;
          status_.kill_switch_detail = keptError;
        }
      }
    }
  }
  busy_.store(false);
}

void TunnelHost::Stop(const std::string& reason) {
  stopRequested_.store(true);
  JoinWorker();
  std::scoped_lock lock(opMutex_);
  StopInternalLocked(reason);
}

// Release retired IoLoops whose done callback has actually fired. Called from
// the stop path and from the reaper tick, so a loop that takes a moment to wind
// down is collected shortly after rather than held for the life of the process.
void TunnelHost::ReapRetiredLoopsLocked() {
  const size_t before = retiredLoops_.size();
  retiredLoops_.erase(std::remove_if(retiredLoops_.begin(), retiredLoops_.end(),
                                     [](const RetiredIoLoop& r) {
                                       return r.finished && r.finished->load();
                                     }),
                      retiredLoops_.end());
  if (before != retiredLoops_.size() && !retiredLoops_.empty()) {
    std::fprintf(stderr, "[tunnel] %zu io loop(s) still winding down\n", retiredLoops_.size());
  }
}

void TunnelHost::StopInternalLocked(const std::string& reason) {
  const bool hadSession = device_.has_value() || tunnel_ || ioLoop_.has_value();
  if (hadSession) {
    std::scoped_lock lock(statusMutex_);
    if (status_.tunnel_state == ctl::TunnelState::Up ||
        status_.tunnel_state == ctl::TunnelState::Starting) {
      status_.tunnel_state = ctl::TunnelState::Stopping;
    }
  }
  // The generation bump makes any IoLoop done callback still in flight a
  // no-op, so our own teardown can never be mistaken for a dead tunnel.
  sessionGeneration_.fetch_add(1);
  ioLoopDied_.store(false);

  // Reverse dependency order. close() actually stops the SDK goroutines and
  // the IoLoop; releasing the handle alone would leak them.
  if (device_) device_->setTunnelStarted(false);
  if (ioLoop_) {
    ioLoop_->close();  // ASYNCHRONOUS: asks the Go loop to stop, returns now
    // RETIRE, do not destroy. Destroying here frees the done callback that Go
    // still holds a raw pointer to, and the deferred call segfaults the daemon.
    retiredLoops_.push_back(RetiredIoLoop{std::move(*ioLoop_), ioLoopFinished_});
    ioLoop_.reset();
  }
  ioLoopFinished_.reset();
  ReapRetiredLoopsLocked();
  tunnel_.reset();  // closes the fd: the tun, its routes and the policy rules go
  if (device_) {
    device_->close();
    device_.reset();
  }
  // AFTER the device is gone, so no SDK socket is ever created unmarked while
  // the capture routes could still be up. The mark is inert once the `ip rule`
  // is removed (nothing consults it), so the ordering costs nothing and the
  // detach keeps the blast radius to the session that asked for it.
  egressMarker_.Detach();
  egressWitnessTicks_ = 0;
  egressWitnessFailures_ = 0;
  if (!reason.empty()) {
    // An explicit stop ALWAYS lifts the policy (windows semantics: only an
    // unexpected drop keeps or installs Armed) — FloorForTransition answers
    // `false` for every transition into Off, so this needs no argument and
    // cannot be given the wrong one. A failure here is remembered in
    // filterRemovalPending_ and retried by the reaper.
    std::string ignored;
    ApplyFilterLocked(FilterState::Off, &ignored);
  }
  // spaceManager_/networkSpace_ persist across sessions (Windows parity).
  {
    std::scoped_lock lock(statusMutex_);
    status_.tunnel_state = ctl::TunnelState::Stopped;
    status_.rpc_port = 0;
    status_.rpc_pinned = false;  // the listener died with the DeviceLocal
    status_.client_id.clear();
    activeConfig_ = ctl::StartTunnelRequest();
    status_.routes_installed = false;
    status_.egress_protected = false;
    status_.dns_applied = false;
    status_.dns_detail.clear();
    status_.tunnel_interface.clear();
    status_.up_since_millis = 0;
    if (!reason.empty()) {
      status_.stop_reason = reason;
      // THIS CLEAR IS WHY THE PROTECTIVE PATH PASSES AN EMPTY REASON. A stop
      // somebody asked for has no error to report, so the previous session's
      // error must not outlive it. But it means any caller that publishes a
      // reason and THEN calls this with a non-empty reason erases its own
      // message one line later — which is exactly what the egress-witness
      // teardown did. See StopUnsafeSessionLocked.
      status_.error.clear();
      status_.error_code.clear();
    }
  }
}

// THE PROTECTIVE TEARDOWN — the sibling of StopInternalLocked, and the
// difference between them is the whole point.
//
// B1, THE SAFETY INVERSION. StopInternalLocked(<non-empty reason>) means "the
// user asked to stop": it ends in ApplyFilterLocked(FilterState::Off), and
// FloorForTransition answers false for EVERY transition into Off ("a user
// disconnect ALWAYS lifts, even with the toggle on" — Tunnel.cpp), so the
// kill-switch floor comes down with the tunnel. That is right for a disconnect
// and precisely backwards here: the guards that call this fire on PROOF that
// traffic is leaving this machine unprotected, and the old code lifted the
// floor at that exact instant. The protection was dropped by the discovery that
// it was needed.
//
// So this does what the io-loop drop in Reap() has always done — tear down with
// an EMPTY reason, which leaves the firewall untouched, and then choose the
// landing state deliberately:
//
//   * the kill switch was asked for -> Armed. FAIL CLOSED. An involuntary
//     teardown IS an unexpected drop, which is the one case Windows parity
//     (docs/linux_agent_help.md §6.3) says arms rather than lifts, and this is
//     the drop the switch was bought for: the user said "never let me out
//     unprotected", and we have just measured unprotected.
//   * it was not -> Off, the table removed. Blocking a machine whose owner
//     never asked to be blocked would be inventing a kill switch on their
//     behalf and stranding them offline behind a toggle that reads "off" — the
//     failure mode this file's doctrine forbids. Stopping the tunnel IS the
//     protection they asked for.
//
// Either way the user is TOLD, in the app: tunnel_state=Error, stop_reason,
// kill_switch=Armed|Failed (so the UI can render the block without parsing
// prose) and an `error` sentence that says in words whether this machine is now
// blocked and what lifts it.
void TunnelHost::StopUnsafeSessionLocked(const std::string& reason,
                                         const std::string& message,
                                         const std::string& code) {
  // 1) TEAR DOWN, firewall untouched. The empty reason is load-bearing twice
  //    over: it keeps ApplyFilterLocked(Off) — the floor lift — out of this
  //    path, and it keeps the tail of StopInternalLocked from clearing the
  //    error we are about to publish.
  StopInternalLocked(std::string());

  // 2) LAND THE FLOOR DELIBERATELY, before anything is published, so what we
  //    publish describes the machine as it actually is now.
  const bool wantKillSwitch = killSwitchRequested_.load();
  std::string detail = message;
  if (wantKillSwitch) {
    std::string filterError;
    // FloorForTransition returns true for Armed structurally ("an Armed state
    // without the floor is not a state, it is an open machine with a label on
    // it"), so this cannot come back floorless and succeed.
    if (ApplyFilterLocked(FilterState::Armed, &filterError)) {
      detail +=
          " This machine is now blocked, because the kill switch is on and there is no tunnel "
          "to carry traffic. Turn the kill switch off, or disconnect, in the app to lift it.";
      DaemonLogf(
          "[tunnel] the session was stopped as UNSAFE (%s) and this machine stays blocked "
          "because the kill switch is armed. Turn it off in the app to lift it, or run: %s\n",
          reason.c_str(), NetFilter::RecoveryCommand());
    } else {
      // Say the smaller, scarier truth rather than the reassuring one: the
      // block we intended is not in force, and whatever is left behind is
      // whatever the failed `nft -f` did not replace. InstallFilterLocked has
      // already published kill_switch=Failed with the detail, so the UI cannot
      // render this as "off".
      detail += " The kill switch could not be armed after the tunnel was stopped (" +
                filterError +
                "), so this machine may not be blocked. If the network stays broken instead, "
                "lift the leftover rules with: " +
                NetFilter::RecoveryCommand() + ".";
      DaemonLogf(
          "[tunnel] ERROR: the session was stopped as UNSAFE (%s) and the kill switch could "
          "NOT be armed: %s\n",
          reason.c_str(), filterError.c_str());
    }
  } else {
    // No floor was asked for, so none is invented. The table goes; a failure
    // here is remembered in filterRemovalPending_ and retried by the reaper.
    //
    // BUT THE OUTCOME IS CHECKED, exactly as the armed branch above checks it.
    // Asserting "this machine is not blocked" on the strength of having ASKED
    // for the table to go is how a user ends up cut off while being told they
    // are fine — the same class of false reassurance this whole change exists
    // to remove.
    std::string filterError;
    if (ApplyFilterLocked(FilterState::Off, &filterError)) {
      detail +=
          " The kill switch is off, so this machine is not blocked and traffic is going out "
          "unprotected until you connect again.";
      DaemonLogf("[tunnel] the session was stopped as UNSAFE (%s); the kill switch is off, so "
                 "this machine is not blocked\n",
                 reason.c_str());
    } else {
      detail += " The leftover firewall rules could NOT be removed (" + filterError +
                "), so this machine may still be blocked even though the kill switch is off. "
                "Lift them with: " +
                NetFilter::RecoveryCommand() + ".";
      DaemonLogf(
          "[tunnel] ERROR: the session was stopped as UNSAFE (%s), the kill switch is off, and "
          "the leftover rules could NOT be removed: %s -- the machine may be blocked. "
          "Recover with: %s\n",
          reason.c_str(), filterError.c_str(), NetFilter::RecoveryCommand());
    }
  }

  // 3) PUBLISH LAST, AND ON PURPOSE.
  //
  //    B2. This is the SECOND time in this project that a published error has
  //    been erased by the very next line: PublishError(...) followed by
  //    StopInternalLocked("egress_unprotected"), whose tail clears
  //    status_.error/error_code for any non-empty reason. The user was told
  //    nothing at all about a teardown that had just cut them off.
  //
  //    The ordering is therefore a rule, not an accident: EVERYTHING that tears
  //    down, applies a filter, or otherwise mutates status_ goes ABOVE this
  //    block, and this block is the last thing the function does. If a step
  //    must run after it, that step may not touch status_.error/error_code/
  //    stop_reason/tunnel_state. All four are set together, under one lock, so
  //    a `status` answered between them cannot report an Error with no reason.
  //    The accepted cost: for the length of one `nft -f` a poll sees the
  //    Stopped that StopInternalLocked published, never Up, never a reasonless
  //    Error — and kill_switch is already correct by then, because step 2 is
  //    what set it.
  {
    std::scoped_lock lock(statusMutex_);
    status_.tunnel_state = ctl::TunnelState::Error;
    status_.stop_reason = reason;
    status_.error = detail;
    status_.error_code = code;
  }
}

// ---- provide mode / kill switch --------------------------------------------

bool TunnelHost::SetProvideMode(const std::string& mode) {
  {
    std::scoped_lock lock(statusMutex_);
    pendingProvideMode_ = mode;  // survives a stop/start cycle
  }
  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    // A bring-up is running and will apply pendingProvideMode_ itself.
    return true;
  }
  if (device_) {
    try {
      device_->setProvideControlMode(mode);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[tunnel] set provide mode failed: %s\n", e.what());
      return false;
    }
  }
  return true;
}

bool TunnelHost::SetKillSwitch(bool enabled, std::string* error) {
  killSwitchRequested_.store(enabled);
  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    // A bring-up owns the session; it reads killSwitchRequested_ when it
    // installs the Connected floor.
    return true;
  }
  const bool up = tunnel_ != nullptr;
  // Both branches below re-derive the floor from killSwitchRequested_, which
  // was set at the top of this function — so the toggle is the input and
  // FloorForTransition is still the only place the answer is produced.
  if (!enabled) {
    // Off while armed lifts immediately. With a live tunnel the Connected
    // floor stays (leak prevention is not a preference) minus the block-all.
    if (up) return ApplyFilterLocked(FilterState::Connected, error);
    std::string ignored;
    ApplyFilterLocked(FilterState::Off, &ignored);
    return true;
  }
  if (up) return ApplyFilterLocked(FilterState::Connected, error);
  // Switching it on with nothing connected must NOT cut the machine off: the
  // preference is recorded and takes effect at the next start, or the moment
  // a live tunnel drops unexpectedly. It must not LIE either: re-asserting the
  // toggle while the machine is already sitting on the Armed floor (an
  // unexpected drop) used to publish Off, i.e. "you are not blocked" to the one
  // user who is.
  {
    std::scoped_lock statusLock(statusMutex_);
    status_.kill_switch = filter_.floorInstalled() ? ctl::KillSwitchState::Armed
                                                   : ctl::KillSwitchState::Off;
    status_.kill_switch_detail.clear();
  }
  return true;
}

// ---- the reaper (main loop) ------------------------------------------------

gboolean TunnelHost::OnReaperTick(gpointer data) {
  static_cast<TunnelHost*>(data)->Reap();
  return G_SOURCE_CONTINUE;
}

void TunnelHost::MaintainFilterLocked() {
  // 1) A teardown that failed is retried until it takes. Nothing else will:
  //    NetFilter::Remove() moves state_ to Off before it looks at nft's exit
  //    status, so neither ~NetFilter nor a later Remove() knows there is
  //    anything left to undo.
  if (filterRemovalPending_.load()) {
    if (device_.has_value() || tunnel_ != nullptr || ioLoop_.has_value()) {
      // A new session installed its own table in the meantime; the stale
      // removal is not ours to make any more.
      filterRemovalPending_.store(false);
    } else {
      std::string error;
      if (InstallFilterLocked(FilterState::Off, /*floor=*/false, &error)) {
        DaemonLogf("[tunnel] the firewall teardown that failed earlier has now completed\n");
      }
      return;  // nothing is installed on purpose, so nothing to verify
    }
  }

  // 2) TAMPER / FLUSH DETECTION. nftables hands out no notification when a
  //    third party destroys our table, and `nft flush ruleset` is the FIRST
  //    LINE of the /etc/sysconfig/nftables.conf Fedora and Bazzite ship — so
  //    `systemctl restart nftables` silently deletes `table inet urnetwork`,
  //    taking the kill switch, the IPv6 fail-closed rules, the DNS floor and
  //    the egress self-exclusion with it, while the UI still says Connected.
  //    Polling Verify() here is the entire mitigation, and re-installing the
  //    SAME config (floor included) is the entire repair.
  if (!filter_.installed()) return;
  if (++filterVerifyTicks_ < kFilterVerifyIntervalSeconds) return;
  filterVerifyTicks_ = 0;

  std::string error;
  if (filter_.Verify(&error)) {
    filterVerifyFailures_ = 0;
    return;
  }
  const bool loud = (filterVerifyFailures_ % kFilterVerifyLogEvery) == 0;
  ++filterVerifyFailures_;
  if (loud) {
    DaemonLogf(
        "[tunnel] the URnetwork nftables table is no longer in force (%s); re-installing it. "
        "Something outside URnetwork flushed the ruleset — on Fedora/Bazzite "
        "`systemctl restart nftables` does exactly that.\n",
        error.c_str());
  }
  std::string reinstallError;
  if (!ReinstallFilterLocked(&reinstallError)) {
    if (loud) {
      DaemonLogf(
          "[tunnel] ERROR: the ruleset could not be re-installed: %s. The tunnel is running "
          "WITHOUT its leak floor%s.\n",
          reinstallError.c_str(),
          killSwitchRequested_.load() ? " and without the kill switch" : "");
    }
    return;
  }
  filterVerifyFailures_ = 0;
  DaemonLogf("[tunnel] the URnetwork nftables table has been re-installed (%s)\n",
             ToString(filter_.state()));
}

namespace {

// Read one of the tun's kernel byte counters. Absent/unreadable -> 0, which the
// guard treats as "no evidence", never as a reason to tear a tunnel down.
uint64_t ReadIfaceCounter(const std::string& iface, const char* which) {
  const std::string path = "/sys/class/net/" + iface + "/statistics/" + which;
  std::ifstream in(path);
  uint64_t v = 0;
  if (in >> v) return v;
  return 0;
}

}  // namespace

// A tunnel that transmits megabytes while receiving essentially nothing is not
// carrying traffic — it is AMPLIFYING it. That is what an egress-exclusion
// failure looks like from outside the SDK: the daemon's own packets fall into
// the tun, are read back out, re-sent, and captured again. Measured once on a
// real machine: 1.34 Gbps out, 0 in, 3.38 Tb sent before a human killed it.
//
// The guard is deliberately dumb and slow to fire — three consecutive strikes,
// each needing a large TX delta AND a negligible RX delta — so a genuinely
// upload-heavy session (a backup, a big send) cannot trip it: real uploads still
// draw ACKs, which move rx_bytes.
bool TunnelHost::CheckTunnelStormLocked() {
  if (!tunnel_) { stormStrikes_ = 0; stormLastTx_ = stormLastRx_ = 0; return false; }
  const std::string iface = tunnel_->name();
  if (iface.empty()) return false;

  const uint64_t tx = ReadIfaceCounter(iface, "tx_bytes");
  const uint64_t rx = ReadIfaceCounter(iface, "rx_bytes");
  if (tx == 0 && rx == 0) return false;  // counters unreadable: no evidence

  const uint64_t dtx = tx > stormLastTx_ ? tx - stormLastTx_ : 0;
  const uint64_t drx = rx > stormLastRx_ ? rx - stormLastRx_ : 0;
  stormLastTx_ = tx;
  stormLastRx_ = rx;

  // Per tick: >64 MiB out with <1 MiB back. At the reaper's cadence that is an
  // order of magnitude above any plausible real session.
  constexpr uint64_t kStormTxDelta = 64ull * 1024 * 1024;
  constexpr uint64_t kStormRxCeiling = 1ull * 1024 * 1024;
  if (dtx >= kStormTxDelta && drx < kStormRxCeiling) {
    ++stormStrikes_;
    std::fprintf(stderr,
                 "[tunnel] runaway: %llu MiB out and %llu KiB back since the last tick "
                 "(strike %d of 3)\n",
                 static_cast<unsigned long long>(dtx / (1024 * 1024)),
                 static_cast<unsigned long long>(drx / 1024), stormStrikes_);
  } else {
    stormStrikes_ = 0;
  }
  if (stormStrikes_ < 3) return false;

  std::fprintf(stderr,
               "[tunnel] STOPPING: the tunnel is amplifying traffic rather than carrying it. "
               "This is what a failed egress exclusion looks like -- the daemon's own packets "
               "are being captured into the tunnel. Tearing it down to protect the network.\n");
  stormStrikes_ = 0;
  // The SAME defect pair as the egress witness had, in the same shape: the
  // PublishError that used to sit here was erased by StopInternalLocked's tail
  // one line later, and that teardown lifted the kill-switch floor for a
  // machine whose tunnel had just been caught amplifying. A storm IS an egress
  // exclusion failure seen from outside the SDK, so it lands the same way.
  StopUnsafeSessionLocked(
      "tunnel_storm",
      "The connection was stopped because it was sending traffic in a loop instead of "
      "carrying it. This is a bug; please report it.",
      ctl::kCodeTunnelStorm);
  return true;
}

// Re-ask the only question that matters, with real packets, while the tunnel
// runs. See the contract in TunnelHost.hpp for why this exists alongside the
// storm guard rather than instead of it.
bool TunnelHost::CheckEgressWitnessLocked() {
  if (!tunnel_ || allowUnprotectedEgress_) {
    egressWitnessTicks_ = 0;
    egressWitnessFailures_ = 0;
    return false;
  }
  // Never while a bring-up owns the session: it is mid-way through swapping
  // the very table whose counters this reads, and a measurement taken across
  // an atomic table swap is not a measurement. The bring-up runs its own
  // witness at step 5 and again at step 7b.
  if (busy_.load()) {
    egressWitnessTicks_ = 0;
    return false;
  }
  if (++egressWitnessTicks_ < kEgressWitnessIntervalSeconds) return false;
  egressWitnessTicks_ = 0;

  TunnelError witnessError;
  if (tunnel_->VerifyEgressWitness("recheck", &witnessError)) {
    egressWitnessFailures_ = 0;
    {
      std::scoped_lock lock(statusMutex_);
      status_.egress_protected = true;
    }
    return false;
  }

  ++egressWitnessFailures_;
  {
    std::scoped_lock lock(statusMutex_);
    status_.egress_protected = false;
  }
  if (egressWitnessFailures_ < 2) {
    DaemonLogf(
        "[tunnel] WARNING: the egress witness failed (%s). Re-checking in %ds; two in a row "
        "tears this tunnel down.\n",
        witnessError.message.c_str(), static_cast<int>(kEgressWitnessIntervalSeconds));
    return false;
  }

  DaemonLogf(
      "[tunnel] STOPPING: this daemon's own traffic is no longer demonstrably outside its own "
      "tunnel, twice running. %s\n",
      witnessError.message.c_str());
  egressWitnessFailures_ = 0;
  // NOT StopInternalLocked("egress_unprotected"). That published nothing (its
  // tail clears the error) and lifted the kill-switch floor on the strength of
  // having PROVEN the traffic unprotected. StopUnsafeSessionLocked fails closed
  // and explains itself; see its contract.
  StopUnsafeSessionLocked(
      "egress_unprotected",
      "The connection was stopped because this app's own traffic was no longer going out "
      "around the tunnel. Left running, that makes the tunnel send in a loop instead of "
      "carrying your traffic. (" + witnessError.message + ")",
      ctl::kCodeEgressUnprotected);
  return true;
}

void TunnelHost::Reap() {
  {
    std::scoped_lock lock(opMutex_);
    ReapRetiredLoopsLocked();
    if (CheckTunnelStormLocked()) return;  // the tunnel is gone; nothing else to reap
    // Ordered AFTER the storm guard: if traffic is already amplifying, stop it
    // on the cheap byte-counter read rather than spending an nft fork first.
    if (CheckEgressWitnessLocked()) return;
  }
  if (busy_.load()) return;  // a bring-up owns the session AND the filter

  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) return;  // next tick

  const bool died = ioLoopDied_.load();
  const int orphanTimeout = orphanTimeoutSeconds_.load();
  const bool orphaned = orphanTimeout > 0 && !ownerConnected_.load() &&
                        ownerLostMonotonicSeconds_ > 0 &&
                        MonotonicSeconds() - ownerLostMonotonicSeconds_ >= orphanTimeout;
  if (!died && !orphaned) {
    // THE STEADY STATE, which is where the whole tamper problem lives and is
    // why NetFilter::Verify() had no caller at all: a machine whose ruleset
    // was flushed under it has neither a dead io loop nor an absent owner. It
    // looks perfectly healthy from in here, and the early return above it used
    // to be the end of the tick.
    MaintainFilterLocked();
    return;
  }

  if (died) {
    // A STALE FLAG IS NOT A DROP, and this guard is part of the B2 fix. The
    // done callback runs on an SDK thread: it compares session generations and
    // only then sets ioLoopDied_, so a callback that read the generation an
    // instant before a teardown bumped it can still land here with no session
    // left. When that happens the drop has ALREADY been accounted for — by
    // StopUnsafeSessionLocked, say, which has just published "your own traffic
    // was no longer outside your own tunnel" — and running the block below
    // would tear down nothing, republish tunnel_state and overwrite that
    // reason with the generic "the tunnel stopped unexpectedly". Whoever
    // handled the drop owns the explanation.
    const bool hadSession = device_.has_value() || tunnel_ != nullptr || ioLoop_.has_value();
    ioLoopDied_.store(false);
    if (!hadSession) {
      MaintainFilterLocked();  // the floor it landed on is still ours to keep
      return;
    }

    // The tunnel went away under us. Before this, the done callback only
    // fprintf'd and the published state stayed Up forever.
    std::fprintf(stderr, "[tunnel] the io loop ended unexpectedly; tearing the session down\n");
    StopInternalLocked(std::string());
    // An UNEXPECTED drop is the one case that arms the kill switch.
    if (killSwitchRequested_.load()) {
      std::string error;
      if (!ApplyFilterLocked(FilterState::Armed, &error)) {
        // Published as KillSwitchState::Failed by InstallFilterLocked, so the
        // UI cannot render this as "off".
        DaemonLogf("[tunnel] ERROR: could not arm the kill switch: %s\n", error.c_str());
      }
    } else {
      std::string ignored;
      ApplyFilterLocked(FilterState::Off, &ignored);
    }
    // PUBLISH LAST, by the same rule StopUnsafeSessionLocked spells out: the
    // teardown and the filter apply both mutate status_, so the reason goes
    // after them, never before. It used to sit above the apply and survive only
    // because ApplyFilterLocked happens not to touch these four fields — an
    // incidental ordering, which is precisely how the erasure this comment
    // exists for got written twice. The cost is bounded and accepted: for the
    // length of one `nft -f` a `status` poll sees Stopped rather than Error,
    // never Up, and never an Error with no reason attached.
    {
      std::scoped_lock statusLock(statusMutex_);
      status_.tunnel_state = ctl::TunnelState::Error;
      status_.stop_reason = "io_loop";
      status_.error = "the tunnel stopped unexpectedly";
      status_.error_code = ctl::kCodeTunOpenFailed;
    }
    return;
  }

  std::fprintf(stderr,
               "[tunnel] no control client has owned this tunnel for %ds; stopping it\n",
               orphanTimeout);
  ownerLostMonotonicSeconds_ = 0;
  StopInternalLocked("orphaned");
}

// ---- systemd-resolved restart watch ----------------------------------------

void TunnelHost::OnResolvedAppeared(GDBusConnection*, const gchar*, const gchar*,
                                    gpointer data) {
  auto* self = static_cast<TunnelHost*>(data);
  if (!self->resolvedSeen_) {
    self->resolvedSeen_ = true;  // the first appearance is just "it is running"
    return;
  }
  std::unique_lock<std::mutex> lock(self->opMutex_, std::try_to_lock);
  if (!lock.owns_lock() || !self->tunnel_) return;
  std::fprintf(stderr, "[tunnel] systemd-resolved restarted: re-applying the DNS override\n");
  const bool applied = self->tunnel_->ApplyDns();
  const TunnelReport& report = self->tunnel_->report();
  {
    std::scoped_lock statusLock(self->statusMutex_);
    self->status_.dns_applied = report.dns_applied;
    self->status_.dns_detail = report.dns_detail;
  }
  if (!applied) {
    std::fprintf(stderr, "[tunnel] the DNS override did not survive the restart: %s\n",
                 report.dns_detail.c_str());
  }
  // The DNS port floor is only correct while the override is in force, and the
  // resolvers it pins come back out of the tunnel on this same pass — so this
  // re-apply is what keeps the pinned :53 permit naming the servers resolved
  // was actually given.
  std::string ignored;
  self->ApplyFilterLocked(FilterState::Connected, &ignored);
}

}  // namespace urnw
