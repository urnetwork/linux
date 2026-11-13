# URnetwork for Linux

Native Linux desktop client for amd64 and arm64. It is built from two
programs: a privileged daemon, `urnetworkd`, and an unprivileged GTK4 GUI,
`urnetwork-gui`. Releases are attached to the GitHub release on
[urnetwork/build](https://github.com/urnetwork/build/releases). There is no
store listing. The Snap was dropped on 2026-08-05 in favour of native packages
plus an AppImage (see `APPIMAGE.md`, `MIGRATION.md` and `PLAN.md` §4, M4).

## Architecture

The GUI and the daemon are separate processes, which mirrors the Windows
app's service split. `MIGRATION.md` is the normative contract for paths,
binary names and artifact names.

```
  unprivileged desktop user              root, systemd (urnetworkd.service)
  urnetwork-gui                          urnetworkd
    gtkmm4 + libadwaita, SNI tray          no GUI dependencies
    urnet::DeviceRemote ── device RPC ──▶  urnet::DeviceLocal (rpc=true)
                   (127.0.0.1:12025, mTLS, while the tunnel is up)
    control client ──── unix socket ────▶  control server
                   (/run/urnetwork/control.sock, SO_PEERCRED)
                                           Tunnel: /dev/net/tun, ip routes and
                                             rules, resolvectl DNS, nftables
                                           urnet::newIoLoop(device, tunFd)
                                           GeoClue /etc/geolocation write
```

- The **daemon** is started by systemd (`Type=notify`). It opens and configures
  the tunnel and holds the SDK's `DeviceLocal`. It keeps its own traffic out of
  the tunnel with a cgroup-BPF socket mark plus nftables, and it refuses to
  start a tunnel when that cannot be proven. It depends only on gio/glib,
  nlohmann_json, threads and the SDK. It is built on Ubuntu 22.04, so it runs
  on glibc 2.35 or newer.
- The **GUI** does the sign-in, the UI and the tray. It has no privilege and
  reaches the daemon over the control socket. Tunnel control is authorized by
  polkit (`packaging/polkit/com.bringyour.network.policy`), or by membership in
  the `urnetwork` group on hosts without polkit.
- `/usr/bin/urnetwork` is a launcher script installed by the daemon package.
  It is the `Exec=` target of the desktop entry, the `urnetwork://` scheme
  handler and autostart. It runs the first GUI it finds, in this order:
  `$URNETWORK_APPIMAGE`, `~/.local/lib/urnetwork/URnetwork.AppImage`,
  `~/Applications/URnetwork*.AppImage`, `/usr/lib/urnetwork/URnetwork.AppImage`,
  `urnetwork-gui` on `$PATH`, then the `com.bringyour.network` Flatpak. The
  autostart entry runs it with `--autostart`, which it hands to the GUI as
  `URNETWORK_AUTOSTART=1`: that launch shows only the tray icon, and every
  other launch shows the window.
- One GUI instance runs per session. A launch that finds it running hands
  itself over (the window shows, or the link opens) and exits; a launch that
  finds it quitting waits for it to end, then starts.

## Excluding an app from the tunnel

`urnetwork-exclude <command> [<argument>...]`, installed by the daemon package,
runs a command and everything it starts outside the tunnel: its traffic leaves
through the physical network, and the kill switch does not block it, while
everything else stays in the tunnel. The command runs in the user's systemd
slice `urnetwork-exclude.slice`, and urnetworkd lets that slice out only for the
user who started the tunnel. It needs systemd and the cgroup v2 unified
hierarchy and refuses on other hosts. Start the app this way while it is not
already running: an app that hands its window to an instance that is already
running stays in the tunnel with that instance. On a systemd-resolved host the
command's name lookups still go through the tunnel's DNS, so while the kill
switch blocks the machine after a drop it can connect but not resolve names.

In the app, Settings > Connections > Exclude apps from the VPN lists the
installed apps. Turning one on writes a copy of its launcher, "<App> (outside
VPN)", to `~/.local/share/applications`, which starts the app through
`urnetwork-exclude`; the app's own launcher is unchanged and stays in the
tunnel. The copy handles no file or link types, so opening a link never starts
it. Flatpak and Snap apps are not listed: `flatpak run` and `snap run` move the
app into a systemd scope of their own, out of the slice. The row is hidden
where `urnetwork-exclude` is not installed or the host is not on the cgroup v2
unified hierarchy.

