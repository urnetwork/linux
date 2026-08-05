# URnetwork Linux App — Port Plan

Port of the URnetwork desktop client to a native Ubuntu 24.04 LTS+ app
(GNOME/Wayland, amd64 + arm64) at `linux/app`. **Like macOS and Windows, Linux
uses a privileged daemon + RPC split** [decided 2026-08-05, was single-process]:
an unprivileged GUI holds `DeviceRemote` and ships as a **user AppImage**, while
a root `urnetworkd` holds `DeviceLocal` and pumps packets through a
`/dev/net/tun` fd via the SDK's existing `IoLoop`, and ships as a **`.deb` or a
one-line `install.sh` tarball**. It keeps the macOS-style menu-bar/tray desktop
UX. Normative interface: `MIGRATION.md`. Build checklist: `APPIMAGE.md` §11.

> The 22.04 floor is gone with the single-process model: 22.04 ships no
> `libgtkmm-4.0-0` at all, and GeoClue 2.5.7 there can never support the
> location override (needs ≥ 2.7.0). **24.04 is the floor.**

Stack decisions marked **[verified]** come from a cited deep-research pass
(2026-07-09; primary sources: GNOME/GTK repos, packages.ubuntu.com, snapcraft
docs, snapd source, Mullvad/Tailscale/Proton). **[judgment]** = engineering
recommendation. **[unverified]** = the research found no surviving evidence —
these are the load-bearing follow-ups.

> ### Status — read this before trusting the rest of the document
>
> **Implementation (verified 2026-08-05):** the app is **built and committed** —
> 20 C++ sources under `linux/app/src/`, 12 commits, first `54867c3` (2026-07-11),
> latest `6b55633` (2026-08-02). The Go tree (`internal/`, `cmd/`, `go.mod`) has
> been **deleted**; nothing Go remains in this repo. Any statement below that the
> app is Go, unwritten, or uncommitted is stale. `NEXTSTEPS.md`'s "not yet
> compiled" is likewise stale.
>
> **Language: C++17 (gtkmm-4.0 + libadwaita C API), NOT gotk4/Go** [decided
> 2026-07-10]. gotk4's memory-leak/maintenance risk (R1) outweighed its
> no-C-boundary benefit, and the cgo `urnetwork_sdk.hpp` wrapper already existed,
> letting the C++ app share SDK-host logic with the WinUI Windows app.
>
> **Packaging: AppImage, NOT Snap** [decided 2026-08-05]. `app/snap/snapcraft.yaml`
> and the snapcraft-container release path still exist and still produce a snap;
> they are superseded until the migration lands. **See `APPIMAGE.md` — it is the
> current packaging document and supersedes every Snap statement here.**
>
> **⚠️ The AppImage decision invalidated this plan's central premise.** §1's
> "crux" was that the snap `network-control` interface granted the confined
> process `/dev/net/tun` + `CAP_NET_ADMIN`, which is what made single-process
> viable with no privileged helper. An AppImage gets no such grant and cannot
> carry file capabilities. **The privilege model is now RESOLVED: a root
> `urnetworkd` daemon + RPC, with the GUI unprivileged** (decided 2026-08-05).
> See §1 for the current architecture, `MIGRATION.md` for the normative
> interface, and `APPIMAGE.md` §11 for the build checklist. Everything elsewhere
> in this document about snap confinement is history, not the design.
>
> **v1 scope: FULL macOS parity** — sign in, sign up, connect (all controls), and
> provide. See M2/M3.

## 1. Architecture (daemon split — the Apple/Windows model)

*(Was "single process — the Android model". Changed 2026-08-05 with the move off
Snap; `MIGRATION.md` holds the normative interface.)*

