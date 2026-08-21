# Linux packaging: the move to AppImage

Status: **direction SETTLED 2026-08-05, not implemented.** The shipping Linux target is
a **privilege split**:

> ### The GUI ships as ONE unprivileged user AppImage.
> ### The privileged daemon ships as a `.deb` **or** a native `install.sh`.

The daemon gets distro-native packaging where we have it (`.deb`, later `.rpm`) and a
distro-agnostic **`.tar.gz` + `install.sh`** — static binary + systemd unit —
everywhere else, installed *and upgraded* by one line:

```sh
curl -fsSL https://get.ur.network/urnetwork-daemon.tar.gz | tar xz && sudo urnetwork-daemon/install.sh
```

The GUI is a single AppImage for every distro, run as the desktop user with **no
privilege of any kind**. Details in §11; the tarball path is §11g.

`PLAN.md`, `app/snap/snapcraft.yaml`, `build.sh` and `build/all/build-linux.sh` still
describe and produce a **Snap** — treat those as superseded intent until this
migration lands.

**What this split resolves, and what it does not.** Read with §1 and §4:

| Problem | Resolved by the split? |
|---|---|
| **Privilege for tun/routes/DNS** (§1) | **Yes.** The daemon is root via systemd, so it opens `/dev/net/tun`, sets routes and DNS directly. This is what every other Linux VPN ships. |
| **polkit unusable inside an AppImage** (§1a) | **Moot.** Nothing privileged runs from inside the AppImage any more. |
| **`urnetwork://` scheme + autostart** (§5) | **Probably** — the `.deb` can install the `.desktop`, icon and MIME handler on the AppImage's behalf, with dpkg triggers running `update-desktop-database`. This turns the AppImage's worst weakness into a solved problem. Needs confirming, incl. a stable `Exec=` path. |
| **Single-process architecture** | **No — it ends.** This is the real cost: the app becomes daemon + RPC, like macOS and Windows. Mitigating factor: **we already have that architecture and its RPC** (`DeviceRemote`), so this is convergence, not new design. |
| **GTK4/libadwaita bundling** (§4) | **No.** The GUI still carries the whole GTK stack, so the 24.04 floor and host-GTK recommendation still stand. |
| **Auto-update for the GUI** (§5) | **Partly.** The daemon updates via apt. The GUI AppImage still needs its own path, and the updater's write-a-new-file behaviour fights a fixed `.deb`-installed `Exec=`. |
| **Signing/trust** (§5) | **Partly.** The `.deb` gets apt's signed-repo trust; the AppImage keeps the weak story. |

**The NetworkManager `owner=<uid>` idea is now largely moot for privilege** — it was
the escape hatch for having *no* daemon. With a root daemon, NM matters for a
different reason: **coexistence**. On Ubuntu Desktop NM manages interfaces and DNS, so
the daemon must not fight it over the tun device, per-link DNS, or the default-route
split. That is now the live NM question.

> ## Decision resolved 2026-08-05. The evidence in §10 stands; read §11 to build it.
>
> Five research streams landed and argued against the AppImage GUI (§10). The decision
> went the other way, and **the deciding objection genuinely dissolves under the chosen
> shape**: the research's strongest surviving point was "disjoint audiences" — an
> AppImage serves distros we do not package for, but those users would have no daemon.
> Adding the native `install.sh` daemon removes that gap, so one GUI artifact now
> covers every distro the daemon reaches. This is the configuration §10d itself named
> as coherent.
>
> **What was never in dispute:** the daemon + RPC split. §10b validates it
> unreservedly, Apple ships exactly it (`DeviceManager.swift:822` creates
> `SdkNewDeviceRemoteWithDefaults` in the app; `extension/PacketTunnelProvider.swift:234`
> creates `SdkNewDeviceLocalWithMemoryTarget` in the tunnel process), and Windows ships
> it as `urnetworkd`. Packaging format and process architecture are independent
> decisions; only the former was ever contested.
>
> **What remains live as risk, not objection:** bundling GTK4 + libadwaita into a
> working AppDir (§4). That is an engineering problem with a real failure precedent,
> and §11 gives it a kill criterion.

> Rationale for the move is not recorded here — add it, because it decides the
> trade-offs below (notably whether the Snap Store remains a secondary channel,
> which would keep `snapcraft.yaml` alive rather than delete it).

This document is the migration surface: what the snap was silently providing, what
breaks without it, and what must be decided before building.

---

## 1. The crux: the tun data plane loses its privilege grant

This is the blocking item. Everything else is work; this is a design decision.

The single-process, no-daemon architecture (`PLAN.md` §2) rests on one snap feature.
`app/snap/snapcraft.yaml:37`:

```yaml
      - network-control    # /dev/net/tun + CAP_NET_ADMIN + netns (the crux)
```

The app takes that grant literally. `app/src/Tunnel.cpp:32` opens `/dev/net/tun`
directly and `:43` issues `TUNSETIFF`; routing and DNS shell out to `ip` and
`resolvectl` (`:98-111`). There is no privilege escalation anywhere in the tree —
the confined process simply *was* privileged.

**An AppImage gets none of this.** It is unconfined, but unconfined is not
privileged: an ordinary user process cannot open `/dev/net/tun` for a new
interface, cannot `TUNSETIFF`, and cannot alter routes. Nor can the usual
workaround apply — file capabilities cannot be attached to a binary inside an
AppImage, because the AppImage is a single squashfs file mounted (typically
`nosuid,nodev`); `setcap` on the outer file does nothing for the binary within.

So the privilege model must be re-chosen. The realistic options:

### 1a. Researched: **polkit does not work from inside an AppImage**

This is not a preference — it fails for three independent structural reasons:

1. **polkit only reads `.policy` files from root-owned directories**
   (`/etc/`, `/run/`, `/usr/local/share/`, `/usr/share/` + `polkit-1/actions/`).
   An AppImage installs nothing, so it has no action to authorize against.
2. **The random mount path defeats action matching.** `pkexec` resolves the target
   with `realpath()` and compares it to the action's
   `org.freedesktop.policykit.exec.path`. The AppImage type-2 runtime mounts at
   `mkdtemp(".mount_<name>XXXXXX")` — a **different path every launch**.
3. **root cannot read the FUSE mount.** By default FUSE mounts are inaccessible to
   other users *and to root*, so the elevated helper cannot even be executed from
   inside the image.

**The precedent proves it.** AirVPN's Eddie is the only VPN shipping an AppImage of
the privileged app, and it had to give up on running from the image at all — its
`AppRun` does `--appimage-extract` into a temp dir on **every launch**. Their own
readme records why: *"the app starts from the AppImage FUSE mount, but elevated
cannot access `/tmp/.mount_*/…/eddie-cli-elevated` (Permission denied). AppRun
therefore uses extract-and-run."* Eddie also prompts for a password on **every run**,
because it skipped the root install step that gives everyone else a silent path.

