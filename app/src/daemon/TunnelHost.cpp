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

// "127.0.0.1:12025" -> 12025; 0 when there is no usable port.
int PortFromHostPort(const std::string& hostPort) {
  const size_t colon = hostPort.rfind(':');
  if (colon == std::string::npos || colon + 1 >= hostPort.size()) return 0;
  return std::atoi(hostPort.c_str() + colon + 1);
}

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

bool TunnelHost::ApplyFilterLocked(FilterState state, bool killSwitch, std::string* error) {
  FilterConfig cfg;
  cfg.state = state;
  cfg.kill_switch = killSwitch;
  cfg.cgroup = cgroup_;
  cfg.block_ipv6 = true;  // leak prevention is not a preference (§6.3)
  if (state == FilterState::Connected && tunnel_) {
    cfg.tun_name = tunnel_->name();
    // Only close the off-tunnel DNS ports when DNS actually landed on the
    // tunnel. Blocking them when the override failed would leave the machine
    // unable to resolve at all; the honest alternative is to report
    // dns_applied=false loudly, which Status() does.
    cfg.block_offtunnel_dns = tunnel_->report().dns_applied;
  }
  const bool ok = filter_.Apply(cfg, error);
  ctl::KillSwitchState published = ctl::KillSwitchState::Off;
  if (!ok && killSwitch) {
    published = ctl::KillSwitchState::Failed;
  } else if (ok && killSwitch) {
    published = (state == FilterState::Connected) ? ctl::KillSwitchState::Connected
                                                  : ctl::KillSwitchState::Armed;
  }
  {
    std::scoped_lock lock(statusMutex_);
    status_.kill_switch = published;
    status_.kill_switch_detail =
        (published == ctl::KillSwitchState::Failed) ? filter_.lastError() : std::string();
    status_.ipv6_blocked = ok && state == FilterState::Connected && cfg.block_ipv6;
  }
  return ok;
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
    // Idempotent restart, but NOT a "stop": no stop_reason is recorded for a
    // teardown that exists only to make room for this start.
    StopInternalLocked(std::string());
    const uint64_t generation = sessionGeneration_.fetch_add(1) + 1;
    ioLoopDied_.store(false);

    try {
      // --- 1) egress self-exclusion FIRST -----------------------------------
      // The mark chain has to exist before a single capture route lands, or
      // the daemon's own SDK sockets (jwt refresh, window enumeration, DoH,
      // contract waits, the provider transports) fall into our own tun and the
      // control plane starves. urnet::setEgressInterfaceIndex cannot do this
      // job: connect:egress_other.go makes applyEgressInterface a no-op off
      // Windows.
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
        // Deliberately NO block floor during the bring-up, even when the kill
        // switch was requested: a failed start must not leave the machine cut
        // off. Windows arms during Connecting because its Go resolver leaves
        // through svchost; on Linux the daemon's own lookups leave its own
        // sockets and are covered by the mark, so the tighter state is free.
        if (!ApplyFilterLocked(FilterState::Idle, /*killSwitch=*/false, &filterError)) {
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

      // --- 3) DeviceLocal, rpc enabled --------------------------------------
      const std::string appVersion =
          config.app_version.empty() ? kUrAppVersionFallback : config.app_version;
      const bool hadStoredMaterial = HasStoredKeyMaterial();
      bool restoreFailed = false;
      if (auto km = LoadKeyMaterial()) {
        try {
          device_ = urnet::newDeviceLocalWithKeyMaterial(
              *networkSpace_, config.by_jwt, UrDeviceDescription(), UrDeviceSpec(), appVersion,
              config.instance_id, /*enable_rpc=*/true, *km);
        } catch (const std::exception& e) {
          restoreFailed = true;
          std::fprintf(stderr, "[tunnel] restore device key material failed: %s\n", e.what());
        }
      }
      if (!device_) {
        device_ = urnet::newDeviceLocalWithDefaults(*networkSpace_, config.by_jwt,
                                                    UrDeviceDescription(), UrDeviceSpec(),
                                                    appVersion, config.instance_id,
                                                    /*enable_rpc=*/true);
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
      // Both halves must pin the SAME generated material; the SDK compares raw
      // certificates for equality, so no "defaults" path can produce a
      // matching pair across two processes with separate storage roots. Absent
      // fields keep the SDK's built-in default listener, which is what shipped
      // before this field existed.
      int rpcPort = ctl::kDeviceRpcPort;
      if (!config.rpc_server_pem.empty()) {
        device_->setRpcServer(config.rpc_server_pem, config.rpc_client_cert_pem,
                              config.rpc_listen_hostport);
        if (const int port = PortFromHostPort(config.rpc_listen_hostport); port > 0) {
          rpcPort = port;
        }
        std::fprintf(stderr, "[tunnel] device rpc pinned on %s\n",
                     config.rpc_listen_hostport.c_str());
      }

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
      ioLoop_ = urnet::newIoLoop(*device_, tunnel_->fd(), [this, generation] {
        // SDK THREAD. Publish only: the teardown (and arming the kill switch
        // on an unexpected drop) happens on the reaper, on the main loop.
        if (sessionGeneration_.load() != generation) return;  // our own Stop
        ioLoopDied_.store(true);
        std::fprintf(stderr, "[tunnel] io loop finished\n");
      });
      device_->setTunnelStarted(true);

      // --- 7) the leak floor, now that the tun exists -----------------------
      std::string filterError;
      const bool wantKillSwitch = killSwitchRequested_.load();
      if (!ApplyFilterLocked(FilterState::Connected, wantKillSwitch, &filterError)) {
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

      {
        std::scoped_lock lock(statusMutex_);
        status_.tunnel_state = ctl::TunnelState::Up;
        status_.rpc_port = rpcPort;
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
      {
        std::scoped_lock lock(statusMutex_);
        status_.tunnel_state = ctl::TunnelState::Error;
        status_.error = keptError;
        status_.error_code = keptCode;
        status_.stop_reason = "start_failed";
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
  if (ioLoop_) ioLoop_->close();
  ioLoop_.reset();
  tunnel_.reset();  // closes the fd: the tun, its routes and the policy rules go
  if (device_) {
    device_->close();
    device_.reset();
  }
  if (!reason.empty()) {
    // An explicit stop ALWAYS lifts the policy (windows semantics: only an
    // unexpected drop keeps or installs Armed).
    std::string ignored;
    ApplyFilterLocked(FilterState::Off, /*killSwitch=*/false, &ignored);
  }
  // spaceManager_/networkSpace_ persist across sessions (Windows parity).
  {
    std::scoped_lock lock(statusMutex_);
    status_.tunnel_state = ctl::TunnelState::Stopped;
    status_.rpc_port = 0;
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
      status_.error.clear();
      status_.error_code.clear();
    }
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
  if (!enabled) {
    // Off while armed lifts immediately. With a live tunnel the Connected
    // floor stays (leak prevention is not a preference) minus the block-all.
    if (up) return ApplyFilterLocked(FilterState::Connected, false, error);
    std::string ignored;
    ApplyFilterLocked(FilterState::Off, false, &ignored);
    return true;
  }
  if (up) return ApplyFilterLocked(FilterState::Connected, true, error);
  // Switching it on with nothing connected must NOT cut the machine off: the
  // preference is recorded and takes effect at the next start, or the moment
  // a live tunnel drops unexpectedly.
  {
    std::scoped_lock statusLock(statusMutex_);
    status_.kill_switch = ctl::KillSwitchState::Off;
    status_.kill_switch_detail.clear();
  }
  return true;
}

// ---- the reaper (main loop) ------------------------------------------------

gboolean TunnelHost::OnReaperTick(gpointer data) {
  static_cast<TunnelHost*>(data)->Reap();
  return G_SOURCE_CONTINUE;
}

void TunnelHost::Reap() {
  if (busy_.load()) return;  // a bring-up owns the session
  const bool died = ioLoopDied_.load();
  const int orphanTimeout = orphanTimeoutSeconds_.load();
  const bool orphaned = orphanTimeout > 0 && !ownerConnected_.load() &&
                        ownerLostMonotonicSeconds_ > 0 &&
                        MonotonicSeconds() - ownerLostMonotonicSeconds_ >= orphanTimeout;
  if (!died && !orphaned) return;

  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) return;  // next tick

  if (died) {
    // The tunnel went away under us. Before this, the done callback only
    // fprintf'd and the published state stayed Up forever.
    std::fprintf(stderr, "[tunnel] the io loop ended unexpectedly; tearing the session down\n");
    StopInternalLocked(std::string());
    {
      std::scoped_lock statusLock(statusMutex_);
      status_.tunnel_state = ctl::TunnelState::Error;
      status_.stop_reason = "io_loop";
      status_.error = "the tunnel stopped unexpectedly";
      status_.error_code = ctl::kCodeTunOpenFailed;
    }
    // An UNEXPECTED drop is the one case that arms the kill switch.
    if (killSwitchRequested_.load()) {
      std::string error;
      if (!ApplyFilterLocked(FilterState::Idle, true, &error)) {
        std::fprintf(stderr, "[tunnel] could not arm the kill switch: %s\n", error.c_str());
      }
    } else {
      std::string ignored;
      ApplyFilterLocked(FilterState::Off, false, &ignored);
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
  // The DNS port floor is only correct while the override is in force.
  std::string ignored;
  self->ApplyFilterLocked(FilterState::Connected, self->killSwitchRequested_.load(), &ignored);
}

}  // namespace urnw
