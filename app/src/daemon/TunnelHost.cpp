// SPDX-License-Identifier: MPL-2.0
#include "TunnelHost.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>

#include <gio/gio.h>

#include "IoLoopFd.hpp"
#include "LogUpload.hpp"
#include "NetworkSpaceConfig.hpp"
#include "ProvideLifecycle.hpp"
#include "TunnelPolicy.hpp"
#include "daemon/HostMemory.hpp"
#include "daemon/DaemonLog.hpp"
#include "daemon/GlogFlusher.hpp"
#include "daemon/SelfRestart.hpp"
#include "daemon/SupportDiagnostics.hpp"
#include "daemon/TextFile.hpp"

namespace urnw {
namespace {

// Persisted device identity, file-per-part like the Windows TunnelController
// (client_key_seed.bin / provide_cert.pem / provide_key.pem).
constexpr const char* kClientKeySeedFile = "client_key_seed.bin";
constexpr const char* kProvideCertFile = "provide_cert.pem";
constexpr const char* kProvideKeyFile = "provide_key.pem";

// The main-loop reaper does the jobs that have to happen somewhere the session
// objects can safely be destroyed: notice a dead IoLoop, arm the kill switch
// after an UNEXPECTED drop, enforce the orphan timeout, and every re-check of
// the machine the session changed. Its cadence and every re-check's take their
// timings from the TunnelHost's HostSettings (daemon/HostSettings.hpp).

int64_t UnixMillis() { return g_get_real_time() / 1000; }
int64_t MonotonicSeconds() { return g_get_monotonic_time() / G_USEC_PER_SEC; }
int64_t MonotonicMillis() { return g_get_monotonic_time() / 1000; }

// The dead-tunnel failsafe's one clock (TunnelWatchdog.hpp). CLOCK_BOOTTIME,
// not the monotonic clock: it runs through a suspend, so a resume reaches the
// watch as the tick gap it is and rebases it, where the monotonic clock would
// hide the sleep and judge a session against evidence from before it.
int64_t WatchdogMillis() {
  timespec now{};
#if defined(CLOCK_BOOTTIME)
  ::clock_gettime(CLOCK_BOOTTIME, &now);
#else  // macOS dev build
  ::clock_gettime(CLOCK_MONOTONIC, &now);
#endif
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// How long the daemon's teardown waits for a log upload's thread to come out
// of the sdk's call (the zip): past it the process exits around it.
constexpr std::chrono::milliseconds kLogUploadReturnBudget{2000};

// What the sdk's upload callback needs to end the upload in the flight. Owned
// by the call once the sdk took it, freed by the callback.
struct LogUploadReport {
  std::shared_ptr<logupload::Flight> flight;
  int64_t uploadId = 0;
  const char* carrierName = "";
};

// The sdk's upload callback (urnet_upload_logs_cb), on an SDK thread: the
// server's answer or the post's error ends the upload in the flight.
void OnLogUploadReport(void* userData, const char* resultJson, const char* error) {
  std::unique_ptr<LogUploadReport> report(static_cast<LogUploadReport*>(userData));
  logupload::FlightState state = logupload::FlightState::Uploaded;
  if (error != nullptr) {
    state = logupload::FlightState::Failed;
    DaemonLogf("[support] the log upload (%s device) failed: %s\n", report->carrierName, error);
  } else if (resultJson != nullptr) {
    try {
      const auto result = nlohmann::json::parse(resultJson).get<urnet::UploadLogsResult>();
      if (result.error) {
        state = logupload::FlightState::Refused;
        DaemonLogf("[support] the log upload (%s device) was refused: %s\n",
                   report->carrierName, result.error->message.c_str());
      }
    } catch (const std::exception& e) {
      state = logupload::FlightState::Failed;
      DaemonLogf("[support] the log upload's (%s device) answer did not parse: %s\n",
                 report->carrierName, e.what());
    }
  }
  if (state == logupload::FlightState::Uploaded) {
    DaemonLogf("[support] the log upload (%s device) finished\n", report->carrierName);
  }
  report->flight->Finish(report->uploadId, state);
}

// The upload's own thread (logupload::Flight::Run). The line naming the carrier
// goes into the files the zip takes, before it; then the sdk zips this
// process's log files, with the GUI's under gui/, and starts the post to
// /log/{feedback_id}/upload. The call goes through the c abi by the device's
// handle, which the flight keeps valid until it returns, so that nothing of
// TunnelHost is touched here. The sdk reads the GUI's files through its own
// duplicates of their descriptors before the call returns
// (DeviceLocal.UploadLogsWithFiles), so they are closed right after it.
void UploadLogsOnDevice(const std::shared_ptr<logupload::Flight>& flight, int64_t uploadId,
                        uint64_t deviceHandle, const std::string& feedbackId,
                        const char* carrierName, logupload::PassedLogFiles& guiLogFiles) {
  urnet::logAppInfo("log-upload", std::string("carrier=") + carrierName +
                                      " gui_log_files=" + std::to_string(guiLogFiles.Size()));
  urnet::UploadLogsFileList uploadLogsFiles;
  for (const logupload::PassedLogFiles::File& file : guiLogFiles.Files()) {
    urnet::UploadLogsFile uploadLogsFile;
    uploadLogsFile.Source = logupload::kGuiLogFilesSource;
    uploadLogsFile.Name = file.name;
    uploadLogsFile.FileDescriptor = file.fd;
    uploadLogsFiles.push_back(std::move(uploadLogsFile));
  }
  // The names come from the GUI: a byte that is not UTF-8 becomes U+FFFD
  // instead of throwing on this thread (ControlProtocol.hpp DumpForWire).
  const std::string uploadLogsFilesJson = ctl::DumpForWire(nlohmann::json(uploadLogsFiles));
  auto report = std::make_unique<LogUploadReport>();
  report->flight = flight;
  report->uploadId = uploadId;
  report->carrierName = carrierName;
  char* error = nullptr;
  const bool started = urnet_device_local_upload_logs_with_files(
      deviceHandle, feedbackId.c_str(), uploadLogsFilesJson.c_str(), &OnLogUploadReport,
      report.get(), &error);
  guiLogFiles.CloseAll();
  if (started) {
    // the callback owns it now
    report.release();
    return;
  }
  const std::string message = error != nullptr ? error : "the device is gone";
  if (error != nullptr) urnet_free_string(error);
  DaemonLogf("[support] the log upload (%s device) did not start: %s\n", carrierName,
             message.c_str());
  flight->Finish(uploadId, logupload::FlightState::Failed);
}

// Key material only, and deliberately not textfile::ReadAll: a read that
// fails after the open throws here. A seed that is there but cannot be read
// must not pass for a missing one, or NewDeviceLocked would write a new
// identity over it; every caller runs under a catch, so the start fails and
// the stored files stay.
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

// The stop record for the next daemon (SelfRestart.hpp): written beside the
// file and renamed over it, so the next daemon reads a whole record or none.
// Plain syscalls: nothing here may call into the SDK.
bool WriteStopRecord(const selfrestart::StopRecord& record, std::string* error) {
  const std::string text = selfrestart::EncodeStopRecord(record);
  const std::string path = selfrestart::kStopRecordPath;
  const std::string staged = path + ".new";
  ::mkdir(ctl::kControlSocketDir, 0755);
  const int fd = ::open(staged.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    *error = std::strerror(errno);
    return false;
  }
  size_t written = 0;
  while (written < text.size()) {
    const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      *error = std::strerror(errno);
      ::close(fd);
      ::unlink(staged.c_str());
      return false;
    }
    written += static_cast<size_t>(n);
  }
  ::close(fd);
  if (::rename(staged.c_str(), path.c_str()) != 0) {
    *error = std::strerror(errno);
    ::unlink(staged.c_str());
    return false;
  }
  return true;
}

// The file-local PortFromHostPort that used to live here is GONE. It parsed
// with std::atoi, so "127.0.0.1:notaport" yielded 0 and the published rpc_port
// silently stayed at the SDK default while the listener was somewhere else —
// a mismatch the GUI had no way to detect. Both halves now call
// ctl::RpcPortFromHostPort, which is the same function the shared validator
// uses, so the reply's rpc_port is a trustworthy cross-check.

// The network path the reaper watches (defined with the reaper's other
// readers, below): needed here too, where a device is created and the
// tracker takes the network that device starts on as its baseline.
LinuxNetworkQualitySnapshot ReadNetworkPath(const std::string& tunnelInterface);

}  // namespace

TunnelHost::TunnelHost(std::string storageRoot, HostSettings settings)
    : settings_(std::move(settings)),
      storageRoot_(std::move(storageRoot)),
      logUploadFlight_(std::make_shared<logupload::Flight>(UnixMillis())) {
  cgroup_ = SelfCgroupV2();
  // urnetwork-exclude needs the unified hierarchy alone; ReportPreflight says
  // which this host is.
  {
    std::ifstream f("/proc/self/cgroup");
    std::ostringstream text;
    text << f.rdbuf();
    cgroupV2Only_ = IsCgroupV2Only(text.str());
  }
  if (const char* env = std::getenv("URNETWORK_ALLOW_UNPROTECTED_EGRESS");
      env != nullptr && *env != '\0' && std::string(env) != "0") {
    allowUnprotectedEgress_ = true;
  }
  // The reaper ticks the dead-tunnel watch, whose windows and freeze bar are
  // counted in the interval it was told it would be ticked at.
  if (static_cast<int64_t>(ReaperIntervalSeconds(settings_)) * 1000 !=
      settings_.dead_tunnel.evaluateIntervalMillis) {
    DaemonLogf("[tunnel] WARNING: the reaper ticks every %us but the failsafe was set up for a "
               "tick every %lldms; its freeze bar is counted in the latter\n",
               ReaperIntervalSeconds(settings_),
               static_cast<long long>(settings_.dead_tunnel.evaluateIntervalMillis));
  }
  reaperId_ =
      g_timeout_add_seconds(ReaperIntervalSeconds(settings_), &TunnelHost::OnReaperTick, this);
  // systemd-resolved forgets every per-link setting across a restart, and a
  // DHCP search domain can beat our `~.` default-route domain afterwards
  // (docs/linux_agent_help.md R5). Watching the bus name is the cheap,
  // event-driven way to notice; the re-push itself runs on the main loop.
  resolvedWatchId_ = g_bus_watch_name(G_BUS_TYPE_SYSTEM, "org.freedesktop.resolve1",
                                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                                      &TunnelHost::OnResolvedAppeared, nullptr, this, nullptr);
}

void TunnelHost::RestoreStopRecord() {
  if (::access(selfrestart::kStopRecordPath, F_OK) != 0) return;
  // A record that cannot be read decodes as unreadable below and is dropped.
  const std::string text = textfile::ReadAll(selfrestart::kStopRecordPath);
  // Read once, whatever it held: a record is about one stop.
  ::unlink(selfrestart::kStopRecordPath);
  const std::optional<selfrestart::StopRecord> record =
      selfrestart::DecodeStopRecord(text, WatchdogMillis());
  if (!record) {
    DaemonLogf("[daemon] dropped a stop record from a previous daemon: stale or unreadable\n");
    return;
  }
  {
    std::scoped_lock lock(statusMutex_);
    status_.tunnel_state = ctl::TunnelState::Error;
    status_.stop_reason = record->stopReason;
    status_.error = record->error;
    status_.error_code = record->code;
  }
  DaemonLogf("[daemon] the previous daemon ended itself after it stopped its tunnel (%s); "
             "status reports that stop until the next start\n",
             record->stopReason.c_str());
  // The stopping daemon could not write this line: it was the SDK's log.
  support::LogTunnelEnded("stopped", record->stopReason, record->code);
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
  // A log upload still zipping is given a moment to come out of the sdk's
  // call, and no more: it holds nothing of this object, and the exit must not
  // wait on a disk.
  if (!logUploadFlight_->WaitReturned(kLogUploadReturnBudget)) {
    DaemonLogf("[support] a log upload was still zipping at shutdown\n");
  }
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
  // The log upload in flight, off the flight's own lock and before this one.
  const logupload::Flight::Reading upload = logUploadFlight_->Read(MonotonicMillis());
  std::scoped_lock lock(statusMutex_);
  ctl::StatusReply s = status_;
  s.log_upload_id = upload.id;
  s.log_upload_state = logupload::ToString(upload.state);
  s.log_upload_carrier = upload.id == 0 ? "" : logupload::ToString(upload.carrier);
  s.owner_connected = ownerConnected_.load();
  // The live session's identity, published on EVERY status — this is what
  // attach_tunnel compares against and what a relaunching GUI matches its
  // stored record to (RpcSessionMatchesStatus). Both are empty unless a tunnel
  // is up; see the members' comment for the single set site and the two clear
  // sites that guarantee it.
  s.instance_id = instanceId_;
  s.rpc_session_id = rpcSessionId_;
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
         // The generation NAME is part of what must match. Adoption may not
         // silently rename a live session: the name is what `status` publishes
         // and what every other holder of this generation would attach by, so
         // adopting under a new one would leave the daemon telling two
         // different clients two different truths about one listener.
         activeConfig_.rpc_session_id == config.rpc_session_id &&
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

void TunnelHost::SetOwnerUid(int64_t uid) { ownerUid_.store(uid); }

void TunnelHost::SetNetworkCountryCode(const std::string& countryCode) {
  // The sdk's own setter, not a device's: the value belongs to the network,
  // and the next device (a Connect, the provider-only device) must be built
  // with it already in force.
  urnet::setNetworkCountryCode(countryCode);
  std::scoped_lock lock(statusMutex_);
  status_.network_country_code = countryCode;
}

// ---- the nftables floor ----------------------------------------------------

FilterConfig TunnelHost::FilterConfigForLocked(FilterState state, bool floor,
                                               uint64_t* excludeId) const {
  if (excludeId != nullptr) *excludeId = 0;
  FilterConfig cfg;
  cfg.state = state;
  cfg.floor = floor;
  cfg.cgroup = cgroup_;
  cfg.socket_mark_proven = socketMarkerProven_;
  cfg.cgroup_socket_match_supported = cgroupSocketMatchSupported_;
  // off-tunnel v6 is refused in every installed state: v6 leaves through the
  // tunnel or not at all -- leak prevention is not a preference (§6.3)
  cfg.block_ipv6 = true;
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
  if (state != FilterState::Off) {
    // The tunnel owner's urnetwork-exclude slice, in every installed state:
    // in Armed too, where the floor lets it out and nothing else.
    const CgroupRef slice = ExcludeSliceLocked(excludeId);
    if (slice.valid) cfg.exclude_cgroups.push_back(slice);
  }
  return cfg;
}

CgroupRef TunnelHost::ExcludeSliceLocked(uint64_t* id) const {
  if (id != nullptr) *id = 0;
  // The slice is matched by socket cgroup: the unified hierarchy has to be the
  // only one, and nft has to support the match (re-probed every session).
  if (!cgroupV2Only_ || !cgroupSocketMatchSupported_) return CgroupRef();
  const std::string path = ExcludeSliceCgroupPath(ownerUid_.load());
  if (path.empty()) return CgroupRef();
  struct stat st {};
  const std::string full = std::string("/sys/fs/cgroup/") + path;
  if (::stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return CgroupRef();
  const uint64_t inode = static_cast<uint64_t>(st.st_ino);
  if (inode == excludeRefusedId_) return CgroupRef();
  CgroupRef ref;
  ref.valid = true;
  ref.path = path;
  ref.level = CgroupPathLevel(path);
  if (id != nullptr) *id = inode;
  return ref;
}

bool TunnelHost::InstallFilterLocked(FilterState state, bool floor, std::string* error) {
  uint64_t excludeId = 0;
  const FilterConfig cfg = FilterConfigForLocked(state, floor, &excludeId);
  const bool ok = filter_.Apply(cfg, error);
  if (ok) {
    // What is in force: NetFilter::Apply drops a slice the kernel refused.
    const bool excluded = !filter_.appliedConfig().exclude_cgroups.empty();
    if (excludeId != 0 && !excluded) {
      excludeRefusedId_ = excludeId;
      DaemonLogf("[tunnel] the per-app exclusion for %s could not be installed; its commands "
                 "stay in the tunnel\n",
                 cfg.exclude_cgroups.front().path.c_str());
    }
    const uint64_t appliedId = excluded ? excludeId : 0;
    if (appliedId != excludeAppliedId_) {
      if (appliedId != 0) {
        DaemonLogf("[tunnel] per-app exclusion in force for %s\n",
                   cfg.exclude_cgroups.front().path.c_str());
      } else if (excludeAppliedId_ != 0) {
        DaemonLogf("[tunnel] per-app exclusion lifted\n");
      }
    }
    excludeAppliedId_ = appliedId;
  }

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
          static_cast<int>(ReaperIntervalSeconds(settings_)), NetFilter::RecoveryCommand());
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
  // The support log's view of the same: what is in force after this
  // transaction, which is what a "my network is blocked" report needs.
  {
    diag::KillSwitchFacts facts;
    facts.requested = killSwitchRequested_.load();
    facts.state = ToString(filter_.state());
    facts.published = ctl::ToString(Status().kill_switch);
    facts.floor = filter_.floorInstalled();
    facts.ipv6_blocked = filter_.installed() && RulesetBlocksIpv6(filter_.appliedConfig());
    facts.dns_floor = filter_.installed() && RulesetPinsDns(filter_.appliedConfig());
    facts.exclusion = excludeAppliedId_ != 0;
    facts.applied = ok;
    support::LogKillSwitch(facts);
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
  // The bring-up writes the SDK's log from here on; the reaper takes over once
  // it is over.
  glogflush::SetActive(true);
  killSwitchRequested_.store(config.kill_switch);
  {
    std::scoped_lock lock(statusMutex_);
    status_.tunnel_state = ctl::TunnelState::Starting;
    status_.error.clear();
    status_.error_code.clear();
    status_.stop_reason.clear();
    status_.failsafe_armed = false;
    status_.routes_installed = false;
    status_.ipv6_captured = false;
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
    // Cleared for the same reason rpc_pinned is: a client polling through an
    // async bring-up must never see the PREVIOUS session's identity attached to
    // a tunnel that is starting. Empty means "there is nothing to attach to",
    // which is the truth from here until the up edge below re-latches it.
    instanceId_.clear();
    rpcSessionId_.clear();
  }
  if (config.async) {
    worker_ = std::thread(&TunnelHost::RunStart, this, config);
    return Status();  // tunnel_state=starting; the client polls
  }
  RunStart(config);
  return Status();
}

// An absent or broken json falls back to the compiled-in default — silence
// means production, never a surprise server.
void TunnelHost::LoadNetworkSpaceLocked(const std::string& networkSpaceJson) {
  if (!spaceManager_) {
    spaceManager_ = urnet::newNetworkSpaceManager(storageRoot_ + "/sdk");
    // The daemon owns this storage, so the move of a space stored under
    // the retired ur.network key is its own job, done once where the
    // manager is created and before the import below can materialize
    // the current key (NetworkSpaceBootstrap.hpp: an existing
    // destination makes the move a no-op, which would strand the
    // device's local state under the old key).
    MigrateLegacyUrNetworkSpace(*spaceManager_);
  }
  networkSpace_.reset();
  if (!networkSpaceJson.empty()) {
    try {
      networkSpace_ = spaceManager_->importNetworkSpaceFromJson(networkSpaceJson);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[tunnel] import network space failed (using default): %s\n",
                   e.what());
    }
  }
  if (!networkSpace_) networkSpace_ = BuildUrNetworkSpace(*spaceManager_);
}

// enable_rpc is always false: a device gets a listener only from setRpcServer
// with pinned material (RunStart step 4), and the provider-only device never
// gets one at all.
urnet::DeviceLocal TunnelHost::NewDeviceLocked(const std::string& byJwt,
                                               const std::string& instanceId,
                                               const std::string& appVersionIn) {
  const std::string appVersion = appVersionIn.empty() ? kUrAppVersionFallback : appVersionIn;
  // The device's connect auth and its requests carry the client info of the
  // space's Api (the sdk reads it as the DeviceLocal is built and on every
  // token change), and the server records it with each use of the session:
  // without it the account's Sessions list showed this machine's tunnel as
  // "Unknown device". The version is the one the device reports beside it.
  ReportClientInfo(networkSpace_->getApi(), appVersion);
  const bool hadStoredMaterial = HasStoredKeyMaterial();
  bool restoreFailed = false;
  // Both constructions size the device at the measured host's memory tier
  // (TunnelPolicy.hpp) instead of the SDK's 20 MiB default, which is what
  // lets the H3 carrier windows reach their full size. The same cached
  // measurement chose the process budget at startup, so the target and the
  // budget backing it are always one tier.
  const urnw::MemoryTier memoryTier =
      urnw::MemoryTierForHost(urnw::HostMemoryByteCountCached());
  std::optional<urnet::DeviceLocal> device;
  if (auto km = LoadKeyMaterial()) {
    try {
      device = urnet::newDeviceLocalWithMemoryTarget(
          *networkSpace_, byJwt, UrDeviceDescription(), UrDeviceSpec(), appVersion, instanceId,
          /*enable_rpc=*/false, *km, memoryTier.device_target_byte_count);
    } catch (const std::exception& e) {
      restoreFailed = true;
      std::fprintf(stderr, "[tunnel] restore device key material failed: %s\n", e.what());
    }
  }
  if (!device) {
    // An empty key material (handle 0) is nil in the SDK: new identity.
    device = urnet::newDeviceLocalWithMemoryTarget(
        *networkSpace_, byJwt, UrDeviceDescription(), UrDeviceSpec(), appVersion, instanceId,
        /*enable_rpc=*/false, urnet::DeviceLocalKeyMaterial{},
        memoryTier.device_target_byte_count);
    // Persist only when nothing was stored. Overwriting after a failed
    // restore silently rotates this device's provider identity — peers
    // stop recognising it and its reputation is gone — for what may be a
    // transient failure. The stored identity is left intact so a later,
    // healthy start can still use it; this session runs on an ephemeral
    // one and says so at error level.
    if (!hadStoredMaterial) {
      try {
        if (auto km = device->getKeyMaterial(); km && !km.isEmpty()) {
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
  return std::move(*device);
}

void TunnelHost::RunStart(ctl::StartTunnelRequest config) {
  // The up edge's support lines, written after opMutex_ is released: they fork
  // resolvectl, which must not hold up a Disconnect or the reaper.
  std::optional<diag::TunnelUpFacts> upFacts;
  std::string upInterface;
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
      //     A failure here is not fatal on its own when the nftables chain
      //     below is available as a genuine belt for sockets that already
      //     exist. If this kernel lacks both mechanisms the preflight below
      //     refuses before it creates the DeviceLocal; the packet witness is
      //     still the final authority on the mechanism that was selected.
      socketMarkerProven_ = false;
      cgroupSocketMatchSupported_ = false;
      {
        std::string markerError;
        if (egressMarker_.Attach(cgroup_, kEgressMark, &markerError)) {
          socketMarkerProven_ = true;
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

      // 1b. The belt: the nftables cgroup mark chain when this kernel accepts
      //     it, plus the ruleset that always carries the packet witness's
      //     counters. The compatibility decision is measured with --check and
      //     is passed into every later ruleset generation.
      const bool nftAvailable = !FindTool("nft").empty();
      std::string cgroupProbeError;
      if (nftAvailable && cgroup_.valid) {
        cgroupSocketMatchSupported_ =
            NetFilter::CheckCgroupSocketMatch(cgroup_, &cgroupProbeError);
      }
      if (!cgroupSocketMatchSupported_ && socketMarkerProven_) {
        DaemonLogf(
            "[tunnel] nftables socket cgroupv2 matching is unavailable (%s). Using the "
            "proven cgroup-BPF socket mark without the nft cgroup belt for this floorless "
            "session. Kill-switch floors and helper-DNS reconnects remain disabled rather "
            "than weakened.\n",
            cgroupProbeError.empty() ? "the kernel rejected the expression"
                                     : cgroupProbeError.c_str());
      }
      const bool egressPossible =
          nftAvailable && cgroup_.valid &&
          (cgroupSocketMatchSupported_ || socketMarkerProven_);
      if (!egressPossible && !allowUnprotectedEgress_) {
        const std::string why =
            !cgroup_.valid
                ? "this system is not running the cgroup v2 unified hierarchy, so the "
                  "daemon's own sockets cannot be marked"
                : !nftAvailable
                      ? "nftables (nft) is not installed"
                      : "the kernel rejected nftables socket cgroupv2 matching and the "
                        "cgroup-BPF socket marker could not be proven";
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
      // a custom-server session would sync against production.
      LoadNetworkSpaceLocked(config.network_space_json);
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
      device_ = NewDeviceLocked(config.by_jwt, config.instance_id, config.app_version);
      // The network this device's first dials go out on, so the reaper's first
      // tick after the bring-up kicks it only for a path that has moved since
      // — a switch during the bring-up included, which a baseline taken on that
      // first tick would have swallowed.
      networkQualityTracker_.Rebase(ReadNetworkPath(PlannedTunName()));

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
      cfg.local_addr_v4 = device_->tunnelLocalAddress();
      if (!IsIpv4Address(cfg.local_addr_v4)) {
        if (!cfg.local_addr_v4.empty()) {
          std::fprintf(stderr, "[tunnel] the device reported an unusable tunnel address '%s'\n",
                       cfg.local_addr_v4.c_str());
        }
        cfg.local_addr_v4 = "169.254.2.1";
      }
      // dns from the device: the dns settings' unencrypted local servers when
      // set, otherwise the distinct plain-DNS UpgradeMux mask. Always plain
      // :53, never OS-level encrypted DNS: the mux performs the
      // unencrypted-DNS -> DoH upgrade in-tunnel, for both families.
      //
      // SANITISED HERE, not inside Tunnel::Open, because Open refuses the WHOLE
      // configuration when any resolver is of the wrong family (the
      // IsDualStackTunnelConfig floor). Open used to drop a bad resolver silently
      // and carry on, so leaving the device's list unfiltered would have turned
      // "the device named one resolver we cannot use" into "the tunnel will not
      // start" — a regression dressed as a fix. Dropping happens where the value
      // enters the daemon, the guard stays the fail-closed floor behind it, and
      // the diagnostic still names the resolver that was thrown away.
      if (auto dns = device_->tunnelDnsAddressesIpv4(); dns) {
        for (const auto& server : *dns) {
          if (IsIpv4Address(server)) {
            cfg.dns_servers_v4.push_back(server);
          } else {
            std::fprintf(stderr, "[tunnel] ignoring an unusable tunnel resolver '%s'\n",
                         server.c_str());
          }
        }
      }
      if (cfg.dns_servers_v4.empty()) {
        // Keep the exceptional fallback coupled to the SDK's separately tested
        // URnetwork-owned UpgradeMux identity -- but never hand Open something
        // it must refuse: a fallback that is not IPv4 leaves the tunnel with no
        // resolver (which the guard permits, and which the nft DNS floor and
        // status.dns_detail both already describe) rather than no tunnel.
        std::string fallback = urnet::getDefaultTunnelDnsAddressIpv4();
        if (IsIpv4Address(fallback)) {
          cfg.dns_servers_v4 = {std::move(fallback)};
        } else {
          std::fprintf(stderr,
                       "[tunnel] the sdk default tunnel resolver '%s' is not an IPv4 literal; "
                       "the tunnel will come up with no resolver of its own\n",
                       fallback.c_str());
        }
      }
      // --- the v6 half, sanitised the same way (connect/IPV6.md C2). The
      //     device draws a ULA from the SDK's fixed /48; the fallback is the
      //     TunnelConfig default (a fixed ULA in the same /48), never nothing:
      //     the tunnel is dual-stack or it does not start.
      const std::string deviceAddr6 = device_->tunnelLocalAddressIpv6();
      if (IsIpv6Address(deviceAddr6) && IsIpv6UniqueLocal(deviceAddr6)) {
        cfg.local_addr_v6 = deviceAddr6;
      } else if (!deviceAddr6.empty()) {
        std::fprintf(stderr, "[tunnel] the device reported an unusable tunnel IPv6 address '%s'\n",
                     deviceAddr6.c_str());
      }
      cfg.prefix_v6 = static_cast<int>(urnet::getTunnelLocalPrefixLengthIpv6());
      if (cfg.prefix_v6 < 1 || cfg.prefix_v6 > 128) cfg.prefix_v6 = 64;
      if (auto dns6 = device_->tunnelDnsAddressesIpv6(); dns6) {
        for (const auto& server : *dns6) {
          if (IsIpv6Address(server)) {
            cfg.dns_servers_v6.push_back(server);
          } else {
            std::fprintf(stderr, "[tunnel] ignoring an unusable tunnel IPv6 resolver '%s'\n",
                         server.c_str());
          }
        }
      }
      if (cfg.dns_servers_v6.empty()) {
        // The SDK's in-tunnel resolver identity for v6, like the v4 fallback
        // above. A fallback that is not a v6 literal leaves the v6 half with
        // no resolver of its own -- the v4 resolver still answers every name,
        // AAAA records included, so this is a note rather than a refusal.
        std::string fallback6 = urnet::getDefaultTunnelDnsAddressIpv6();
        if (IsIpv6Address(fallback6)) {
          cfg.dns_servers_v6 = {std::move(fallback6)};
        } else {
          std::fprintf(stderr,
                       "[tunnel] the sdk default tunnel IPv6 resolver '%s' is not an IPv6 "
                       "literal; the v6 half comes up with no resolver of its own\n",
                       fallback6.c_str());
        }
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
        status_.ipv6_captured = report.ipv6_captured;
        status_.egress_protected = report.egress_protected;
        status_.dns_applied = report.dns_applied;
        status_.dns_detail = report.dns_detail;
        status_.tunnel_interface = report.interface;
      }
      // PER-SESSION LATCH for the reaper's DNS check. It cannot gate on the
      // live dns_applied, because the very failure it exists to catch clears
      // that flag — gating on it would make the check switch itself off at the
      // moment it became necessary. This records what the session was BUILT
      // with: false means the user opted out via URNETWORK_ALLOW_UNPROTECTED_DNS
      // and must not be nagged, true means DNS protection is owed for the whole
      // session.
      dnsProtectionExpected_ = tunnel_->report().dns_applied;
      dnsVerifyTicks_ = 0;
      dnsVerifyFailures_ = 0;
      // No repair yet this session: the first one is never "contested".
      dnsTicksSinceRepair_ = settings_.dns_contested_window_ticks;
      dnsContested_ = false;
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
      const int ioLoopFd = DuplicateIoLoopFd(tunnel_->fd());
      if (ioLoopFd < 0) {
        const int errorNumber = errno;
        const std::string message =
            "could not duplicate the tun descriptor for the SDK: " +
            std::string(std::strerror(errorNumber));
        PublishError(message, ctl::kCodeTunOpenFailed);
        throw std::runtime_error(message);
      }
      // newIoLoop has no error return: ownership transfers during this call.
      // Passing Tunnel::fd() itself would leave both runtimes closing one fd
      // number and let a late IoLoop close hit an unrelated reused socket.
      ioLoop_ = urnet::newIoLoop(*device_, ioLoopFd,
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
        PublishError("connected, but the off-tunnel IPv6 and DNS leak floor could not be "
                     "installed: " +
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
        if (!tunnel_->VerifyEgressWitness("connected", /*requireOffTunnelRoute=*/false,
                                          &witnessError)) {
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
        // THE UP EDGE IS THE ONLY PLACE THIS IS SET. Latched from the accepted
        // request, in the same locked block that flips the state to Up, so the
        // published identity and the published state can never disagree: a
        // client that sees Up sees a session it can name, and attach_tunnel
        // (which requires Up) always has both halves to compare.
        instanceId_ = config.instance_id;
        rpcSessionId_ = config.rpc_session_id;
        try {
          status_.client_id = device_->getClientId();
        } catch (const std::exception&) {
          // status must never throw across the wire
        }
      }
      // Watched from the up edge on, and by nothing before it: a bring-up has
      // its own witnesses.
      StartDeadTunnelWatchLocked();
      std::fprintf(stderr,
                   "[tunnel] up (client=%s rpc=127.0.0.1:%d routes=%d ipv6=%d egress=%d dns=%d)\n",
                   Status().client_id.c_str(), rpcPort,
                   tunnel_->report().routes_installed ? 1 : 0,
                   tunnel_->report().ipv6_captured ? 1 : 0,
                   tunnel_->report().egress_protected ? 1 : 0,
                   tunnel_->report().dns_applied ? 1 : 0);
      upFacts.emplace();
      upFacts->dns_backend = support::DnsBackendName(tunnel_->dnsBackend());
      upFacts->dns_applied = tunnel_->report().dns_applied;
      upFacts->ipv6_captured = tunnel_->report().ipv6_captured;
      upFacts->egress_protected = tunnel_->report().egress_protected;
      upFacts->socket_marker = socketMarkerProven_;
      upInterface = tunnel_->name();
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
      // The machine now, the device after the landing below, as
      // StopUnsafeSessionLocked orders them: a device that stopped answering
      // must not hold the landing up.
      RevertSessionMachineLocked();

      // AND THE FIREWALL, which this path used to walk straight past.
      // StopInternalLocked(<empty reason>) deliberately does not touch the
      // filter (it is also the "make room for this start" path), so the
      // Connecting ruleset installed at step 1 — which blocks all OFF-TUNNEL
      // global IPv6 machine-wide — survived every failed start: no tunnel, no UI signal,
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
      StopInternalLocked(std::string());
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
      // The code, never the prose: the message can name paths and addresses.
      support::LogTunnelEnded("start-failed", "start_failed", keptCode);
      support::LogConnectivity("start-failed");
    }
  }
  busy_.store(false);
  if (upFacts) support::LogTunnelUp(*upFacts, upInterface);
  // The SDK's log of the bring-up, up or failed, on disk now rather than at
  // glog's next 30 s flush, from the flusher's thread.
  glogflush::Request();
}

void TunnelHost::Stop(const std::string& reason) {
  stopRequested_.store(true);
  JoinWorker();
  {
    std::scoped_lock lock(opMutex_);
    StopInternalLocked(reason);
  }
  // The teardown's lines on disk now, from the flusher's thread: this is the
  // main loop, and the flush fsyncs.
  glogflush::Request();
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

void TunnelHost::RevertSessionMachineLocked() {
  // From here on a poll reads the session as going, never as up.
  const bool hadSession = device_.has_value() || tunnel_ || ioLoop_.has_value();
  if (hadSession) {
    std::scoped_lock lock(statusMutex_);
    if (status_.tunnel_state == ctl::TunnelState::Up ||
        status_.tunnel_state == ctl::TunnelState::Starting) {
      status_.tunnel_state = ctl::TunnelState::Stopping;
    }
  }
  // The verdict goes with its session.
  StopDeadTunnelWatchLocked();
  // The generation bump makes any IoLoop done callback still in flight a
  // no-op, so our own teardown can never be mistaken for a dead tunnel. Ahead
  // of the IoLoop's close, which is what makes that callback fire.
  sessionGeneration_.fetch_add(1);
  ioLoopDied_.store(false);

  // THE DNS UNDO GOES FIRST, WHILE urnet0 STILL EXISTS.
  //
  // MEASURED, on every teardown in the journal without exception:
  //     [dns] resolvectl revert urnet0: exit 1: Failed to resolve interface
  //           "urnet0": No such device
  // The revert lived in ~Tunnel, and ~Tunnel runs at `tunnel_.reset()` below —
  // after `ioLoop_->close()`. Before the descriptor ownership fix, Go and C++
  // both owned the same fd number and the asynchronous Go close could destroy
  // the link first, so the revert addressed a device that no longer existed. Every
  // teardown left resolved's per-link override to be garbage-collected by the
  // link's disappearance instead of removed on purpose — benign on
  // systemd-resolved, NOT benign on the tier-2/3 hosts where the undo is a file
  // on disk, and a failure nobody could distinguish from a real one.
  //
  // Reordering inside Tunnel.cpp could not have fixed this: the revert was
  // already the first thing ~Tunnel did. The ordering that was wrong is this
  // one. Tunnel::RevertDns() is idempotent, so ~Tunnel's own call below stays
  // exactly as it was for the paths that never reach this function.
  if (tunnel_) tunnel_->RevertDns();

  // Reverse dependency order. The IoLoop's close only cancels it (the SDK's
  // IoLoop.Close), so it cannot wait on the device either.
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
}

void TunnelHost::StopInternalLocked(const std::string& reason) {
  // First, and in every teardown: the provider-only device never shares a
  // moment with a tunnel session's device. RunStart opens with this function,
  // so a Connect retires the provider before the egress marker, the capture
  // routes or the session's own DeviceLocal (the same identity) exist. A
  // no-op when there is none, which is every teardown of a tunnel session.
  RetireProviderDeviceLocked();
  // The standalone device a log upload ran on, for the same reason. Its upload
  // goes on: the POST runs on the network space's API, not on the device.
  RetireUploadDeviceLocked();

  // The machine first: its DNS, routes and policy rules (a no-op when the
  // caller has already reverted it to land the floor itself)...
  RevertSessionMachineLocked();
  if (!reason.empty()) {
    // An explicit stop ALWAYS lifts the policy (windows semantics: only an
    // unexpected drop keeps or installs Armed) — FloorForTransition answers
    // `false` for every transition into Off, so this needs no argument and
    // cannot be given the wrong one. A failure here is remembered in
    // filterRemovalPending_ and retried by the reaper.
    //
    // Before the device's calls below, which take its state lock: a device
    // that never answers again must not keep this machine filtered.
    std::string ignored;
    ApplyFilterLocked(FilterState::Off, &ignored);
  }

  // ...and the SDK last. Every call on the device takes its state lock, and a
  // wedged one (the dead-tunnel failsafe's SdkUnresponsive) stops the teardown
  // here, with the machine already given back. close() itself only cancels:
  // the SDK hands its joins to its own lifecycle workers.
  if (device_) {
    device_->setTunnelStarted(false);
    device_->close();
    ReleaseDeviceLocked(device_);
  }
  networkQualityTracker_.Reset();
  // AFTER the device is gone, so no SDK socket is ever created unmarked while
  // the capture routes could still be up. The mark is inert once the `ip rule`
  // is removed (nothing consults it), so the ordering costs nothing and the
  // detach keeps the blast radius to the session that asked for it.
  egressMarker_.Detach();
  socketMarkerProven_ = false;
  egressWitnessTicks_ = 0;
  egressWitnessFailures_ = 0;
  // spaceManager_/networkSpace_ persist across sessions (Windows parity).
  {
    std::scoped_lock lock(statusMutex_);
    status_.tunnel_state = ctl::TunnelState::Stopped;
    status_.rpc_port = 0;
    status_.rpc_pinned = false;  // the listener died with the DeviceLocal
    status_.client_id.clear();
    // The session is over, so its name names nothing. Leaving either behind
    // would let an attach_tunnel match a tunnel that is no longer running (the
    // Up gate would catch it today, but the identity is the thing being
    // compared and it must not outlive what it identifies).
    instanceId_.clear();
    rpcSessionId_.clear();
    activeConfig_ = ctl::StartTunnelRequest();
    status_.failsafe_armed = false;
    status_.routes_installed = false;
    status_.ipv6_captured = false;
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
                                         const std::string& code, const char* why,
                                         bool sdkWedged) {
  // 1) The machine, firewall untouched: DNS, routes and policy rules, and no
  //    call that waits on the device, so the landing below cannot be held up
  //    by an SDK that stopped answering.
  RevertSessionMachineLocked();

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
          "[tunnel] the session was stopped %s (%s) and this machine stays blocked "
          "because the kill switch is armed. Turn it off in the app to lift it, or run: %s\n",
          why, reason.c_str(), NetFilter::RecoveryCommand());
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
          "[tunnel] ERROR: the session was stopped %s (%s) and the kill switch could "
          "NOT be armed: %s\n",
          why, reason.c_str(), filterError.c_str());
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
      DaemonLogf("[tunnel] the session was stopped %s (%s); the kill switch is off, so "
                 "this machine is not blocked\n",
                 why, reason.c_str());
    } else {
      detail += " The leftover firewall rules could NOT be removed (" + filterError +
                "), so this machine may still be blocked even though the kill switch is off. "
                "Lift them with: " +
                NetFilter::RecoveryCommand() + ".";
      DaemonLogf(
          "[tunnel] ERROR: the session was stopped %s (%s), the kill switch is off, and "
          "the leftover rules could NOT be removed: %s -- the machine may be blocked. "
          "Recover with: %s\n",
          why, reason.c_str(), filterError.c_str(), NetFilter::RecoveryCommand());
    }
  }

  // 3) The device, last of the teardown. The empty reason is load-bearing
  //    twice over: it keeps ApplyFilterLocked(Off) — the floor lift — out of
  //    this path, and it keeps the tail of StopInternalLocked from clearing the
  //    error we are about to publish.
  //
  //    Unless the SDK has stopped answering: then every call left on the
  //    device waits for good, so the process ends itself here instead, for
  //    systemd to start a clean daemon that publishes what step 4 would have.
  if (sdkWedged) RestartForWedgedSdkLocked(reason, detail, code);
  StopInternalLocked(std::string());

  // 4) PUBLISH LAST, AND ON PURPOSE.
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
  // Touches no status_ field, so the rule above holds.
  support::LogTunnelEnded("stopped", reason, code);
}

// THE WAY OUT OF A WEDGED SDK (SelfRestart.hpp). The machine is already given
// back: the routes, the DNS and the firewall's landing are done and published.
// What is left would all wait on the device's state lock, so none of it is
// done here: the process ends, and the next daemon says why.
void TunnelHost::RestartForWedgedSdkLocked(const std::string& reason, const std::string& error,
                                           const std::string& code) {
  selfrestart::StopRecord record;
  record.stopReason = reason;
  record.error = error;
  record.code = code;
  record.writtenMillis = WatchdogMillis();
  std::string writeError;
  if (!WriteStopRecord(record, &writeError)) {
    DaemonLogf("[tunnel] the stop record could not be written to %s (%s); the next daemon "
               "will not say why this tunnel stopped\n",
               selfrestart::kStopRecordPath, writeError.c_str());
  }
  // The program stays attached to this service's cgroup after the process
  // ends; nothing needs it now that the capture routes are gone.
  egressMarker_.Detach();
  DaemonLogf("[tunnel] the sdk stopped answering, so the session's device cannot be closed "
             "without waiting on it, and neither can anything else in this daemon. The machine "
             "is given back already. Ending this daemon now (exit status %d): under systemd, "
             "Restart=on-failure starts a clean one in 2 s, which reports this stop.\n",
             selfrestart::kExitStatus);
  // _exit, never exit or a return: unwinding runs destructors that call into
  // the SDK, where a thread is stuck.
  ::_exit(selfrestart::kExitStatus);
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
  if (providerDevice_) {
    // The provider-only device exists for exactly the modes that provide while
    // disconnected; any other mode ends it, and that is how Never stops it.
    const provide::ControlMode controlMode = provide::ControlModeFrom(mode);
    if (!provide::ProviderRuns(controlMode, /*connected=*/false)) {
      DaemonLogf("[provide] provide mode %s does not provide while disconnected; stopping the "
                 "provider device\n",
                 provide::ToString(controlMode));
      RetireProviderDeviceLocked();
      return true;
    }
    try {
      providerDevice_->setProvideControlMode(mode);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[provide] set provide mode failed: %s\n", e.what());
      return false;
    }
    providerConfig_.provide_mode = mode;
    RefreshProviderStatusLocked();
  }
  return true;
}

// ---- the provider-only device ----------------------------------------------

namespace {

// How long a retired provider device may take to close its sockets before it
// is released anyway. close() returns first; the wait keeps a late packet from
// the provider's sockets (unmarked, because no egress marker runs without a
// tunnel) out of the capture routes a Connect installs moments later.
constexpr int64_t kProviderCloseWaitMillis = 2000;

}  // namespace

TunnelHost::ProviderStartResult TunnelHost::StartProvider(
    const ctl::StartProviderRequest& request) {
  ProviderStartResult result;
  // Re-validated here as start_tunnel is: this process is root, and ControlServer
  // is not the only possible caller.
  if (const auto invalid = ctl::ValidateStartProviderRequest(request)) {
    result.error = invalid->message;
    result.code = invalid->code;
    return result;
  }
  if (busy_.load()) {
    result.error = "a tunnel start is in progress";
    result.code = ctl::kCodeStartInProgress;
    return result;
  }
  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    result.error = "a tunnel start is in progress";
    result.code = ctl::kCodeStartInProgress;
    return result;
  }
  if (device_.has_value() || tunnel_ != nullptr || ioLoop_.has_value()) {
    result.error = "a tunnel session is running, and its device is already the provider";
    result.code = ctl::kCodeTunnelSessionActive;
    return result;
  }
  if (filter_.state() == FilterState::Armed) {
    // The floor an unexpected drop armed is the only thing in force until the
    // user reconnects or lifts it. A provider would leave through the daemon's
    // own permit, which exists so a reconnect can; it is not started here.
    result.error =
        "the kill switch is holding this machine blocked; providing resumes after you "
        "reconnect or lift it";
    result.code = ctl::kCodeKillSwitchArmed;
    return result;
  }

  // The same request again (a relaunched GUI adopting the provider, or a
  // retried frame): keep the device, apply the mode. A changed network space
  // (a saved DoH server list, say) is a different device.
  if (providerDevice_ && ctl::SameProviderDevice(providerConfig_, request)) {
    try {
      providerDevice_->setProvideControlMode(request.provide_mode);
    } catch (const std::exception& e) {
      result.error = std::string("the provide mode could not be applied: ") + e.what();
      result.code = ctl::kCodeProviderStartFailed;
      return result;
    }
    providerConfig_.provide_mode = request.provide_mode;
    {
      std::scoped_lock statusLock(statusMutex_);
      pendingProvideMode_ = request.provide_mode;
    }
    RefreshProviderStatusLocked();
    result.ok = true;
    return result;
  }

  RetireProviderDeviceLocked();
  // A standalone log upload device runs under the same identity; its upload
  // goes on without it (StopInternalLocked says why).
  RetireUploadDeviceLocked();
  try {
    // RunStart's steps 2 and 3 and nothing after them: no egress marker, no
    // nftables transaction, no setRpcServer, no tun, no IoLoop, no DNS. The
    // device's sockets follow the main routing table like any process's.
    LoadNetworkSpaceLocked(request.network_space_json);
    providerDevice_ = NewDeviceLocked(request.by_jwt, request.instance_id, request.app_version);
    // Its own baseline, not whatever the tracker last saw: a provider retired
    // and started again later must not inherit the network of the one before
    // it and be kicked within a second of its first dial.
    networkQualityTracker_.Rebase(ReadNetworkPath(std::string()));
    if (!request.provider_transport_settings_json.empty()) {
      providerDevice_->setProviderTransportSettings(
          nlohmann::json::parse(request.provider_transport_settings_json)
              .get<urnet::TransportSettings>());
    }
    providerDevice_->setProvideControlMode(request.provide_mode);
    providerConfig_ = request;
    {
      // A later tunnel start applies the same mode (RunStart step 4).
      std::scoped_lock statusLock(statusMutex_);
      pendingProvideMode_ = request.provide_mode;
    }
    OpenProviderViewControllersLocked();
    RefreshProviderStatusLocked();
    DaemonLogf("[provide] providing without a tunnel (mode %s, tier %lld); this machine's own "
               "traffic is not routed through URnetwork\n",
               provide::ToString(provide::ControlModeFrom(request.provide_mode)),
               static_cast<long long>(Status().provider_mode));
    result.ok = true;
  } catch (const std::exception& e) {
    RetireProviderDeviceLocked();
    result.error = std::string("the provider could not be started: ") + e.what();
    result.code = ctl::kCodeProviderStartFailed;
    DaemonLogf("[provide] %s\n", result.error.c_str());
  }
  return result;
}

bool TunnelHost::ProviderRunning() const {
  std::scoped_lock lock(statusMutex_);
  return status_.provider_running;
}

ctl::ProviderStatsReply TunnelHost::ProviderStats(bool pollStatus) {
  ctl::ProviderStatsReply reply;
  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  // A bring-up owns the session, and it retired the provider-only device first.
  if (!lock.owns_lock() || !providerDevice_) return reply;
  reply.running = true;
  // Every read is local to this process (the controllers sample the device and
  // keep the last answer), and each one is guarded on its own: an SDK error
  // costs its own field, never the reply. Asked about once a second, so a
  // failing read is logged the first time and then once a minute.
  static int readFailures = 0;
  auto noteReadFailure = [](const char* what, const std::exception& e) {
    if ((readFailures++ % 60) == 0) {
      DaemonLogf("[provide] reading the provider %s failed: %s\n", what, e.what());
    }
  };
  try {
    reply.has_provider_stats = providerDevice_->getProviderPacketStats().has_value();
  } catch (const std::exception& e) {
    noteReadFailure("packet stats", e);
  }
  // The device's own extender role, which runs on this device as on a tunnel
  // session's while it provides and the setting is on: what the GUI reads off
  // its DeviceRemote with a tunnel (getExtenderProvideStatus).
  try {
    if (auto status = providerDevice_->getExtenderProvideStatus()) {
      reply.extender_provide_status_json = ctl::DumpForWire(nlohmann::json(*status));
    }
  } catch (const std::exception& e) {
    noteReadFailure("extender status", e);
  }
  // The setting beside it, the connect page's Extender switch, and that this
  // daemon takes the switch's write (SetProvideExtender): never said without a
  // setting to show.
  try {
    reply.provide_extender = providerDevice_->getProvideExtender();
    reply.provide_extender_writable = true;
  } catch (const std::exception& e) {
    noteReadFailure("extender setting", e);
  }
  // And the lag.dev measurements setting beside it, the speed tests switch
  // under the Extender switch, which this daemon writes too
  // (SetMeasurementsEnabled): never said without a setting to show.
  try {
    reply.measurements_enabled = providerDevice_->getMeasurementsEnabled();
    reply.measurements_enabled_writable = true;
  } catch (const std::exception& e) {
    noteReadFailure("measurements setting", e);
  }
  if (providerContractVc_) {
    try {
      if (auto points = providerContractVc_->getProviderThroughputPoints()) {
        reply.provider_throughput_points_json = ctl::DumpForWire(nlohmann::json(*points));
      }
      if (auto distribution = providerContractVc_->getProviderTransportDistribution()) {
        reply.provider_transport_distribution_json =
            ctl::DumpForWire(nlohmann::json(*distribution));
      }
    } catch (const std::exception& e) {
      reply.provider_throughput_points_json.clear();
      reply.provider_transport_distribution_json.clear();
      noteReadFailure("series", e);
    }
    try {
      if (auto points = providerContractVc_->getExtenderThroughputPoints()) {
        reply.extender_throughput_points_json = ctl::DumpForWire(nlohmann::json(*points));
      }
    } catch (const std::exception& e) {
      noteReadFailure("extender series", e);
    }
  }
  if (providerStatusVc_) {
    reply.status_open = true;
    try {
      // Started by the first ask (one poll at once, then about once a minute)
      // and kept polling only while a GUI that shows it keeps asking.
      if (pollStatus && providerStatusLease_.Renew(MonotonicMillis())) providerStatusVc_->start();
      reply.status_loaded = providerStatusVc_->getIsLoaded();
      reply.status_last_fetch_error = providerStatusVc_->getLastFetchError();
      if (auto status = providerStatusVc_->getProviderStatus()) {
        reply.provider_status_json = ctl::DumpForWire(nlohmann::json(*status));
      }
    } catch (const std::exception& e) {
      // a malformed document reads as a failed poll, as the GUI's own read
      // of its controller does
      reply.status_loaded = false;
      reply.status_last_fetch_error = e.what();
      reply.provider_status_json.clear();
    }
  }
  return reply;
}

TunnelHost::ExtenderResetResult TunnelHost::ResetExtenders(
    const ctl::ResetExtendersRequest& request) {
  ExtenderResetResult result;
  std::optional<urnet::NetworkSpace> space;
  {
    std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      // A bring-up owns the session and imports the space it was started
      // with; the next import of this space applies the reset.
      result.error = "a tunnel operation is in progress";
      result.code = ctl::kCodeStartInProgress;
      return result;
    }
    // The manager loads every space this daemon stores; without one no device
    // has run yet and nothing is held.
    if (spaceManager_) {
      urnet::NetworkSpaceKey key;
      key.host_name = request.host_name;
      key.env_name = request.env_name;
      if (urnet::NetworkSpace held = spaceManager_->getNetworkSpace(key)) {
        space = std::move(held);
      }
    }
  }
  result.ok = true;
  if (!space) {
    DaemonLogf("[extender] reset %s of %s/%s: no space is held for the key; its next import "
               "applies it\n",
               request.extender_reset_id.c_str(), request.host_name.c_str(),
               request.env_name.c_str());
    return result;
  }
  // Outside opMutex_: the sdk joins the space's old extender client before the
  // new one starts. The devices running in the space keep running.
  result.reset = space->applyExtenderReset(request.extender_reset_id);
  DaemonLogf("[extender] reset %s of %s/%s: %s\n", request.extender_reset_id.c_str(),
             request.host_name.c_str(), request.env_name.c_str(),
             result.reset ? "applied" : "nothing new to apply");
  return result;
}

bool TunnelHost::SetProvideExtender(bool on, std::string* error) {
  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    // A bring-up owns the session. The GUI reads the setting again, so the
    // switch shows it as it stands.
    if (error) *error = "a tunnel start is in progress";
    return false;
  }
  const provide::ExtenderSettingTarget target = provide::ExtenderSettingTargetFor(
      providerDevice_.has_value(), device_.has_value(), networkSpace_.has_value());
  try {
    switch (target) {
      case provide::ExtenderSettingTarget::ProviderDevice:
        // persisted in its space and applied at once: the role starts or stops
        providerDevice_->setProvideExtender(on);
        break;
      case provide::ExtenderSettingTarget::SessionDevice:
        // the GUI's DeviceRemote hears it through its status listener
        device_->setProvideExtender(on);
        break;
      case provide::ExtenderSettingTarget::NetworkSpace:
        // The next import of this key reuses this space, or replaces it with
        // one that reads the file this writes.
        networkSpace_->getAsyncLocalState().getLocalState().setProvideExtender(on);
        break;
      case provide::ExtenderSettingTarget::None:
        if (error) {
          *error = "no device has run in this daemon yet, so there is no network space to keep "
                   "the setting in";
        }
        return false;
    }
  } catch (const std::exception& e) {
    if (error) {
      *error = std::string("the provide extender setting could not be written to ") +
               provide::ToString(target);
    }
    DaemonLogf("[provide] writing the provide extender setting to %s failed: %s\n",
               provide::ToString(target), e.what());
    return false;
  }
  DaemonLogf("[provide] provide extender %s (%s)\n", on ? "on" : "off",
             provide::ToString(target));
  return true;
}

bool TunnelHost::SetMeasurementsEnabled(bool on, std::string* error) {
  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    // A bring-up owns the session. The GUI reads the setting again, so the
    // switch shows it as it stands.
    if (error) *error = "a tunnel start is in progress";
    return false;
  }
  // the provider extender setting's rule: the same space holds both, and the
  // same devices read them
  const provide::ExtenderSettingTarget target = provide::ExtenderSettingTargetFor(
      providerDevice_.has_value(), device_.has_value(), networkSpace_.has_value());
  try {
    switch (target) {
      case provide::ExtenderSettingTarget::ProviderDevice:
        // persisted in its space and applied at once: the measurement API
        // starts or stops, and the operator withdraws or restores the name
        providerDevice_->setMeasurementsEnabled(on);
        break;
      case provide::ExtenderSettingTarget::SessionDevice:
        device_->setMeasurementsEnabled(on);
        break;
      case provide::ExtenderSettingTarget::NetworkSpace:
        networkSpace_->getAsyncLocalState().getLocalState().setMeasurementsEnabled(on);
        break;
      case provide::ExtenderSettingTarget::None:
        if (error) {
          *error = "no device has run in this daemon yet, so there is no network space to keep "
                   "the setting in";
        }
        return false;
    }
  } catch (const std::exception& e) {
    if (error) {
      *error = std::string("the measurements setting could not be written to ") +
               provide::ToString(target);
    }
    DaemonLogf("[provide] writing the measurements setting to %s failed: %s\n",
               provide::ToString(target), e.what());
    return false;
  }
  DaemonLogf("[provide] lag.dev measurements %s (%s)\n", on ? "on" : "off",
             provide::ToString(target));
  return true;
}

bool TunnelHost::Logout(const std::string& networkSpaceJson, std::string* error,
                        const char** code) {
  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    // A bring-up owns the session. The GUI's stop_tunnel, sent first, joins
    // one, so this is a start that came after it; the sign-out stays owed.
    if (error) *error = "a tunnel start is in progress";
    if (code) *code = ctl::kCodeStartInProgress;
    return false;
  }
  // Nothing runs on the identity deleted below: the session as an explicit
  // stop ends it (which also lifts the kill-switch floor and forgets the
  // session's request, credentials included), and every teardown retires the
  // provider-only device and a log upload's standalone device.
  StopInternalLocked("user");
  // A queued log upload carries the signed-out account's credentials, and
  // building its device would make a new identity for them.
  if (queuedUpload_) {
    logUploadFlight_->Finish(queuedUploadId_, logupload::FlightState::Failed);
    queuedUpload_.reset();
    queuedGuiLogFiles_.CloseAll();
    queuedUploadId_ = 0;
  }
  // What the account asked for: the next start brings its own.
  {
    std::scoped_lock statusLock(statusMutex_);
    pendingProvideMode_.clear();
  }
  killSwitchRequested_.store(false);

  bool cleared = true;
  for (const char* file : {kClientKeySeedFile, kProvideCertFile, kProvideKeyFile}) {
    const std::string path = storageRoot_ + "/" + file;
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
      DaemonLogf("[tunnel] logout: %s could not be deleted: %s\n", file, std::strerror(errno));
      cleared = false;
    }
  }
  try {
    LoadNetworkSpaceLocked(networkSpaceJson);
    networkSpace_->getAsyncLocalState().getLocalState().logout();
  } catch (const std::exception& e) {
    DaemonLogf("[tunnel] logout: the account's sdk state could not be cleared: %s\n", e.what());
    cleared = false;
  }
  if (!cleared) {
    if (error) *error = "the signed-out account's state could not all be cleared";
    return false;
  }
  DaemonLogf("[tunnel] logged out (cleared the device identity and the account's sdk state)\n");
  return true;
}

void TunnelHost::OpenProviderViewControllersLocked() {
  if (!providerDevice_) return;
  try {
    providerContractVc_ = providerDevice_->openContractViewController();
  } catch (const std::exception& e) {
    providerContractVc_.reset();
    DaemonLogf("[provide] the provider series could not be opened: %s\n", e.what());
  }
  try {
    providerStatusVc_ = providerDevice_->openProviderStatusViewController();
  } catch (const std::exception& e) {
    providerStatusVc_.reset();
    DaemonLogf("[provide] the provider status could not be opened: %s\n", e.what());
  }
}

void TunnelHost::CloseProviderViewControllersLocked() {
  // a new controller polls only once asked again
  providerStatusLease_.Release();
  if (providerDevice_ && providerStatusVc_) {
    try {
      providerDevice_->closeProviderStatusViewController(*providerStatusVc_);
    } catch (const std::exception& e) {
      DaemonLogf("[provide] closing the provider status failed: %s\n", e.what());
    }
  }
  if (providerDevice_ && providerContractVc_) {
    try {
      providerDevice_->closeContractViewController(*providerContractVc_);
    } catch (const std::exception& e) {
      DaemonLogf("[provide] closing the provider series failed: %s\n", e.what());
    }
  }
  providerStatusVc_.reset();
  providerContractVc_.reset();
}

void TunnelHost::RetireProviderDeviceLocked() {
  // The controllers read the device, so they close before it does.
  CloseProviderViewControllersLocked();
  if (providerDevice_) {
    try {
      providerDevice_->close();
      if (!providerDevice_->waitForClose(kProviderCloseWaitMillis)) {
        DaemonLogf("[provide] the provider device had not finished closing after %lld ms; "
                   "releasing it\n",
                   static_cast<long long>(kProviderCloseWaitMillis));
      }
    } catch (const std::exception& e) {
      DaemonLogf("[provide] closing the provider device failed: %s\n", e.what());
    }
    ReleaseDeviceLocked(providerDevice_);
    DaemonLogf("[provide] stopped providing without a tunnel\n");
  }
  providerConfig_ = ctl::StartProviderRequest();
  providerPeersLogged_ = -1;
  providerProvidingLogged_ = -1;
  std::scoped_lock lock(statusMutex_);
  status_.provider_running = false;
  status_.provider_control_mode.clear();
  status_.provider_mode = 0;
  status_.provider_network_key = false;
  status_.provider_client_count = -1;
  status_.provider_network_peers_json.clear();
}

void TunnelHost::RefreshProviderStatusLocked() {
  if (!providerDevice_) return;
  int64_t tier = 0;
  bool networkKey = false;
  // unread until its read succeeds, so a failed read shows no count
  int64_t clientCount = -1;
  int64_t providing = 0;
  std::string peersJson;
  try {
    tier = providerDevice_->getProvideMode();
    if (auto keys = providerDevice_->getProvideSecretKeys()) {
      for (const auto& key : *keys) {
        if (key.provide_mode == urnet::ProvideModeNetwork) {
          networkKey = true;
          break;
        }
      }
    }
    // the count a tunnel session's device gives the GUI over the device rpc:
    // its connected network peers, none while its provider has no client yet.
    // The peers themselves go with it, from the same reading, each cut to what
    // the GUI's rows show and connect with: an unredacted status reaches every
    // local user while no tunnel runs, unlike a session's mTLS device rpc, and
    // a peer's principal and roles are not for them.
    auto peers = providerDevice_->getNetworkPeers();
    urnet::NetworkPeerList rows;
    if (peers && peers->Connected) {
      rows.reserve(peers->Connected->size());
      for (auto& peer : *peers->Connected) {
        urnet::NetworkPeer row;
        row.ClientId = std::move(peer.ClientId);
        row.ProvideEnabled = peer.ProvideEnabled;
        row.DeviceName = std::move(peer.DeviceName);
        row.DeviceSpec = std::move(peer.DeviceSpec);
        rows.push_back(std::move(row));
      }
    }
    clientCount = static_cast<int64_t>(rows.size());
    providing = static_cast<int64_t>(
        std::count_if(rows.begin(), rows.end(), [](const urnet::NetworkPeer& peer) {
          return peer.ProvideEnabled;
        }));
    peersJson = ctl::DumpForWire(nlohmann::json(rows));
  } catch (const std::exception&) {
    // status must never throw across the wire; the next tick reads again
    clientCount = -1;
    peersJson.clear();
  }
  // What the device holds of its network peers, in the log, when its first
  // reading lands and whenever the counts move: counts only, never names.
  // Nothing else records what the platform delivered to a device with no
  // DeviceRemote.
  if (0 <= clientCount &&
      (clientCount != providerPeersLogged_ || providing != providerProvidingLogged_)) {
    DaemonLogf("[provide] the provider device holds %lld connected network peer(s), %lld "
               "providing\n",
               static_cast<long long>(clientCount), static_cast<long long>(providing));
    providerPeersLogged_ = clientCount;
    providerProvidingLogged_ = providing;
  }
  std::scoped_lock lock(statusMutex_);
  status_.provider_running = true;
  status_.provider_control_mode = providerConfig_.provide_mode;
  status_.provider_mode = tier;
  status_.provider_network_key = networkKey;
  status_.provider_client_count = clientCount;
  status_.provider_network_peers_json = std::move(peersJson);
}

// ---- the log upload (upload_logs) -------------------------------------------
//
// "Send feedback with logs" uploads this process's glog files, which is where
// everything support reads about the tunnel, the provider and the network is.
// It used to reach them only through the GUI's DeviceRemote, i.e. only while a
// tunnel session ran. Now the GUI asks here, connected or not. The sdk's
// upload zips the log directory inside its call, so the call runs on the
// upload's own thread (logupload::Flight), never on the main loop that serves
// every control request, the reaper and the kill switch.

TunnelHost::LogUploadResult TunnelHost::UploadLogs(const ctl::UploadLogsRequest& request,
                                                   logupload::PassedLogFiles guiLogFiles) {
  LogUploadResult result;
  // Re-validated here as start_provider is: this process is root, and the
  // feedback id becomes part of the url its device posts to.
  if (const auto invalid = ctl::ValidateUploadLogsRequest(request)) {
    result.error = invalid->message;
    result.code = invalid->code;
    return result;
  }
  std::unique_lock<std::mutex> lock(opMutex_, std::defer_lock);
  if (busy_.load() || !lock.try_lock()) {
    // A bring-up owns the session. Once it is over, the device it leaves
    // behind carries the upload (the session's; after a failed start, a
    // standalone one): the reaper starts it then. It is in flight from now on.
    const int64_t nowMillis = MonotonicMillis();
    const int64_t uploadId =
        logUploadFlight_->Begin(/*queued=*/true, logupload::Carrier::Queued, nowMillis);
    if (uploadId == 0) {
      result.error = "a log upload is in flight already";
      result.code = ctl::kCodeLogUploadBusy;
      return result;
    }
    queuedUpload_ = request;
    queuedGuiLogFiles_ = std::move(guiLogFiles);
    queuedUploadMillis_ = nowMillis;
    queuedUploadId_ = uploadId;
    DaemonLogf("[support] a log upload waits for the tunnel start in progress\n");
    result.ok = true;
    result.carrier = logupload::ToString(logupload::Carrier::Queued);
    result.uploadId = uploadId;
    return result;
  }
  return StartLogUploadLocked(request, /*queuedUploadId=*/0, std::move(guiLogFiles));
}

TunnelHost::LogUploadResult TunnelHost::StartLogUploadLocked(
    const ctl::UploadLogsRequest& request, int64_t queuedUploadId,
    logupload::PassedLogFiles guiLogFiles) {
  LogUploadResult result;
  const int64_t nowMillis = MonotonicMillis();
  const logupload::Carrier carrier =
      logupload::CarrierFor(device_.has_value(), providerDevice_.has_value());
  const char* carrierName = logupload::ToString(carrier);
  // Admitted before anything is built: one upload at a time.
  int64_t uploadId = 0;
  if (queuedUploadId != 0) {
    if (logUploadFlight_->Start(queuedUploadId, carrier, nowMillis)) uploadId = queuedUploadId;
  } else {
    uploadId = logUploadFlight_->Begin(/*queued=*/false, carrier, nowMillis);
  }
  if (uploadId == 0) {
    result.error = "a log upload is in flight already";
    result.code = ctl::kCodeLogUploadBusy;
    return result;
  }
  uint64_t deviceHandle = 0;
  try {
    if (carrier == logupload::Carrier::Tunnel) {
      deviceHandle = device_->handle();
    } else if (carrier == logupload::Carrier::Provider) {
      deviceHandle = providerDevice_->handle();
    } else {
      // Neither runs: a device for the upload alone, built as StartProvider
      // builds the provider-only device (the persisted identity, the request's
      // credentials and network space) and nothing after that: no egress
      // marker, no nftables transaction, no setRpcServer, no tun, no IoLoop, no
      // DNS. It provides to nobody. One at a time, under the one identity.
      RetireUploadDeviceLocked();
      LoadNetworkSpaceLocked(request.network_space_json);
      uploadDevice_ = NewDeviceLocked(request.by_jwt, request.instance_id, request.app_version);
      uploadDevice_->setProvideControlMode("never");
      uploadDeviceBuiltMillis_ = nowMillis;
      deviceHandle = uploadDevice_->handle();
    }
  } catch (const std::exception& e) {
    if (carrier == logupload::Carrier::Standalone) RetireUploadDeviceLocked();
    logUploadFlight_->Finish(uploadId, logupload::FlightState::Failed);
    result.error = std::string("the logs could not be uploaded: ") + e.what();
    result.code = ctl::kCodeLogUploadFailed;
    DaemonLogf("[support] %s\n", result.error.c_str());
    return result;
  }
  // The zip and the post, on the upload's own thread. It holds the flight, the
  // device's handle and the GUI's log files, and nothing else of this object;
  // the flight keeps the device alive until the call returns
  // (ReleaseDeviceLocked), and the call's callback ends the upload in it,
  // which status reports.
  const size_t guiLogFileCount = guiLogFiles.Size();
  logUploadFlight_->Run(uploadId, deviceHandle,
                        [flight = logUploadFlight_, uploadId, deviceHandle,
                         feedbackId = request.feedback_id, carrierName,
                         guiLogFiles = std::make_shared<logupload::PassedLogFiles>(
                             std::move(guiLogFiles))] {
                          UploadLogsOnDevice(flight, uploadId, deviceHandle, feedbackId,
                                             carrierName, *guiLogFiles);
                        });
  DaemonLogf("[support] uploading this daemon's logs for a feedback (%s device), with %zu of "
             "the GUI's log files\n",
             carrierName, guiLogFileCount);
  result.ok = true;
  result.carrier = carrierName;
  result.uploadId = uploadId;
  return result;
}

void TunnelHost::RetireUploadDeviceLocked() {
  uploadDeviceBuiltMillis_ = 0;
  if (!uploadDevice_) return;
  try {
    uploadDevice_->close();
    if (!uploadDevice_->waitForClose(kProviderCloseWaitMillis)) {
      DaemonLogf("[support] the log upload device had not finished closing after %lld ms; "
                 "releasing it\n",
                 static_cast<long long>(kProviderCloseWaitMillis));
    }
  } catch (const std::exception& e) {
    DaemonLogf("[support] closing the log upload device failed: %s\n", e.what());
  }
  ReleaseDeviceLocked(uploadDevice_);
}

void TunnelHost::ReleaseDeviceLocked(std::optional<urnet::DeviceLocal>& device) {
  if (!device) return;
  const uint64_t deviceHandle = device->handle();
  // Handed back unless the upload's call is on it; released here then.
  std::shared_ptr<void> released = logUploadFlight_->KeepUntilReturned(
      deviceHandle, std::make_shared<urnet::DeviceLocal>(std::move(*device)));
  device.reset();
  released.reset();
}

void TunnelHost::MaintainLogUploadLocked() {
  const int64_t nowMillis = MonotonicMillis();
  const logupload::Flight::Reading upload = logUploadFlight_->Read(nowMillis);
  if (uploadDevice_ &&
      logupload::RetireStandaloneDevice(logupload::IsFinished(upload.state),
                                        uploadDeviceBuiltMillis_, nowMillis)) {
    RetireUploadDeviceLocked();
  }
  if (!queuedUpload_) return;
  const ctl::UploadLogsRequest request = std::move(*queuedUpload_);
  logupload::PassedLogFiles guiLogFiles = std::move(queuedGuiLogFiles_);
  const int64_t uploadId = queuedUploadId_;
  queuedUpload_.reset();
  queuedUploadId_ = 0;
  if (logupload::QueuedUploadExpired(queuedUploadMillis_, nowMillis)) {
    logUploadFlight_->Finish(uploadId, logupload::FlightState::Failed);
    DaemonLogf("[support] a log upload waited too long for a tunnel start and was dropped\n");
    return;
  }
  StartLogUploadLocked(request, uploadId, std::move(guiLogFiles));
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

  // 2) Per-app exclusion. nft binds the urnetwork-exclude slice to its cgroup
  //    id when the ruleset loads, so a slice that appears (the first
  //    urnetwork-exclude of a login), disappears (logout) or comes back, and a
  //    tunnel that changes owner, all need the same state installed again with
  //    the slice as it is now. One stat a tick; an nft run only on a change.
  if (filter_.installed()) {
    uint64_t excludeId = 0;
    ExcludeSliceLocked(&excludeId);
    if (excludeId != excludeAppliedId_ && --excludeRetryTicks_ <= 0) {
      std::string excludeError;
      if (ReinstallFilterLocked(&excludeError)) {
        excludeRetryTicks_ = 0;
        excludeRetryFailures_ = 0;
      } else {
        // The tamper repair's cadence and log rate: an nft that keeps failing
        // is not worth a fork every second.
        excludeRetryTicks_ = settings_.filter_verify_interval_ticks;
        if (FilterFailureLogged(settings_, excludeRetryFailures_++)) {
          DaemonLogf("[tunnel] could not update the per-app exclusion: %s\n",
                     excludeError.c_str());
        }
      }
    }
  }

  // 3) Tamper / flush detection. nftables hands out no notification when a
  //    third party destroys our table, and `nft flush ruleset` is the FIRST
  //    LINE of the /etc/sysconfig/nftables.conf Fedora and Bazzite ship — so
  //    `systemctl restart nftables` silently deletes `table inet urnetwork`,
  //    taking the kill switch, the IPv6 fail-closed rules, the DNS floor and
  //    the egress self-exclusion with it, while the UI still says Connected.
  //    Polling Verify() here is the entire mitigation, and re-installing the
  //    SAME config (floor included) is the entire repair.
  if (!filter_.installed()) return;
  if (++filterVerifyTicks_ < settings_.filter_verify_interval_ticks) return;
  filterVerifyTicks_ = 0;

  std::string error;
  if (filter_.Verify(&error)) {
    filterVerifyFailures_ = 0;
    return;
  }
  const bool loud = FilterFailureLogged(settings_, filterVerifyFailures_);
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

void TunnelHost::MaintainDnsLocked() {
  // THE CONSUMER FOR Tunnel::VerifyDnsStillApplied. It had none — the check was
  // written, documented as the mitigation for the NetworkManager rewrite, and
  // then called from nowhere, which is the same shape as NetFilter::Verify()
  // before MaintainFilterLocked existed and as the egress witness before the
  // reaper called it. A safety check with no caller is not a mitigation; it is
  // a comment that compiles.
  //
  // What it defends against: the override is applied ONCE at bring-up, and
  // every mechanism that owns /etc/resolv.conf on a desktop (NetworkManager's
  // dns=default plugin, dhcpcd, a resolved restart) rewrites it later, on a
  // schedule nothing tells us about. The DNS floor means the consequence is an
  // outage rather than a leak — but an unexplained outage on a machine that
  // says "Connected" is precisely the failure a tester cannot diagnose.
  if (tunnel_ == nullptr) return;
  if (!dnsProtectionExpected_) return;  // the session opted out at bring-up
  // Every tick where the check is a file read and nothing is failing; the
  // slower interval where it forks resolvectl, where a repair has just failed,
  // and where the file is contested (HostSettings.hpp says why each).
  if (DnsRepairContested(settings_, dnsTicksSinceRepair_)) ++dnsTicksSinceRepair_;
  const int verifyInterval = DnsVerifyIntervalTicks(
      settings_, /*forkingCheck=*/tunnel_->dnsBackend() == DnsBackend::SystemdResolved,
      /*repairFailing=*/dnsVerifyFailures_ != 0, dnsContested_);
  if (++dnsVerifyTicks_ < verifyInterval) return;
  dnsVerifyTicks_ = 0;

  std::string detail;
  if (tunnel_->VerifyDnsStillApplied(&detail)) {
    dnsVerifyFailures_ = 0;
    // A whole contested window in force since the last repair: no longer
    // contested.
    if (!DnsRepairContested(settings_, dnsTicksSinceRepair_)) dnsContested_ = false;
    return;
  }
  ++dnsVerifyFailures_;
  DaemonLogf("[tunnel] the DNS override is no longer in force (%s); re-applying it\n",
             detail.c_str());

  // REPAIR FIRST, the same order MaintainFilterLocked uses: re-applying is
  // almost always enough, because the displacing write is a one-shot.
  const DnsBackend backendBefore = tunnel_->dnsBackend();
  const std::vector<std::string> resolversBefore = tunnel_->resolvers();
  const bool applied = tunnel_->ApplyDns();
  {
    std::scoped_lock statusLock(statusMutex_);
    status_.dns_applied = tunnel_->report().dns_applied;
    status_.dns_detail = tunnel_->report().dns_detail;
  }
  support::LogDnsOverride("lost", applied, tunnel_->dnsBackend());
  if (applied) {
    dnsVerifyFailures_ = 0;
    dnsContested_ = DnsRepairContested(settings_, dnsTicksSinceRepair_);
    dnsTicksSinceRepair_ = 0;
    DaemonLogf("[tunnel] the DNS override has been re-applied (%s)\n",
               tunnel_->report().dns_detail.c_str());
    // The floor pins the resolvers the override just (re)published, so it has
    // to be rebuilt from the new report rather than left naming the old ones —
    // unless nothing it pins changed: the same backend and the same resolvers,
    // under a Connected floor that already closes off-tunnel DNS around exactly
    // those. A writer that rewrites /etc/resolv.conf at every network switch
    // would otherwise cost an nft table swap per rewrite, and every swap starts
    // the egress witness's counters over. A floor an EARLIER failed repair
    // lifted (block_offtunnel_dns false) is rebuilt, never assumed current.
    const FilterConfig& inForce = filter_.appliedConfig();
    const bool floorCurrent = filter_.state() == FilterState::Connected &&
                              inForce.block_offtunnel_dns == tunnel_->report().dns_applied &&
                              inForce.tunnel_resolvers == tunnel_->resolvers();
    if (tunnel_->dnsBackend() == backendBefore && tunnel_->resolvers() == resolversBefore &&
        floorCurrent) {
      return;
    }
    std::string ignored;
    ApplyFilterLocked(FilterState::Connected, &ignored);
    return;
  }

  // Repair failed. Rebuild the filter anyway so the floor tracks
  // dns_applied=false honestly, then escalate: a session whose names cannot be
  // protected is the same class of defect as proven-unprotected egress, and
  // bring-up already refuses it. Staying up would mean the tunnel is stricter
  // about the leak at connect time than it is one second later.
  std::string ignored;
  ApplyFilterLocked(FilterState::Connected, &ignored);
  if (dnsVerifyFailures_ < settings_.dns_repair_attempts) {
    DaemonLogf("[tunnel] the DNS override could not be re-applied (attempt %d of %d): %s\n",
               dnsVerifyFailures_, settings_.dns_repair_attempts,
               tunnel_->report().dns_detail.c_str());
    return;
  }
  StopUnsafeSessionLocked(
      "dns override lost",
      "Your DNS stopped going through the tunnel and could not be restored, so the "
      "connection was stopped rather than left resolving names outside it. Something "
      "on this machine is taking ownership of /etc/resolv.conf — NetworkManager and "
      "dhcpcd both do. (" + tunnel_->report().dns_detail + ")",
      ctl::kCodeDnsApplyFailed);
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

// /proc/net/route, /proc/net/ipv6_route, /proc/net/wireless and the path
// interface's sysfs carrier, carrier counters and speed, which a Wi-Fi or
// link-down interface answers with EINVAL (TextFile.hpp): never a throw out of
// the reaper tick.
std::string ReadTextFile(const std::string& path) { return textfile::ReadAll(path); }

// The interface holding `address` in `family`, from getifaddrs; "" when none
// does (an address removed between the probe and this read).
std::string InterfaceOwningAddress(const ifaddrs* interfaces, int family,
                                   const std::string& address) {
  for (const ifaddrs* ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_name == nullptr) continue;
    if (ifa->ifa_addr->sa_family != family) continue;
    char text[INET6_ADDRSTRLEN] = {0};
    const void* raw = nullptr;
    if (family == AF_INET6) {
      raw = &reinterpret_cast<const sockaddr_in6*>(ifa->ifa_addr)->sin6_addr;
    } else {
      raw = &reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr;
    }
    if (::inet_ntop(family, raw, text, sizeof(text)) != nullptr && address == text) {
      return ifa->ifa_name;
    }
  }
  return std::string();
}

// How many times `iface`'s carrier has come up: carrier_up_count (4.16+), else
// carrier_changes (3.15+, which counts both edges and grows the same way); 0
// when neither is readable.
std::uint64_t ReadCarrierUps(const std::string& iface) {
  for (const char* file : {"carrier_up_count", "carrier_changes"}) {
    std::istringstream value(ReadTextFile("/sys/class/net/" + iface + "/" + file));
    std::uint64_t count = 0;
    if (value >> count) return count;
  }
  return 0;
}

// One family's way off the tunnel, as the SDK's next dial would take it (see
// NetworkQuality.hpp): the probe's source address, the interface holding it,
// that interface's default next hop and its carrier counter. Empty when the
// family has no route, when only a link-local or loopback source is left, and
// when the probe landed on the tun itself -- a missing mark, the egress
// witness's to report, and never a way off the tunnel.
LinuxFamilyPath ReadFamilyPath(int family, const ifaddrs* interfaces,
                               const std::string& tunnelInterface) {
  LinuxFamilyPath path;
  const std::string source = ProbeEgressSource(family);
  if (source.empty() || !LinuxSourceLeavesTheLink(source)) return path;
  const std::string iface = InterfaceOwningAddress(interfaces, family, source);
  if (iface.empty() || iface == tunnelInterface) return path;
  path.interface_name = iface;
  if (family == AF_INET6) {
    path.source = LinuxIpv6Prefix64(source);
    path.gateway = LinuxDefaultRouteGatewayV6(ReadTextFile("/proc/net/ipv6_route"), iface);
  } else {
    path.source = source;
    path.gateway = LinuxDefaultRouteGateway(ReadTextFile("/proc/net/route"), iface);
  }
  if (path.source.empty()) return LinuxFamilyPath();
  path.carrier_ups = ReadCarrierUps(iface);
  return path;
}

// The reaper's network-path snapshot: both families' ways off the tunnel, and
// the quality readings of the interface IPv4 leaves through (IPv6's when IPv4
// has none). Two probe sockets, two /proc reads and a getifaddrs per tick.
LinuxNetworkQualitySnapshot ReadNetworkPath(const std::string& tunnelInterface) {
  LinuxNetworkQualitySnapshot snapshot;
  ifaddrs* interfaces = nullptr;
  if (::getifaddrs(&interfaces) != 0) interfaces = nullptr;
  snapshot.v4 = ReadFamilyPath(AF_INET, interfaces, tunnelInterface);
  snapshot.v6 = ReadFamilyPath(AF_INET6, interfaces, tunnelInterface);
  if (interfaces != nullptr) ::freeifaddrs(interfaces);
  snapshot.interface_name =
      snapshot.v4.present() ? snapshot.v4.interface_name : snapshot.v6.interface_name;
  if (snapshot.interface_name.empty()) return snapshot;

  const std::string prefix = "/sys/class/net/" + snapshot.interface_name + "/";
  {
    std::istringstream value(ReadTextFile(prefix + "carrier"));
    value >> snapshot.carrier;
  }
  std::int64_t speed = 0;
  {
    std::istringstream value(ReadTextFile(prefix + "speed"));
    value >> speed;
  }
  snapshot.speed_bucket_mbps =
      LinuxNetworkSpeedBucket(speed > 0 ? static_cast<std::uint64_t>(speed) : 0);
  snapshot.wireless_signal_level = LinuxWirelessSignalLevel(
      ReadTextFile("/proc/net/wireless"), snapshot.interface_name);
  return snapshot;
}

// CLOCK_BOOTTIME minus CLOCK_MONOTONIC: the time this machine has spent
// suspended since it booted (LinuxSuspendDetector). 0 on a macOS dev build,
// which has no suspend clock to read.
std::int64_t SuspendedMillis() {
#if defined(CLOCK_BOOTTIME)
  timespec boot{};
  timespec monotonic{};
  ::clock_gettime(CLOCK_BOOTTIME, &boot);
  ::clock_gettime(CLOCK_MONOTONIC, &monotonic);
  return (static_cast<std::int64_t>(boot.tv_sec) - monotonic.tv_sec) * 1000 +
         (static_cast<std::int64_t>(boot.tv_nsec) - monotonic.tv_nsec) / 1000000;
#else
  return 0;
#endif
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
  if (++egressWitnessTicks_ < settings_.egress_witness_interval_ticks) return false;
  egressWitnessTicks_ = 0;

  // No route off the tunnel is a pass here: a network switch in progress, or an
  // IPv6-only network, is not a session to stop (Tunnel::VerifyEgressWitness). So
  // is an IPv6 route with no global address to leave by, read again each time: a
  // global address that arrives, or leaves, changes this check's reading, and a
  // reading a change caught half way costs one warning (StepEgressRecheck).
  TunnelError witnessError;
  const bool passed =
      tunnel_->VerifyEgressWitness("recheck", /*requireOffTunnelRoute=*/false, &witnessError);
  const EgressRecheckStep step = StepEgressRecheck(settings_, passed, &egressWitnessFailures_);
  {
    std::scoped_lock lock(statusMutex_);
    status_.egress_protected = passed;
  }
  if (step == EgressRecheckStep::Passed) return false;
  if (step == EgressRecheckStep::Warned) {
    DaemonLogf(
        "[tunnel] WARNING: the egress witness failed (%s). Re-checking in %ds; %d in a row "
        "tears this tunnel down.\n",
        witnessError.message.c_str(),
        settings_.egress_witness_interval_ticks *
            static_cast<int>(ReaperIntervalSeconds(settings_)),
        settings_.egress_witness_failures_to_stop);
    return false;
  }

  DaemonLogf(
      "[tunnel] STOPPING: this daemon's own traffic is no longer demonstrably outside its own "
      "tunnel, %d checks running. %s\n",
      egressWitnessFailures_, witnessError.message.c_str());
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

// ---- the dead-tunnel failsafe ----------------------------------------------

namespace {

// The sentence a failsafe teardown leads its `error` with, naming the window the
// watch was built with. StopUnsafeSessionLocked adds whether this machine is now
// blocked.
std::string DeadTunnelMessage(watchdog::DeadTunnelReason reason,
                              const watchdog::DeadTunnelSettings& settings) {
  const auto seconds = [](int64_t millis) { return std::to_string(millis / 1000); };
  switch (reason) {
    case watchdog::DeadTunnelReason::NoInbound:
      return "The connection was stopped because it carried nothing: this computer kept "
             "sending into the tunnel and nothing came back for " +
             seconds(settings.deadFastMillis) + " seconds.";
    case watchdog::DeadTunnelReason::NoExit:
      return "The connection was stopped because it carried nothing: no provider could carry "
             "its traffic for " +
             seconds(settings.deadSlowMillis) + " seconds.";
    case watchdog::DeadTunnelReason::SdkUnresponsive:
      return "The connection was stopped because it carried nothing: the URnetwork engine "
             "stopped answering for " +
             seconds(settings.sdkUnresponsiveMillis) +
             " seconds, so the URnetwork service restarted itself.";
    case watchdog::DeadTunnelReason::None:
      break;
  }
  return "The connection was stopped because it carried nothing.";
}

}  // namespace

void TunnelHost::StartDeadTunnelWatchLocked() {
  if (!device_ || !tunnel_) return;
  // The window's level at the up edge comes from the sampler's first sample,
  // taken at once on its own thread, so the first destination the app picks
  // reads as the new generation it is. Nothing here waits on the device.
  if (!exitSampler_.Start(device_->handle(), &WatchdogMillis,
                          watchdog::SampleIntervalMillis(deadTunnelWatch_.settings()))) {
    return;
  }
  watchdog::WatchInputs inputs;
  inputs.nowMillis = WatchdogMillis();
  const std::string iface = tunnel_->name();
  inputs.outboundPackets = ReadIfaceCounter(iface, "tx_packets");
  inputs.inboundPackets = ReadIfaceCounter(iface, "rx_packets");
  inputs.windowSampled = false;
  inputs.offline = networkQualityTracker_.offline();
  deadTunnelWatch_.Start(inputs);
  deadTunnelWatching_ = true;
  DaemonLogf("[tunnel] failsafe: watching this tunnel. It is stopped if it carries nothing: no "
             "proven exit for %llds, %llds of sending with nothing coming back, or %llds of "
             "sdk silence. An account out of balance is stopped for the sdk's silence only. "
             "Nothing reconnects afterwards.\n",
             static_cast<long long>(deadTunnelWatch_.settings().deadSlowMillis / 1000),
             static_cast<long long>(deadTunnelWatch_.settings().deadFastMillis / 1000),
             static_cast<long long>(deadTunnelWatch_.settings().sdkUnresponsiveMillis / 1000));
}

void TunnelHost::StopDeadTunnelWatchLocked() {
  exitSampler_.Stop();
  deadTunnelWatching_ = false;
}

bool TunnelHost::CheckDeadTunnelLocked() {
  // Not while the io loop's death waits for the reaper below: that teardown
  // owns the explanation.
  if (!deadTunnelWatching_ || !tunnel_ || !device_ || ioLoopDied_.load()) return false;

  watchdog::WatchInputs inputs;
  inputs.nowMillis = WatchdogMillis();
  {
    std::scoped_lock lock(statusMutex_);
    inputs.tunnelUp = status_.tunnel_state == ctl::TunnelState::Up;
    inputs.routesInstalled = status_.routes_installed;
  }
  const std::string iface = tunnel_->name();
  // On a tun, transmit is what the host sends into the tunnel (the SDK reads
  // it) and receive is what the SDK writes back out to the host.
  inputs.outboundPackets = ReadIfaceCounter(iface, "tx_packets");
  inputs.inboundPackets = ReadIfaceCounter(iface, "rx_packets");
  const ExitSampler::Reading sample = exitSampler_.Read();
  inputs.provenCount = sample.provenCount;
  inputs.lastProvenMillis = sample.lastProvenMillis;
  inputs.lastSampleMillis = sample.lastSampleMillis;
  inputs.connectionGeneration = sample.connectionGeneration;
  inputs.providerWindowMinSatisfied = sample.providerWindowMinSatisfied;
  inputs.windowSampled = sample.sampled;
  // Whether this machine has a network at all, as the path tracker last read it
  // (on the tick before this one: the reaper reads the path after this check).
  // Offline, the no-exit rules hold and their windows leave the time out; back
  // after a real stretch, every window starts again (TunnelWatchdog.hpp).
  inputs.offline = networkQualityTracker_.offline();
  // Out of balance the no-exit rules hold and the traffic stays captured.
  inputs.balance = sample.balance;

  const watchdog::WatchTick tick = deadTunnelWatch_.Tick(inputs);
  if (tick.balanceStep == watchdog::BalanceStep::Entered) {
    DaemonLogf("[tunnel] failsafe: this account is out of balance. Its traffic stays in the "
               "tunnel, and the no-exit rules wait until a contract goes through or the user "
               "disconnects\n");
  } else if (tick.balanceStep == watchdog::BalanceStep::Released) {
    DaemonLogf("[tunnel] failsafe: contracts are going through again, so every window starts "
               "again from now\n");
  }
  if (tick.wentOffline) {
    DaemonLogf("[tunnel] failsafe: this computer has no network; the no-exit rules wait for "
               "one, and the time offline is not counted\n");
  } else if (tick.offlineStep == watchdog::OfflineStep::BackRebased) {
    DaemonLogf("[tunnel] failsafe: back on a network after %llds offline, so every window "
               "starts again from now\n",
               static_cast<long long>(tick.offlineStretchMillis / 1000));
  } else if (tick.offlineStep == watchdog::OfflineStep::BackFromBlip) {
    DaemonLogf("[tunnel] failsafe: back on a network after %lldms offline; that time is not "
               "counted\n",
               static_cast<long long>(tick.offlineStretchMillis));
  }
  if (tick.froze) {
    DaemonLogf("[tunnel] failsafe: nothing watched this tunnel for %lldms (a suspend, or a "
               "stalled daemon), so every window starts again from now\n",
               static_cast<long long>(tick.tickGapMillis));
  } else if (tick.verdictClockReset) {
    DaemonLogf("[tunnel] failsafe: destination generation %lld replaced the provider window; "
               "every window starts again, the no-inbound one once the window forms\n",
               static_cast<long long>(sample.connectionGeneration));
  } else if (tick.trafficClockReset) {
    DaemonLogf("[tunnel] failsafe: destination generation %lld formed its window; the %llds "
               "no-inbound window starts now\n",
               static_cast<long long>(sample.connectionGeneration),
               static_cast<long long>(deadTunnelWatch_.settings().deadFastMillis / 1000));
  }
  bool armedChanged = false;
  {
    std::scoped_lock lock(statusMutex_);
    if (status_.failsafe_armed != tick.verdict.armed) {
      status_.failsafe_armed = tick.verdict.armed;
      armedChanged = true;
    }
  }
  // A countdown that was showing ends with a verdict (logged below), a rebase,
  // a new destination's clocks, or something getting through, and the log says
  // which.
  if (armedChanged && tick.verdict.armed) {
    DaemonLogf("[tunnel] failsafe: this tunnel is stopped in %llds unless something gets "
               "through\n",
               static_cast<long long>(tick.verdict.millisToFailsafe / 1000));
  } else if (armedChanged && tick.froze) {
    DaemonLogf("[tunnel] failsafe: the rebase reset the countdown\n");
  } else if (armedChanged && tick.offlineStep == watchdog::OfflineStep::Offline) {
    DaemonLogf("[tunnel] failsafe: the countdown waits while this computer has no network\n");
  } else if (armedChanged && tick.offlineStep == watchdog::OfflineStep::BackRebased) {
    DaemonLogf("[tunnel] failsafe: the return to a network reset the countdown\n");
  } else if (armedChanged && tick.balanceStep == watchdog::BalanceStep::Released) {
    DaemonLogf("[tunnel] failsafe: the balance coming back reset the countdown\n");
  } else if (armedChanged && tick.outOfBalance) {
    DaemonLogf("[tunnel] failsafe: the countdown waits while this account is out of balance\n");
  } else if (armedChanged && (tick.verdictClockReset || tick.trafficClockReset)) {
    DaemonLogf("[tunnel] failsafe: the new destination's clocks reset the countdown\n");
  } else if (armedChanged && tick.verdict.reason == watchdog::DeadTunnelReason::None) {
    DaemonLogf("[tunnel] failsafe: the countdown ended; the tunnel is carrying again\n");
  }
  if (tick.verdict.reason == watchdog::DeadTunnelReason::None) return false;

  const watchdog::DeadTunnelSignals& s = tick.signals;
  const int64_t now = inputs.nowMillis;
  DaemonLogf(
      "[tunnel] STOPPING: this tunnel is up but carries nothing (%s). Watched for %llds; the "
      "sdk last answered %lldms ago with %lld proven exit(s), the last proven one %lldms ago; "
      "the last packet back out of the tunnel %lldms ago, %llu packet(s) in since. Nothing is "
      "reconnected automatically: the next attempt is the user's.\n",
      watchdog::StopReasonOf(tick.verdict.reason),
      static_cast<long long>((now - s.upSinceMillis) / 1000),
      static_cast<long long>(watchdog::detail::AgeSince(s.lastSampleMillis, s.upSinceMillis, now)),
      static_cast<long long>(s.provenCount),
      static_cast<long long>(watchdog::detail::AgeSince(s.lastProvenMillis, s.upSinceMillis, now)),
      static_cast<long long>(
          watchdog::detail::AgeSince(s.lastInboundMillis, s.trafficStartMillis, now)),
      static_cast<unsigned long long>(s.outboundSinceInbound));
  // The landing every involuntary teardown takes: the armed floor when the
  // kill switch was asked for, no table otherwise, the outcome checked and
  // said, and everything published last.
  StopUnsafeSessionLocked(watchdog::StopReasonOf(tick.verdict.reason),
                          DeadTunnelMessage(tick.verdict.reason, deadTunnelWatch_.settings()),
                          ctl::kCodeTunnelDead, "because it carried nothing",
                          /*sdkWedged=*/tick.verdict.reason ==
                              watchdog::DeadTunnelReason::SdkUnresponsive);
  return true;
}

void TunnelHost::Reap() {
  // A SUSPEND IS MEASURED FIRST, before the try-lock below can return: a
  // bring-up holding opMutex_ through the first ticks after a resume must not
  // hide it, and the measure is the suspend clock itself, not a gap between
  // ticks, so a stall of this loop (a long nft run, that same bring-up) never
  // reads as one (LinuxSuspendDetector). The kick it owes waits in
  // resumePending_ for the first tick that holds the lock.
  if (suspendDetector_.Observe(SuspendedMillis())) resumePending_ = true;
  // Try-locked, as every main-loop callback here is. A bring-up holds opMutex_
  // for the whole of its run, and waiting for it here froze the control loop,
  // `status` included, from the first tick of an async start to its end. The
  // guards below cannot judge a session mid-bring-up anyway; they run on the
  // next tick that finds the lock free.
  std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);
  if (!lock.owns_lock()) return;  // next tick
  // The SDK writes its log while a device runs, so that is when it is flushed
  // every second.
  glogflush::SetActive(device_.has_value() || providerDevice_.has_value() ||
                       uploadDevice_.has_value());
  ReapRetiredLoopsLocked();
  if (CheckTunnelStormLocked()) return;  // the tunnel is gone; nothing else to reap
  // Ordered AFTER the storm guard: if traffic is already amplifying, stop it
  // on the cheap byte-counter read rather than spending an nft fork first.
  if (CheckEgressWitnessLocked()) return;
  if (busy_.load()) return;  // a bring-up owns the session AND the filter
  // After the guards above, which end a session for something worse than
  // carrying nothing; whichever ends it owns the explanation.
  if (CheckDeadTunnelLocked()) return;

  // The log upload's standalone device once its upload has reported, and a
  // request that waited for a bring-up which is now over.
  MaintainLogUploadLocked();

  // The tunnel session's device or the provider-only device, whichever is live
  // (never both): a provider's transports follow a path change too.
  urnet::DeviceLocal* liveDevice =
      device_ ? &*device_ : (providerDevice_ ? &*providerDevice_ : nullptr);
  // The kick a suspend owes: every connection the transports held may have
  // died in it with nothing about the network to show, so the next tick with a
  // path kicks. With no device there is nobody to tell, and the next one is
  // rebased on whatever network it finds.
  if (resumePending_ && liveDevice != nullptr) {
    DaemonLogf("[tunnel] resumed from a suspend; refreshing transports on the next network "
               "path\n");
    networkQualityTracker_.ForceKickOnNextPath();
  }
  resumePending_ = false;
  if (liveDevice != nullptr) {
    const std::string tunnelInterface = tunnel_ ? tunnel_->name() : std::string();
    // The SDK's own route per family, measured (ReadNetworkPath), against the
    // path the transports were last kicked onto; the tracker decides when a
    // difference is worth a kick (NetworkQuality.hpp), so a carrier blip, a
    // family that only appears and a burst inside one switch do not each cost
    // a dial.
    const LinuxNetworkChange networkChange =
        networkQualityTracker_.Observe(ReadNetworkPath(tunnelInterface), WatchdogMillis());
    if (networkChange == LinuxNetworkChange::Path) {
      DaemonLogf("[tunnel] physical network path changed (%s); refreshing transports\n",
                 DescribeLinuxNetworkPath(networkQualityTracker_.kicked()).c_str());
      support::LogConnectivity("path-change");
    } else if (networkChange == LinuxNetworkChange::Quality) {
      DaemonLogf("[tunnel] physical network quality changed; remeasuring transfer pacing\n");
    }
    const bool path = networkChange == LinuxNetworkChange::Path;
    if (networkChange != LinuxNetworkChange::None && device_ && deadTunnelWatching_) {
      // The watched session's device is told by the failsafe's sampler, off
      // this loop: the call takes the device's state lock, and a link drop, the
      // usual path change, is when a wedged device would hold this loop and
      // the verdict that ends its session with it.
      exitSampler_.NoteNetworkChange(path);
    } else if (networkChange != LinuxNetworkChange::None) {
      try {
        if (path) {
          liveDevice->networkChanged();
        } else {
          liveDevice->networkQualityChanged();
        }
      } catch (const std::exception& e) {
        DaemonLogf("[tunnel] network change notification failed: %s\n", e.what());
      }
    }
  }
  // The provider-only device's tier, keys and peers are the sdk's to change,
  // so `status` re-reads them once a second (a no-op without that device).
  RefreshProviderStatusLocked();
  // ...and its provider status controller stops polling the API once no GUI
  // that shows it has asked for a while (provider_stats with poll_status).
  if (providerStatusVc_ && providerStatusLease_.Expire(MonotonicMillis())) {
    try {
      providerStatusVc_->stop();
    } catch (const std::exception& e) {
      DaemonLogf("[provide] stopping the provider status failed: %s\n", e.what());
    }
  }

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
    MaintainDnsLocked();
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
    // The machine, its landing, then the device, as StopUnsafeSessionLocked
    // orders them.
    RevertSessionMachineLocked();
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
    StopInternalLocked(std::string());
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
    support::LogTunnelEnded("stopped", "io_loop", ctl::kCodeTunOpenFailed);
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
  support::LogDnsOverride("resolved-restarted", applied, self->tunnel_->dnsBackend());
  // The DNS port floor is only correct while the override is in force, and the
  // resolvers it pins come back out of the tunnel on this same pass — so this
  // re-apply is what keeps the pinned :53 permit naming the servers resolved
  // was actually given.
  std::string ignored;
  self->ApplyFilterLocked(FilterState::Connected, &ignored);

  if (!applied) {
    // THIS USED TO BE AN fprintf AND NOTHING ELSE, and the ApplyFilterLocked
    // above re-derives block_offtunnel_dns from the now-false dns_applied — so
    // the same pass that failed to restore the override also REMOVED the floor
    // that was containing the failure, leaving the LAN permit to accept :53 to
    // the router. A session that comes up refusing this exact condition must
    // not tolerate it mid-flight; hand it to the reaper, which repairs a few
    // times and then stops the session with a reason the user can act on.
    DaemonLogf("[tunnel] the DNS override did not survive the systemd-resolved restart: %s\n",
               report.dns_detail.c_str());
    // Check on the next tick, whichever interval that check runs at.
    self->dnsVerifyTicks_ = std::max(self->settings_.dns_verify_interval_ticks,
                                     self->settings_.dns_file_verify_interval_ticks);
  }
}

}  // namespace urnw