**No VPN ships a self-contained AppImage that performs its own tunnel.** Surveyed:
Mullvad (deb/rpm only — *"ANY other distribution method is an unofficial third party
distribution"*; an AppImage request sat open 2019→2025 and shipped nothing),
Tailscale, IVPN, Windscribe (AppImage requested, never delivered), Cloudflare WARP,
NordVPN, Proton VPN, Mozilla VPN. **Every one uses a root systemd daemon installed
from a package**, reached over a unix socket gated by socket group, `SO_PEERCRED`
uid check, or a polkit `.rules` that silently returns YES for an admin group. The
one-time root install is exactly what buys "no prompt on every launch".

### 1b. Revised options

| Option | Shape | Verdict |
|---|---|---|
| **NM-created tun with `owner=<uid>`** | one privileged step creates a *persistent* tun owned by the user (`nmcli con add type tun … owner <uid>`, or `ip tuntap add … user <uid>`); the unprivileged app then opens `/dev/net/tun` + `TUNSETIFF` with **zero capabilities** | **Most promising** — the kernel explicitly supports this: *"If you want to create persistent devices and give ownership of them to unprivileged users…"* (`tuntap.rst`). Preserves the single-process design. **But routes/DNS still need privilege, and NM also *manages* the interface, which may fight our userspace pump — needs a spike.** |
| **systemd root daemon + IPC** | what every shipping Linux VPN does | Works, well-trodden, but needs a real installer and ends the single-process model |
| **setcap'd helper installed separately** | `cap_net_admin+ep` on a helper on the host filesystem | Works; still an install step |
| **polkit/pkexec helper from inside the AppImage** | — | **Ruled out** — see §1a |
| **NM VPN plugin** | register a plugin so NM runs the tunnel | **Ruled out** — plugins are `.name` files in libnm's root-owned `vpnservicedir`, and NM dispatches only to registered service names. Also assumes WireGuard/OpenVPN/IPsec, not a bespoke pump |

### 1c. Flatpak is only marginally better, and for one specific reason

Flatpak cannot grant `CAP_NET_ADMIN` either. Both official VPN Flatpaks
(`com.protonvpn.www`, `org.mozilla.vpn`) punch exactly one hole —
`--system-talk-name=org.freedesktop.NetworkManager` — and **delegate the tunnel to
host NetworkManager**; Mozilla's Flatpak build swaps out its own root daemon to do
so. That only helps if NM already speaks your protocol. A bespoke userspace tun pump
hits the same wall, **unless** combined with the `owner=<uid>` tun trick plus
`--device=all`. There is no official Mullvad, NordVPN or Tailscale Flatpak.

### 1c-bis. The framing that matters most

**The single-process design was never a general Linux design — it was a snapd-specific
one.** snapd can grant real kernel capabilities because strict confinement is a policy
layer over a normal host process; its AppArmor profile literally contains
`/dev/net/tun rw`, `capability net_admin`, `capability net_raw`. **Nothing outside
snapd does that.** Not AppImage. Not Flatpak — bubblewrap sets `PR_SET_NO_NEW_PRIVS`
and Flatpak has no capability-granting permission at all (`--device=all` grants device
*nodes*, not `CAP_NET_ADMIN`). Not a bare binary without `setcap`.

So **decide the privilege architecture first and the packaging second.** Leaving snap
breaks the single-process premise no matter what we move to.

Useful precision on what privilege is actually needed — one part is easier than
assumed:

- **Opening `/dev/net/tun` is not the barrier.** systemd ships it mode **0666**.
- **Creating the interface is.** The kernel: *"CAP_NET_ADMIN is required for creating
  network devices **or for connecting to network devices which aren't owned by the
  user in question**."* That second clause is the whole loophole behind the
  `owner=<uid>` route — a device you own needs no capability to attach to.
- **Routes/DNS still need `CAP_NET_ADMIN`**, and **`SO_MARK`** (split tunnel, egress
  self-exclusion) needs it too. The `owner` trick does not cover these.

### 1d. What this means for the packaging decision

The evidence says **AppImage fights this app category**: the format's defining
property (a self-contained file that installs nothing) is exactly what removes every
mechanism for obtaining privilege. The realistic shapes are therefore:

- **AppImage GUI + separately-installed privileged component** — i.e. the user still
  runs an installer, so the AppImage buys much less than it appears to; or
- **`owner=<uid>` tun** — the only route that keeps a genuinely install-free app, and
  only if the routes/DNS half can also be solved and NM can be kept from fighting the
  interface; or
- **revisit the format** — `.deb`/`.rpm` (what every competitor ships) or Flatpak.

This is worth weighing against whatever motivated the move to AppImage — which is
still unrecorded at the top of this document.

**Consequence to weigh honestly:** the appeal of AppImage is "download and run, no
install". A VPN needs privileged network setup, which needs *something* installed
or a privilege prompt. The migration therefore either (a) accepts a first-run
polkit prompt and an installed helper, or (b) reconsiders whether AppImage is the
right vehicle for this particular app. Decide this before writing packaging.

The rest of this document assumes the app still runs from an AppImage and that
privilege is solved by one of the above.

---

## 2. What else the snap was providing

`app/snap/snapcraft.yaml` is short, which hides how much it did. Each line below is
a thing that must now be handled explicitly.

| Snap feature | What it gave | AppImage replacement |
|---|---|---|
| `extensions: [gnome]` | GTK4 + libadwaita runtime, **Adwaita icon theme**, GTK themes, portals, wayland/x11 glue | bundle, or depend on the host's GTK4 (see §4) |
| `plugs: [network-control]` | `/dev/net/tun`, `CAP_NET_ADMIN`, netns | **nothing** — see §1 |
| `plugs: [desktop]` | session bus access for the SNI tray | free (unconfined) |
| `plugs: [browser-support]` | opening the system browser for OAuth/Stripe | free (unconfined) |
| `desktop: usr/share/applications/urnetwork.desktop` | snapd installed the `.desktop` on the host **and** exported the `urnetwork://` scheme handler via xdg-mime | must be done by the app or an integration step (see §3) |
| Snap Store | transactional auto-update, signing, distribution | zsync/AppImageUpdate + own hosting + GPG (see §5) |
| `base: core24` | fixed, known runtime ABI | pinned by the *build* host's glibc/GTK instead (see §4) |

---

## 3. Two relocation bugs already in the tree

An AppImage mounts its payload at a random path (`/tmp/.mount_XXXXXX/usr/...`), so
any **compile-time absolute path** silently resolves against the host instead of
the bundle. Two instances exist today.

**a) Translations will silently fall back to English.** `app/src/main.cpp:48` calls
`bindtextdomain(GETTEXT_PACKAGE, UR_LOCALEDIR)`, where `UR_LOCALEDIR` is baked at
build time from `get_option('prefix') / get_option('localedir')`
(`app/meson.build:24`) — i.e. `/usr/share/locale` for the release build. Inside an
AppImage the catalogs live at `$APPDIR/usr/share/locale`, so gettext reads the
*host's* `/usr/share/locale` and finds no `urnetwork` domain. Every string falls
back to the msgid. Nothing crashes, nothing logs — the app just becomes
English-only, which is exactly the kind of defect that survives to release.

*Fix:* resolve the locale dir at runtime relative to the executable (`$APPDIR` when
set, else `/proc/self/exe` → `../share/locale`), and keep the compiled constant as
the fallback for a distro/FHS install.

**b) Symbolic icons depend on a host icon theme.** The UI uses named icons
throughout (`go-previous-symbolic`, `emblem-ok-symbolic`, `dialog-warning-symbolic`,
… — see `app/src/AuthViews.cpp:29`, `app/src/ConnectDrawer.cpp:100`, and ~15 more).
The snap's `gnome` extension guaranteed the Adwaita icon theme was present. On a
bare host — or a non-GNOME desktop — these resolve to nothing and the UI shows
blank or missing icons.

*Fix:* bundle the icon theme subset (and set `XDG_DATA_DIRS`), or ship the handful
of icons as app resources.

Anything else compiled from `prefix` is suspect for the same reason. The good news:
`app/meson.build:96` already uses `install_rpath : '$ORIGIN/../lib'`, which is
relocatable and needs no change — the `libURnetworkSdk.so` will be found correctly.

---

## 4. Runtime strategy: bundling GTK4/libadwaita is the second hard problem

Researched. Short version: **the standard tooling does not support GTK4 +
libadwaita, and actively breaks it.**

**`linuxdeploy-plugin-gtk` is not an option as-is.**
- Its GTK4 branch is a stub (~8 lines vs ~25 for GTK3) and carries a dead-variable
  bug: the rpath-fixup loop reads GTK3-only variables, so under `DEPLOY_GTK_VERSION=4`
  it runs `find "" …` and **no GTK4 module gets its rpath fixed or its deps deployed**.
- It exports `GTK_THEME=Adwaita:dark|light`, which **overrides libadwaita's own
  theming** — the plugin's issue #60 reports the app rendering correctly *without*
  the plugin and wrongly *with* it.
- It exports `GDK_BACKEND=x11` unconditionally, putting every user on XWayland — on
  a backend GTK has deprecated for removal in GTK5.