```
+-- unprivileged, desktop user ------+   +-- root, systemd ------------------+
| urnetwork  (GUI, AppImage)         |   | urnetworkd  (deb | install.sh)   |
|                                    |   |   no GTK dependency at all       |
|  GTK4 + libadwaita (gtkmm-4.0      |   |                                  |
|    + adw_* C API), C++17           |   |  libURnetworkSdk.so:             |
|        | cgo C ABI:                |   |    DeviceLocal(enable_rpc=true)  |
|        |   urnetwork_sdk.hpp       |   |        | in-process fd loop      |
|  DeviceRemote  ====================|===|=> device rpc (loopback + mTLS)   |
|  control client  ------------------|---|-> unix sock, SO_PEERCRED, 0660   |
|                                    |   |  urnet::newIoLoop(device, tunFd) |
|  never needs root                  |   |        <--> /dev/net/tun         |
|  degrades cleanly with no daemon   |   |  routes (0.0.0.0/1+128.0.0.0/1)  |
|                                    |   |  DNS (resolvectl), SO_MARK (R4)  |
|                                    |   |  GeoClue /etc/geolocation write  |
+------------------------------------+   +----------------------------------+
```

The app links the **c-shared build of the Go SDK** and talks to it through the
generated `urnetwork_sdk.hpp` wrapper (`app/src/SdkHost.hpp` includes it):
objects cross as `uint64_t` handles, compound values as JSON parsed with
nlohmann.

The SDK-binding mechanics above are unchanged by the split — both processes link
the same wrapper; only which `Device` each one creates differs.

**Current architecture (decided 2026-08-05, being implemented):** an unprivileged
GUI process holding **`DeviceRemote`**, and a root `urnetworkd` holding
`DeviceLocal(enable_rpc=true)` + the tun fd + `urnet::newIoLoop(device, fd, cb)`,
joined by a `SO_PEERCRED`-authorized unix control socket plus the device RPC.
This is the **Apple and Windows shape** — `apple/…/DeviceManager.swift:822`
creates `SdkNewDeviceRemoteWithDefaults` in the app while
`apple/app/extension/PacketTunnelProvider.swift:234` creates
`SdkNewDeviceLocalWithMemoryTarget` in the tunnel process, and Windows ships the
same split as its `urnetworkd` service. Linux is now converging on it rather
than being the one platform that differs.

> **Why the single-process model ended.** It was justified *solely* by the snap
> `network-control` interface granting the *confined process itself*
> `/dev/net/tun` rw, `CAP_NET_ADMIN` and netns (snapd `network_control.go`) —
> which is why no privileged helper was needed, unlike every commercial Linux VPN
> studied (Mullvad, Tailscale), all of which use a root daemon + RPC. Dropping
> Snap removed that grant, and an AppImage cannot replace it: it is unconfined
> but privileged by nothing, cannot carry file capabilities (it is a single
> mounted file), and cannot install a polkit policy (root-only directories).
>
> **Two independent reasons the daemon is the right answer, not merely the
> remaining one.** First, `app/src/Tunnel.cpp` opens `/dev/net/tun`, issues
> `TUNSETIFF`, and shells out to `ip` and `resolvectl` — none of which works as
> an ordinary user process. Second, and load-bearing regardless of packaging:
> **`connect/egress_other.go` is a no-op on Linux** (macOS network extensions
> bypass their own tunnel automatically, Android uses `VpnService.protect`;
> Linux has neither), so with `Tunnel.cpp`'s 31 capture prefixes the SDK's own
> provider sockets route back into the tunnel. Fixing that needs `SO_MARK`,
> which `socket(7)` gates behind `CAP_NET_ADMIN`. See R4 in §5.

## 2. Stack decisions

