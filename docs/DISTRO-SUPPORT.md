# URnetwork for Linux — distribution support matrix

**Purpose.** This project is going universal: AppImage, Flatpak, `.deb`, `.rpm` and
whatever else a Linux desktop actually installs, tested one distro at a time. That
plan only survives contact with reality if we first write down *every assumption the
daemon and the installers make about the host*, and then say — per distro family, and
honestly — which of those assumptions hold, which break, **by what mechanism**, and
what the smallest test is that would settle it.

**This document is not a compatibility claim.** Exactly one column below is measured.
Everything else is inference, and it is labelled as inference, because this project has
been burned repeatedly by confident statements that were never executed (the
`egress_protected = true` that shipped 3.38 Tb of loop; the `--selftest-egress` mode
exists because "it should work" was not good enough).

## How to read the labels

| Label | Means |
|---|---|
| **[M]** | Measured on the owner's Bazzite machine on 2026-08-16, read-only commands only. |
| **[M-src]** | Read straight out of this repo's source. A fact about *our code*, not about any distro. |
| **[M-bin]** | Measured against the built binaries in this tree (`readelf`/`nm`). |
| **[I]** | **Inference.** Believed true from general knowledge of the distro. **Never executed here.** |
| **[?]** | Unknown. The test that settles it is named. |

The one measured column is **Bazzite 44 (Silverblue variant), kernel 7.1.5, SELinux
enforcing, ostree-booted, `/usr` read-only** — daemon installed at
`/usr/local/lib/urnetwork/urnetworkd`, unit at `/etc/systemd/system/urnetworkd.service`,
glibc 2.43 **[M]**. On that machine the VPN works end to end. Nothing below extends
that measurement to any other host.

---

## Part A — The complete host-assumption inventory

Enumerated by reading `app/src/daemon/main.cpp`, `app/src/daemon/TunnelHost.cpp`,
`app/src/daemon/ControlServer.cpp`, `app/src/Tunnel.cpp` / `Tunnel.hpp`,
`app/packaging/urnetworkd.service`, `packaging/tarball/install.sh`,
`packaging/deb/nfpm.yaml` and the maintainer scripts. Every row is a thing that can be
absent or different on a host we have not tried.

### A.1 Hard requirements — the tunnel cannot come up without these

| # | Assumption | Who needs it | What happens when it is absent | Smallest test |
|---|---|---|---|---|
| H1 | **cgroup v2 unified hierarchy, and this process is NOT in the root cgroup** | `SelfCgroupV2()` parses the `0::` line of `/proc/self/cgroup`; a bare `/` is rejected because it would match every process **[M-src]** | `start_tunnel` **refuses**: "this system is not running the cgroup v2 unified hierarchy" (unless `URNETWORK_ALLOW_UNPROTECTED_EGRESS=1`) | `cat /proc/self/cgroup` — must print `0::/something` |
| H2 | **cgroup-BPF**: `BPF_PROG_TYPE_CGROUP_SOCK` attachable at `BPF_CGROUP_INET_SOCK_CREATE`, and the mark must actually land on a fresh socket | `EgressSocketMarker::Attach` — the *primary* egress self-exclusion mechanism. Needs `CONFIG_CGROUP_BPF` + `CONFIG_BPF_SYSCALL`, CAP_BPF (CAP_SYS_ADMIN pre-5.8), CAP_NET_ADMIN, and the LSM must permit `bpf()` **[M-src]** | The nftables belt alone is **not** accepted: `VerifyEgressWitness` fails and the tunnel refuses to start or is torn down **[M-src]** | `sudo urnetworkd --selftest-egress`; exit 0 = works, 1 = does not, 2 = could not be measured. Starts no tunnel, sends no packet, touches no firewall/route/DNS. |
| H3 | **`nft` on PATH** (incl. `/usr/sbin`, `/sbin` — `FindTool` appends them **[M-src]**) and a kernel `nf_tables` that accepts our ruleset | egress self-exclusion, the IPv6 fail-closed floor, the DNS floor, the kill switch | `start_tunnel` **refuses**: "nftables (nft) is not installed" | `command -v nft && nft --version` |
| H4 | nftables features: `inet` family, `type route hook output`, **named counters**, and **`socket cgroupv2 level N "path"`** | `BuildNftRuleset` **[M-src]**. `socket cgroupv2` needs kernel ≥ 5.13 and nftables ≥ 1.0.1 **[I]** | `nft -f` rejects the whole transaction → no floor, no exclusion, no start | `printf 'table inet t { chain c { type filter hook output priority 0; socket cgroupv2 level 1 "init.scope" counter accept } }' \| nft --check -f -` (parses and evaluates, **never commits**) |
| H5 | nft resolves a cgroup path **at load time**, so the path must exist | `CgroupInstallable` guards it — an absent path fails the *entire* transaction, taking the kill switch with it **[M-src]** | Guarded; listed so nobody removes the guard | covered by H4 |
| H6 | **`ip` (iproute2)** | tun MTU/address/up, the 31 capture prefixes in table **51821**, the `ip rule` at pref **32763** | `tun_config_failed` / `missing_tool` | `command -v ip` |
| H7 | **`/dev/net/tun`** (or a loadable `tun` module — one `modprobe tun` is attempted **[M-src]**) | `Tunnel::Open` | `tun_module_missing` / `tun_open_failed` | `ls -l /dev/net/tun` — present, `crw-rw-rw-` on Bazzite **[M]** |
| H8 | The LSM permits root to open `/dev/net/tun`, create raw sockets, and connect to :443 | See A.4 | `tun_permission_denied` — and the message correctly says this is a MAC refusal, not a missing capability **[M-src]** | `sudo urnetworkd --diagnose`, then attempt one connect and read `journalctl -t audit \| grep denied` (SELinux) or `dmesg \| grep -i apparmor` |
| H9 | glibc, dynamically linked | The SDK is cgo/`c-shared`; both binaries `NEEDED libc.so.6` **[M-bin]** | Will not load at all on musl | `ldd --version` |