## Stack

**C++17 + GTK4 (gtkmm-4.0) + libadwaita**, on top of the shared **cgo SDK**
(`libURnetworkSdk.so` plus the header-only `urnetwork_sdk.hpp` C++ wrapper).
The Windows app uses the same SDK surface. Build: meson + ninja.

## Features

- Sign in with Google or Apple (in the system browser, returning through
  `urnetwork://`), email or phone and password, a seed phrase, an auth code, a Bittensor or Solana wallet, or an instant (guest)
  account. Also sign-up, verification, password reset, and upgrading a guest
  account.
- Connect: best available or a chosen location or provider, a globe view,
  connection details (contracts, transport, DNS, split rules) and a kill switch.
- Provide, extender share and import (QR), and location override (GeoClue).
- Account, earnings (points, and SN25α claims to a Bittensor wallet) and
  leaderboards, referrals, redeem codes, upgrade (Stripe checkout), support,
  settings and licenses. The Solana payout wallet was removed on 2026-11-13:
  USDC provider payouts ended on 2026-11-12.
- Tray icon (StatusNotifierItem + `com.canonical.dbusmenu` over raw GDBus),
  hide-to-tray, and launch on system startup (a Settings switch that links
  the autostart template into `~/.config/autostart`; a login then shows only
  the tray icon).
- Localized with gettext. `app/po/` is generated from the shared localization
  store and is never edited by hand.

## Layout

| Path | What |
|---|---|
| `app/src/` | GUI sources (`main.cpp`, `MainWindow`, `HomeShell`, the pages and sheets, `SdkHost`, `ControlClient`, `Tray`, …) |
| `app/src/daemon/` | `urnetworkd`: `main.cpp`, `ControlServer`, `TunnelHost`, `DaemonLog`, `HostMemory` |
| `app/src/Tunnel.{hpp,cpp}` | `/dev/net/tun`, routes, nftables and `resolvectl` configuration (used by the daemon) |
| `app/tests/` | pure-logic unit tests (no GTK, no SDK), run by `meson test` |
| `app/meson.build`, `app/meson_options.txt` | build; this is the single source of truth for the installed layout |
| `app/packaging/` | desktop entry (+ `urnetwork://` scheme), AppStream metainfo, icons, systemd unit, launcher, autostart template, NetworkManager and udev files |
| `app/po/` | generated gettext catalogs |
| `app/scripts/fetch-deps.sh` | vendors the cgo SDK into `app/third_party/urnetwork-sdk/<arch>/` |
| `app/snap/snapcraft.yaml` | **superseded**, not built or shipped. Kept only while "Snap as a secondary channel" is an open question |
| `packaging/make-*.sh` | artifact builders: `deb`, `rpm`, `arch`, `install-tarball`, `appimage`, `flatpak` |
| `packaging/{deb,rpm,arch}/` | nfpm configs and maintainer scripts |
| `packaging/tarball/` | `install.sh` / `uninstall.sh` for distros without a native package |
| `packaging/flatpak/` | Flatpak manifest (GUI only) |
| `packaging/polkit/`, `packaging/selinux/` | polkit actions and SELinux policy module |
| `build.sh` | local smoketest of the full release build |
| `docs/` | distro support matrix, testing guides, parity notes |

## Build (local dev, on Linux)

```bash
cd app

# 1. vendor the SDK (from a local zip or SDK_ZIP_URL); produces
#    third_party/urnetwork-sdk/{amd64,arm64}/{libURnetworkSdk.so,urnetwork_sdk.hpp}
scripts/fetch-deps.sh /path/to/URnetworkSdkLinux.zip

# 2. configure, build, test
meson setup build -Dsdk_arch=$(dpkg --print-architecture)
meson compile -C build     # build/urnetwork-gui and build/urnetworkd
meson test -C build
```