- Last functional commit **2023-10-01**; the maintainer: *"I did not invest time in
  GTK 4.x, because I lost interest with GTK."* (linuxdeploy *core* is maintained; the
  GTK plugin is not.)

**The precedent is discouraging.** Gaphor — GTK4 + libadwaita, the closest analogue
to this app — **deleted its AppImage in 2023**, citing *"every month some issue with
the build needs to be fixed"*, under 0.2% of installs, a breakage that went unnoticed
for releases, and *"The GNOME project provides zero support for creating AppImages,
the entire ecosystem is setup for Flatpak."* GNOME's own documentation names Flatpak
as its preferred distribution framework and does not mention AppImage.

**GTK4 makes the Mesa rule bite harder.** The official excludelist forbids bundling
libGL/libEGL/libdrm/Mesa; GTK4's GSK renderer is GPU-based by default (Cairo is
explicitly last-resort), so graphics-stack mismatches surface as libadwaita apps
aborting inside `gsk_gl_renderer_render` where GTK3 was fine. There is also a genuine
contradiction in the official guidance: "build on the oldest distro" produces a
bundled older `libstdc++`, which then prevents the **host's** Mesa/libLLVM from
resolving `GLIBCXX_*` and breaks every DRI driver.

**Our optional `webkitgtk-6.0` is a serious risk.** WebKitGTK is multi-process and
hardcodes **absolute paths to its helper executables at compile time**; relocating it
into an AppDir breaks it, and the `WEBKIT_EXEC_PATH` escape only works if WebKitGTK
was built with developer mode — distro builds are not. No AppImage tool handles it.
Since we use it only for the upgrade sheet's embedded Stripe checkout, it must be
**host-provided and dlopen'd, degrading cleanly when absent** — never on the startup
path. (Meson already has it as `required: false`.)

**The one demonstrably working path** is pkgforge's `sharun` + `uruntime` "bundle
everything" model, which explicitly rejects the excludelist and **bundles Mesa**
(pointing `VK_DRIVER_FILES`/`__EGL_VENDOR_LIBRARY_DIRS` into the bundle). It has a
working GTK4 demo, needs no FUSE, but wants an Arch build host and means taking the
opposite side of the ecosystem's sharpest unresolved disagreement.

**Options, ranked:**
1. **Flatpak** — what GNOME supports, what comparable apps ship, and (per §1c) the
   only format with a sanctioned hole to host NetworkManager.
2. **sharun + uruntime**, bundling everything including Mesa.
3. **Hand-rolled AppRun over linuxdeploy core, no GTK plugin** — then we owe by hand:
   GSettings schemas (note GTK4 ids are `org.gtk.gtk4.*`; a missing schema is a fatal
   abort), pixbuf loaders + patched cache, `gio-querymodules`, the Adwaita icon theme
   (neither mainstream tool ships one for GTK4), fontconfig overrides, and a
   `GSK_RENDERER=cairo` fallback probe. **One invariant:** bundle the whole GLib
   family *with* its GIO modules, or none of it — never split.
4. **Require host GTK4/libadwaita** — smallest artifact, but sets a distro floor and
   fails confusingly where libadwaita is absent.

**If linuxdeploy's GTK plugin is used anyway**, at minimum post-process its generated
hook to delete the `GTK_THEME` and `GDK_BACKEND` exports; those two lines alone make
a libadwaita app look wrong and run on XWayland for every user.

---

## 5. Desktop integration and auto-update: now the app's problem

**`.desktop` + URL scheme.** `app/packaging/urnetwork.desktop` already declares
`MimeType=x-scheme-handler/urnetwork;` — and its own comment records the mechanism
that is going away:

> `# The snap `desktop` extension exports this handler to the host via xdg-mime.`

`urnetwork://` carries OAuth/SSO callbacks and Solana wallet deep links, so if the
scheme is not registered, **sign-in via SSO and wallet connect break**. An AppImage
installs nothing by default, and **neither integration helper is available on a stock
Ubuntu**: `appimaged` has been **archived since 2020**, and AppImageLauncher is still
beta and **not in the Ubuntu archive at all**. So the app must integrate itself.

**The Ubuntu-specific trap that makes this non-negotiable:** Ubuntu's Firefox is a
snap, and snapd's userd only hands a URL scheme to the host if it is on a fixed
allowlist (`http`, `https`, `mailto`, `snap`, `zoommtg`, …) — `urnetwork` is not.
It then falls back to `xdg-mime query default x-scheme-handler/urnetwork`, and allows
the scheme **only if that resolves to a real desktop file**. So the OAuth callback
works if and only if a `.desktop` is installed *and* `update-desktop-database` has
been run. Otherwise the user gets a silent *"Supplied URL scheme is not allowed"*.
There is no ephemeral alternative — an AppImageKit request for temporary scheme
handlers was closed as not planned.

> ### ⚠️ `install.sh` must run `update-desktop-database` and `gtk-update-icon-cache` by hand
>
> **This is the one place the `.deb` genuinely gets something for free that the
> native path does not** (§11g). Debian Policy §9.6 means a `.deb` needs no
> dependency and no maintainer script: `desktop-file-utils` and
> `hicolor-icon-theme` ship `interest-noawait` **dpkg file triggers** on
> `/usr/share/applications` and `/usr/share/icons/hicolor`, so dpkg refreshes both
> caches automatically. **Those triggers fire only for files dpkg itself installs.**
> A tarball `install.sh` writing the same files triggers nothing.
>
> **And the bug hides on a Debian/Ubuntu dev box, which is why it is easy to ship.**
> Three compounding reasons:
>
> 1. **It looks like it worked.** Desktop environments enumerate
>    `/usr/share/applications` directly, so the launcher entry and icon appear
>    regardless. What `update-desktop-database` actually rebuilds is
>    `mimeinfo.cache`, which is what `x-scheme-handler/*` lookups consult — so the
>    app shows up in the menu while `urnetwork://` silently fails to resolve.
> 2. **The visible half of the feature is fine.** Launching the app works. Only the
>    OAuth/SSO callback and wallet deep links break, and only for a user who gets
>    that far.
> 3. **Any later `apt install` silently repairs it.** The next package that touches
>    `/usr/share/applications` fires `desktop-file-utils`' trigger, which reruns
>    `update-desktop-database` over the whole directory — including *our* entry. So
>    on a machine that installs packages regularly the fault heals itself and
>    disappears from testing, while persisting on a user's box that never installs
>    anything else.
>
> **Test it the way §10e item 7 specifies** — `xdg-mime query default
> x-scheme-handler/urnetwork` must resolve, and a `urnetwork://` link clicked in
> **snap Firefox** must reach the app — on a machine where the tarball path is the
> *only* thing that has been installed. `uninstall.sh` must rerun both commands too,
> or a removed app keeps claiming the scheme.

First-run integration must therefore write
`~/.local/share/applications/urnetwork.desktop` with **`Exec="$APPIMAGE" %u`** (the
runtime sets `$APPIMAGE` to the real path — never `$APPDIR` or `/proc/self/exe`,
which are the ephemeral `/tmp/.mount_*` path), install the icon, run
`update-desktop-database`, and write `~/.config/autostart/`. **Re-run all of it after
every update**, because the updater writes a new filename. Getting this wrong is a
documented, recurring bug in other apps (autostart entries pointing at a dead
`/tmp/.mount_*` path).

**Autostart.** A tray VPN is expected to start at login. The snap could rely on
desktop session handling; an AppImage must write `~/.config/autostart/*.desktop`
itself, again referencing a stable path.

**Auto-update — measured, and worse than expected.** The Snap Store updated the app
transactionally for free. The AppImage equivalent (`AppImageUpdate` + zsync) has
never had a stable release — every tag is `2.0.0-alpha-1-<date>` — and:

- **Deltas barely help.** Measured on two consecutive releases of a comparable app:
  **~34% block reuse**, i.e. a point release still downloads two-thirds of the file.
  Cause: the payload is a *compressed* squashfs, so a small change shifts everything
  after it. **For us it is worse** — our 38.7 MB payload is a Go c-shared library that
  Go relays out with fresh build IDs every build, so expect ~0% reuse there.
