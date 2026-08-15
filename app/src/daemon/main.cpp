// urnetworkd — the privileged URnetwork daemon (linux/MIGRATION.md workstream
// A). Runs as root under systemd, owns /dev/net/tun + routes + DNS via
// TunnelHost (DeviceLocal with the loopback mTLS device RPC enabled) and the
// privileged GeoClue write, and serves the unix control socket the
// unprivileged GUI drives (ControlServer).
//
// Deliberately NO GUI dependency of any kind — plain glib (GMainLoop), gio's
// glib, nlohmann_json and the SDK. The packaging depends on this: the daemon
// package must install without pulling GTK.
//
// systemd integration: Type=notify readiness is spoken directly over
// $NOTIFY_SOCKET (one datagram, "READY=1"), so there is no libsystemd
// dependency either. New-style daemons never self-daemonize; --foreground is
// accepted for interactive runs and simply skips the readiness notification.
//
// SPDX-License-Identifier: MPL-2.0
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cstddef>  // offsetof
#include <cstdio>
#include <cstring>
#include <string>

#include <glib-unix.h>
#include <glib.h>

#include <urnetwork_sdk.hpp>

#include "LocationOverride.hpp"
#include "Tunnel.hpp"
#include "daemon/ControlServer.hpp"
#include "daemon/DaemonLog.hpp"
#include "daemon/TunnelHost.hpp"

// The release version, threaded in via the -Dapp_version meson option (the
// pipeline passes $VERSION). This is what the hello reply's daemon_version
// reports — the field the GUI's "daemon out of date" rendering names — so it
// must be the REAL release version, never a hardcoded constant.
#ifndef UR_APP_VERSION
#define UR_APP_VERSION "0.0.0"
#endif