For a fast check on macOS or Linux with Python 3 and a C++17 compiler, run
`python3 -B app/tests/Preflight.py` from the repository root. It exercises the
real auth/logout methods, their retired-epoch and asynchronous-logout mutation
controls, crypto payment UI wiring, display/edit boundaries and localization
catalog coverage without GTK or the SDK. `CXX` selects
the compiler. The release harness runs this check after localization generation,
before allocating a version or building artifacts, even when `BUILD_TEST` is
empty. The full Meson suite remains required during the Linux build.

`URnetworkSdkLinux-<version>.zip` is attached to every release on
urnetwork/build. You can also build it from `sdk/cgo` (`make build_linux`).

GUI dev dependencies (Ubuntu 24.04, the oldest Ubuntu that packages GTK4 for
C++): `g++ meson ninja-build pkg-config gettext libgtkmm-4.0-dev
libadwaita-1-dev libsecret-1-dev libzxing-dev libglib2.0-dev
nlohmann-json3-dev`. `libzxing-dev` (zxing-cpp 2.1 or newer) decodes the
extender share QR from an image file; the encoder is vendored in
`app/third_party/qrcodegen`. `libwebkitgtk-6.0-dev` is optional: with it the
upgrade sheet embeds the Stripe checkout, and without it the checkout opens in
the system browser.

The daemon alone builds without any GTK. With the default `-Dgui=auto`, a
machine without gtkmm/libadwaita builds only `urnetworkd`, which needs
`g++ meson ninja-build pkg-config gettext libglib2.0-dev nlohmann-json3-dev`.
Pass `-Dgui=enabled` to make a missing toolkit an error. Other options are in
`app/meson_options.txt` (`app_version`, `glibc_floor`,
`walletconnect_project_id`, and `host_integration=false` for the GUI-only
Flatpak).

The GUI does not run a tunnel itself. To try a dev build, install a released
daemon package (or run `packaging/tarball/install.sh`), then start the GUI
with the vendored SDK on the library path:
`LD_LIBRARY_PATH=third_party/urnetwork-sdk/$(dpkg --print-architecture) ./build/urnetwork-gui`. The GUI reports when the daemon is unreachable or out
of date. The daemon is not on `$PATH`: every package installs it at
`/usr/lib/urnetwork/urnetworkd` (the install tarball uses
`/usr/local/lib/urnetwork/urnetworkd` on an immutable `/usr`). For host checks,
run `/usr/lib/urnetwork/urnetworkd --diagnose` (no root needed; it includes
the IPv4 and IPv6 policy rules, routes and address counts the daemon's own
sockets meet) and `sudo /usr/lib/urnetwork/urnetworkd --selftest-egress`.
`sudo /usr/lib/urnetwork/urnetworkd --revert` lifts the firewall and routes if
a machine is left blocked. The daemon's own messages name the path it runs
from. When the daemon stops a session because its own packets left through
its own tunnel ("left through this daemon's own tunnel"), the kernel log
names those packets, up to 6 a minute:
`sudo journalctl -k | grep urnetwork-egress-witness` shows each one's
addresses, protocol and ports (add `-b -1` after a reboot).

## Release artifacts

`build.sh` runs the same build as the release pipeline
(`build/all/build-linux.sh`, driven by `build/all/run.sh`) on a macOS host.
First the cgo SDK is cross-built with zig. Then, per arch, one Docker image
builds the daemon packages on Ubuntu 22.04 (glibc 2.35 floor) and another
builds the GUI AppImage on 24.04. `build/all/linux/verify.sh` checks the
artifacts in the container. The Flatpak is built afterwards in its own
container (`build/all/linux/build-flatpak.sh`).

Per release (names are normative, see `MIGRATION.md`):

| Artifact | For |
|---|---|
| `urnetwork-daemon_<v>_<amd64\|arm64>.deb` | daemon: Debian, Ubuntu and derivatives |
| `urnetwork-daemon-<v>.<x86_64\|aarch64>.rpm` | daemon: Fedora, RHEL, openSUSE |
| `urnetwork-daemon-<v>-<x86_64\|aarch64>.pkg.tar.zst` | daemon: Arch, CachyOS, EndeavourOS, Manjaro |
| `urnetwork-daemon-<v>-<amd64\|arm64>.install.tar.gz` | daemon: any systemd distro without one of the above (e.g. immutable or read-only `/usr` hosts) |
| `URnetwork-<v>-<amd64\|arm64>.AppImage` | GUI, every distro |
| `URnetwork-<v>-<arch>.flatpak` | GUI, Flatpak. Built for the build host's architecture only (currently arm64) |