### A.2 Soft requirements — the tunnel comes up, but degraded (and one of them silently disables the kill switch)

| # | Assumption | Consequence when absent | Smallest test |
|---|---|---|---|
| S1 | **`resolvectl` / systemd-resolved, running** — `Tunnel::ApplyDns` issues `resolvectl dns/domain ~./dnsovertls/flush-caches` and verifies with `resolvectl status <tun>` **[M-src]** | `dns_applied=false`; the daemon reports it honestly. **But**: `RunStart` **refuses the connect outright** when the kill switch is requested and DNS did not land (`kCodeDnsApplyFailed`) **[M-src]**. So on a host without systemd-resolved, **the kill switch cannot be used at all.** Without the switch you connect, but LAN/router DNS goes off-tunnel (public resolvers still ride the tunnel and get DoH-upgraded by the mux). | `command -v resolvectl && systemctl is-active systemd-resolved` — **both**. The binary ships with systemd on several distros while the service is disabled. |
| S2 | `modprobe` | `/dev/net/tun` must already exist | `command -v modprobe` |
| S3 | D-Bus system bus + `org.freedesktop.resolve1` name | `g_bus_watch_name` re-pushes the DNS override when resolved restarts **[M-src]**; without it a resolved restart silently drops our DNS pin (documented as R5) | `busctl status org.freedesktop.resolve1` |
| S4 | **NetworkManager** honours `[keyfile] unmanaged-devices=interface-name:urnet0` (`/etc/NetworkManager/conf.d/95-urnetwork.conf`) and the udev `NM_UNMANAGED=1` rule **[M-src]** | On a host managed by something else (systemd-networkd, netctl, connman, wicked) these two files are inert. Whether that other manager grabs `urnet0` is **[?]** | after connect: `nmcli device status \| grep urnet0` should read `unmanaged`; on non-NM hosts, `networkctl status urnet0` |
| S5 | GeoClue ≥ 2.7.0 + writable `/etc/geolocation` | Optional location-override feature only; the installer says so and keeps going **[M-src]** | `pkg-config --modversion geoclue-2.0` or the distro query the installer already does |
| S6 | `/run` is a tmpfs (armed marker `/run/urnetwork/kill-switch-armed`) | The marker is what makes a crash-restart come back **armed** rather than open; a non-tmpfs `/run` would leak it across reboots | `findmnt -no FSTYPE /run` → `tmpfs` |

### A.3 systemd assumptions

`urnetworkd` is written to be systemd-*native* but not systemd-*linked* (no libsystemd;
`READY=1` is one datagram on `$NOTIFY_SOCKET` **[M-src]**). The **binary** degrades:
`--foreground` skips readiness, and `StateDir()`/`LogDir()` fall back to
`/var/lib/urnetwork` and `/var/log/urnetwork` **[M-src]**. The **safety design does
not** degrade — these four unit directives are load-bearing:

| Directive | Why it is load-bearing |
|---|---|
| `Type=notify` | The control socket exists before the unit is called ready. Under `Type=exec` the readiness code is dead and every client races the socket. |
| `RuntimeDirectoryPreserve=yes` | Keeps `/run/urnetwork/kill-switch-armed` across a restart *and* keeps the directory inode stable so a **Flatpak GUI's bind-mount does not become `/urnetwork//deleted`** — measured, and it presented as a bogus "group membership" failure **[M-src]** |
| `ExecStopPost=-… --revert-unless-armed` | The **only** teardown that runs on SIGKILL / OOM / stop-timeout. nftables state is not process-bound, so without it a killed daemon leaves the machine filtered. |
| `StartLimitIntervalSec=0` | A crash loop that lands in `failed` while the fail-closed table is in the kernel = a machine off the network with no control socket. Restarting forever is strictly safer. |
| **`NoNewPrivileges` deliberately unset** | Setting it breaks the daemon outright on every SELinux-enforcing distro: `nft` is `iptables_exec_t`, `ip` is `ifconfig_exec_t`, both require a domain transition, and NNP forbids it → `execve` EACCES → no ruleset, no routes **[M-src, measured on Bazzite]** |

There is **no polkit anywhere in the daemon** **[M-src]**. Authorization is
`SO_PEERCRED` + `getgrouplist` against the `urnetwork` system group, socket `0660
root:urnetwork` in a `0750` directory. That is the Mullvad/Tailscale model and it is
distro-portable; the cost is that **group membership only applies to new login
sessions**, which both installers warn about **[M-src]**.

### A.4 LSM (this is where distro families genuinely diverge)

- **SELinux (Fedora family).** A daemon installed outside a distribution package
  carries no policy, so systemd runs it in `init_t`, which is forbidden the three
  things a VPN daemon must do. Measured on Bazzite: `avc denied { read write }
  tun_tap_device_t`, `avc denied { name_connect } dest=443 http_port_t`, `avc denied
  { create } rawip_socket` — as **root with CAP_NET_ADMIN**; the MAC layer refuses
  before the capability is consulted **[M-src]**. Fixed by
  `packaging/selinux/urnetwork.te`, which **widens `init_t` for the whole system** —
  an honest, documented cost, and the properly scoped answer (a real `urnetworkd_t`
  domain entered through a labelled `urnetworkd_exec_t`) is still owed.
  Labelling the binary `unconfined_exec_t` was tried and **fails** on Bazzite (no
  unconfined module → `203/EXEC`) **[M-src]** — do not reach for it again.