- **GitHub Releases breaks zsync.** Probed: single-range requests return 206, but the
  **multi-range requests zsync needs return HTTP 501 "Unsupported client range"**.
  The update transport `appimagetool` auto-selects under GitHub Actions is the one
  that fails. Self-hosting on nginx works.
- **It writes a NEW file by default**, so every `.desktop` `Exec=` and autostart entry
  pointing at the old filename breaks after an update.
- **No version ordering** — it compares hashes, so a mispointed `.zsync` silently
  *downgrades* users. A live instance of exactly this was found in a shipping app.
- Of sixteen AppImage apps surveyed, **three** embed update info; most simply don't
  auto-update.

**Signing — a real trust regression for a VPN.** `appimagetool --sign` embeds the
**signer's public key inside the AppImage**, and the validator imports that key *from
the file it is validating*. "Valid signature" therefore means only *"signed by
whoever's key is in this file"* — an attacker who rebuilds and re-signs validates
perfectly. There is no external keyring, no revocation, and **nothing verifies
signatures by default** (the runtime's `--appimage-signature` only *prints* them).
Moving a privacy product from a reviewed, identity-bound, revocable store to an
unsigned-by-default file is adversarially quotable. If we ship this: sign, publish
detached `.asc` for the AppImage **and** the `.zsync`, publish the fingerprint
out-of-band, and never rotate silently.

---

## 6. Build pipeline

Today: `linux/build.sh` → `build/all/build-linux.sh` → cgo SDK cross-built with zig,
then snaps packaged in the **Canonical snapcraft rock** Docker container, producing
`*.snap` for amd64 + arm64 (`build/BUILD-PLATFORMS.md` documents the host/toolchain
matrix).

Changes needed:
- Replace the snapcraft container step with an AppDir assembly + `appimagetool`
  (or `linuxdeploy`) step, per arch. The cgo SDK cross-build via zig is unaffected.
- The build container becomes the ABI floor (see §4) rather than `core24`.
- `build/BUILD-PLATFORMS.md` output column changes from `*.snap` to `*.AppImage`.
- `build/all/DESKTOP_BUILD.md` store-submission plumbing for Linux no longer applies;
  distribution becomes file hosting + update metadata.
- arm64 AppImage tooling needs verifying on the M1 build host (the amd64 snap build
  already runs under qemu emulation, so expect a similar arrangement).

Unaffected: the localization step (`localizations` → `app/po/*.po` → `msgfmt`) is
packaging-agnostic — but see §3(a), because *finding* the installed catalogs at
runtime is not.

---

## 7. Side effect: mock location becomes possible

The provider-locations feature (`sdk/PROVIDERLOCATIONS.md`) includes an Android
"sync device location with oldest provider" toggle. On Linux the equivalent would go
through GeoClue, and under strict snap confinement it was effectively out of reach —
a confined app cannot touch `/etc/geoclue/geoclue.conf`.

Unconfined AppImage removes that specific obstacle. Researched — the answers:

- GeoClue has **no setter, no writable property and no plugin interface**, and its
  D-Bus name is owned by root/`geoclue`. Config paths are compile-time constants with
  **no user-level override**. So there are exactly two mechanisms.
- **Static source** (`/etc/geolocation`, live-monitored, enabled by default): a
  **root** write, but it serves every accuracy tier.
- **NMEA over mDNS** (publish `_nmea-0183._tcp` via Avahi, serve synthetic `$GPGGA`):
  **no root needed** — Avahi's bus policy lets any unprivileged process register a
  service — and an HDOP ≤ 1 fix trips GeoClue's priority lock, overriding Wi-Fi/IP for
  30 s. But it **advertises the fake GPS fix to the whole LAN**, misses CITY-only
  consumers such as automatic timezone, and needs `avahi-daemon`.

**This is where §1 comes back.** The research assumed "AppImage ⇒ nothing installed",
which made the root route look unusable (a polkit prompt per provider change is
unacceptable). But §1 already establishes that a VPN AppImage **must** obtain
privilege for `/dev/net/tun` somehow — a polkit helper, a setcap'd helper, or a
daemon. **If that helper exists, the static-source route is nearly free**: the helper
writes `/etc/geolocation` on each provider change with no further prompting, and it
is strictly better behaved than the LAN broadcast (no mDNS, no LAN visibility, works
for CITY-level consumers).

**Therefore: decide §1 first.** The privilege model determines which GeoClue
mechanism is available, so the mock-location decision is downstream of it — not an
independent choice. Full analysis in `sdk/PROVIDERLOCATIONS.md` → "Linux".

Also worth measuring before building either: MLS shut down in 2024 and GeoClue ships
no default Wi-Fi URL, so **IP geolocation is often the only live source** — and it
egresses through the tunnel. GNOME Maps may already follow the VPN exit with no code
at all.

---

## 8. Decisions needed before implementation

> **Read §1 and §4 together before deciding.** The research found AppImage to be a
> poor fit for this app on **two independent axes**, neither of which was known when
> the move was chosen:
>
> - **Privilege (§1):** polkit cannot work from inside an AppImage at all (three
>   structural reasons), and **no VPN ships a self-contained AppImage that runs its
>   own tunnel** — the one vendor that tried had to extract-and-run on every launch
>   and still prompts for a password each time.
> - **GTK4 + libadwaita (§4):** the standard tooling's GTK4 path is an unmaintained
>   stub that actively breaks libadwaita theming and forces XWayland; the closest
>   comparable app (Gaphor, GTK4+libadwaita) **abandoned its AppImage**; GNOME
>   supports only Flatpak.
>
> **Correction on Flatpak:** it is *not* a privilege escape either — bubblewrap sets
> `PR_SET_NO_NEW_PRIVS` and Flatpak has no capability-granting permission. It scores
> better only because (a) it is GNOME's supported path for GTK, and (b) it sanctions
> a host-D-Bus hole to NetworkManager, which is exactly how Proton VPN and Mozilla
> VPN ship. **Only snapd could grant `CAP_NET_ADMIN` directly** — which is why the
> single-process design is snap-specific and breaks under *any* other format.
>
> **The two configurations that actually cohere:**
> 1. **`.deb` + apt repo — root systemd daemon + unprivileged GUI over a socket.**
>    What Mullvad, IVPN, Windscribe, NordVPN and WARP all ship. It fixes privilege,
>    desktop integration, auto-update and signing **at once**. It reintroduces the
>    daemon+RPC split — but we already have that architecture on macOS and Windows
>    (`DeviceRemote`/RPC), so the marginal cost is lower for us than for most teams.
> 2. **AppImage — but only if the `owner=<uid>` NetworkManager delegation prototypes
>    successfully.** That is the one world where a single unprivileged process is a
>    coherent Linux VPN, and therefore the only world where AppImage's premise holds.
>    Unproven for a bespoke userspace data plane; treat it as a spike with a kill
>    criterion, not a plan.
>
> **Also worth checking:** if the motivation for leaving Snap was the
> `network-control` auto-connect store review (R3) rather than Snap itself, a `.deb`
> sidesteps that without giving up transactional updates or a signed channel. And
> Snap's open unknowns (R4/R5 — do routes and per-link DNS work confined?) apply
> equally to *any* single-process design, including the AppImage one.

0. **RESOLVED 2026-08-05 — AppImage GUI, confirmed.** One unprivileged user AppImage
   for every distro; the privileged daemon ships as a `.deb` **or** a native
   `install.sh`. The daemon reaching non-Debian systems via `install.sh` is what makes
   the single GUI artifact pay off. Build it per the §11 checklist.
1. **Privilege model for tun** (§1) — **resolved: root daemon.** §10b validates it
   independently of packaging: it is what every shipping Linux VPN does, the marginal
   cost is low (`DeviceRemote` already exists in the C++ wrapper and Windows already
   uses it), and it is the *only* way to fix the Linux egress self-exclusion no-op in
   `connect/egress_other.go`. The `owner=<uid>` persistent-tun route is **dropped** —
   §10c shows it buys nothing once the daemon holds `CAP_NET_ADMIN` and the fd.
2. **Bundle GTK4/libadwaita or require the host's** (§4), and the supported distro
   floor that follows.
3. **Desktop integration** (§5) — self-integrate on first run, or require
   AppImageLauncher, or ship an optional installer script. Decides whether SSO
   sign-in works out of the box.
4. **Auto-update mechanism** (§5) and where artifacts are hosted.
5. **Does the Snap remain a secondary channel?** — **still open, but no longer
   blocking.** Current state as of the 2026-08-05 migration: the release pipeline
   no longer invokes snapcraft, so nothing builds a `.snap`; `app/snap/snapcraft.yaml`
   is **retained with a SUPERSEDED banner** rather than deleted, so the option stays
   open without anyone mistaking it for current. The `network-control` premise is
   already dropped from `PLAN.md`. **If the answer turns out to be yes, that file
   needs rewriting, not reviving** — it describes the single-process architecture,
   and the app binary now holds `DeviceRemote` and cannot open a tun fd.

## 9. Fix list (independent of the decisions above)

- [ ] Runtime-relative `bindtextdomain` (§3a) — silent English-only regression.
- [ ] Icon theme staging or in-app icon resources (§3b).
- [ ] Audit for any other compile-time absolute path resolved at runtime.
- [x] **Done 2026-08-05.** `PLAN.md`'s single-process rationale is corrected: §1's
      diagram and heading now show the daemon split, the "why single-process works in
      a strict snap" passage is reframed as *why the model ended* (the grant was its
      only justification, and an AppImage replaces none of it), and the top banner
      records the privilege model as **resolved** rather than the top open decision.
      The SDK-binding mechanics in that section were left intact — they are still
      accurate. Normative interface now lives in `MIGRATION.md`.