You need a daemon package and one GUI. Publishing to an apt/dnf repository is
still a manual follow-up; today everything ships from the GitHub release.

## Updates

The GUI checks for updates itself (`app/src/UpdateChecker.cpp`): thirty seconds
after launch, at most once every six hours, and from the "Check now" button of
Settings > General > "Check for updates" (and the Developer page's), under which
Settings says what the last check came to. Settings > General > "Check for
updates automatically" turns the timed checks off. The check reads the official
[urnetwork/linux releases](https://github.com/urnetwork/linux/releases) (the
nightly `urnetwork/build` releases are never consulted), skips drafts,
prereleases and any release whose code names an instant more than two days
after the release list's own `Date` header (a mistyped or hostile far-future
tag would otherwise outrank every real release), and compares the release
tag `v<version>` with the running version by its release code. It asks the
GitHub API for the list by the repository's numeric id
(`/repositories/1297137671/releases`) and refuses a redirect, so a renamed
repository, or someone registering a name it used to have, cannot change
where updates come from. Requests are anonymous, and GitHub allows 60 an hour
per address: when it refuses one and says when to ask again (`Retry-After`,
or the rate limit's reset), no check is sent before then, manual ones
included, for at most a day, and Settings says until when. While automatic
checks are on and none has reached GitHub for three days, Settings says
"Couldn't check for updates since <date>", since a newer release may be out.

What the app does with a newer release depends on how it was installed:

- **AppImage** — Settings shows "Update available" with an **Install** button.
  Install downloads the own-architecture `URnetwork-<v>-<arch>.AppImage` to a
  temporary file beside the running one, verifies its SHA-256 against the
  digest GitHub publishes for that release asset, makes it executable and
  renames it over the running file (the previous file is kept as
  `<name>.AppImage.bak` until the next launch). **Relaunch** then starts the
  new file. Nothing that fails the digest check is ever installed or kept. If
  the AppImage's folder is not writable (for example a root-owned
  `/usr/lib/urnetwork/URnetwork.AppImage`), the verified file is saved to the
  Downloads folder instead and Settings shows where.
- **deb / rpm / Arch / install tarball / Flatpak** — Settings shows "Update
  available" with the release page and the file to download for this install
  plus its package-manager command (`apt`, `dnf`, `pacman`, `flatpak`, or the
  tarball's `install.sh`). The app never elevates and never runs a package
  manager.
- **Daemon install tarball, from a shell** — `sudo urnetwork-daemon/install.sh
  --update` (run from any extracted tarball) asks the GitHub API for the latest
  stable urnetwork/linux release (by the same repository id, without following
  a redirect), downloads its
  `urnetwork-daemon-<v>-<arch>.install.tar.gz`, verifies the SHA-256 against
  the asset digest GitHub publishes, and runs that tarball's installer. It
  fails with a clear message when there is no stable release or the release
  has no tarball. `--url <url>` (optionally `--sha256 <hex>`) uses a specific
  tarball instead. `packaging/tarball-update.test.sh` covers it.

The daemon package and the GUI update independently; a GUI newer than the
daemon shows the "service out of date" state until the daemon package is
updated. The AppImage carries no embedded zsync update information: GitHub
Releases cannot serve the multi-range requests zsync needs, so there is no
`.zsync` sidecar and `appimageupdatetool` does not apply.

## Supported distros

The daemon needs systemd, glibc 2.35 or newer, cgroup v2, cgroup-BPF,
`nft`, `ip` and `/dev/net/tun`. systemd-resolved is needed for tunnel DNS and
for the kill switch. The released AppImage takes glibc and libstdc++ from the
host and is gated at glibc 2.39 (it is built on Ubuntu 24.04). The tray needs a
desktop with StatusNotifierItem support. The measured host is Bazzite (Fedora Silverblue base). Other
distros are packaged but, per distro, only inferred to work.
`docs/DISTRO-SUPPORT.md` lists every host assumption and how to test it.
`docs/TESTING-CACHYOS.md` is a step-by-step test guide for Arch-based systems.