- **AppArmor (Ubuntu, Debian, openSUSE Tumbleweed).** We ship no profile.
  AppArmor is path-based and default-*unconfined*: a binary with no matching profile
  is not mediated **[I]**. That is the structural reason to expect AppArmor to be a
  **non-event** for the daemon — the opposite of SELinux, where the *absence* of
  policy is what denies you. The live risks are narrower and worth naming instead of
  hand-waving: (a) a distro-shipped profile that attaches by path, (b) AppArmor's
  `bpf` mediation applying if we ever end up confined, (c) Ubuntu 24.04's
  `kernel.apparmor_restrict_unprivileged_userns=1`, which breaks apps that *create
  unprivileged user namespaces* — the daemon is root and creates none, and the GUI is
  GTK4 with no bubblewrap, so the widely-reported "AppImages broke on 24.04" class of
  failure is **expected not to apply to us** **[I]**. All three settle with one boot.
- **No LSM (Arch, most Debian installs in practice).** Nothing to do; the SELinux
  branch of the installer no-ops because `getenforce` is absent **[M-src]**.

### A.5 Firewall coexistence

We install exactly one table, `table inet urnetwork`, with four base chains plus a
verdict-free counter chain **[M-src]**. Coexistence facts worth stating because they
are the reason this is *not* a per-distro risk:

- Every registered base chain runs at every hook. `accept` is terminal only for its
  own chain; **`drop`/`reject` are terminal for the packet**. So no other tool —
  firewalld, ufw, Docker, iptables-nft, iptables-legacy — can turn our drop into an
  accept, whatever its priority **[M-src]**.