namespace {

// Same bound the app used in-process: the data plane's memory target scales
// from it (SetMemoryLimit -> connect defaults).
constexpr int64_t kMemoryLimit = 64ll * 1024 * 1024;

// State (device identity + SDK storage) and log locations. systemd's
// StateDirectory=/LogsDirectory= set the env vars; the fallbacks match the
// unit, and URNETWORK_* allow an unprivileged dev run.
std::string StateDir() {
  if (const char* env = std::getenv("URNETWORK_STATE_DIR"); env && *env) return env;
  if (const char* env = std::getenv("STATE_DIRECTORY"); env && *env) return env;
  return "/var/lib/urnetwork";
}

std::string LogDir() {
  if (const char* env = std::getenv("URNETWORK_LOG_DIR"); env && *env) return env;
  if (const char* env = std::getenv("LOGS_DIRECTORY"); env && *env) return env;
  return "/var/log/urnetwork";
}

// Type=notify readiness without libsystemd: one datagram on $NOTIFY_SOCKET.
// '@'-prefixed (abstract) addresses per sd_notify(3). No-op when unset.
void NotifySystemdReady() {
  const char* path = std::getenv("NOTIFY_SOCKET");
  if (path == nullptr || path[0] == '\0') return;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const size_t len = std::strlen(path);
  if (len >= sizeof(addr.sun_path)) return;
  std::memcpy(addr.sun_path, path, len);
  socklen_t addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
  if (path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace
#if defined(SOCK_CLOEXEC)
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#else  // macOS dev build: no SOCK_CLOEXEC; the fd lives three lines anyway
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
#endif
  if (fd < 0) return;
  static const char kReady[] = "READY=1";
  ::sendto(fd, kReady, sizeof(kReady) - 1, 0, reinterpret_cast<const sockaddr*>(&addr),
           addrLen);
  ::close(fd);
}

struct Daemon {
  GMainLoop* loop = nullptr;
  urnw::TunnelHost* tunnel = nullptr;
  urnw::ControlServer* server = nullptr;
};

gboolean OnTerminate(gpointer data) {
  auto* d = static_cast<Daemon*>(data);
  std::fprintf(stderr, "[daemon] terminating\n");
  // Stop accepting/serving first, then tear the tunnel down cleanly (routes,
  // policy rules, the nftables table and the resolvectl revert all happen
  // under Stop).
  d->server->Stop();
  d->tunnel->Stop("daemon_shutdown");
  g_main_loop_quit(d->loop);
  return G_SOURCE_REMOVE;
}

// Everything the data plane needs from the host, resolved ONCE at startup and
// printed, so a broken environment is named before the first Connect instead
// of during it (the audit's "no preflight for ip/resolvectl being on PATH").
// Returns the number of REQUIRED tools that are missing.
int ReportPreflight() {
  struct Tool {
    const char* name;
    bool required;
    const char* why;
  };
  static const Tool kTools[] = {
      {"ip", true, "iproute2: the tun address, the capture routes and the policy rules"},
      {"nft", true, "nftables: egress self-exclusion (without it the daemon's own sockets "
                    "fall into its own tunnel), the IPv6 and DNS leak floor, the kill switch"},
      {"resolvectl", false,
       "systemd-resolved: pointing DNS at the tunnel. Without it the session comes up with "
       "dns_applied=false and says so"},
      {"modprobe", false, "loading the tun kernel module when /dev/net/tun is absent"},
  };
  int missingRequired = 0;
  for (const Tool& tool : kTools) {
    const std::string path = urnw::FindTool(tool.name);
    if (!path.empty()) {
      std::fprintf(stderr, "[preflight] %-11s %s\n", tool.name, path.c_str());
      continue;
    }
    if (tool.required) ++missingRequired;
    std::fprintf(stderr, "[preflight] %-11s MISSING (%s) — %s\n", tool.name,
                 tool.required ? "required" : "optional", tool.why);
  }
  const urnw::CgroupRef cgroup = urnw::SelfCgroupV2();
  if (cgroup.valid) {
    std::fprintf(stderr, "[preflight] cgroup      %s (level %d)\n", cgroup.path.c_str(),
                 cgroup.level);
  } else {
    ++missingRequired;
    std::fprintf(stderr,
                 "[preflight] cgroup      MISSING (required) — no cgroup v2 unified "
                 "hierarchy, so the daemon's own sockets cannot be marked and no tunnel can "
                 "be started safely\n");
  }
  if (::access("/dev/net/tun", F_OK) == 0) {
    std::fprintf(stderr, "[preflight] /dev/net/tun present\n");
  } else {
    std::fprintf(stderr,
                 "[preflight] /dev/net/tun absent — the tun module is not loaded yet "
                 "(one modprobe is attempted at the first start)\n");
  }
  return missingRequired;
}

}  // namespace

int main(int argc, char** argv) {
  bool foreground = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--version") {
      std::printf("urnetworkd %s (control protocol %d, sdk %s)\n", UR_APP_VERSION,
                  urnw::ctl::kControlProtocolVersion, urnet::version().c_str());
      return 0;
    }
    if (arg == "--foreground") {
      foreground = true;
      continue;
    }
    if (arg == "--diagnose") {
      // Print-and-exit field-support tool: everything the data plane needs
      // from the host, without touching the network or the control socket.
      std::printf("urnetworkd %s (control protocol %d, sdk %s)\n", UR_APP_VERSION,
                  urnw::ctl::kControlProtocolVersion, urnet::version().c_str());
      std::printf("control socket: %s\n", urnw::ControlServer::SocketPath().c_str());
      std::printf("state dir:      %s\n", StateDir().c_str());
      std::printf("log dir:        %s\n", LogDir().c_str());
      std::fflush(stdout);
      return ReportPreflight() == 0 ? 0 : 1;
    }
    if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: urnetworkd [--foreground] [--diagnose] [--version]\n"
          "URnetwork privileged daemon: control socket at %s,\n"
          "device RPC on 127.0.0.1:%d while the tunnel is up.\n"
          "  --diagnose  print the host preflight (ip/nft/resolvectl, cgroup, tun) and exit\n",
          urnw::ControlServer::SocketPath().c_str(), urnw::ctl::kDeviceRpcPort);
      return 0;
    }
    std::fprintf(stderr, "urnetworkd: unknown argument '%s'\n", arg.c_str());
    return 2;
  }

  // A dead control client must surface as a send() error, not kill the daemon.
  std::signal(SIGPIPE, SIG_IGN);

  const std::string stateDir = StateDir();
  const std::string logDir = LogDir();
  g_mkdir_with_parents(stateDir.c_str(), 0700);
  g_mkdir_with_parents(logDir.c_str(), 0700);
  urnet::setLogDir(logDir);
  // The log ring is the READ half of the Advanced-Mode session log: the SDK
  // writes its [rel] reliability stream to glog files in this directory, and
  // the ring tails them so the app can show them live over the control socket.
  // Without these two calls the ring exists, serves log_tail, and truthfully
  // reports that it holds nothing — a working pipe with no water in it.
  urnw::DaemonLog::Instance().SetSdkLogDir(logDir);
  urnw::DaemonLog::Instance().StartSdkLogPolling();
  urnw::DaemonLogf("[daemon] urnetworkd %s starting (log dir %s)\n", UR_APP_VERSION, logDir.c_str());
  urnet::setMemoryLimit(kMemoryLimit);

  // Clear a location override left behind by a previous run BEFORE serving any
  // client: nothing else on the system ever reverts /etc/geolocation, so an
  // override surviving a crash/reboot would keep reporting a provider's city
  // indefinitely. Only a file carrying the URnetwork marker is touched — an
  // admin's hand-written static location is not ours to delete. Once a client
  // claims the override (location_override_write), the server clears it again
  // when that client disconnects.
  urnw::DirectGeoClueWriter geoWriter;
  if (urnw::DirectGeoClueWriter::SystemOverridePresent()) {
    if (geoWriter.Clear()) {
      std::fprintf(stderr, "[daemon] cleared stale location override at startup\n");
    } else {
      std::fprintf(stderr, "[daemon] could not clear stale location override\n");
    }
  }

  // Crash safety is NOT inherited on Linux: unlike the Windows dynamic WFP
  // session, an nftables table and a set of `ip rule`s outlive the process
  // that installed them. Sweep our own leftovers (by table name, by our fwmark
  // and by our route-table id — never by priority alone, which would let us
  // delete a co-installed WireGuard's rules) before serving anyone.
  urnw::NetFilter::SweepStaleState();
  if (const int missing = ReportPreflight(); missing > 0) {
    // Do not refuse to start: the control socket must still come up so the GUI
    // gets a real answer instead of "the service is not running". The first
    // start_tunnel is what fails, with a code naming the missing piece.
    std::fprintf(stderr,
                 "[daemon] %d required host component(s) are missing: the control socket will "
                 "serve, but start_tunnel will refuse until they are installed\n",
                 missing);
  }

  urnw::TunnelHost tunnel(stateDir);
  // Off by default: a tunnel survives a GUI crash or restart and is adoptable.
  // Set $URNETWORK_ORPHAN_TIMEOUT_SECONDS to have the daemon stop a tunnel
  // nobody has owned for that long — the "captured machine with no UI"
  // recovery, for setups that prefer it to the tray action.
  if (const char* env = std::getenv("URNETWORK_ORPHAN_TIMEOUT_SECONDS");
      env != nullptr && *env != '\0') {
    const int seconds = std::atoi(env);
    if (seconds > 0) {
      tunnel.SetOrphanTimeoutSeconds(seconds);
      std::fprintf(stderr, "[daemon] orphan timeout: %ds\n", seconds);
    }
  }
  urnw::ControlServer server(tunnel, geoWriter);
  server.SetDaemonVersion(UR_APP_VERSION);
  // exact-match enforced against the GUI's hello: the gob device RPC carries
  // no version of its own, so a drifted SDK pair must be refused here
  server.SetSdkVersion(urnet::version());
  if (!server.Start()) {
    std::fprintf(stderr, "urnetworkd: could not start the control server\n");
    return 1;
  }

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  Daemon daemon{loop, &tunnel, &server};
  g_unix_signal_add(SIGTERM, &OnTerminate, &daemon);
  g_unix_signal_add(SIGINT, &OnTerminate, &daemon);

  if (!foreground) NotifySystemdReady();
  std::fprintf(stderr, "[daemon] urnetworkd %s ready (state=%s)\n", UR_APP_VERSION,
               stateDir.c_str());
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  return 0;
}