---

## 10. Deep research findings (2026-08-05) — recommendation NOT taken; evidence retained

Five streams, primary sources throughout (upstream man-page XML and source, vendor
repos, distro packaging, live repo metadata).

> **Read this section as evidence, not as the plan.** It recommended shipping the GUI
> as a `.deb`+`.rpm`; the decision went to the AppImage (§8 item 0). It is kept in full
> and unedited because the *facts* are load-bearing for implementation even where the
> *recommendation* was declined — §10c in particular is the constraint list §11 is
> built from. Where a specific objection dissolved under the chosen shape, it is marked
> inline.

### 10a. The case against the hybrid

**1. The load-bearing argument is inverted.** The split's premise is "bundle the hard
part (GTK4) once, package the easy part (daemon) N times." But GTK4 is only hard to
package *as a self-contained bundle*. As a `.deb` with `Depends:` it is the easiest
possible case — the distro already ships it, ABI-stable:

| | 22.04 jammy | 24.04 noble | Debian 13 trixie | 26.04 resolute |
|---|---|---|---|---|
| `libgtkmm-4.0-0` | **absent** | 4.10.0 | 4.18.0 | 4.20.0 |
| `libadwaita-1-0` | 1.1.7 | 1.5.0 | — | 1.9.1 |

Same package name ⇒ same soname across all of them. `PLAN.md` already sets the floor
at **24.04**, which was the only release where bundling was necessary. One GUI `.deb`
built on 24.04 installs on 24.04, 25.10, 26.04 and Debian 13 with a two-line
`Depends:`. The AppImage converts that solved problem back into §4's unsolved one
(broken GTK4 tooling, the Mesa excludelist argument, WebKitGTK's absolute helper
paths).

**2. Zero precedent.** 15 projects checked against actual release artifacts; none
ships a privileged system daemon as a distro package *and* its GUI as an AppImage. The
space sorts into three buckets, and the hybrid is in none:
- *Daemon/GUI split → both halves are distro packages.* **IVPN** is the exact
  structural analogue and the most damning: its apt repo carries exactly two packages,
  `ivpn` and `ivpn-ui`, and **`ivpn-ui` `Depends: ivpn`**. Its 20 most recent GitHub
  releases have empty asset arrays and `electron-builder.config.js` sets
  `linux: { target: "dir" }` — no AppImage target exists. Same shape: **OpenSnitch**
  (root daemon + GUI, `.deb`+`.rpm`), **Proton VPN** (separate daemon/GUI repos, both
  with `debian/` and `rpmbuild/`).
- *Daemon + GUI in one package.* **Mullvad** (one 364 MB `mullvad-vpn` package),
  **Windscribe** (privileged helper rides in the same `.deb`).
- *AppImage ships, but it is the whole app.* AirVPN Eddie, Sunshine, RustDesk.

  The one near-match — OpenRGB — pairs its AppImage with a **static udev rules file**,
  and its docs tell AppImage users to `cp` the file rather than install the `.deb`. A
  config file with no protocol has no version-skew surface at all.

**3. Disjoint audiences.** ~~The AppImage exists to serve distros we do not package
for. Those exact users have no `.deb` from which to install the daemon.~~
**→ DISSOLVED by the 2026-08-05 decision.** This was the strongest surviving objection,
and the chosen shape answers it directly: the daemon also ships as a native
`install.sh` (static binary + systemd unit), so it reaches the same distros the
AppImage does. One GUI artifact then covers every system the daemon runs on, which is
exactly the payoff this objection said was missing.

