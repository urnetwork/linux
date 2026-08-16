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

// Where this binary is installed (the unit's ExecStart). Printed in the
// recovery lines, so what the user is told to run is a path that exists on
// their machine; a dev run from a build tree prints its own argv[0] instead.
constexpr const char* kInstalledPath = "/usr/lib/urnetwork/urnetworkd";

std::string SelfPath(const char* argv0) {
  if (argv0 != nullptr && argv0[0] == '/') return argv0;
  return kInstalledPath;
}

// THE WAY BACK ONTO THE NETWORK, and the only thing on this machine that
// prints it. The kill switch is an nftables table, and nftables is not tied to
// process lifetime: a daemon that is SIGKILLed while armed leaves a machine
// that cannot reach anything — including any page explaining how to fix it. So
// the fix has to be reachable OFFLINE, from a surface a blocked user still has:
// `urnetworkd --help`, `urnetworkd --diagnose`, the journal (the floor logs it
// the moment it goes in), the log tail the GUI shows, and the comment block in
// the shipped unit (`systemctl cat urnetworkd`).
//
// The ORDER matters and is why this is prose and not one line: while the daemon
// is alive its reaper re-installs the table within seconds of anything deleting
// it (that is the tamper protection), so the daemon has to be stopped first.
void PrintRecovery(const char* argv0) {
  const std::string self = SelfPath(argv0);
  std::printf(
      "\n"
      "If this machine is cut off (the kill switch is armed, or urnetworkd died while a\n"
      "tunnel was up), this is the way back — no network access required:\n"
      "\n"
      "    sudo systemctl stop urnetworkd\n"
      "    sudo %s --revert\n"
      "\n"
      "  --revert removes the firewall table, the policy rules, the capture routes and the\n"
      "  armed marker, so the next start comes up open. With no systemd and no working\n"
      "  daemon binary, the firewall half alone is:\n"
      "\n"
      "    %s\n"
      "\n"
      "  Stop the daemon FIRST: while it is running it re-installs its own ruleset within\n"
      "  seconds of anything removing it. With the daemon running, disconnecting in the\n"
      "  app is the normal way to lift the kill switch, and it always works — the app talks\n"
      "  to the daemon over a unix socket, which no firewall rule here can block.\n",
      self.c_str(), urnw::NetFilter::RecoveryCommand());
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
      // WHICH AUTHORITY WOULD BE IN FORCE, and therefore which remediation a
      // refused user should be given. Under polkit there is no group to join
      // and nothing to log out of; under the fallback there is, and telling
      // someone to log out and back in when it would change nothing is exactly
      // the confusion this design set out to remove. Read-only: one access(2).
      {
        const bool polkit = urnw::ControlServer::PolkitPolicyPresent();
        std::printf("authorization:  %s (%s %s)\n",
                    polkit ? urnw::ctl::kAuthModePolkit : urnw::ctl::kAuthModeGroup,
                    urnw::ControlServer::PolicyPath().c_str(),
                    polkit ? "installed" : "MISSING");
        if (polkit) {
          std::printf("                the person signed in at this device's screen is "
                      "authorized in their current session:\n"
                      "                no group membership and no log out / log back in.\n");
        } else {
          std::printf("                no polkit action file, so urnetworkd falls back to the "
                      "'%s' group.\n"
                      "                Members of that group, plus root, may control the "
                      "tunnel — and group\n"
                      "                membership only applies to NEW login sessions.\n",
                      urnw::ctl::kControlGroupName);
        }
      }
      // Read-only, no subprocess: the marker is a tmpfs file. A user reading
      // this while blocked needs to know WHICH of the two situations they are
      // in before they are told what to type.
      const bool armed = ::access(urnw::NetFilter::ArmedMarkerPath(), F_OK) == 0;
      std::printf("kill switch:    %s (marker %s)\n",
                  armed ? "ARMED — this machine is deliberately blocked" : "not armed",
                  urnw::NetFilter::ArmedMarkerPath());
      const int missing = ReportPreflight();
      PrintRecovery(argv[0]);
      std::fflush(stdout);
      return missing == 0 ? 0 : 1;
    }
    if (arg == "--revert" || arg == "--revert-unless-armed") {
      // The documented escape hatch, and the unit's ExecStopPost.
      //
      //   --revert                unconditional: lift everything, clear the
      //                           armed marker. This is what a stuck human
      //                           runs.
      //   --revert-unless-armed   the same sweep, except that a machine which
      //                           was ARMED when the daemon died has its floor
      //                           REPLACED with a fresh armed one in a single
      //                           atomic `nft -f` — no open window across the
      //                           crash, which is the entire point of the kill
      //                           switch. Only the unit uses this.
      const bool preserveArmed = (arg == "--revert-unless-armed");
      if (::geteuid() != 0) {
        std::fprintf(stderr,
                     "urnetworkd %s: this changes the kernel firewall and must run as root.\n"
                     "  try: sudo %s %s\n",
                     arg.c_str(), SelfPath(argv[0]).c_str(), arg.c_str());
        return 1;
      }
      if (!preserveArmed && ::access(urnw::ControlServer::SocketPath().c_str(), F_OK) == 0) {
        std::fprintf(stderr,
                     "urnetworkd --revert: WARNING — %s still exists, so urnetworkd may still be "
                     "running. It re-installs its own ruleset within seconds of anything removing "
                     "it; stop it first (systemctl stop urnetworkd) or this will not stick.\n",
                     urnw::ControlServer::SocketPath().c_str());
      }
      // Whether the sweep is SUPPOSED to leave a table behind, decided before
      // it runs (it clears the marker on the paths where it does not).
      const bool keepsArmedFloor =
          preserveArmed && ::access(urnw::NetFilter::ArmedMarkerPath(), F_OK) == 0;
      // Idempotent by construction (add-then-delete), so running it on a
      // machine with nothing installed is a successful no-op.
      urnw::NetFilter::SweepStaleState(preserveArmed);
      // CONFIRM IT. SweepStaleState() returns void and only logs, and this is
      // the command a cut-off user is told to trust: reporting a failed sweep
      // as a completed one would send them away from the one thing that was
      // going to fix it. `nft list table` is read-only.
      if (const std::string nft = urnw::FindTool("nft"); !nft.empty() && !keepsArmedFloor) {
        if (urnw::RunCommand({nft, "list", "table", "inet", urnw::kNftTableName}).ok()) {
          std::fprintf(stderr,
                       "urnetworkd %s: FAILED — `table inet %s` is still in the kernel. This "
                       "machine may still be filtered. Try: %s\n",
                       arg.c_str(), urnw::kNftTableName, urnw::NetFilter::RecoveryCommand());
          return 1;
        }
      }
      std::fprintf(stderr, "urnetworkd %s: done%s\n", arg.c_str(),
                   keepsArmedFloor ? " (the kill switch was armed, so the block floor was kept "
                                     "and re-installed)"
                                   : "");
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: urnetworkd [--foreground] [--diagnose] [--revert] [--version]\n"
          "URnetwork privileged daemon: control socket at %s,\n"
          "device RPC on 127.0.0.1:%d while the tunnel is up.\n"
          "  --diagnose             print the host preflight (ip/nft/resolvectl, cgroup, tun),\n"
          "                         the authorization mode, the kill-switch state and the\n"
          "                         recovery steps, then exit\n"
          "  --revert               lift the URnetwork firewall table, policy rules and capture\n"
          "                         routes, and clear the armed marker; then exit. Run this when\n"
          "                         a machine is stuck blocked. Requires root.\n"
          "  --revert-unless-armed  the same sweep, but a machine that was armed when the daemon\n"
          "                         died stays armed (used by the unit's ExecStopPost)\n"
          "  --foreground           do not send the systemd readiness notification\n",
          urnw::ControlServer::SocketPath().c_str(), urnw::ctl::kDeviceRpcPort);
      PrintRecovery(argv[0]);
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
  //
  // preserveArmed=true is the whole crash story and it is NOT the default of
  // the parameter: calling SweepStaleState() bare (which is what this line used
  // to do) deletes the table unconditionally, so a daemon that was SIGKILLed
  // with the kill switch ARMED came back up and OPENED the machine — the one
  // moment the floor must be continuous. With the intent passed explicitly, the
  // stale table is REPLACED by a fresh armed one in a single atomic `nft -f`
  // when (and only when) the /run marker says this machine was armed when we
  // died. A reboot clears the marker with the rest of /run, so nothing comes up
  // blocked before a user has asked for anything.
  const bool sweptArmedFloor = urnw::NetFilter::SweepStaleState(/*preserveArmed=*/true);
  if (sweptArmedFloor) {
    std::fprintf(stderr,
                 "[daemon] this machine is BLOCKED by a kill-switch floor carried over from a "
                 "daemon that died while armed. Turn the kill switch off in the app, or:\n%s\n",
                 urnw::NetFilter::RecoveryHelpText().c_str());
  }
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
  // A floor carried over from a crash has an OWNER from here on: status tells
  // the truth, the reaper re-installs it if it is flushed, and shutdown tears
  // it down. Adopting after construction (not inside it) keeps the sweep and
  // the host independent of each other's ordering.
  if (sweptArmedFloor) tunnel.AdoptArmedFloor();
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
  // Into the log ring as well as the journal, once per start: the ring is what
  // the GUI's log tail shows, and this is the line that has to be in front of a
  // user whose machine is blocked. Cheap insurance against the one failure mode
  // that has no other way out.
  // auth=… is in the READY line, not only in ControlServer's own start
  // breadcrumb, because it is the first thing to look at when a user reports
  // "it asked me for a password" or "it says I am not allowed": the answer is
  // always which authority this daemon latched at start, and the log ring is
  // the surface the GUI can show them.
  urnw::DaemonLogf(
      "[daemon] urnetworkd %s ready (state=%s, auth=%s). If this machine ends up blocked with "
      "no way to reach the daemon: stop the service, then run `%s --revert` (firewall half "
      "alone: %s)\n",
      UR_APP_VERSION, stateDir.c_str(), server.AuthModeName(), SelfPath(argv[0]).c_str(),
      urnw::NetFilter::RecoveryCommand());
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  return 0;
}