- **UI: GTK4 + libadwaita in C++ (gtkmm4 + libadwaita C API), via the cgo
  `urnetwork_sdk.hpp` wrapper.** [DECIDED 2026-07-10 — was gotk4/Go]
  Rationale: gotk4 (the earlier pick) is single-maintainer, autogenerated, and
  its own README warns of memory leaks/crashes — a real liability for a multi-day
  tray/VPN process (this was R1). Its only advantage was "no C boundary," but the
  cgo C++ wrapper already exists and is verified (the Windows app uses it; it
  exposes the unix-only `IoLoop` for the fd data plane), so that advantage is
  moot. GTK4 + gtkmm-4.0 is battle-tested official GNOME. **Bonus:** the C++ app consumes the
  SAME wrapper as the WinUI Windows app, so the SDK-facing host logic (auth,
  connect, provide control) is **shared C++ across Windows and Linux** — only the
  UI toolkit and the tun/network layer differ per platform.
  - **libadwaita from C++**: gtkmm wraps GTK4; the libadwaita C++ bindings
    (libadwaitamm) are young, so use gtkmm4 for GTK and call the libadwaita **C
    API** (`adw_*`) directly for Adw widgets (mixing gtkmm + C GObject is fine).
  - **⚠️ CORRECTION 2026-08-05 — the distro floor is Ubuntu 24.04, not 22.04.**
    An earlier version of this bullet claimed "22.04 ships gtkmm 4.6, 24.04 ships
    4.14". **Both figures were wrong.** Verified against packages.ubuntu.com:
    **22.04 ships no `gtkmm-4.0`, no `glibmm-2.68` and no `libsigc++-3.0` at all**
    (the C++ bindings first appeared in Ubuntu 23.10); 24.04 ships gtkmm **4.10**
    (4.14 is the GTK version, not gtkmm's). Independently, `src/` already calls
    `gtk_file_dialog_*`, which is **GTK ≥ 4.10**, so the app cannot build or run
    against 22.04's GTK 4.6 regardless. 22.04 would additionally need GLib ≥ 2.79
    (it has 2.72) and pango ≥ 1.52 for modern GTK4. **Supporting 22.04 means
    bundling GLib → pango → harfbuzz, a documented cascade of breakages
    (`APPIMAGE.md` §4). Don't: 22.04 standard support ends May 2027.**
  - **Cost:** the app can only be built on a Linux box with the GTK toolchain
    (no macOS cross-verify, same as the Windows C++ app); the tun + IoLoop data
    plane is already proven (container test) and calls the same IoLoop via the
    wrapper, so that confidence carries over. The earlier Go core
    (`internal/vpn` + `cmd/urnetworkd`) was **deleted**, not retired — no Go
    remains in this repo.
- **Tray/status: StatusNotifierItem (SNI/AppIndicator) via the Canonical GNOME
  Shell extension.** [verified] GNOME removed the legacy XEmbed systray; SNI is
  the D-Bus successor, works under Wayland, and is what Proton/Mullvad/Nord use.
  The `gnome-shell-extension-appindicator` is **preinstalled on Ubuntu** (a
  refuted claim wrongly said it needs manual install — on Ubuntu it's default).
  **As built** (`app/src/Tray.cpp`): SNI + `com.canonical.dbusmenu` spoken
  directly over raw GDBus — deliberately not libappindicator/ayatana, which pull
  in GTK3. **Risk R2**: on non-Ubuntu vanilla GNOME the
  extension isn't default; a manual enable needs logout/login (Wayland can't
  hot-restart the shell). Ubuntu (our target) is covered.
- **Data plane: in-process `/dev/net/tun` + SDK `IoLoop`.** The app opens
  `/dev/net/tun`, creates the tun interface, and hands the fd to
  `urnet::newIoLoop(device, fd, cb)`. Same fd path as Android; reuses the SDK
  unchanged. Userspace-networking (gVisor, no kernel tun) exists as a fallback
  but is proxy-only, not a system data plane — kernel tun is the right path.
  **The privilege to do this is no longer granted** — see §1's historical note
  and `APPIMAGE.md` §1.
- **Packaging: AppImage** [decided 2026-08-05, supersedes the Snap plan below].
  See `APPIMAGE.md` for the full migration surface: privilege model, GTK
  bundling vs host GTK, desktop integration (`.desktop`, the `urnetwork://`
  scheme — **without which SSO sign-in and wallet connect break**), autostart,
  auto-update (no store to do it for us), glibc baseline, and the two
  already-identified relocation bugs (`UR_LOCALEDIR`, icon themes).
  - *Superseded:* strict-confinement Snap on core22/core24 with the
    `network-control` interface, Proton VPN as precedent, and **R3** (that
    `network-control` does not auto-connect and needs a store-review grant).
    `app/snap/snapcraft.yaml` still exists and still builds; keep or delete it
    per the "is Snap a secondary channel?" decision in `APPIMAGE.md` §8.
- **Privilege for a non-confined package:** now the **v1 problem**, not a `.deb`
  footnote. `setcap cap_net_admin+ep` cannot apply to a binary inside an
  AppImage (single mounted squashfs, typically `nosuid`), so the realistic
  options are a polkit/pkexec helper, a separately-installed setcap'd helper, or
  a root daemon + IPC. Ranked in `APPIMAGE.md` §1.

## 3. Component mapping

| macOS / Android | Linux |
|---|---|
| `MenuBarExtra` + 4 icons | SNI/AppIndicator tray, 4 state icons (reuse the generated art) |
| Main window (Connect/Account/Leaderboard/Support) | libadwaita `AdwApplicationWindow` + `AdwViewSwitcher` |
| `DeviceManager` → NetworkSpace/Api/LocalState + **DeviceRemote** | **`DeviceRemote` in the GUI** (as Windows), `DeviceLocal` in the root daemon |
| macOS extension `PacketTunnelProvider` / Android `IoLoop` | `sdk.NewIoLoop(deviceLocal, tunFd)` **in the daemon** |
| `VPNManager`/`NETunnelProviderManager` (start/stop) | daemon: open tun, `IoLoop`, set routes/DNS |
| Windows `urnetworkd` service + RPC | **same shape** — `urnetworkd` + control socket + device RPC |
| Auth (email/pw/verify, guest, code; Google browser) | same, via `Api` directly; `urnetwork://` OAuth via xdg-desktop-portal |
| StoreKit / Stripe | Stripe checkout via `xdg-open` browser |
| Storage (per-process dir) | `$XDG_DATA_HOME/urnetwork` → NetworkSpaceManager |
| Logs | `$XDG_STATE_HOME/urnetwork/logs` → `sdk.SetLogDir` (daemon: `/var/log`) |
| Split tunnel (Win driver / Android per-app) | cgroup v2 + SO_MARK/nftables, **in the daemon** (needs `CAP_NET_ADMIN`) |
| — *(no macOS/Windows analogue)* | **egress self-exclusion**: `SO_MARK` + policy rules — see R4 |
| Notifications | `xdg-desktop-portal` Notification / libnotify |
| Launch at login | XDG autostart `.desktop` or systemd user service |

## 4. Milestones

- **M0 — headless tunnel, one process, no GUI.** *(A throwaway proving step, not
  the architecture — M1 splits it into the shipping daemon. The "Go program" here
  is historical: the app became C++ on 2026-07-10, so this is now simply
  `urnetworkd` run in the foreground.)* Build NetworkSpace, create
  `DeviceLocal`, open `/dev/net/tun`, wire `newIoLoop`, set the default route +
  DNS, and browse through a provider from a CLI. Run under `sudo` first; the
  systemd unit and the control socket land in M1. Proves the data plane on Linux
  — and per `NEXTSTEPS.md` **the app has never been compiled**, so nothing
  downstream can be trusted until this passes on a real Linux box.
- **M1 — split the daemon out.** *(Replaces the former "snap confinement"
  milestone, which is void — see the banner at the top of this document.)* Move
  `SdkHost::StartTunnel` (`app/src/SdkHost.cpp:486`) into a root `urnetworkd`
  systemd service holding `DeviceLocal` + the tun fd + `IoLoop`; the GUI keeps
  `DeviceRemote`. This is the **Windows shape**, and it is cheap: `DeviceRemote`
  is already in the C++ wrapper (`sdk/cgo/include/urnetwork_sdk.hpp:9463`) and
  the Windows GUI already uses it. Deliverables: control socket at
  `/run/urnetwork/` (dir `0750 root:urnetwork`, socket `0660`, `SO_PEERCRED`
  uid check on every `accept`, **never** pid-based — see `APPIMAGE.md` §10c);
  `.deb` with `dh_installsystemd`; **and the egress self-exclusion fix (R4)**,
  which this milestone exists to make possible. Decide here whether the device
  RPC needs a unix transport (it is loopback-TCP-only today,
  `sdk/device_rpc.go:109`) — a root daemon binding a port every local user can
  reach makes control-channel authz the entire boundary.
- **M2 — GTK4 UI + tray + full auth.** gotk4 + libadwaita window, SNI tray
  (4-state icon + menu), and the FULL auth surface: sign in (email/pw+verify,
  guest, auth-code), **sign up (create network + verify)**, reset password, and
  **Google + Apple via system-browser OAuth** (`urnetwork://` callback via the
  portal). gotk4 stability soak (R1) — decide Go vs gtkmm fallback here.
- **M3 — full feature parity (connect + provide + account).** ALL connect
  controls: the location/provider picker (REST `findLocations`/provider lists +
  the `ConnectGrid`), connect/best-available/selected-location, and the detail
  sheets (contracts, in-tunnel split/block rules, DNS, throughput). **PROVIDE**:
  provide toggle + control mode (never/always/auto/manual) + network mode via
  `ProvideViewController` (in-process DeviceLocal provides directly — simplest of
  the three platforms). Account/Wallet (connect wallet, payout, balance codes,
  reliability, points) + Stripe upgrade, Leaderboard, Support, settings, blocked
  locations, `urnetwork://` OAuth via portal, notifications, autostart, light/dark.
- **M3.5 — split tunneling.** cgroup v2 + SO_MARK/nftables (Mullvad-style), in
  the daemon. Confinement is no longer a variable; `CAP_NET_ADMIN` is held.
- **M4 — distribution.** *(Replaces "Snap Store submission", which is void.)*
  amd64+arm64 packages plus a **signed static apt repo** — `apt-ftparchive` +
  `gpg --clearsign` + object storage behind a CDN, which is what Tailscale ships —
  **plus** the native daemon path for distros without a `.deb` — a **`.tar.gz`**
  installed *and upgraded* by one line
  (`curl -fsSL …/urnetwork-daemon.tar.gz | tar xz && sudo urnetwork-daemon/install.sh`,
  idempotent, refuses to run where dpkg owns the install) — and the GUI AppImage
  with its self-hosted zsync channel.
  Requirements, all from `APPIMAGE.md` §10b/§10c: `Signed-By:` a **dearmored**
  keyring in `/usr/share/keyrings` (never `/etc/apt/trusted.gpg.d`, which would
  trust our key for *all* repositories); a modern key, because apt 3.2 in 26.04
  verifies via Sequoia and `apt-key` is gone; `Acquire-By-Hash: yes`. **No longer
  blocked** — GUI packaging resolved 2026-08-05 to a single unprivileged AppImage
  (`APPIMAGE.md` §8 item 0), so this milestone ships three artifacts: the daemon
  `.deb`, the daemon `install.sh`, and the GUI AppImage with its zsync channel.

## 5. Risks

- **R1 — RESOLVED 2026-07-10 by choosing C++/gtkmm over gotk4.** gotk4's
  documented memory leaks/crashes made it unsuitable for a multi-day tray
  process; the UI is now C++ (gtkmm4 + libadwaita) via the mature GTK stack and
  the cgo wrapper. The residual risk moves to standard C++/gtkmm ergonomics +
  the libadwaita-from-C++ (C API) approach — low, well-trodden.
- **R2 — tray depends on the AppIndicator extension**: default on Ubuntu, not
  vanilla GNOME; manual enable needs logout/login. Document for other distros.
- **R3 — RESOLVED (void).** Was "snap network-control does not auto-connect".
  There is no snap and no store review.
- **R4 — egress self-exclusion is MISSING on Linux** [confirmed 2026-08-05, was
  "full data plane inside strict confinement"]. This replaced the old R4 and is
  **more serious than the risk it replaces, because it is not a risk — it is a
  known defect**. `connect/egress_other.go` is a **no-op**: its own comment
  explains macOS network extensions bypass their tunnel automatically and Android
  uses `VpnService.protect`/`addDisallowedApplication`. **Linux has neither.**
  With `Tunnel.cpp`'s 31 capture prefixes installed, the SDK's own provider
  sockets route back into the tunnel. Only `egress_windows.go` implements this
  (via `IP_UNICAST_IF`). Fix: `SO_MARK` + policy rules
  (`ip rule … suppress_prefixlength 0`) plus a rule-restore watchdog — the shape
  wg-quick, Mullvad and Tailscale all ship. `socket(7)` requires `CAP_NET_ADMIN`
  (or `CAP_NET_RAW` since 5.17) to set `SO_MARK`, **which is the single strongest
  architectural argument for the root daemon, independent of packaging.**
- **R5 — per-link DNS is lost on `systemd-resolved` restart** [confirmed;
  replaces "DNS inside a snap"]. The approach itself is **validated**:
  `Tunnel.cpp:98-105`'s three `resolvectl` calls are the same three operations,
  in the same order, Mullvad issues over D-Bus, and NM ≥ 1.26.6 provably does not
  clobber per-link DNS on an unmanaged link. Two real gaps: (a) nothing re-applies
  after resolved restarts — Tailscale watches D-Bus `NameOwnerChanged` on
  `org.freedesktop.resolve1` and re-pushes; (b) `~.` has zero labels, so any
  DHCP-supplied search domain (`lan`, `home`) is a **longer** match and wins,
  meaning `~.` alone does not capture everything.
- **R6 — NetworkManager will manage our tun once it is up** [confirmed; replaces
  "split tunnelling inside a snap"]. `nm-device.c`'s
  `_dev_unmanaged_is_external_down` protects an externally-created device **only
  while the link is down**, and `Tunnel.cpp:68` brings it up. Mark it unmanaged
  via `[keyfile] unmanaged-devices=` (authoritative, cannot be overruled)
  **before** `ip link set up`. ⚠️ Ubuntu ships
  `/usr/lib/NetworkManager/conf.d/10-globally-managed-devices.conf` which would
  already cover us, but something on Ubuntu Desktop evidently shadows it and
  **which component is not established** — assume shadowed and ship our own.
- **R7 — desktop integration** [largely resolved]: the `.deb` gets it for free.
  Debian Policy §9.6 — `.desktop` files in `/usr/share/applications` are
  refreshed by **dpkg triggers**, no dependency or maintainer script needed; §9.7.1
  covers `x-scheme-handler/urnetwork`. ⚠️ Triggers fire only for files **dpkg**
  installs, so never symlink one in from `postinst` (IVPN's latent bug). Residual:
  `main.cpp:67`'s app id `network.ur.urnetwork` does not match
  `urnetwork.desktop`; harmless today, but D-Bus activation requires them to match.
- **R8 — GeoClue ≥ 2.7.0 is required for the location override** [confirmed]:
  Ubuntu 22.04 (2.5.7) and Debian 12 (2.6.0) can **never** support it. Those users
  see "setup required" permanently. See `sdk/PROVIDERLOCATIONS.md`.
- **R9 — toolchain floor is not self-verifying** [open]: `sdk/cgo/Makefile:21`
  pins `gnu.2.35`, but zig issue #25415 (open) shows `-target x86_64-linux-gnu`
  emitting a `hypot@GLIBC_2.35` reference on a 2.34 host. libstdc++ is sharper
  still (22.04 `GLIBCXX_3.4.30` vs 24.04 `3.4.33`) — link
  `-static-libstdc++ -static-libgcc`. **Add a `readelf --dyn-syms` CI gate** on
  the daemon *and* `libURnetworkSdk.so`; it is the only thing that proves the floor.

## 6. Open questions

*(The four former questions were all Snap-premised and are void: there is no snap,
so confinement does not gate the data plane, split tunnelling, or desktop
integration, and "is single-process proven on Linux desktop" is moot now that the
answer is a root daemon — which is what every shipping Linux VPN uses.)*

1. **RESOLVED 2026-08-05 — GUI ships as one unprivileged user AppImage**, daemon as
   a `.deb` **or** a native `install.sh`. The research recommended a GUI
   `.deb`+`.rpm` (`APPIMAGE.md` §10); the decision went the other way, and the
   `install.sh` daemon dissolves its strongest objection by letting the single
   GUI artifact reach every distro the daemon does. Build per `APPIMAGE.md` §11.
   The GTK4 AppDir remains a live engineering risk with a kill criterion (§11e),
   and that fallback does not disturb anything else here.
2. **How much lives in the daemon?** Full data plane (the Windows `urnetworkd`
   model, and what M1 above assumes) versus a thin privilege broker with the data
   plane staying in the GUI. M1 assumes the former; confirm before building.
3. **Does the device RPC need a unix-socket transport?** It is loopback-TCP-only
   today (`sdk/device_rpc.go:109`). This is an SDK change, so it needs deciding
   before M1 hardens the control plane.
4. **Multi-user policy.** Tailscale gives non-owners read-only; NordVPN uses
   `0660 root:<group>`; Mullvad ships `0766` with no auth at all and documents it
   as outside their threat model. Pick deliberately rather than inheriting a
   default — and note `systemd.socket(5)`'s `SocketMode=` default is **0666**.

## 7. SDK reuse

- **No SDK changes needed for the data plane**: `IoLoop` is already `!windows`
  and fd-based (Android's path); the sdk package builds for linux. The app calls
  the exported Go API directly (like Android's gomobile surface, but in-process
  Go rather than JNI).
- The Linux app IS a C++ consumer of the cgo `.so` + `urnetwork_sdk.hpp` (same
  wrapper as Windows), so `run.sh` building `URnetworkSdkLinux.zip` is now load-
  bearing for the app (not just a third-party artifact). The wrapper exposes the
  unix-only `IoLoop` for the fd data plane. SDK-host logic is shared C++ with the
  Windows app; only the UI toolkit + tun/network layer differ per platform.
- Reuse the generated tray icon art (`windows/app/tools/make-icons.py` source →
  PNG for SNI; SNI takes themed PNG/named icons rather than .ico).
- **Egress self-exclusion IS needed — this bullet previously said the opposite and
  was wrong.** [corrected 2026-08-05] It claimed single-process made it moot; it
  never did, and the daemon split makes that reasoning void anyway.
  `connect/egress_other.go` is a **no-op on Linux**: its own comment records that
  macOS network extensions bypass their tunnel automatically and Android uses
  `VpnService.protect`/`addDisallowedApplication`, and **Linux has neither**. Only
  `egress_windows.go` implements it (`IP_UNICAST_IF`). With `Tunnel.cpp`'s 31
  capture prefixes the SDK's own provider sockets route back into the tunnel. The
  fix is `SO_MARK` + `ip rule … suppress_prefixlength 0` with a rule-restore
  watchdog — what wg-quick, Mullvad and Tailscale all ship — and `socket(7)` gates
  `SO_MARK` behind `CAP_NET_ADMIN`, so it lives in the daemon. **Tracked as R4**,
  and it is the strongest architectural argument for the daemon independent of
  packaging.