**4. It invents a version-skew surface no other URnetwork platform has.** Windows
installs `URnetwork.exe` + `urnetworkd.exe` from one MSI; macOS puts the extension
inside the app bundle. Worse, our skew guard is *declared but not implemented*:
`windows/app/src/Common/Protocol.h:27` defines `kProtocolVersion = 1` ("bump when the
wire format changes incompatibly; hello negotiates it"), and a repo-wide grep finds it
**set** in `Service/TunnelController.cpp:241` and **never checked** in
`App/ServiceClient.cpp:42`. Harmless under one MSI; a live bug under split updates.

**5. The `.deb` cannot fix auto-update** (it *can* fix desktop integration — §10b).
**→ Constraint, not blocker.** This one is real and survives, but it only bites if the
AppImage is installed to a root-owned path. §11 resolves it by keeping the AppImage in
a **user-writable** directory with the `.deb`/`install.sh` shipping only a launcher.
`appimageupdatetool -O` replaces the file via `rename()`, which needs write+execute on
the **containing directory** — so an AppImage in root-owned `/opt` **cannot self-update
from an unprivileged GUI**, and `chmod` on the file does not help. The obvious
workaround (`chown` the directory to the user) puts a root-owned-tree executable path
under unprivileged write control — a privilege-escalation shape. Additionally: a
successful self-update diverges from dpkg's record and the next `apt upgrade` silently
overwrites (possibly downgrades) it, and `appimaged` watches `/opt`, producing a
duplicate launcher entry.

**6. The cost saving is ~one `.rpm` build.** `nfpm` emits `.deb` **and** `.rpm` from
one YAML with no `dpkg`/`rpmbuild`, on any Go-supported host — including the existing
macOS build server. It supports maintainer scripts, deb triggers, signatures and file
modes. "N times" is closer to "1.2 times".

**The one genuine, unshared win for the hybrid:** `libfuse2` is not installed by
default on Ubuntu 22.04+, and a `.deb` could simply `Depends: libfuse2t64` where every
AppImage-only vendor must tell users to install it by hand. That is real — and also an
admission that the AppImage is not "download and run" on our target OS.

**Is it still an AppImage?** If the `.deb` installs it to `/opt`, registers its scheme,
owns its autostart template, and it cannot self-update, then appimage.org's three
headline claims ("no need to install", "no system preferences are altered", "without
root rights") are each false. What remains is squashfs-with-a-runtime used as a private
bundling format inside a `.deb` — legitimate, but then use `/opt` + `$ORIGIN` rpath
(which `app/meson.build:96` already does) and drop the runtime, FUSE dep and zsync.

### 10b. What the research validates unreservedly

- **Move to a root daemon.** Independent of packaging. Every shipping Linux VPN does
  it, and the marginal cost is unusually low: `DeviceRemote` is **already in the C++
  wrapper** (`sdk/cgo/include/urnetwork_sdk.hpp:9463`) and the Windows GUI already uses
  it. `SdkHost.cpp:486` (`StartTunnel`) is the clean split point.
- **It is the only way to fix egress self-exclusion.** `connect/egress_other.go` is a
  **no-op on Linux** — its comment explains macOS extensions bypass their own tunnel
  automatically and Android uses `VpnService.protect`; **Linux has neither**. With ~all
  of IPv4 captured by the 31 `/1`-style prefixes, the SDK's own provider sockets route
  back into the tunnel. The fix is `SO_MARK` + policy rules, and `socket(7)` requires
  `CAP_NET_ADMIN`/`CAP_NET_RAW` to set it. wg-quick, Mullvad and Tailscale all do this.
- **The `.deb` genuinely solves desktop integration, automatically.** Debian Policy
  §9.6: `.desktop` files in `/usr/share/applications` are refreshed via **dpkg
  triggers**, so no dependency and no maintainer script is needed;
  `desktop-file-utils` and `hicolor-icon-theme` ship `interest-noawait` triggers.
  Policy §9.7.1 covers `x-scheme-handler/*` for `urnetwork://`. The precedent is exact:
  Debian's **`steam-installer`** is a 60 kB `.deb` owning desktop integration for a
  self-updating payload it does not ship, with `Exec=` pointing at a wrapper in a
  stable path — which `packaging/urnetwork.desktop` already does.
  ⚠️ Triggers fire only for files **dpkg** installs; a symlink created in `postinst`
  does not activate them (IVPN has exactly this latent bug).
- **The current DNS approach is correct.** `Tunnel.cpp:98-105`'s three `resolvectl`
  calls are the same three operations, in the same order, that Mullvad issues over
  D-Bus (`talpid-dns/src/linux/systemd_resolved.rs`: `disable_dot`, `set_domains(".")`,
  `set_dns`). NM ≥ 1.26.6 provably will not clobber per-link DNS on an unmanaged link.
- **`/opt` is legitimate for a vendor `.deb`** (FHS 3.0 §3.13) even though Lintian
  errors on it for *archive* packages. Chrome, Mullvad, Windscribe and IVPN all do it.
- **A signed static apt repo is low-effort**: Tailscale's is a static tree behind
  CloudFront. `apt-ftparchive` + `gpg --clearsign` + `aws s3 sync`, no database.

### 10c. Sharp edges to carry into implementation regardless of the decision

- **NM will manage an externally-created tun once it is up.** `nm-device.c`'s
  `_dev_unmanaged_is_external_down` protects the device **only while the link is
  down**, and `Tunnel.cpp:68` does `ip link set dev … up`. Mark it unmanaged
  (`[keyfile] unmanaged-devices=` is authoritative and cannot be overruled) **before**
  bringing it up. ⚠️ Ubuntu ships
  `/usr/lib/NetworkManager/conf.d/10-globally-managed-devices.conf` with
  `unmanaged-devices=*,except:type:wifi,…`, which would already cover us — but
  something on Ubuntu Desktop evidently shadows it, and **which component is not
  established**. Assume shadowed; ship our own marking.
- **Per-link DNS is lost when `systemd-resolved` restarts.** Tailscale watches D-Bus
  `NameOwnerChanged` on `org.freedesktop.resolve1` and re-pushes. `Tunnel.cpp` sets it
  once. Also: a DHCP-supplied `search` domain (`lan`, `home`) is a *longer* match than
  `~.` and will beat it — `~.` alone does not capture everything.
- **`SO_PEERCRED` on every `accept()`, authorize on uid, never on pid.** polkit's own
  source embeds the CVE-2019-6133 report explaining why pid-based checks are broken.
  Socket at `/run/urnetwork/…` (never abstract — permissions are meaningless there),
  dir `0750 root:urnetwork`, socket `0660`. ⚠️ `systemd.socket(5)`'s `SocketMode=`
  default is **0666**. Field practice varies wildly: Mullvad ships **0766 with no auth
  at all** (documented as outside their threat model), Tailscale 0666 + `SO_PEERCRED`
  uid ladder, NordVPN 0660 `root:nordvpn` + `SO_PEERCRED`, IVPN a **world-readable**
  port/secret file over loopback TCP.
- **Peer-binary verification of an AppImage client is impossible.** Root can
  `readlink /proc/<pid>/exe` but **cannot read the file**: `fs/fuse/dir.c` requires an
  exact uid match against the mount owner, and root ≠ uid 1000. AirVPN Eddie retested
  this on Ubuntu 24.04 on 2026-06-29 and fell back to extract-and-run. It is also
  unsound in principle — the bytes are served by the very process being authenticated.
  **The only identity authenticable over a unix socket is the UID.**
- **The device RPC is loopback-TCP-only today** (`sdk/device_rpc.go:109`,
  `deviceRpcDefaultAddress = "127.0.0.1:12025"`). A root daemon binding a port any
  local user can reach makes control-channel authz the entire boundary. **Adding a unix
  transport to the SDK is the cleaner fix.**
- **Always-running daemon, not socket-activated.** `network-pre.target` is passive; a
  lazily-activated unit cannot participate in pre-network firewalling. All five VPNs
  are always-enabled. Steal Mullvad's `RestartKillSignal=SIGUSR1` (enter lockdown
  before stopping, to prevent leaks on `systemctl restart`).
- **`dh_installsystemd` generates the maintainer scripts** — do not hand-write them,
  and note `--restart-after-upgrade` is the compat-10 default; for a VPN holding a tun
  fd, prefer `--no-restart-after-upgrade`. Debian Policy §9.3.3.1: enabling on install
  is correct, but **start idle** — never bring up a tunnel with nobody authenticated.
- **glibc/libstdc++ floor.** `sdk/cgo/Makefile:21` already targets `gnu.2.35` (= jammy)
  which covers everything above it, but libstdc++ is the sharper edge (22.04 ships
  `GLIBCXX_3.4.30`, 24.04 `3.4.33`) — **link `-static-libstdc++ -static-libgcc`**. And
  do not trust zig blind: issue #25415 (open) has `-target x86_64-linux-gnu` emitting a
  `hypot@GLIBC_2.35` reference on a 2.34 host. **Add a CI gate**: `readelf --dyn-syms`
  asserting max `GLIBC_2.x` ≤ 2.35 on the daemon *and* `libURnetworkSdk.so`.
- **apt gets strict in 26.04, not 24.04.** apt 3.2 uses Sequoia; `apt-key` is gone;
  `Signed-By` must point at a dearmored keyring in `/usr/share/keyrings`, never
  `/etc/apt/trusted.gpg.d` (which would trust our key for *all* repositories).
- **Desktop-file naming**: `main.cpp:67` uses app id `com.bringyour.network` but the
  desktop file is `urnetwork.desktop`. Nothing is broken today
  (`StartupWMClass` covers window association), but D-Bus activation — the clean way to
  deliver a `urnetwork://` URI to a running instance — requires the names to match.
  Rename to `com.bringyour.network.desktop`.

### 10d. The recommended alternative, if the direction changes

Two `.deb`s **and** two `.rpm`s from one `nfpm` config, built on the existing macOS
host plus one Ubuntu 24.04 container:
- `urnetwork-daemon` — root systemd daemon, no GTK; owns the `.desktop`, icons,
  scheme handler, NM `conf.d` marking, autostart template, apt keyring.
- `urnetwork` — the GTK GUI, `Depends: urnetwork-daemon (>= <same version>),
  libgtkmm-4.0-0, libadwaita-1-0`.

This is IVPN's exact shape, and the **versioned dependency is a strictly better answer
to skew than any handshake** — apt refuses to install a mismatched pair. It is also
less work: no AppDir assembly, no GTK4 bundling, no Mesa question, no WebKitGTK
relocation, no FUSE dep, no zsync, no self-signed-key trust regression.
`app/meson.build`'s existing `install_rpath : '$ORIGIN/../lib'` already works for
`/usr/lib/urnetwork/`.

**If a "download and run" artifact for non-Debian distros is still wanted**, the
coherent shape is *not* AppImage-GUI + `.deb`-daemon. It is: `.deb`+`.rpm` for both
halves on Debian/Ubuntu/Fedora, **plus** a distro-agnostic daemon installer (static Go
binary + systemd unit + `install.sh`), **plus** the AppImage GUI on top of that. Those
two pieces at least serve the same user.

### 10e. Must be verified on a real Linux box (ordered)

`NEXTSTEPS.md` records that the app has **never been compiled**, so everything here is
pre-first-build.

1. **Compile it** on Ubuntu 24.04. Nothing else can be trusted until this happens.
2. **NM + tun**: bring the tun up, confirm NM leaves it alone, and determine what
   shadows `10-globally-managed-devices.conf` on Ubuntu Desktop.
3. **DNS end-to-end**: assert `~.` on the tun and not on `wlan0`/`enp*`; restart
   `systemd-resolved` and confirm the config is lost (it will be).
4. **Egress self-exclusion**: confirm whether provider sockets loop; if so implement
   `SO_MARK` + `suppress_prefixlength 0` with a rule-restore watchdog.
5. **Daemon/GUI split spike**: `DeviceRemote` ↔ `DeviceLocal` + `IoLoop` in a root
   daemon; measure whether the SDK needs the unix transport.
6. **glibc/libstdc++ floor gate** via `readelf`, then actually run on the floor distro.
7. **dpkg triggers**: `xdg-mime query default x-scheme-handler/urnetwork` must resolve,
   and a `urnetwork://` link from **snap Firefox** must reach the app (the SSO path).
8. **26.04 apt** against the signed repo — Sequoia rejects weak keys noble accepts.

---

## 11. Implementation checklist for the settled shape

**Target:** unprivileged user AppImage (GUI, `DeviceRemote`) ⟷ privileged daemon
(`DeviceLocal` + tun + `IoLoop`), shipped as a `.deb` **or** a native `install.sh`.
The GUI never needs root, and the daemon never needs a display.

### 11a. The one hard constraint: where the AppImage lives

**Install it to a user-writable path. Never `/opt`, never anything root-owned.**
`appimageupdatetool -O` replaces the file with `rename()`, which requires write **and
execute on the containing directory** — so a root-owned location makes self-update
structurally impossible, and `chmod` on the file does not help because the check is on
the directory. The tempting fix (`chown` the install dir to the user) is strictly
worse: it puts a root-owned-tree executable path under unprivileged write control, so
anything that ever launches it with privilege executes attacker-controlled code.

- [ ] AppImage lives in `~/.local/lib/urnetwork/` (or `~/Applications/`), owned by the
      desktop user. Never packaged as a dpkg-owned file — a self-update would diverge
      from dpkg's record and the next `apt upgrade` would silently overwrite, possibly
      downgrading, and leave `.zs-old`/`.part` debris that survives purge.
- [ ] The privileged package ships **only a launcher** at `/usr/bin/urnetwork` that
      searches `~/.local/lib/urnetwork/`, `~/Applications/`, then a system fallback and
      `exec`s the first hit with `"$@"`. This is the stable `Exec=` target, and
      `packaging/urnetwork.desktop` already assumes it (`Exec=urnetwork %u`).
- [ ] Verify a running instance survives its own replacement (the mounted squashfs
      keeps the old inode through `rename()`; only `cp`/`>` into the live inode
      corrupts it). **Unverified — test it.**
- [ ] Expect `appimaged`, if the user runs it, to add a **duplicate** launcher entry
      for anything under its watched dirs. Decide whether to care.

### 11b. Version skew — implement the check that already exists on paper

The two halves now update on **independent schedules** (apt/install.sh vs. zsync), which
no other URnetwork platform does. The guard is declared and dead: `Protocol.h:27`
defines `kProtocolVersion = 1` ("bump when the wire format changes incompatibly; hello
negotiates it"), `Service/TunnelController.cpp:241` **sets** it, and
`App/ServiceClient.cpp:42` **never checks it**.

- [ ] Put `protocol_version` in the `hello` **request** as well as the reply; the
      daemon refuses anything below `kMinSupportedClientProtocol`.
- [ ] The GUI refuses a daemon below `kMinSupportedDaemonProtocol` and renders
      **"daemon out of date"** and **"daemon unreachable"** as *distinct, actionable*
      states — never as a blank or a zero. Precedent in-tree: the RPC-hosted stats
      render a gray "discovery disabled" rather than stale zeros.
- [ ] Bump `kProtocolVersion` only on wire-format changes, decoupled from app version
      (Eddie's independently-versioned tag; it is at `v1378` while the app is 2.24.6).
- [ ] Fix this **on Windows too** — same dead code path, currently masked by one MSI.

**⚠️ And there is a second, larger skew surface the control-protocol check does not
cover: the device RPC has NO version negotiation at all.** `sdk/device_rpc.go`'s
`DeviceRemoteSyncRequest` carries `InstanceId` — which is *pairing* ("an address can
be reused… reject a sync from a remote built for a different instance"), not
versioning — plus listener id lists. Nothing else.

That was harmless everywhere else because both halves ship in a single artifact (one
MSI, one app bundle, one APK), so the two SDK copies are byte-identical by
construction. **This split makes Linux the first platform where they can drift**: the
GUI AppImage bundles its own `libURnetworkSdk.so` and self-updates via zsync on the
user's schedule, while the daemon's copy updates via apt or `install.sh` on another.

The channel is gob-encoded Go structs, and gob's failure modes are quiet exactly where
it hurts: adding or removing a field is tolerated (unknown fields ignored, missing
fields left zero), **a renamed field silently decodes as zero** — a feature stops
working with no error anywhere — a changed type is a decode error, and a changed
*meaning* is invisible.

- [ ] Carry `sdk_version` in `hello` **both ways** and require an **exact match**,
      refusing before any `DeviceRemote` is constructed. `urnet::version()` already
      exists (`sdk/cgo/include/urnetwork_sdk.hpp:17273`), so neither side needs new
      plumbing to report its own build.
- [ ] Exact match, not a floor: both halves come out of the same release pipeline
      stamped with the same `$EXTERNAL_WARP_VERSION`, so any difference is a genuinely
      mismatched pair and gob offers no compatibility guarantee that would justify
      anything looser.
- [ ] Keep it **independent** of `kControlProtocolVersion` — that one bumps only on
      control-socket wire changes; this one must simply agree. Do not conflate them.
- [ ] Surface "GUI and daemon SDK builds differ" as its own actionable state, distinct
      from "unreachable" and "too old".

### 11c. Socket authorization — UID only

- [ ] Path socket under `/run/urnetwork/`, **never abstract** (`unix(7)`: permissions
      are meaningless for abstract sockets, and anything in the netns can reach them).
- [ ] Directory `0750 root:urnetwork`, socket node `0660`. If declared in a `.socket`
      unit, **set `SocketMode=` explicitly** — the `systemd.socket(5)` default is 0666.
      Prefer `RuntimeDirectory=urnetwork` + `RuntimeDirectoryMode=0750` so systemd
      cleans up on stop.
- [ ] `SO_PEERCRED` on every `accept()`, authorize on **uid**, cache the ucred on the
      connection. Never authorize on pid (polkit's own source embeds the CVE-2019-6133
      report explaining why start-time pid checks are bypassable).
- [ ] **Do not attempt peer-binary attestation.** Root can `readlink
      /proc/<pid>/exe` but *cannot read the file*: `fs/fuse/dir.c` requires an exact
      uid match against the FUSE mount owner, and root ≠ uid 1000. AirVPN Eddie
      retested this on Ubuntu 24.04 on 2026-06-29 and fell back to extract-and-run. It
      is unsound anyway — the bytes are served by the very process being authenticated.
      **UID is the only identity a unix socket can authenticate.**
- [ ] Decide multi-user policy deliberately: Tailscale gives non-owners read-only,
      NordVPN uses `0660 root:<group>`, Mullvad ships `0766` with no auth and documents
      it as outside their threat model.
- [ ] Decide whether the device RPC needs a **unix transport**. It is loopback-TCP-only
      today (`sdk/device_rpc.go:109`, `127.0.0.1:12025`); a root daemon binding a port
      every local user can reach makes the control channel the entire boundary.

### 11d. What the privileged package must carry

- [ ] `Depends: libfuse2t64` — **a genuine win of this shape.** libfuse2 is not
      installed by default on Ubuntu 22.04+, and every AppImage-only vendor has to walk
      users through installing it by hand. We just declare it.
      (`install.sh` must check for it and say so plainly.)
- [ ] Desktop integration, which the package gets essentially free: `.desktop` +
      hicolor icons in `/usr/share/`, refreshed by **dpkg triggers** with no dependency
      and no maintainer script (Debian Policy §9.6), and `x-scheme-handler/urnetwork`
      per §9.7.1. ⚠️ Triggers fire only for files **dpkg installs** — never symlink one
      in from `postinst` (IVPN's latent bug). **`install.sh` must run
      `update-desktop-database` and `gtk-update-icon-cache` itself** — see the §5
      callout for why this one silently passes testing and reaches users broken.
- [ ] Rename the desktop file to `com.bringyour.network.desktop` to match
      `main.cpp:67`'s app id. Harmless today, but D-Bus activation — the clean way to
      hand a `urnetwork://` URI to a running instance — requires the names to match.
- [ ] Autostart via a root-owned **inert template** (`/etc/urnetwork/autostart/`) that
      the GUI symlinks into `~/.config/autostart/` **atomically** (temp + `rename`).
      Windscribe's comment explains why: a remove-then-create leaves a window with no
      autostart entry that becomes permanent if the process is killed during shutdown.
      Use `TryExec=` so a stale entry self-disables; `X-GNOME-Autostart-enabled` is
      GNOME-only legacy and not in the spec.
- [ ] NM marking (`[keyfile] unmanaged-devices=`) + udev rule, applied **before**
      `ip link set up` — see R6 in `PLAN.md`.
- [ ] `dh_installsystemd` generates the maintainer scripts; do not hand-write them.
      Consider `--no-restart-after-upgrade` for a daemon holding a live tun fd, or
      Mullvad's `RestartKillSignal=SIGUSR1` lockdown-before-stop.

### 11e. The remaining real risk: the AppDir itself

This is the part with a failure precedent, so **spike it first and give it a kill
criterion**. Per §4: `linuxdeploy-plugin-gtk`'s GTK4 branch is an unmaintained stub
whose rpath-fixup loop reads GTK3-only variables (so under `DEPLOY_GTK_VERSION=4` no
GTK4 module gets its deps deployed), it exports `GTK_THEME` which overrides
libadwaita's own theming, and it pins `GDK_BACKEND=x11`. Gaphor — GTK4 + libadwaita,
the closest analogue to this app — deleted its AppImage in 2023 over exactly this.

- [ ] Hand-roll the AppDir: GTK4 + libadwaita + GLib family + GIO modules + pixbuf
      loaders + **GSettings schemas** + Adwaita icon theme + fontconfig.
- [ ] `meson.build:59` links `webkitgtk-6.0` optionally, and it carries **absolute
      helper paths** that no tool relocates. Decide early: host-provide and `dlopen`,
      or drop the feature in the AppImage build.
- [ ] Settle the Mesa question (bundle vs. excludelist) — the sharpest unresolved
      argument in the AppImage ecosystem.
- [ ] Fix the two relocation bugs already in the tree (§3): runtime-relative
      `bindtextdomain`, and icon-theme staging.
- [ ] Ship `usr/share/urnetwork/world-110m.json` — the provider-locations globe
      resolves `$APPDIR` + `UR_PKGDATADIR`, then `UR_PKGDATADIR`, then `./assets`; a
      miss silently costs the land layer.
- [ ] **Kill criterion:** if a working GTK4 AppDir is not demonstrable on a non-Ubuntu
      host within the spike's budget, fall back to a GUI `.deb`+`.rpm` (§10d) — the
      RPC split is unaffected either way, so this decision stays reversible to the end.

### 11f. Update channel

- [ ] Daemon: apt from a signed static repo (§10b), or `install.sh --update`.
- [ ] GUI: `appimageupdatetool -O "$APPIMAGE"` against **self-hosted** zsync. Not
      GitHub Releases — it returns HTTP 501 on the multi-range requests zsync needs.
- [ ] Detached `.asc` for both the AppImage and its `.zsync`, fingerprint published
      out of band. `appimagetool --sign` embeds the signer's key in the file it
      validates, which is not a trust story.

### 11g. The `install.sh` tarball — one line to install *or* upgrade

**Shape (decided 2026-08-05):** the native daemon path ships as a **`.tar.gz`** whose
one-line extract-and-run both installs and upgrades:

```sh
curl -fsSL https://get.ur.network/urnetwork-daemon.tar.gz | tar xz && sudo urnetwork-daemon/install.sh
```

Shipping the script *inside* the tarball (rather than the `curl … | sh` idiom that
fetches a script which then downloads a payload) is the better choice here: one fetch,
one TLS session, and the script cannot be served independently of the binaries it
installs — so there is no window where a stale or swapped script runs against a
different payload.

- [ ] **Single top-level directory** `urnetwork-daemon/` in the archive — never a
      tarbomb. The one-liner extracts into `$PWD`, which is the accepted cost of the
      idiom; keeping everything in one directory makes it obvious and removable. Print
      the extracted path at the end so the user can delete it.
- [ ] **The same command installs and upgrades.** `install.sh` is idempotent: detect an
      existing install, compare versions, and do the right thing without a separate
      `--upgrade` flag. Upgrading **must preserve** `/var/lib` state, `/etc` config, and
      the created system group; only binaries, units and integration files are replaced.
- [ ] **Refuse to fight dpkg.** If `dpkg -s urnetwork-daemon` (or rpm equivalent)
      reports an install, **abort with a message pointing at `apt`**. Two package
      managers owning the same paths is the worst failure mode available here, and it
      is silent until an upgrade half-replaces files.
- [ ] **Stop the daemon cleanly before replacing its binary** — it holds a live tun fd.
      Use the same lockdown-before-stop discipline as the `.deb`
      (Mullvad's `RestartKillSignal=SIGUSR1`), and restore the prior state on failure so
      a botched upgrade does not leave the user with no daemon and no tunnel.
- [ ] **Detect arch** (`uname -m` → amd64/arm64) and ship both, or publish per-arch
      tarballs. Fail loudly on anything else rather than installing a binary that
      cannot exec.
- [ ] **Preflight, and say why on failure**: systemd present (`/run/systemd/system`);
      glibc floor met; `/dev/net/tun` available; GeoClue ≥ 2.7.0 if the location
      override is wanted (§7 — Ubuntu 22.04 and Debian 12 can never satisfy it).
- [ ] **Do the desktop integration the dpkg triggers would have done** — run
      `update-desktop-database` **and** `gtk-update-icon-cache` after installing the
      `.desktop`, icons, scheme handler and autostart template, and again from
      `uninstall.sh` (or a removed app keeps claiming `urnetwork://`).
      **This is the one place the `.deb` genuinely gets something for free**, and the
      one most likely to ship broken: the failure is invisible on a dev box because the
      launcher still appears, only the SSO/deep-link path breaks, and any later
      `apt install` silently repairs the cache. **Full explanation and the test that
      actually catches it: the callout in §5.**
- [ ] **Ship `uninstall.sh` alongside** — stop and disable the unit, remove binaries,
      units, integration files, the system group, and `/etc/geolocation` if we wrote it
      (nothing else reverts that; see §7). Prompt before removing state.
- [ ] **Verifiable, not just HTTPS.** Publish a detached signature and a checksum for
      the tarball, with the fingerprint out of band; document a two-line verified
      install for users who want it. A checksum embedded *inside* the tarball proves
      only that the archive is intact, not that it is ours — do not imply otherwise.
- [ ] **`install.sh --update` / a periodic check** is the daemon's update channel where
      there is no apt; it re-fetches this same tarball. Keep it opt-in, and never
      auto-upgrade a daemon holding a live tunnel without the user's say-so.