- The real hazard is a **full flush**, not a competing rule. Fedora's shipped
  `/etc/sysconfig/nftables.conf` *begins* with `flush ruleset`, so
  `systemctl restart nftables` silently destroys our table **[M-src]**. Mitigation is a
  5 s poll (`NetFilter::Verify` on the reaper) — a bounded 5 s window, not zero.
  **Whether `firewall-cmd --reload` (firewalld's `FlushAllOnReload`) also destroys
  foreign tables is [?]** and is a named test below.
- `iptables-nft` vs `iptables-legacy` is **not** a compatibility axis for us: we never
  call `iptables`, and both backends' rules coexist with ours under the terminal-drop
  rule above **[M-src]**.

### A.6 Filesystem layout

| Assumption | Detail |
|---|---|
| `/usr` writable | The tarball installer **detects** read-only `/usr` (`/run/ostree-booted`, then `findmnt -no OPTIONS --target /usr`, then a write probe) and remaps `/usr/*` → `/usr/local/*` and `/lib/systemd/system` → `/etc/systemd/system`, **rewriting `ExecStart`** to match **[M-src]**. Verified live on Bazzite **[M]**. |
| merged-`/usr` | The `.deb` installs its unit to **`/lib/systemd/system`** **[M-src]**. Fine on Debian/Ubuntu (merged-usr). **Not acceptable for an RPM** — Fedora forbids packaging into the `/lib` symlink; the RPM must use `/usr/lib/systemd/system` (the installer's `map_path` already understands both spellings **[M-src]**). |
| `/run/urnetwork` | `0750 root:urnetwork`, owned by `RuntimeDirectory=` |
| `/var/lib/urnetwork` | `0700`, device identity + SDK storage |

### A.7 Toolchain the *installer* assumes (`packaging/tarball/install.sh` **[M-src]**)

`bash` (not POSIX sh), GNU-ish `install -D`, `od`, `sort -V` (probed — falls open if
absent), `getent`, `mktemp -d`, `find`, `sed -i`, `groupadd|addgroup`,
`usermod|gpasswd`. Hard preflight failures: **not Linux**, **unsupported arch**, **not
root**, **`/run/systemd/system` missing**, **glibc unknown or < 2.35**, **`/dev/net/tun`
missing**, and **dpkg/rpm already own `urnetwork-daemon`**. Soft: GeoClue version,
`checkmodule`/`semodule_package`/`semodule`, `update-desktop-database`,
`gtk-update-icon-cache`, `nmcli`, `udevadm`, `restorecon`.

`--dry-run` and `--prefix` mutate **nothing** and print the whole plan including the
detected layout **[M-src]**. That makes "what would this installer do on distro X" a
**zero-risk** question on any VM.

### A.8 ABI floors — measured, and the declaration is one release too conservative

| Binary | Max `GLIBC_` symbol | `NEEDED` |
|---|---|---|
| `urnetworkd` (jammy build in this tree) | **GLIBC_2.34** **[M-bin]** | `libgio-2.0.so.0`, `libglib-2.0.so.0`, `libURnetworkSdk.so`, `libc.so.6`, `ld-linux-x86-64` **[M-bin]** |
| `libURnetworkSdk.so` | **GLIBC_2.34** **[M-bin]** | `libc.so.6` only **[M-bin]** |

- No `libstdc++` in `NEEDED` — it is statically linked, as the packaging comment
  claims **[M-bin]**.
- The 34 undefined `g_*` symbols are all glib ≤ 2.36 vintage (`g_unix_fd_add` is the
  newest, 2.36) **[M-bin]**, so the glib floor is not a constraint on any target.
- The declared floor is **2.35** (`meson_options.txt`, the `.deb`'s
  `libc6 (>= 2.35)`, the installer's `GLIBC_FLOOR`) but the artifacts only *need*
  **2.34**. **Consequence: RHEL 9 / Rocky 9 / Alma 9 (glibc 2.34) may actually be
  reachable, and it is our own declaration that blocks them** — see D.1.
- The **GUI AppImage has a different floor**: it is built on Ubuntu 24.04 and CI
  explicitly raises `UR_GLIBC_CEILING` for it **[M-src]**. Call it **2.39**. So the
  daemon reaches further down than the GUI does, and Ubuntu 22.04 / Debian 12 can
  install the daemon but **cannot run the AppImage**.

---

## Part B — The per-family matrix

Channel legend: **T** = install tarball (`install.sh`), **D** = `.deb` (shipping),
**R** = `.rpm` (**in flight, uncommitted, does not build yet** — see C10),
**A** = GUI AppImage (shipping), **F** = GUI Flatpak (builds and runs locally; the
CI bundle is part of the same in-flight change).

### B.1 Fedora / RHEL / Silverblue / Bazzite

| | Bazzite 44 (Silverblue) | Fedora Workstation (mutable) | Silverblue / Kinoite | RHEL / Rocky / Alma 9 | RHEL 10 |
|---|---|---|---|---|---|
| Channel today | **T** + **A** (measured) | **T** + **A** | **T** + **A** | — (excluded by the 2.35 declaration) | — |
| Channel it should be | **R** via `rpm-ostree install` | **R** via dnf | **R** via `rpm-ostree` | **R**, if the floor is lowered deliberately | **R** |
| init | systemd **[M]** | systemd **[I]** | systemd **[I]** | systemd **[I]** | systemd **[I]** |
| LSM | SELinux **enforcing** (`/sys/fs/selinux/enforce` = 1) **[M]** | SELinux enforcing **[I]** | enforcing **[I]** | enforcing **[I]** | enforcing **[I]** |
| Firewall | nftables; firewalld present **[I]** | firewalld/nftables **[I]** | same **[I]** | same **[I]** | same **[I]** |
| Resolver | systemd-resolved, `resolvectl` present **[M]** | resolved (F33+) **[I]** | resolved **[I]** | resolved **[I]** | resolved **[I]** |
| cgroup v2 | unified, `/sys/fs/cgroup/cgroup.controllers` present **[M]** | **[I]** | **[I]** | **[I]** | **[I]** |
| BPF marker | **works** — proven on this kernel and in production use **[M]** | **[I]** | **[I]** | kernel 5.14+backports **[?]** | **[I]** |
| `/dev/net/tun` | present, `crw-rw-rw-` **[M]** | **[I]** | **[I]** | **[I]** | **[I]** |
| `/usr` writable | **no**, ostree, `ro,relatime,…` **[M]** | yes **[I]** | **no** **[I]** | yes **[I]** | yes **[I]** |
| glibc | **2.43** **[M]** | 2.39–2.41 **[I]** | as Fedora **[I]** | **2.34** **[I]** | 2.39 **[I]** |

**Known to work (measured, Bazzite only):** the tarball installer detects the immutable
layout, installs to `/usr/local` + `/etc/systemd/system`, rewrites `ExecStart`, builds
and loads the SELinux module (`checkmodule`, `semodule_package`, `semodule` are all
present here **[M]**), and the tunnel carries traffic with IPv6 fail-closed, DNS pinned
and the egress marker proven.

**Expected to break, with the mechanism:**

1. **Stock Fedora Workstation may not be able to build the SELinux module.**
   `checkmodule` ships in `checkpolicy` and `semodule_package` in
   `policycoreutils-devel`; neither is in a default Workstation install **[I]**. The
   installer then only *warns*, and the very next connect fails with
   `tun_permission_denied` **[M-src]**. Bazzite happens to have all three **[M]** —
   which is exactly why this has never been seen.
   *Smallest test:* `rpm -q checkpolicy policycoreutils-devel` on a stock F42 VM,
   then `sudo ./install.sh` and read the preflight lines.
2. **RHEL/Rocky/Alma 9 are excluded by a declaration that today's binaries do not
   actually need.** glibc 2.34 **[I]** vs a declared floor of 2.35 → `install.sh`
   preflight dies, and the in-flight rpm's `Requires: glibc >= 2.35` says so in as
   many words. But **both shipped artifacts top out at `GLIBC_2.34`** **[M-bin]**.
   Read that precisely: the SDK is *built* with a `gnu.2.35` target, so 2.34 is what
   this build happened to need, not a guarantee for the next one. Making el9 a
   supported target is therefore a three-line change *and* a build-target change
   (`gnu.2.34` in the SDK, the `glibc_floor` meson option, both package
   declarations) — not just editing `Requires:`.
   *Smallest test, and it changes nothing on the machine:* copy `urnetworkd` and
   `libURnetworkSdk.so` to a Rocky 9 VM and run
   `LD_LIBRARY_PATH=. ./urnetworkd --version`. If it prints a version, the exclusion
   is a policy choice rather than a technical one, and can be made deliberately.
3. **`rpm-ostree` layering will not behave like `dnf`.** Scriptlets that call
   `systemctl` are no-ops during layering, so `%systemd_post` cannot enable the unit —
   Fedora **preset** coverage must do it. `groupadd` in `%post` is the wrong mechanism
   too; `/usr/lib/sysusers.d/urnetwork.conf` is **[I]** the portable one. And the
   SELinux module must ship **prebuilt** (`.pp` + `%selinux_modules_install`), because
   requiring end users to have policy *build* tools is a build dependency pushed onto
   the customer.
   *Smallest test:* `rpm-ostree install ./urnetwork-daemon-*.rpm` on a Silverblue VM,
   reboot, `systemctl is-enabled urnetworkd` and `getent group urnetwork`.
4. **`firewall-cmd --reload` may flush our table** the way `systemctl restart nftables`
   does **[?]**.
   *Smallest test:* with a tunnel up, `sudo nft list table inet urnetwork | head -1`,
   then `sudo firewall-cmd --reload`, then the same command again — and watch the
   journal for the reaper's re-install line (it should recover within ~5 s).
5. **RPM path rule:** the unit must be `/usr/lib/systemd/system/urnetworkd.service`,
   not the `.deb`'s `/lib/systemd/system` **[I]** — `rpmbuild`/Fedora review refuses
   files under the `/lib` symlink.

### B.2 Debian

| | Debian 12 (bookworm) | Debian 13 (trixie) |
|---|---|---|
| Channel | **D** (exists) + **F** for the GUI | **D** + **A** or **F** |
| init | systemd **[I]** | systemd **[I]** |
| LSM | AppArmor enabled since Debian 10, profiles for few binaries; **we are unconfined** **[I]** | same **[I]** |
| Firewall | nftables backend; `iptables` is iptables-nft **[I]** | same **[I]** |
| Resolver | **`/etc/resolv.conf`, no systemd-resolved by default** **[I]** | same **[I]** |
| cgroup v2 | unified **[I]** | unified **[I]** |
| glibc | 2.36 **[I]** | 2.41 **[I]** |

**Expected to break:**

1. **The kill switch is unusable.** No systemd-resolved → `resolvectl` missing →
   `dns_applied=false` → `RunStart` refuses every kill-switch connect with
   `kCodeDnsApplyFailed` **[M-src]**. Without the switch you get a working tunnel whose
   DNS to a LAN/router resolver goes off-tunnel. **This is the single largest
   non-packaging portability gap in the product.**
   *Smallest test:* on a bookworm VM, connect with the toggle off (expect
   `dns_applied=false` in the UI), then with it on (expect a refusal naming DNS).
2. **`nft` may simply not be installed, and the `.deb` does not ask for it.**
   `depends:` is `libc6 (>= 2.35)`, `iproute2`, `libfuse2t64 | libfuse2` **[M-src]** —
   **no `nftables`**. `start_tunnel` then refuses with "nftables (nft) is not
   installed" *after* a successful install. **[?]** whether bookworm's default install
   carries it.
   *Smallest test:* `debootstrap`-minimal or a fresh netinst VM →
   `dpkg -l nftables; command -v nft`.
3. **`libglib2.0-0` is undeclared** although `urnetworkd` `NEEDED`s
   `libgio-2.0.so.0` and `libglib-2.0.so.0` **[M-bin]**. In practice glib is pulled in
   by almost everything, so this hides on a desktop and bites a minimal/headless box.
   *Smallest test:* `apt install ./urnetwork-daemon_*.deb` in a minimal container, then
   `ldd /usr/lib/urnetwork/urnetworkd | grep 'not found'`.
4. **The AppImage GUI will not run** (needs glibc 2.39 **[M-src]**, bookworm has 2.36
   **[I]**). The Flatpak is the only GUI for Debian 12 — and **CI publishes no Flatpak
   bundle**.
   *Smallest test:* `./URnetwork-*.AppImage --appimage-extract-and-run` on bookworm;
   expect a `GLIBC_2.39 not found` loader error.

### B.3 Ubuntu (including 24.04+)

| | Ubuntu 22.04 | Ubuntu 24.04 / 25.x |
|---|---|---|
| Channel | **D** (+ Flatpak GUI) | **D** + **A** |
| init | systemd **[I]** | systemd **[I]** |
| LSM | AppArmor, we are unprofiled → unconfined **[I]** | AppArmor + `apparmor_restrict_unprivileged_userns=1` **[I]** |
| Firewall | ufw (inactive by default) over iptables-nft **[I]** | same **[I]** |
| Resolver | **systemd-resolved (default)** **[I]** | **systemd-resolved** **[I]** |
| glibc | **2.35 — exactly the declared floor** **[I]** | 2.39 **[I]** |
| GUI | **AppImage will NOT run (needs 2.39)** | AppImage OK **[I]** |

**Expected to work** better than any other unmeasured family: resolved is default (so
the kill switch is available), the `.deb` exists, and the daemon's floor is exactly
22.04's glibc.

**Expected to break / to be checked:**

1. **The `nftables` package again** — same missing dependency as Debian **[?]**.
2. **AppArmor is a live risk but a *narrow* one, and probably not the one people
   expect.** The daemon is unprofiled → unconfined → not mediated **[I]**. The famous
   24.04 breakage (`apparmor_restrict_unprivileged_userns`) targets processes that
   create **unprivileged user namespaces**; our daemon is root and creates none, and
   the GUI is GTK4 with no bwrap/Electron sandbox **[M-src]**. The AppImage type2
   runtime mounts via **FUSE**, not userns **[I]**. Expected verdict: **non-event** —
   but that is inference, and it costs one boot to settle.
   *Smallest test, in this order:* `sudo aa-status | grep -i urnetwork` (expect
   nothing), `sudo ./urnetworkd --selftest-egress` (this is the call that exercises
   `bpf()`, the one syscall AppArmor could plausibly mediate), then one connect and
   `sudo dmesg | grep -i 'apparmor.*DENIED'`.
3. **`libfuse2t64 | libfuse2` in the daemon package is probably stale and misplaced.**
   CI builds the AppImage against the **static type2-runtime** **[M-src]**, which
   statically links libfuse3, so libfuse2 should no longer be needed; and a *daemon*
   package on a headless box has no business depending on the GUI's FUSE stack.
   *Smallest test:* on a 24.04 VM with `libfuse2t64` **removed**, run the AppImage.

### B.4 Arch

> **CachyOS is now a MEASURED column — 2026-08-16.** A real CachyOS box (kernel
> `6.18.42-1-cachyos-lts`, glibc 2.44) ran the tarball installer end to end.
> Measured there: host+pacman detection correct; `ip`/`nft`/`modprobe` all
> present; cgroup v2 unified; `/dev/net/tun` present; SELinux branch correctly
> **skipped** (not errored); unit resolved to `/usr/lib/systemd/system` through
> the `/lib` symlink; GeoClue absent and degraded gracefully; and
> **`--selftest-egress` PASSED on that custom kernel** — all four socket kinds
> marked `0x55524e57`, control socket unmarked. The cgroup-BPF marker is no
> longer a Bazzite-only measurement.
>
> **AND ONE PREDICTION BELOW IS WRONG FOR CachyOS: it ships
> `systemd-resolved` ACTIVE**, with `/etc/resolv.conf` pointing at it. The row
> below is correct for *vanilla* Arch and wrong for this derivative — so
> CachyOS takes **tier 1**, exactly like Bazzite, and does NOT exercise the
> direct-`/etc/resolv.conf` tier. Everything from sign-in onward is still
> unmeasured anywhere on Arch.

| | Arch (rolling) |
|---|---|
| Channel today | **T** (tarball) + **A** |
| Channel it should be | AUR `PKGBUILD` producing a native package |
| init | systemd **[I]** |
| LSM | none by default **[I]** — the SELinux branch no-ops (`getenforce` absent) **[M-src]** |
| Firewall | nftables available, nothing enabled by default **[I]** |
| Resolver | **`resolvectl` binary present (ships with systemd) but `systemd-resolved.service` is NOT enabled by default** **[I]** |
| cgroup v2 | unified **[I]** |
| glibc | newest — forward-compatible with our 2.34 symbols **[M-bin] + [I]** |

**Expected to break:**

1. **`resolvectl` present ≠ resolved running.** Our preflight tests only for the
   *binary* on PATH **[M-src]**. On Arch the binary exists while the service is masked
   or inactive → `resolvectl dns urnet0` fails at runtime → `dns_applied=false` → the
   kill switch refuses. The preflight will have printed a reassuring
   `[preflight] resolvectl /usr/bin/resolvectl`.
   *Smallest test:* `systemctl is-active systemd-resolved` — and this is worth turning
   into a **code fix** (probe liveness, not presence).
2. `nftables` is not installed by default **[?]** — same class as Debian.
3. The tarball installer needs `sort -V` (coreutils, present), `getent`, `usermod` —
   all present **[I]**. No expected installer failure.

### B.5 openSUSE

| | Tumbleweed | MicroOS / Aeon |
|---|---|---|
| Channel | **R** (does not exist) → **T** today | **R** via `transactional-update` → **T** today |
| init | systemd **[I]** | systemd **[I]** |
| LSM | **AppArmor default** (SELinux selectable) **[I]** | **SELinux enforcing by default** **[I]** |
| Firewall | firewalld over nftables **[I]** | same **[I]** |
| Resolver | **netconfig → `/etc/resolv.conf`; systemd-resolved not default** **[I]** | **[I]** |
| `/usr` writable | yes **[I]** | **no — read-only, btrfs snapshots (not ostree)** **[I]** |
| glibc | newest **[I]** | newest **[I]** |

**Expected to break:**

1. **Same DNS/kill-switch gap as Debian and Arch** (netconfig, not resolved) **[I]**.
2. **MicroOS/Aeon immutability is detected by a *different* signal than Bazzite's.**
   There is no `/run/ostree-booted`; detection falls through to
   `findmnt -no OPTIONS --target /usr` matching `ro` **[M-src]**, which should fire
   **[I]** — but even if it does, writing into a running transactional system outside
   `transactional-update` is the wrong shape: the change is not part of a snapshot and
   is lost or inconsistent at the next update.
   *Smallest, zero-risk test:* `findmnt -no OPTIONS --target /usr` and
   `sudo ./install.sh --dry-run` — the dry run prints `layout: immutable host …` if
   and only if detection worked, and **changes nothing**.
3. **SELinux on Aeon/MicroOS** puts us back in the `init_t` situation, so the policy
   module matters there too **[I]** — with the same "are the policy build tools even
   installed" question as stock Fedora.

### B.6 Alpine / musl — **OUT OF SCOPE, and this is a decision, not an oversight**

Three independent blockers, in order of severity:

1. **The SDK is glibc-linked, measured.** `libURnetworkSdk.so` `NEEDED`s `libc.so.6`
   and carries versioned `GLIBC_2.x` symbols **[M-bin]**. musl provides neither
   `libc.so.6` nor glibc symbol versioning. `gcompat` is not a general answer for a
   cgo binary. Fixing this means a **musl cross-build of the Go SDK with
   `CGO_ENABLED=1`** — a new build target, a new artifact, a new CI matrix leg.
2. **No systemd.** Under OpenRC there is no `Type=notify`, no `RuntimeDirectoryPreserve`
   (so no armed marker across a restart) and — the one that actually matters — **no
   `ExecStopPost`**, which is the only teardown that runs on SIGKILL. Since nftables
   state outlives the process, an Alpine port without a designed replacement would
   ship a kill switch that can strand a machine with nothing to lift it.
3. **The installer refuses by construction:** it dies on missing `/run/systemd/system`
   and on being unable to determine a glibc version **[M-src]**.

The same reasoning — blocker (2) alone — puts **Devuan, Artix and OpenRC Gentoo** out of
scope today even though their glibc is fine. **NixOS** is a separate shape again (no
FHS `/usr`, packaging is a module, not a tarball) and is not covered here.

**Say this plainly in user-facing docs:** URnetwork for Linux requires **glibc ≥ 2.35
(2.34 in fact) and systemd**. That is the supported surface.

---

## Part C — Cross-cutting findings from this read

These came out of reading the source against the packaging, and each is independent of
which distro you test next.

| # | Finding | Evidence |
|---|---|---|
| C1 | **The `.deb` does not depend on `nftables`, but `nft` is a hard requirement.** A perfectly successful `apt install` can be followed by a connect that refuses. Independently corroborated: the in-flight rpm template declares `nftables` and leaves a note calling the Debian side "a genuine gap" **[M]**. | `packaging/deb/nfpm.yaml` `depends:` vs `TunnelHost::RunStart` **[M-src]** |
| C2 | **The `.deb` does not depend on glib** although `urnetworkd` `NEEDED`s `libgio-2.0.so.0` + `libglib-2.0.so.0`. | `readelf -d` **[M-bin]** |
| C3 | ~~No systemd-resolved ⇒ no kill switch, anywhere.~~ **FIXED** — `ApplyDns` now falls back to resolvconf and then to a direct `/etc/resolv.conf` takeover, so a host without resolved can still put DNS on the tunnel. Bring-up is fail-closed if no tier takes. **Tiers 2 and 3 have still never executed on any machine.** | `Tunnel::ApplyDns` **[M-src]** |
| C4 | ~~`resolvectl` presence is tested; resolved *liveness* is not.~~ **FIXED** — `ReportPreflight` now runs `ProbeDnsHost` and prints the DNS tier the tunnel will actually take, probing `/run/systemd/resolve` for liveness rather than `$PATH` for the binary. | `ReportPreflight`, `ProbeDnsHost` **[M-src]** |
| C5 | **The declared glibc floor (2.35) is one release above what today's binaries need (2.34).** Our own preflight and the in-flight rpm's `Requires:` are what exclude RHEL/Rocky/Alma 9 — the artifacts themselves would load there. The SDK's build target is `gnu.2.35`, so this is a *policy* to be set deliberately, not a free win. | `meson_options.txt`, `nfpm.yaml`, `install.sh` vs `readelf` **[M-src] + [M-bin]** |
| C6 | **Daemon floor 2.34/2.35, GUI AppImage floor 2.39.** Ubuntu 22.04 and Debian 12 can run the service but not the shipped GUI. | `.github/workflows/beta-build.yml` **[M-src]** |
| C7 | **`/lib/systemd/system` is fine for the `.deb` and wrong for an RPM.** | `nfpm.yaml` vs Fedora usrmerge rules **[M-src] + [I]** |
| C8 | **The SELinux module is built on the *user's* machine at install time.** That makes `checkpolicy` + `policycoreutils-devel` a runtime dependency of a successful tunnel, on a family where they are not installed by default. | `install.sh` SELinux block **[M-src]**; Bazzite happens to have them **[M]** |
| C9 | **The SELinux module widens `init_t` system-wide.** Documented and accepted, but it is the reason a Fedora review would push back; a real `urnetworkd_t` domain is still owed. | `packaging/selinux/urnetwork.te` **[M-src]** |
| C10 | **RPM + Flatpak-bundle work is IN FLIGHT in the working tree** as this was written (uncommitted): `packaging/rpm/{nfpm.yaml,scripts/{pre,post,preun}}` exist, `beta-build.yml` has +221 lines adding an rpm build and a Flatpak bundle asset, and `UR_REQUIRE_RPM: "true"` makes the rpm release-blocking. **`packaging/make-rpm.sh` does not exist yet**, and `scripts/postun` is referenced by `nfpm.yaml` but not yet on disk — so the RPM does not build today. Read D.1.2 as a **review checklist for that work**, not as a request to start it. | `git status`, `packaging/rpm/*` **[M]** |
| C11 | **Containers are not a valid test bed for the data plane.** A cgroup namespace makes `/proc/self/cgroup` read `0::/`, which the daemon rejects (H1), and `/dev/net/tun` is absent unless passed through. **Every distro column below needs a VM, not a distrobox/toolbox/podman container.** | `SelfCgroupV2` + `RunSelftestEgressChild` **[M-src]** |
| C12 | **Asset names are a contract** — the in-app service checker parses them. An RPM added to the release must have its name decided deliberately, not incidentally. | `beta-build.yml` header **[M-src]** |

---

## Part D — What to fix first

Ordered by "how much universality does this unlock per unit of work", with the
measured-vs-inferred honesty attached.

### D.0 — Before anything else: one command per distro

`sudo urnetworkd --selftest-egress` is the highest-value portability probe we own. It
loads and attaches the real BPF program, proves the mark on real sockets of four kinds,
and **starts no tunnel, sends no packet, and touches no firewall, route or DNS**
**[M-src]**. Combined with `--diagnose` (preflight + kill-switch state + recovery
text), two commands characterise a new distro **before** any risk is taken.

**There is now a harness for exactly this.** `packaging/distro-smoke.sh` appeared in
the working tree while this document was being written (uncommitted, 1151 lines,
another session's work **[M]**): it asks the host every question in Part A in one run,
is read-only by default, and in `--privileged` mode the only non-read it performs is
the daemon's own `--selftest-egress`. **If it lands, it is the right first command on
any new distro** and supersedes typing the sequence below by hand. The raw sequence is
kept here because it depends on nothing but the shipped artifacts.

The zero-risk column-opening sequence on a fresh VM, in order — steps 1–4 change
nothing:

```
./urnetworkd --version                 # 1. does the binary even load here?
sudo ./urnetworkd --diagnose           # 2. ip / nft / resolvectl / cgroup / tun
sudo ./urnetworkd --selftest-egress    # 3. does the kernel mechanism work? echo $?
sudo ./install.sh --dry-run            # 4. what would the installer do (layout!)
sudo ./install.sh                      # 5. commit
systemctl is-active systemd-resolved   # 6. is the kill switch even available here?
```

### D.1 P0 — unblocks whole families, small work

1. **Add the missing runtime dependencies** to the `.deb` (`nftables`,
   `libglib2.0-0`), and carry them into the RPM's `Requires:` from day one
   (`nftables`, `iproute2`, `glib2`). *Prevents the "installs fine, refuses to
   connect" class outright.* (C1, C2)
2. **Finish and review the in-flight RPM** (`packaging/rpm/`, uncommitted — see C10).
   Four Fedora-family hazards were identified independently in this read; checked
   against what is on disk **[M]**:

   | Hazard | State of the in-flight work |
   |---|---|
   | Unit must be `/usr/lib/systemd/system`, not `/lib/…` | **handled** — explicit, with the merged-usr reasoning written down |
   | `%systemd_post` cannot enable a unit under `rpm-ostree` layering → needs a **preset** | **handled** — `/usr/lib/systemd/system-preset/85-urnetwork.preset` + `systemctl preset` in `%post` |
   | SELinux module must ship **prebuilt** (`.pp`), not be compiled on the user's machine | **handled** — `.pp` under `/usr/share/selinux/packages/urnetwork/`, `semodule -X 200 -i`, with a compile-from-source fallback for the libsepol-too-new case, and `Requires(post): policycoreutils` |
   | `nftables` must be a hard dependency | **handled** — `Requires: iproute, nftables, shadow-utils, systemd` |
   | Group creation | **open** — `groupadd` in `%pre`. Portable on ordinary rpm hosts; `sysusers.d` is **[I]** the mechanism `rpm-ostree` layering prefers. *Test:* `rpm-ostree install`, reboot, `getent group urnetwork`. |
   | Missing pieces | none — `packaging/make-rpm.sh` and `packaging/rpm/scripts/postun` both exist, and the rpm has been **built end to end on this host** (exit 0, ~15 MB, 123 payload entries) **[M]**. An earlier revision of this table said they were absent; that was read off a tree mid-write by a concurrent session, and was wrong. |
   | Scriptlets | **unexecuted [M]** — `%pre`/`%post`/`%preun`/`%postun` are verified only as embedded, syntactically valid `/bin/sh`. Whether `semodule -X 200 -i` works from inside a `dnf` `%post`, whether the preset actually enables the unit, and whether `init_t` may execute an rpm-labelled `/usr/lib/urnetwork/urnetworkd` are all open until a Fedora VM runs them. |
   | Erase while the kill switch is armed | **open [M]** — `--revert-unless-armed` deliberately preserves the armed floor (`main.cpp:869-886`), so erasing the package removes the binary that would clear `table inet urnetwork` and can leave a **blocked machine**. `scripts/preun:24-32` asserts the opposite. |
3. **Keep the Flatpak bundle release-blocking too, or say why not.** The in-flight
   workflow makes the rpm required (`UR_REQUIRE_RPM: "true"`) and the Flatpak
   switchable by repo variable **[M]**. Given C6 (the AppImage cannot run on Ubuntu
   22.04 or Debian 12), the Flatpak is not a nice-to-have — it is **the only GUI those
   two releases have**. (C6, C10, C12)

### D.2 P1 — makes the next distro actually correct

4. **A DNS path that does not require systemd-resolved.** Today the kill switch is
   effectively Fedora/Ubuntu-only. The wg-quick-shaped answer (`resolvconf`/`openresolv`
   when present, otherwise an atomic `/etc/resolv.conf` rewrite with backup and revert
   on teardown) covers Debian, Arch and openSUSE in one change. Whatever is chosen, it
   must keep the current honesty property: `dns_applied` reports what is **in force**.
   (C3)
5. **Probe resolved's liveness, not just `resolvectl`'s presence** in `ReportPreflight`
   — one `systemctl is-active` or one D-Bus name check. (C4)
6. **Decide the glibc floor on purpose.** Either move the SDK build target to
   `gnu.2.34` and drop the declaration in all three places (gaining the
   RHEL/Rocky/Alma 9 family, pending the one-command VM check in B.1), or keep 2.35
   and write down that el9 is out. Right now the number is inherited rather than
   chosen, and the in-flight rpm has already hard-coded the consequence. (C5)

### D.3 P2 — the honest remainder

7. **The GUI floor.** Ubuntu 22.04 and Debian 12 are prime `.deb` targets with no
   runnable GUI. Two options only: build the GUI against the floor (zig/sysroot), or
   make the **Flatpak the official GUI** for those releases and say so in the docs.
   Shipping a `.deb` whose GUI cannot run on the distro it targets is the worst of the
   three. (C6)
8. **AppArmor: measure before confining.** Do not ship a profile pre-emptively. Run
   B.3's three-command check on 24.04; only write a profile if something is actually
   denied.
9. **A real SELinux domain** (`urnetworkd_t` via `urnetworkd_exec_t`) to replace the
   `init_t` widening — needed for Fedora review, and it removes the end-user policy
   build-tool dependency along the way. (C8, C9)
10. **Alpine/musl stays out of scope** until someone wants to fund a musl SDK build
    *and* an OpenRC design for the crash-safety half of the kill switch. Document the
    requirement (glibc ≥ 2.35 + systemd) rather than half-supporting it.

### D.4 Suggested order to open the columns

| Order | Distro | Why this one next | What it proves |
|---|---|---|---|
| 1 | **Fedora Workstation (mutable)** | Nearest neighbour to the only measured host; isolates *mutable-`/usr` + SELinux* from *ostree* | The SELinux story without the immutable remap; whether the policy build tools are there (C8) |
| 2 | **Ubuntu 24.04** | Largest install base; resolved is default so the kill switch is testable; settles the AppArmor question | AppArmor is or is not a non-event; the `.deb` on its native distro |
| 3 | **Debian 12** | The `.deb`'s other native home, and the worst case for DNS + the GUI floor | C3 (kill switch) and C6 (GUI) at the same time |
| 4 | **Silverblue or Bazzite via `rpm-ostree install`** | Validates the RPM as it will actually be consumed on immutable hosts | D.1.2's four RPM changes |
| 5 | **Arch** | Cheapest way to catch the "binary present, service dead" class | C4 |
| 6 | **openSUSE Tumbleweed, then MicroOS** | AppArmor-by-default plus a *non-ostree* immutable host | The `findmnt` half of the layout detector |

Each column is one VM and roughly the six commands in D.0. Nothing in that sequence
requires a tunnel to be brought up until step 5, and nothing before step 4 modifies the
machine at all.

---

## Appendix — the recovery path, because portability testing will strand a machine

`nftables` state is not process-bound. A daemon killed while armed leaves the host
blocked **on purpose**. On any distro, the way back is **[M-src]**:

```
sudo systemctl stop urnetworkd
sudo /usr/lib/urnetwork/urnetworkd --revert        # /usr/local/... on immutable hosts
# firewall half alone, if the binary is broken:
sudo nft delete table inet urnetwork
```

Stop the service **first** — while it runs, the reaper re-installs the ruleset within
~5 s of anything deleting it. With the daemon alive, disconnecting in the app always
works: the control channel is AF_UNIX, which no rule this daemon can emit is able to
block.
