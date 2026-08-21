# Testing URnetwork on CachyOS — the tester's procedure

**Target:** CachyOS (Arch-based), x86_64, systemd, cgroup v2, a desktop session.
**Channels covered:** the daemon **install tarball**, the GUI **AppImage**, the GUI **Flatpak**.
**Not covered:** `.deb` and `.rpm` — Arch has neither. On this distro the tarball is the
daemon's only channel, so if the tarball is broken there is no fallback.

---

## 0. Read this before you touch anything

### 0.1 Nothing in this document has been run on Arch

> **UPDATE 2026-08-16:** the installer half now HAS been — see §0.2b for measured
> results from a real CachyOS box, including `--selftest-egress` passing on a
> custom CachyOS-LTS kernel. Everything from sign-in onward is still unrun.

Every command and every expected line below was derived by **reading this repository's
source**, and every claim that something *works* was measured **on the owner's Bazzite
(Fedora/ostree) machine**, not here. The three states are labelled throughout:

| Label | Meaning |
|---|---|
| **MEASURED (Bazzite)** | Observed on the owner's Fedora-atomic box. Says nothing about Arch. |
| **FROM SOURCE** | Read out of the code in this repo. The string/behaviour is exact; whether the host reaches that code path on Arch is unknown. |
| **UNKNOWN (Arch)** | Nobody has run it on Arch. This is what you are being paid to find out. |

If a step's real output does not match the expected output here, **that is a result, not a
mistake** — record it. Do not "fix" it into agreement.

### 0.2 DNS on Arch: what the code now does

**Read this before §5 — an earlier draft of this document described the opposite behaviour,
because the code changed underneath it.**

Arch does not enable `systemd-resolved` by default, and URnetwork used to point DNS at the
tunnel *only* through `resolvectl`. On a stock Arch box that meant the tunnel came up, carried
traffic, and sent every name lookup to the ISP resolver. That is now fixed, in three parts:

**1. Three tiers, tried in order** (`app/src/Tunnel.cpp::ApplyDns`):

| Tier | Mechanism | Chosen when |
|---|---|---|
| 1 | `systemd-resolved` via `resolvectl` | resolved is **actually running** — probed via `/run/systemd/resolve`, its `RuntimeDirectory`, not by asking `resolvectl` (which can D-Bus-activate a service the admin deliberately disabled) |
| 2 | `openresolv` / Debian `resolvconf` | a real `resolvconf` exists that is **not** systemd's compat shim (checked with `realpath`) |
| 3 | Direct `/etc/resolv.conf` takeover | nothing else can — **the expected tier on stock CachyOS** |

Tier 3 replaces the file (or the **symlink**, which is replaced rather than written through),
records the original bytes, mode and link target under `/etc` — not `/run`, so a reboot cannot
orphan them — and restores it on disconnect, on the next daemon start, and from the unit's
`ExecStopPost`, so a `SIGKILL` still gets you your resolver back.

**2. It is now fail-closed, and NOT tied to the kill switch.** If no tier can apply DNS the
daemon **refuses the connection** with `dns_apply_failed` rather than carrying traffic while
names leak. Turning the kill switch on or off does not change this.

**3. `--diagnose` tells you which tier you will get, before you connect.** It used to answer
the DNS question with a bare "is `resolvectl` on `$PATH`" test — which on Arch says **yes** on
a machine where systemd-resolved is not running at all, because the `systemd` package ships the
binary either way. It now runs the same probe the tunnel runs:

```
[preflight] dns         tier 3 (direct /etc/resolv.conf takeover)
[preflight]             <the reason, naming what it found>
```

### 0.2b MEASURED ON A REAL CachyOS BOX — 2026-08-16, and it corrects §0.2

First run on the tester's machine (CachyOS, kernel `6.18.42-1-cachyos-lts`,
glibc 2.44, KDE). **Everything below is MEASURED, not inferred** — the first
non-Bazzite column in this project with real data behind it.

| Check | Result |
|---|---|
| Host detection | CachyOS, `pacman` family — correct |
| glibc | 2.44 (floor is 2.35) |
| `ip`, `nft`, `modprobe` | all present; **no missing packages** |
| cgroup v2 | unified hierarchy present |
| `/dev/net/tun` | present |
| SELinux | not active → policy module correctly **skipped**, not errored |
| Unit path | `/usr/lib/systemd/system/urnetworkd.service` — the `/lib`→`/usr/lib` symlink canonicalisation **works** |
| GeoClue | absent → degrades to "location override unavailable", install continues |
| **`--selftest-egress`** | **PASS on a custom CachyOS-LTS kernel.** All four socket kinds (`AF_INET`/`AF_INET6` × `SOCK_DGRAM`/`SOCK_STREAM`) came back carrying `0x55524e57`; the control socket outside the cgroup did not |

**THE CORRECTION: CachyOS ships `systemd-resolved` ACTIVE, with
`/etc/resolv.conf` pointing at it.** §0.2 was written on the premise that Arch
does not enable it and that **tier 3 would be the expected path here**. That is
true of vanilla Arch and **false of CachyOS**, which enables it as part of its
default network setup.

Consequences, and they cut both ways:

* The DNS leak this whole workstream was built around **does not occur on a
  stock CachyOS box.** It takes **tier 1**, the same path Bazzite takes.
* So **tier 3 remains unexecuted on any machine on earth.** The one host we
  expected to exercise it does not. Reaching it now requires deliberately
  forcing it — see §0.3 — and that is a *separate* test run, not something that
  falls out of ordinary use.
* The fail-closed refusal is likewise not exercised by a normal connect here.

Do the ordinary connect first and get a clean baseline. Forcing tier 3 is a
second pass, and it is the more interesting one.

### 0.3 What we have NOT tested — and why your box is the only way to find out

**Tiers 2 and 3 have never executed on any machine.** Every host we have selects tier 1. Their
file mechanics were proven on scratch paths (symlink replacement, byte-exact restore, mode
preservation — 16/16), but **no real `/etc/resolv.conf` takeover has ever run**, anywhere. Your
CachyOS box is the first machine that will take tier 3 for real. Treat §5 and §7 as the actual
experiment, not a formality.

To force a tier that your machine would not otherwise pick (all three cannot be reached on one
host), set `URNETWORK_DNS_BACKEND=resolved|resolvconf|file` in the daemon's environment:

```bash
sudo systemctl edit urnetworkd        # add:  [Service]  Environment=URNETWORK_DNS_BACKEND=file
sudo systemctl restart urnetworkd
```

An unrecognised value is refused loudly rather than silently falling back.

`URNETWORK_ALLOW_UNPROTECTED_DNS=1` skips the fail-closed refusal and connects with DNS
unprotected. **Do not set it** unless we ask you to reproduce something specific — it is the
old leaking behaviour, kept only as an escape hatch. It does not fake a pass:
`dns_applied` stays `false`.

**Two known-open risks worth watching for, both specific to tier 3:**

* **NetworkManager owns `/etc/resolv.conf` on most CachyOS installs.** Its `dns=default`
  plugin rewrites that file on every connectivity change. The daemon now re-checks the
  override every 10s and re-applies it; if it cannot, it stops the session rather than leave
  names unprotected. If you see the connection drop with *"Your DNS stopped going through the
  tunnel"*, that is this — **report it, with `journalctl -u urnetworkd`**. The shipped
  `95-urnetwork.conf` marks only `urnet0` unmanaged; it does **not** stop NetworkManager
  owning `resolv.conf`.
* **`sudo urnetworkd --revert` while connected** restores `/etc/resolv.conf` underneath the
  live session. Don't run it as a "cleanup" mid-test.

### 0.4 What you are actually being asked to discover

These are open questions. Please answer each one explicitly in your report, even if the
answer is "worked, nothing to say".

1. Does the **cgroup-BPF socket marker** load, attach and actually mark sockets on a
   **CachyOS custom kernel**? (§5. This is the single most important unknown — CachyOS ships
   non-stock kernels and the marker is what keeps the daemon's own traffic out of its own
   tunnel.)
2. Does the **tarball installer** take its **standard** path (`/usr`, not the immutable
   `/usr/local` remap) on Arch, and does the unit land somewhere systemd loads it given that
   **`/lib` is a symlink to `/usr/lib`**? (§6)
3. Does the installer **skip the SELinux work cleanly** on a machine with no SELinux and no
   AppArmor — silently and successfully, not with an error? (§6)
4. Are **`nftables`** and **`iproute2`** actually present, and does the daemon's preflight
   name them correctly if they are not? (§4, §5)
5. Is **DNS** going through the tunnel — and if not, exactly which of the two failure modes
   in §0.2 is it? (§7.6)
6. Does the **AppImage** run at all on Arch (FUSE, GTK4/libadwaita stack, glibc)? Arch's
   glibc is far newer than the 2.35 floor and the AppImage bundles a 24.04 GTK stack that is
   *older* than Arch's — the safe direction, but unverified. (§9)
7. Does the **Flatpak** reach the host daemon through `/run/urnetwork`, and what differs from
   the AppImage in practice? (§10)
8. Does anything about the **kill switch and its recovery** behave differently here? (§8, §2)

---

## 1. RECOVERY CARD — read this now, before you install anything

The kill switch is an **nftables table**. nftables rules are **not** tied to process
lifetime: a daemon that is killed while armed leaves the machine blocked on purpose, and it
will come back armed. You can end up on a machine that cannot reach anything, including the
page telling you how to fix it. So learn the way out first.

### The one command that lifts the block

```bash
sudo nft delete table inet urnetwork
```

This exact string is built from the table-name constant in `app/src/Tunnel.hpp`
(`NetFilter::RecoveryCommand()`), so it can never drift from the table the daemon creates.

### The full sequence, in order

```bash
sudo systemctl stop urnetworkd                       # FIRST — see below
sudo /usr/lib/urnetwork/urnetworkd --revert
```

`--revert` removes the table, the policy rules, the capture routes and the armed marker, so
the next start comes up open. If the binary itself is broken:

```bash
sudo nft delete table inet urnetwork
sudo ip -4 rule delete table 51821
sudo ip -4 route flush table 51821
sudo rm -f /run/urnetwork/kill-switch-armed
```

The first command alone restores the network. The last one matters because
`/run/urnetwork/kill-switch-armed` is what makes a crash-restart come back **armed** — remove
it too if the block returns by itself. (A reboot clears `/run`, so a reboot also clears it.)

**Stop the daemon FIRST.** While it runs, its reaper re-installs the ruleset within about a
second of anything deleting it — that is deliberate tamper protection against
`systemctl restart nftables`, whose shipped config on some distros begins with
`flush ruleset`. Deleting the table under a live daemon will look like it did nothing.

**With the daemon running, the normal way out is the app:** disconnect, or toggle the kill
switch off. The app reaches the daemon over a unix socket, which no rule this daemon installs
can block.

### If the daemon will not start

```bash
systemctl status urnetworkd
sudo journalctl -u urnetworkd -b --no-pager | tail -50
```

Read the failure in this order:

| Symptom | Meaning | What to do |
|---|---|---|
| `status=203/EXEC` | systemd cannot execute `ExecStart`. On Arch this should not happen, but it is the classic symptom of the unit's `ExecStart=/usr/lib/urnetwork/urnetworkd` not matching where the file landed. | `ls -l /usr/lib/urnetwork/urnetworkd` and `systemctl cat urnetworkd`; report both. |
| `error while loading shared libraries: libURnetworkSdk.so` | The SDK `.so` is not beside the binary. The daemon's RPATH is `$ORIGIN` (`app/meson.build`), so the two files must sit in the same directory. | `ls -l /usr/lib/urnetwork/`; report. |
| `Unit urnetworkd.service not found` | The unit file did not land in a directory systemd loads. **This is unknown-on-Arch item #2** (`/lib` → `/usr/lib` symlink). | `ls -l /usr/lib/systemd/system/urnetworkd.service /lib/systemd/system/urnetworkd.service`; `systemctl daemon-reload`; report. |
| Starts, then `[daemon] N required host component(s) are missing` | `ip` or `nft` is not installed, or there is no cgroup v2. The daemon deliberately **still serves the control socket** so the GUI gets a real answer; the first Connect is what fails. | §2 prerequisites. |
| The unit restarts in a loop | There is **no start rate limit** (`StartLimitIntervalSec=0`, deliberate — a `failed` unit with a live kill-switch table has no control socket and no way back). | Stop it, then run the recovery sequence above. |

The recovery text is also reachable offline from the machine itself:
`urnetworkd --help`, `urnetworkd --diagnose`, and `systemctl cat urnetworkd`.

---

## 2. Prerequisites

### 2.1 Required — the daemon refuses to build a tunnel without these

```bash
sudo pacman -S --needed nftables iproute2
```

| Package | Provides | What breaks without it | Required? |
|---|---|---|---|
| `nftables` | `nft` | Egress self-exclusion (**the daemon's own sockets fall into its own tunnel**), the IPv6 and DNS leak floor, the kill switch. The daemon refuses to start a tunnel. | **Yes** |
| `iproute2` | `ip` | The tun address, the 31 capture routes, the policy rule. | **Yes** |

`iproute2` is part of Arch's `base`, so `ip` should already be there. **`nft` is a different
story:** it comes from the `nftables` package, and `iptables-nft` — which uses the nft
*backend* — does **not** provide the `nft` command. **UNKNOWN (Arch):** whether CachyOS's
default install already has `nftables`. Check and report:

```bash
command -v nft ip
pacman -Qo /usr/bin/nft 2>/dev/null || echo "nft NOT INSTALLED"
```

### 2.2 The DNS decision — pick a track, and tell us which

This is the point of the whole exercise. Choose one and run the full procedure on it; if you
have time, run both.

**Track A — as Arch ships it (no systemd-resolved).** This is the configuration a real
first-time Arch user is in, and the one that exercises **tier 3, which has never run
anywhere**. It is the single most valuable thing you can test. **Do this one first.** Do not
enable `systemd-resolved`. Record what `/etc/resolv.conf` is and where it points:

```bash
systemctl is-enabled systemd-resolved 2>&1
systemctl is-active  systemd-resolved 2>&1
command -v resolvectl || echo "resolvectl NOT on PATH"
ls -l /etc/resolv.conf
cat /etc/resolv.conf
```

Note carefully: **`resolvectl` being present does not mean resolved is running.** The
`systemd` package ships the binary either way — which is exactly why the daemon probes
`/run/systemd/resolve` instead of trusting `$PATH`. The `[preflight] dns tier N` line accounts
for this; the older `[preflight] resolvectl <path>` line above it does **not** and never did.
If they appear to disagree, **the tier line is the authoritative one** — and say so in your
report.

**Track B — with systemd-resolved, the configuration the code was written for.**

```bash
sudo systemctl enable --now systemd-resolved
sudo ln -sf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf
resolvectl status
```

Under Track B you should get **tier 1**, `dns_applied=1` and the DNS floor installed. That is
what Bazzite does, and it is **MEASURED (Bazzite)** to work there — but not here.

Track A should now ALSO reach `dns_applied=1`, via **tier 3**. If it does not, the connection
is refused rather than leaking — and that refusal, with its reason, is the finding.

### 2.3 Optional, but install them or the results get muddier

Install these one group at a time — a package name that does not exist on your Arch snapshot
would otherwise abort the whole transaction and leave you with none of them:

```bash
sudo pacman -S --needed fuse2 flatpak socat tcpdump
sudo pacman -S --needed desktop-file-utils gtk-update-icon-cache geoclue
sudo pacman -S --needed bind || sudo pacman -S --needed bind-tools   # whichever provides dig
```

| Package | Why |
|---|---|
| `fuse2` | Type-2 AppImages need **libfuse2**. Arch ships fuse3 by default; the AppImage will not mount without this (workaround in §9.1). |
| `flatpak` | §10. |
| `socat` | Lets you ask the daemon for its own status over the control socket (§7.6) — the only place `dns_applied` is readable without the journal. |
| `tcpdump` | The optional "where did that DNS query actually go" check (§7.6 D). |
| `bind` | `dig`. On some Arch installs this package is still called `bind-tools`; if `pacman -S bind` fails, try `bind-tools`, or use `getent hosts` / `drill`. |
| `desktop-file-utils` | `update-desktop-database`. Without it `urnetwork://` SSO/deep links silently do not resolve — the installer warns and continues (§6.3). |
| `gtk-update-icon-cache` | The launcher icon. Cosmetic. Installer warns and continues. |
| `geoclue` | The **optional** location-override feature (needs ≥ 2.7.0). Absent is fine; the installer says so and installs everything else. |

### 2.4 Facts to record about the machine, before anything is installed

```bash
uname -r
cat /etc/os-release | head -3
getconf GNU_LIBC_VERSION
stat -fc %T /sys/fs/cgroup            # expect: cgroup2fs
ls -ld /lib                            # expect: /lib -> usr/lib   (Arch usrmerge)
findmnt -no OPTIONS --target /usr      # expect: rw,...  (NOT ro)
ls -l /dev/net/tun                     # absent is OK; one modprobe is tried at first start
getenforce 2>/dev/null || echo "no SELinux (expected on Arch)"
aa-status 2>/dev/null | head -2 || echo "no AppArmor (expected on Arch)"
```

Expected on Arch, and each one is an assumption worth confirming rather than believing:

* `stat -fc %T /sys/fs/cgroup` → `cgroup2fs`. **cgroup v2 is required.** No v2, no tunnel.
* `/lib` → `usr/lib`. The installer writes the unit to `/lib/systemd/system/urnetworkd.service`;
  through the symlink that should land at `/usr/lib/systemd/system/urnetworkd.service`, which
  systemd loads. **UNKNOWN (Arch)** — verify in §6.2.
* `/usr` is **writable**, so the installer should choose the **standard** layout, not the
  ostree/immutable remap into `/usr/local`. **UNKNOWN (Arch)** — verify in §6.2.
* `getenforce` should not exist. The installer only touches SELinux when
  `command -v getenforce` succeeds **and** it does not report `Disabled` — so on Arch the
  whole SELinux block should be skipped without a word. **UNKNOWN (Arch)** — verify in §6.2.
* glibc will be far newer than the installer's `2.35` floor. Fine for the daemon. The
  AppImage bundles a **24.04** GTK stack, i.e. older than Arch's — the safe direction, but
  see §9.

---

## 3. Getting the artifacts

Asset names are a **contract** — the build pipeline asserts them, and the in-app updater
parses them. Do not rename the files.

Repo: `Ryanmello07/urnetwork-linux`. Current beta at time of writing:
`v2026.8.16-1020679030-beta`, i.e. `VERSION = 2026.8.16-1020679030-beta`.

| Asset | You need it for |
|---|---|
| `urnetwork-daemon-<VERSION>-amd64.install.tar.gz` | **The daemon. The only daemon channel on Arch.** |
| `URnetwork-<VERSION>-amd64.AppImage` | The GUI, AppImage channel. |
| `URnetwork-<VERSION>-amd64.AppImage.zsync` | Update control file. Attached for mirroring only — GitHub Releases answers zsync's multi-range requests with HTTP 501, so the in-app updater uses the self-hosted copy. You do not need it to test. |
| `urnetwork-daemon_<VERSION>_amd64.deb` | **Ignore.** No use on Arch. |
| `URnetwork-<VERSION>-amd64.flatpak` | The GUI, Flatpak channel — **if the release has it.** This asset is newly added by the pipeline and is attached *after* the release is created, so it may be missing from the build you download. §10 covers both cases. |

```bash
mkdir -p ~/urnetwork-test && cd ~/urnetwork-test

# Set this once; every later step in this document reuses $V.
V=2026.8.16-1020679030-beta

# With the GitHub CLI (easiest):
gh release download "v$V" -R Ryanmello07/urnetwork-linux -p '*'

# Or by hand:
B=https://github.com/Ryanmello07/urnetwork-linux/releases/download/v$V
curl -fLO "$B/urnetwork-daemon-$V-amd64.install.tar.gz"
curl -fLO "$B/URnetwork-$V-amd64.AppImage"

ls -l
sha256sum *.tar.gz *.AppImage
```

If you open a new terminal later, re-export `V` (or substitute the version by hand).

If `.sha256` files are published alongside, verify them (`sha256sum -c <file>.sha256`). There
is **no GPG signature** unless a release key was configured for that build — the build script
says so in its own output rather than faking one. Do not report an unsigned artifact as a
defect; do report a **failing checksum** immediately and stop.

Extract, but **do not install yet** — §5 runs first, from the extracted tree.

```bash
tar xzf urnetwork-daemon-$V-amd64.install.tar.gz
ls urnetwork-daemon/
# expect exactly: VERSION  install.sh  payload/  selinux/  uninstall.sh
cat urnetwork-daemon/VERSION
```

The tarball is guaranteed to have a **single top-level directory** (`urnetwork-daemon/`) — the
build asserts it. If it explodes into your current directory, that is a packaging bug; stop
and report.

---

## 4. Step 0 — the dry run (changes nothing)

```bash
cd ~/urnetwork-test
sudo ./urnetwork-daemon/install.sh --dry-run
```

**FROM SOURCE**, this prints the whole plan and touches nothing. The lines that matter on Arch:

* `urnetwork-daemon <VERSION> installer (payload: amd64)`
* `[dry-run] no changes will be made`
* **It must NOT print** `layout: immutable host (/usr is read-only) -- installing under /usr/local and /etc/systemd/system`.
  If it does, the layout detection has mis-fired on Arch — **record it, and stop**; every
  subsequent path in this document will be wrong.
* A `Preflight:` block with `- systemd: running`, `- glibc: <yours> (>= 2.35)`,
  `- /dev/net/tun: present`, and a GeoClue line either way.
* A `Plan (install):` block listing every file at its **`/usr/...` and `/lib/systemd/system/...`**
  destination.

A `warning:` here is not necessarily a failure — `--dry-run` downgrades hard preflight
failures to warnings on purpose, so read them as "this would have aborted".

Copy the whole output into your report. It is the cheapest possible record of what the
installer thinks this machine is.

---

## 5. Step 1 — prove the kernel BEFORE installing anything

This is the highest-value five minutes in the whole procedure. Both commands run **from the
extracted tarball**, with nothing installed, because the daemon's RPATH is `$ORIGIN` and
`libURnetworkSdk.so` sits right next to the binary.

```bash
cd ~/urnetwork-test
D=./urnetwork-daemon/payload/usr/lib/urnetwork/urnetworkd
$D --version
```

Expected shape (**FROM SOURCE**):

```
urnetworkd 2026.8.16-1020679030-beta (control protocol 1, sdk <sdk-version>)
```

Write the `sdk <sdk-version>` string down — §7.6 needs it verbatim.

If this fails with `error while loading shared libraries`, the tarball payload is incomplete
or the host is missing glib. The daemon links **gio/glib only** (no GTK), with libstdc++ and
libgcc **statically linked**, so the only interesting host dependency is `glib2`.

### 5.1 `--diagnose` — the host preflight, no root needed, no network touched

```bash
$D --diagnose 2>&1 ; echo "exit=$?"
```

The `2>&1` matters: the `[preflight]` lines go to **stderr** while everything around them goes
to stdout, so a bare `> file` redirect silently loses exactly the part you came for.

Expected shape (**FROM SOURCE**; paths and the cgroup line will differ on your box):

```
urnetworkd 2026.8.16-1020679030-beta (control protocol 1, sdk <sdk-version>)
control socket: /run/urnetwork/control.sock
state dir:      /var/lib/urnetwork
log dir:        /var/log/urnetwork
kill switch:    not armed (marker /run/urnetwork/kill-switch-armed)
[preflight] ip          /usr/bin/ip
[preflight] nft         /usr/bin/nft
[preflight] resolvectl  /usr/bin/resolvectl
[preflight] modprobe    /usr/bin/modprobe
[preflight] cgroup      user.slice/user-1000.slice/session-2.scope (level 3)
[preflight] /dev/net/tun present

egress self-exclusion: this build marks the daemon's own sockets at creation with
a cgroup-BPF program. To prove that works on THIS kernel (no tunnel, no routes, no
nftables, no packets):

    sudo /usr/lib/urnetwork/urnetworkd --selftest-egress

If this machine is cut off (the kill switch is armed, or urnetworkd died while a
tunnel was up), this is the way back — no network access required:
...
```

`exit=0` means no **required** component is missing; `exit=1` means at least one is.

**How to read it on Arch:**

* `[preflight] nft         MISSING (required) — nftables: ...` → install `nftables` (§2.1) and
  re-run. Do not proceed.
* `[preflight] ip          MISSING (required) — iproute2: ...` → install `iproute2`.
* `[preflight] resolvectl  MISSING (optional) — ...` → expected on Track A, and **not fatal**:
  read the `dns tier` line below it. Record both.
* `[preflight] dns         tier 3 (direct /etc/resolv.conf takeover)` → **the expected Track A
  answer, and the configuration that has never been exercised anywhere.** Record this line and
  the reason line under it verbatim.
* `[preflight] resolvectl  /usr/bin/resolvectl` **while `systemctl is-active systemd-resolved`
  says `inactive`** → the old false-green case. The tier line should say **3**, not 1. If it
  says `tier 1 (systemd-resolved)` here, that is a **bug — report it immediately**, because it
  means the liveness probe is wrong and DNS will silently fail to apply.
  This asymmetry is worth a line in your report on its own.
* `[preflight] cgroup      MISSING (required)` → no cgroup v2. Stop; nothing below can work.
* `[preflight] /dev/net/tun absent` → the module is not loaded. The daemon attempts one
  `modprobe` at first start. `sudo modprobe tun` and re-check.

Run `--diagnose` **as your normal user**, not under sudo — it needs no root, and the cgroup
line is more interesting from a login session.

### 5.2 `--selftest-egress` — the cheapest possible signal on a new distro

**Why this first.** The daemon keeps its own traffic out of its own tunnel with a
four-instruction `BPF_PROG_TYPE_CGROUP_SOCK` program attached at
`BPF_CGROUP_INET_SOCK_CREATE`. If that mechanism does not work, the daemon reads its own
packets back out of its own tun and re-sends them — on 2026-08-15 that loop moved **3.38 Tb
in forty minutes** on the owner's machine before a human noticed. CachyOS ships **custom
kernels**, so this is exactly the assumption that must be measured rather than hoped.

**What it touches:** it creates **one** temporary cgroup under its own, forks a child into it,
loads and attaches the real program, reads `SO_MARK` off four fresh sockets, and removes the
cgroup again — on every exit path including `^C`, `SIGTERM` and a 60 s watchdog. **No tun
device, no `ip route`/`ip rule`, no nftables, no DNS change, and not one packet on any wire.**
It is safe to run on a machine you care about.

```bash
sudo $D --selftest-egress ; echo "exit=$?"
```

Expected output on a **pass** (**FROM SOURCE**; the `[…]` values will be yours):

```
urnetworkd 2026.8.16-1020679030-beta — egress socket-marker self-test
kernel Linux 6.17.x-cachyos

Question: on THIS kernel, does a cgroup-BPF program attached at
BPF_CGROUP_INET_SOCK_CREATE actually put fwmark 0x55524e57 on a socket at the moment
it is created — before connect() chooses a route and a source address?
That is the whole mechanism the daemon now relies on to keep its own traffic out
of its own tunnel.

This test starts NO tunnel and touches NO networking: no tun device, no routes,
no policy rules, no nftables, no DNS, and not one packet on any wire. It creates
one temporary cgroup, runs a child inside it, and removes it again.

  step 1  this process's cgroup v2 .......... ok    0::/user.slice/... (level 4)
  step 2  privileges ........................ ok    running as root
  control this process, outside the cgroup .. 0x00000000 (unmarked, as it should be)
  step 2b temporary cgroup .................. ok    created /sys/fs/cgroup/.../urnw-selftest-<pid>
  step 3  child joined the test cgroup ...... ok    pid <pid> is in 0::/.../urnw-selftest-<pid>
  step 4  bpf(BPF_PROG_LOAD) ................ ok    the verifier accepted the 4-instruction program
  step 5  bpf(BPF_PROG_ATTACH) .............. ok    BPF_CGROUP_INET_SOCK_CREATE, BPF_F_ALLOW_MULTI
  step 6  SO_MARK on a fresh socket ......... ok    cgroup-bpf sock_create marker on ...: every socket this daemon opens now carries mark 0x55524e57 before connect()
  step 7  the same question, one socket kind at a time:
            AF_INET  SOCK_DGRAM   0x55524e57  marked
            AF_INET  SOCK_STREAM  0x55524e57  marked
            AF_INET6 SOCK_DGRAM   0x55524e57  marked
            AF_INET6 SOCK_STREAM  0x55524e57  marked
  control this process, after the test ...... 0x00000000 (unmarked)

VERDICT: THE MECHANISM WORKS ON THIS KERNEL.
  A socket created inside the test cgroup came back carrying 0x55524e57.
  An identical socket created by this process, outside that cgroup, did not.
  ...

Nothing on this machine was changed: the temporary cgroup is removed, no
packet was sent, and no firewall, route or DNS state was touched.
```

(The dot padding aligns every verdict to a fixed column; only the words and the
`ok` / `FAILED` matter.)

**Exit status is the machine-readable verdict:**

| Exit | Verdict | What it means on CachyOS | What to do |
|---|---|---|---|
| `0` | `THE MECHANISM WORKS ON THIS KERNEL` | The CachyOS kernel supports the marker. **Unknown #1 answered — report it.** | Continue to §6. |
| `1` | `bpf(BPF_PROG_LOAD) FAILED` | The kernel would not accept the program. Likely `CONFIG_CGROUP_BPF=n` / `CONFIG_BPF_SYSCALL=n` in the CachyOS kernel config, or an LSM denying `bpf()`. | **STOP. Do not connect.** Report the verbatim verifier text, `uname -r`, and `zgrep -E 'CGROUP_BPF\|BPF_SYSCALL' /proc/config.gz` (or `/boot/config-$(uname -r)`). |
| `1` | `bpf(BPF_PROG_ATTACH) FAILED` | Loaded but not attachable: missing `CAP_NET_ADMIN`, a cgroup v1 hybrid hierarchy, or another controller's restriction. | **STOP.** Report, plus `stat -fc %T /sys/fs/cgroup` and `cat /proc/self/cgroup`. |
| `1` | `THE MARK NEVER LANDED` | **The dangerous one.** Load and attach both succeeded — every green check short of an actual measurement would have said "protected" — and the sockets would still have gone into the tunnel. | **STOP, and flag this loudly.** This is the exact failure the self-test exists to catch. |
| `2` | `NO ANSWER` | The test could not run: not root, no cgroup v2, `mkdir` refused (`EROFS` = a container, `EACCES`/`EPERM` = v1 hybrid or a delegation boundary). Nothing was proven either way. | Fix the cause and re-run. Never treat `2` as a pass. |

If `--selftest-egress` does not return `0`, **do not go on to §7.** A tunnel on a host where
the marker does not work is the storm scenario. The daemon has its own gate for this (the
packet witness refuses the bring-up), but there is no reason to lean on it.

> **TODO(tester):** paste the full `--selftest-egress` output and the exit code into the
> report, pass or fail. It is the only measurement of this kernel that exists.

---

## 6. Step 2 — install the daemon (tarball channel)

```bash
cd ~/urnetwork-test
sudo ./urnetwork-daemon/install.sh
```

### 6.1 Expected output, and what each line is telling you

**FROM SOURCE.** The shape:

```
urnetwork-daemon <VERSION> installer (payload: amd64)

Fresh install of urnetwork-daemon <VERSION>.

Preflight:
- systemd: running
- glibc: <yours> (>= 2.35)
- /dev/net/tun: present
- GeoClue: <version> (location override supported)      # or "not found — ... optional ..."
creating system group 'urnetwork'
adding <you> to the urnetwork group
installing files...

urnetwork-daemon <VERSION> install complete.
The daemon is running idle; nothing connects until you sign in from the app.

IMPORTANT: you were added to the 'urnetwork' group, and group membership
only applies to NEW login sessions. Log out and back in (or reboot) before
starting the app, or it will report that the service is not running.
Next: install the URnetwork GUI AppImage to ~/.local/lib/urnetwork/URnetwork.AppImage
and run 'urnetwork' (https://ur.io/download).
Uninstall later with: sudo /usr/lib/urnetwork/uninstall.sh
```

**Do not skip the log-out.** The control socket is `0750 root:urnetwork`; a user who is not
yet in the group gets `EACCES` on `connect(2)` and the app reports
*"This app is not allowed to use the URnetwork system service. Add your user to the urnetwork
group and sign in again."* Log out and back in (or reboot) now.

**Arch-specific things that should NOT appear.** Each one is a finding if it does:

* `layout: immutable host (/usr is read-only) ...` — wrong layout detection on Arch.
* `warning: SELinux is enforcing but the policy tools are missing ...` — the SELinux branch
  ran on a machine with no SELinux. The installer only enters it if `command -v getenforce`
  succeeds **and** `getenforce` does not say `Disabled`, so on Arch it should be **silent**.
* `error: ... is installed and owned by dpkg/apt` or `... owned by rpm` — the installer
  refuses to fight another package manager. Should be impossible here, but `pacman` is not
  checked at all, so if you have somehow installed an AUR `urnetwork` package, remove it first.

**Warnings that are acceptable on Arch and are not bugs:**

* `warning: update-desktop-database not found (desktop-file-utils): urnetwork:// SSO/deep
  links will NOT resolve until it runs.` — install `desktop-file-utils` if you want the
  browser sign-in callback to work (§7.2), then re-run the installer or run
  `sudo update-desktop-database /usr/share/applications`.
* `warning: gtk-update-icon-cache not found: ...` — cosmetic.
* `- GeoClue: not found -- the optional location-override feature will be unavailable` — fine.

### 6.2 Verify the install landed where systemd can see it

This answers unknowns #2 and #3.

```bash
ls -l /usr/lib/urnetwork/
ls -l /usr/bin/urnetwork
ls -l /lib/systemd/system/urnetworkd.service /usr/lib/systemd/system/urnetworkd.service
systemctl cat urnetworkd | head -5
systemctl is-enabled urnetworkd
systemctl is-active  urnetworkd
getent group urnetwork
id -nG | tr ' ' '\n' | grep -x urnetwork && echo "group OK (this session)"
ls -ld /run/urnetwork /run/urnetwork/control.sock
```

Expect:

* `/usr/lib/urnetwork/` holds `urnetworkd`, `libURnetworkSdk.so`, `uninstall.sh`,
  `.install-manifest`, `.installed-version`.
* Both unit paths resolve to the **same file** through the `/lib` → `usr/lib` symlink, and
  `systemctl cat urnetworkd` prints it. If `systemctl cat` says `No files found`, that is
  **unknown #2 failing** — report it with the `ls -l` output.
* `enabled`, `active`.
* `/run/urnetwork` is `drwxr-x--- root urnetwork`, and `control.sock` exists with mode `srw-rw----`.
* **No SELinux context** is expected in `ls -l`. If you want the label (there should not be
  one), use `stat -c %C`, **never `ls -Z`** — see §11.

```bash
sudo journalctl -u urnetworkd -b --no-pager | head -30
```

You should see the same `[preflight]` block as §5.1 (this time from inside the service, so
the cgroup line reads `system.slice/urnetworkd.service (level 2)`), then:

```
[daemon] urnetworkd <VERSION> starting (log dir /var/log/urnetwork)
...
[daemon] urnetworkd <VERSION> ready (state=/var/lib/urnetwork). If this machine ends up blocked with no way to reach the daemon: stop the service, then run `/usr/lib/urnetwork/urnetworkd --revert` (firewall half alone: sudo nft delete table inet urnetwork)
```

**The daemon starts idle. It does not bring a tunnel up until a client signs in and asks.**
At this point nothing about your networking has changed — confirm that:

```bash
sudo nft list table inet urnetwork 2>&1 | head -2     # expect: "Error: No such file or directory"
ip -4 rule show                                        # expect: only 0 / 32766 / 32767
```

### 6.3 Record your baseline, before any tunnel

```bash
curl -4 -s https://api.ipify.org ; echo      # your real public IPv4 — write it down
curl -6 -s --max-time 8 https://api64.ipify.org ; echo "(v6 exit=$?)"
ip -4 route show default
cat /etc/resolv.conf
```

---

## 7. Step 3 — the GUI (AppImage), sign in, connect, and verify

### 7.1 Put the AppImage where the launcher expects it

```bash
V=2026.8.16-1020679030-beta                  # if this shell does not already have it
mkdir -p ~/.local/lib/urnetwork
install -m 0755 ~/urnetwork-test/URnetwork-$V-amd64.AppImage \
  ~/.local/lib/urnetwork/URnetwork.AppImage
urnetwork          # /usr/bin/urnetwork, installed by the tarball, execs the AppImage
```

`/usr/bin/urnetwork` searches, in order: `$URNETWORK_APPIMAGE`,
`~/.local/lib/urnetwork/URnetwork.AppImage`, the newest `~/Applications/URnetwork*.AppImage`,
`/usr/lib/urnetwork/URnetwork.AppImage`, then any `urnetwork-gui` on `$PATH`. If none exists it
exits `127` with a one-line hint.

If the AppImage will not mount, see §9.1 — that is an AppImage-channel problem, not a daemon
problem.

Run it from a terminal for this first launch so you can see its log lines:

```bash
~/.local/lib/urnetwork/URnetwork.AppImage 2>&1 | tee ~/urnetwork-test/gui.log
```

### 7.2 Sign in

Sign in with the account credentials you were given. If sign-in uses a browser callback
(`urnetwork://…`) and nothing comes back, that is the `desktop-file-utils` gap from §6.1 —
install it, run `sudo update-desktop-database /usr/share/applications`, and try again. Report
whether it was needed.

If the app says **"This app is not allowed to use the URnetwork system service"** you did not
log out after §6.1. If it says **"The URnetwork system service is not running"** then the
socket is genuinely absent — check §6.2. These are two different messages for two different
causes and the app distinguishes them deliberately; tell us which one you got.

### 7.3 Connect — and watch the journal while you do it

In a second terminal, before you press Connect:

```bash
sudo journalctl -u urnetworkd -f
```

Press **Connect** in the app. In the app's own log (`gui.log`) you should see, in order
(**FROM SOURCE**):

```
connect: toggle pressed (connected=false, hasDevice=...)
connect: start_tunnel requested
connect: sending start_tunnel (fresh session, rpc 127.0.0.1:<12026-12125>, kill switch off)
```

In the **daemon journal**, the sequence that matters, in this order:

```
[tunnel] egress: cgroup-bpf sock_create marker on system.slice/urnetworkd.service: every socket this daemon opens now carries mark 0x55524e57 before connect()
[filter] dns helper cgroup: system.slice/NetworkManager.service (level 2)
[filter] connecting (floor=0 ipv6_blocked=1 dns_pinned=0 helper_dns=0 lan=1 cgroup=system.slice/urnetworkd.service)
[tunnel] device rpc pinned on 127.0.0.1:<port>
[tun] egress witness (bring-up) passed: control socket (mark 0) bound <tunnel-addr> = the tunnel, so capture is in force; daemon socket bound <your-LAN-ip> with mark 0x55524e57, so it is steered around it; 0 marked daemon packets have entered urnet0 and <N> have left via a physical interface
[tun] up urnet0 addr=<tunnel-addr>/24 mtu=1440 dns=<tunnel-resolver> (routes=1 egress_protected=1 dns_applied=<0 or 1>)
[filter] connected (floor=0 ipv6_blocked=1 dns_pinned=<0 or 1> helper_dns=0 lan=1 cgroup=system.slice/urnetworkd.service)
[tun] egress witness (connected) passed: ...
[tunnel] up (client=<uuid> rpc=127.0.0.1:<port> routes=1 egress=1 dns=<0 or 1>)
```

**How to read the three that decide everything:**

| Field | `1` means | `0` means |
|---|---|---|
| `routes=1` | The 31 capture prefixes and the policy rule are installed. `0` is impossible here — a partial capture set fails the bring-up outright. | — |
| `egress_protected=1` / `egress=1` | The witness **measured** that the daemon's own sockets leave via a physical interface and none entered the tun. | The bring-up would have been refused. If you somehow see `0`, disconnect and report. |
| `dns_applied=` / `dns=` / `dns_pinned=` | DNS is on the tunnel (via whichever tier won — see §0.2). | The bring-up would have been **refused**. Seeing `0` on a live session means the escape hatch is set, or the override was lost mid-session — either way, **report it**. |

The `[tunnel] egress:` line comes **before** `[filter] connecting` — the BPF marker is
attached first, and the nftables belt second. A `[filter] dns helper cgroup: ...` line (or
`[filter] no local DNS helper cgroup is present; a Connecting-with-floor reconnect depends on
the daemon's in-process resolver`) is informational either way: it lists which of
`systemd-resolved`, `dnsmasq`, `unbound`, `dnscrypt-proxy` or `NetworkManager` exist as
services, and it only matters for a floored reconnect. On Arch expect `NetworkManager` alone,
or the "no local DNS helper" line — neither is a defect.

There are **two** egress witnesses on purpose — one at bring-up and one after the `Connected`
ruleset swap, because the swap destroys the table the first one measured. The daemon then
re-checks every **30 s**; two consecutive failures tear the tunnel down with
`[tunnel] STOPPING: this daemon's own traffic is no longer demonstrably outside its own tunnel`.

**Failure lines and what they mean:**

| Journal line | Meaning |
|---|---|
| `[tun] egress witness (bring-up) FAILED: ...` | The daemon refuses to run the tunnel. This should be impossible after a passing §5.2, so if you see it, §5.2 and this disagree — a very interesting result. Report both. |
| `[tunnel] WARNING: this daemon's own sockets could NOT be marked at creation (...)` | The BPF marker did not attach at runtime even though the self-test passed. Report with the parenthesised reason. |
| `refusing to start: the daemon's own traffic would be captured by its own tunnel (nftables (nft) is not installed)` | §2.1. |
| `[tunnel] WARNING: leak floor not installed: <reason>` | Connected but the IPv6/DNS floor is missing. The app also gets an error. Report. |
| `[tunnel] start failed: the kill switch needs DNS on the tunnel, and it could not be applied: <reason>` | You connected with the kill switch **on** on a machine with no working resolved — §0.2, the safe branch. |

### 7.4 Verify the tunnel is actually carrying traffic

```bash
# 1. Public IP must have CHANGED from your §6.3 baseline
curl -4 -s https://api.ipify.org ; echo

# 2. Both tun counters must be MOVING (run twice, ~15 s apart, while browsing)
ip -s link show urnet0
sleep 15
ip -s link show urnet0
```

Both the `RX:` and the `TX:` `bytes`/`packets` figures must be non-zero and **larger the
second time**. TX moving with RX flat is the storm signature — the daemon sending into its own
tunnel and getting nothing back. If you see that, **disconnect immediately** and report.

```bash
# 3. The 31 capture routes
ip -4 route show table 51821 | wc -l         # expect exactly: 31
ip -4 route show table 51821 | head -3
# expect lines like:  0.0.0.0/5 dev urnet0 scope link

# 4. The single policy rule
ip -4 rule show
# expect, between local and main:
#   32763:  from all not fwmark 0x55524e57 lookup 51821

# 5. The interface itself
ip -4 addr show urnet0     # expect <tunnel-addr>/24, mtu 1440
```

**Why 31 and not "a default route":** the capture set is 31 IPv4 prefixes covering the public
internet while deliberately **excluding** RFC1918 (`10/8`, `172.16/12`, `192.168/16`), so LAN
traffic falls through to `main`. A count other than 31 means routes were dropped and the
bring-up should have failed — report the count and `ip -4 route show table 51821` in full.

### 7.5 Verify IPv6 is blocked, not leaked

The tunnel is IPv4-only, so IPv6 is fail-closed at all times — `block_ipv6` is hardcoded
`true`, not a preference.

```bash
curl -6 -s --max-time 8 https://api64.ipify.org ; echo "exit=$?"     # expect NON-zero
ping -6 -c1 -W3 2606:4700:4700::1111 ; echo "exit=$?"                # expect NON-zero
```

Now prove the **rule** is what stopped it, rather than an absent IPv6 network:

```bash
sudo nft list chain inet urnetwork urnw_out | grep -n 'nfproto ipv6'
```

Expect something like `meta nfproto ipv6 counter packets 4 bytes 288 reject`. The counter
must be **non-zero after your attempt above** — that is the measurement. A zero counter plus a
failing `curl -6` only means you have no IPv6 to begin with, which proves nothing; say so.

`reject` on output (not `drop`) is deliberate: the ICMPv6 error is generated locally and never
reaches the wire, so `connect()` fails in milliseconds and Happy Eyeballs falls straight back
to IPv4. On **input** it is `drop`, because a reject would emit a packet from your real IPv6
address — a beacon.

### 7.6 Verify DNS — the Arch question

This is the reason the whole exercise exists. Do all four.

**A. The daemon's own verdict (authoritative).**

```bash
sudo journalctl -u urnetworkd -b --no-pager | grep -E '\[tun\] up|\[filter\] connected|\[tunnel\] up'
```

* `dns_applied=1` / `dns_pinned=1` / `dns=1` → DNS is on the tunnel. **Expected on BOTH
  tracks now** — Track B via tier 1, Track A via tier 3. Record which tier `--diagnose` said.
* `dns_applied=0` / `dns_pinned=0` / `dns=0` **on a session that is up** → should not be
  reachable unless `URNETWORK_ALLOW_UNPROTECTED_DNS=1` is set. **Report it.**
* **No session at all, with `dns_apply_failed`** → all three tiers failed. This is the
  fail-closed refusal working. The `dns_detail` string lists every tier tried and why each
  one did not take — capture it verbatim, it is the most useful single line you can send us.

**B. The reason, in the daemon's own words.** The journal prints the flags but not the
`dns_detail` string; that lives on the control socket. `hello` is mandatory and the SDK
version must match **exactly** (from §5's `--version`):

```bash
SDK=<paste the sdk version from `urnetworkd --version`>
printf '{"verb":"hello","id":1,"protocol_version":1,"sdk_version":"%s"}\n{"verb":"status","id":2}\n' "$SDK" \
  | sudo socat - UNIX-CONNECT:/run/urnetwork/control.sock
```

Read-only; it starts and stops nothing. The second reply line carries the whole status object.
The fields to quote in your report:

```
"dns_applied": false,
"dns_detail":  "systemd-resolved (resolvectl) is not installed: DNS is NOT going through the tunnel",
"routes_installed": true, "egress_protected": true, "ipv6_blocked": true,
"kill_switch": "off", "tunnel_interface": "urnet0", "rpc_pinned": true
```

The two `dns_detail` strings that distinguish the §0.2 failure modes:

* `systemd-resolved (resolvectl) is not installed: DNS is NOT going through the tunnel` → mode 1.
* `resolvectl dns: exit <N> ...` → mode 2 (binary present, service not running).
* `systemd-resolved accepted the override but does not report <ip> on urnet0` → a third mode:
  resolved took the command and then did not honour it. If you see this one, say so — it is
  not in the known-defect list.

If `socat` refuses with a `hello` error, the SDK string is wrong; re-read `--version`.

**C. Where are queries actually going.**

```bash
cat /etc/resolv.conf
resolvectl status urnet0 2>&1 | head -20      # meaningful only if resolved is running
sudo nft list chain inet urnetwork urnw_out | grep -c 'dport 53'
```

* With `dns_applied=1`, `urnw_out` contains the pinned pair
  (`oifname "urnet0" ip daddr { <resolver> } udp dport 53 counter accept`) **and** the
  `udp dport 53 counter reject` / `tcp dport 53 counter reject` closers. The `grep -c` will be
  ≥ 6.
* With `dns_applied=0`, **none of those rules exist** and the count is `0`. Off-tunnel `:53`
  is wide open. If `/etc/resolv.conf` names `192.168.x.1` (or any RFC1918 address), those
  queries also bypass the tunnel entirely, because the capture set deliberately excludes
  RFC1918.

**D. Optional but conclusive — watch a query leave.**

```bash
sudo tcpdump -ni any 'port 53' -c 5 &
dig +short arch-leak-check.example.com
```

Read the destination address of the query. If it is your router or an ISP resolver reached
over your **physical** interface (not `urnet0`), the leak is demonstrated on the wire. Include
the tcpdump lines (they contain no personal data beyond your LAN addresses; redact if you
prefer).

**E. The end-user-visible version.** Open <https://dnsleaktest.com> in a browser and run the
**extended** test. Report which resolvers it names, and whether they are your ISP's.

> **TODO(tester):** report `dns_applied`, the exact `dns_detail` string, the contents of
> `/etc/resolv.conf`, and the dnsleaktest result — separately for Track A and Track B if you
> run both.

---

## 8. Step 4 — the kill switch

Two orderings, and **FROM SOURCE** they behave differently. Test both.

### 8.1 Kill switch ON before connecting

Disconnect. Enable the kill switch in the app's settings. Connect.

* **Track B (resolved running):** expect a normal connect, plus
  `[filter] connected (floor=1 ipv6_blocked=1 dns_pinned=1 ...)` and
  `[filter] the block floor is in force. If this daemon dies while armed the machine stays
  blocked; recover with: sudo nft delete table inet urnetwork`.
* **Track A (no resolved):** expect the **same normal connect as Track B**, because tier 3
  should have applied DNS. The kill switch no longer changes the DNS outcome — the refusal is
  unconditional and happens at bring-up whenever no tier can apply DNS, kill switch or not.
  If Track A is refused here, that means tier 3 failed: send the `dns_detail` line.

### 8.2 Kill switch toggled ON while already connected

Connect with the kill switch **off** (Track A). Then toggle it **on** in the app.

`SetKillSwitch` re-applies the `Connected` ruleset with the floor. The DNS floor itself is
re-derived from the live `dns_applied` on every install, so with tier 3 holding, the pinned
`:53` rules should be present and nothing should break. **The case worth hunting** is
`dns_applied=0` + kill switch on: the LAN permit sits above the block-all, so a router
resolver at `192.168.x.1` would be permitted. That should now be unreachable without the
escape hatch — if you can reach it, that is a real finding. Then:

```bash
sudo nft list chain inet urnetwork urnw_out | sed -n '1,40p'
getent hosts example.com ; echo "exit=$?"
dig +short example.com   ; echo "exit=$?"
cat /etc/resolv.conf
```

Predicted outcomes, both of which are bugs:

* `/etc/resolv.conf` → a **LAN** address (`192.168.x.1`): resolution still works, and it is
  still leaking, **with the kill switch armed**, permitted by
  `ip daddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 } counter accept`.
* `/etc/resolv.conf` → a **public** resolver (`1.1.1.1`, an ISP address): the floor rejects
  it and **name resolution stops entirely** while the UI still says Connected.

Report which one you got — or that neither happened, which would mean the prediction is wrong
and is equally worth knowing.

### 8.3 The armed-crash path (only if you are comfortable, and only with §1 open)

With the kill switch armed and a tunnel up:

```bash
sudo systemctl kill -s SIGKILL urnetworkd
```

Expected: the machine **stays blocked** (that is the point), `/run/urnetwork/kill-switch-armed`
still exists, systemd restarts the daemon after ~2 s, and it comes back **armed** via a single
atomic `nft -f` — no open window. The journal should say
`[daemon] this machine is BLOCKED by a kill-switch floor carried over from a daemon that died
while armed.`

Get back out with the app (toggle the kill switch off) or with §1. Confirm afterwards:

```bash
sudo nft list table inet urnetwork 2>&1 | head -2   # expect: No such file or directory
ip -4 rule show                                      # expect: no 32763 rule
ls /run/urnetwork/kill-switch-armed 2>&1             # expect: No such file or directory
curl -4 -s https://api.ipify.org ; echo              # expect: your real IP again
```

---

## 9. Step 5 — the AppImage channel, on its own terms

### 9.1 If it will not run

```bash
~/.local/lib/urnetwork/URnetwork.AppImage
```

| Error | Cause | Fix |
|---|---|---|
| `dlopen(): error loading libfuse.so.2` / `AppImages require FUSE to run` | Arch ships **fuse3**; type-2 AppImages need **libfuse2**. | `sudo pacman -S fuse2`, or run once with `--appimage-extract-and-run` to prove the payload itself is fine. |
| `Permission denied` | Not executable. | `chmod +x` |
| GTK/GL errors, blank or corrupted window | The bundled GTK4 stack vs. Arch's drivers. | Try `GSK_RENDERER=cairo ~/.local/lib/urnetwork/URnetwork.AppImage` — that is the documented user-side fallback for broken GL stacks. Report whether it was needed. |
| Missing GSettings schema abort | Should be impossible; the AppRun points `GSETTINGS_SCHEMA_DIR` at the bundle. | Report verbatim. |

Useful debugging split:

```bash
~/.local/lib/urnetwork/URnetwork.AppImage --appimage-extract-and-run   # bypasses FUSE
```

If it works extracted but not mounted, the problem is FUSE, not the app.

### 9.2 What to record about the AppImage on Arch

* Does it start? With or without `fuse2`?
* Does the tray icon appear? (Needs a `StatusNotifierWatcher`; on a plain GNOME session there
  may not be one. Not a defect by itself — note the desktop environment.)
* Does the window render correctly (fonts, icons, the globe on the Connect page)?
* Does `GSK_RENDERER=cairo` change anything?
* Anything on stderr, verbatim.

The AppImage bundles its GTK4/libadwaita/glib closure and prepends `$APPDIR/usr/share` to
`XDG_DATA_DIRS`. It deliberately does **not** set `GTK_THEME` or `GDK_BACKEND`, so it should
follow your session (Wayland stays Wayland).

---

## 10. Step 6 — the Flatpak channel

### 10.1 Get it

**If the release has `URnetwork-<VERSION>-amd64.flatpak`:**

```bash
V=2026.8.16-1020679030-beta                  # if this shell does not already have it
flatpak install --user ~/urnetwork-test/URnetwork-$V-amd64.flatpak
```

**If it does not** (see §3 — the asset is attached after the release is created and may be
absent), build it from the checkout. This takes a while and pulls the GNOME 49 runtime:

```bash
sudo pacman -S --needed flatpak flatpak-builder
flatpak remote-add --if-not-exists --user flathub https://dl.flathub.org/repo/flathub.flatpakrepo
git clone https://github.com/Ryanmello07/urnetwork-linux.git
cd urnetwork-linux
./packaging/make-flatpak.sh --install
```

### 10.2 Run it

```bash
flatpak run com.bringyour.network
```

### 10.3 What is different from the AppImage — and what to test because of it

**The Flatpak is GUI-only. It contains no daemon and cannot contain one:** a Flatpak sandbox
has no `/dev/net/tun`, no `CAP_NET_ADMIN` and no way to install a system unit. It talks to the
**host's** `urnetworkd` — so §6 (the tarball) is a hard prerequisite for the Flatpak, exactly
as it is for the AppImage.

| | AppImage | Flatpak |
|---|---|---|
| Runtime stack | Bundled GTK4 from Ubuntu 24.04 | `org.gnome.Platform//49` |
| Host FS access | Full (runs as you, unsandboxed) | Sandboxed. `/run/urnetwork` is bind-mounted **read-only**; that is the only host path it gets. |
| Needs `fuse2` | Yes | No |
| Reaches the daemon via | `/run/urnetwork/control.sock` directly | The same socket, through the sandbox bind mount |
| Group membership | You must be in `urnetwork` | **Same** — the sandbox runs as your uid, so the group check applies identically |
| Portals | Not used | `OpenURI` for links, `Background` for autostart, `GeoClue2` on the system bus |
| Tray | `StatusNotifierWatcher` on the session bus | Same, via `--talk-name` |

**The Flatpak-specific failure to look for.** The daemon's unit sets
`RuntimeDirectoryPreserve=yes` specifically because of this channel: when
`/run/urnetwork` is recreated, a **running** sandbox is left holding a mount of the dead
inode, and every `connect()` fails with `EACCES` — which historically got reported to the user
as a group-membership problem. The app now detects this and says so instead. To test it:

```bash
flatpak run com.bringyour.network &          # leave it running
sudo systemctl restart urnetworkd
# now press Connect in the Flatpak GUI
```

Expected: it still works (the directory is preserved). If instead the app claims it is not
allowed to use the service, or reports a stale-sandbox condition, capture:

```bash
grep urnetwork /proc/$(pgrep -x urnetwork-gui | head -1)/mountinfo
```

A `/urnetwork//deleted` entry there is the smoking gun. Report it.

**Also check:** whether the Flatpak can perform the browser sign-in (it goes through the
`OpenURI` portal, not `xdg-open`), and whether the tray icon behaves the same as the
AppImage's.

---

## 11. Collecting evidence for a bug report

Run this whole block and attach the output. Everything in it is **read-only**.

```bash
OUT=~/urnetwork-test/evidence-$(date +%Y%m%d-%H%M%S).txt
{
  echo "=== host ==="
  uname -a; cat /etc/os-release; getconf GNU_LIBC_VERSION
  stat -fc %T /sys/fs/cgroup; ls -ld /lib; findmnt -no OPTIONS --target /usr
  echo "=== packages ==="
  pacman -Q nftables iproute2 systemd fuse2 flatpak geoclue desktop-file-utils 2>&1
  echo "=== versions ==="
  /usr/lib/urnetwork/urnetworkd --version
  echo "=== diagnose ==="
  /usr/lib/urnetwork/urnetworkd --diagnose
  echo "=== unit ==="
  systemctl status urnetworkd --no-pager -l
  systemctl cat urnetworkd | head -5
  echo "=== daemon journal (this boot) ==="
  sudo journalctl -u urnetworkd -b --no-pager
  echo "=== routing ==="
  ip -4 rule show
  ip -4 route show table 51821
  ip -4 route show default
  ip -s link show urnet0 2>&1
  echo "=== nftables ==="
  sudo nft list table inet urnetwork 2>&1
  echo "=== dns ==="
  ls -l /etc/resolv.conf; cat /etc/resolv.conf
  systemctl is-active systemd-resolved 2>&1
  resolvectl status 2>&1 | head -40
  echo "=== labels (stat, NOT ls -Z) ==="
  stat -c '%n %U:%G %a %C' /usr/lib/urnetwork/urnetworkd /usr/bin/urnetwork \
      /run/urnetwork /run/urnetwork/control.sock 2>&1
  echo "=== groups ==="
  getent group urnetwork; id -nG
} > "$OUT" 2>&1
echo "wrote $OUT"
```

Plus, separately:

* **The full `--selftest-egress` output and its exit code** (§5.2). Always.
* **The app's own log** — the `connect:` lines. From a terminal launch that is `gui.log` from
  §7.1; otherwise `journalctl --user -b | grep -E 'urnetwork|connect:'`, or for the Flatpak,
  `flatpak run com.bringyour.network 2>&1 | tee flatpak-gui.log`.
* **The installer's full output** (§4 dry-run and §6 real run).

**Use `stat -c %C`, never `ls -Z`.** On a machine with no SELinux, `ls -Z` prints `?` and
some coreutils builds fail outright, so a `ls -Z` transcript from Arch tells us nothing and
looks like an error. `stat -c '%n %U:%G %a %C'` gives owner, group, mode and (if any) context
in one line and degrades gracefully.

**Redact before sending:** your public IPv4 (the `curl ipify` outputs), your LAN addressing if
you would rather not share it, and anything account-shaped in the GUI log. Client ids and
tunnel addresses (`169.254.x.x`) are fine and are useful to us.

---

## 12. Cleaning up

```bash
# disconnect in the app FIRST, then:
sudo systemctl stop urnetworkd
sudo /usr/lib/urnetwork/uninstall.sh          # add --purge to also remove state + group
rm -f ~/.local/lib/urnetwork/URnetwork.AppImage
flatpak uninstall --user com.bringyour.network
```

Then confirm the machine is back to normal:

```bash
sudo nft list table inet urnetwork 2>&1 | head -2   # expect: No such file or directory
ip -4 rule show                                      # expect: 0 / 32766 / 32767 only
ip -4 route show table 51821                         # expect: nothing
curl -4 -s https://api.ipify.org ; echo              # expect: your real IP
cat /etc/resolv.conf
```

The uninstaller prompts before removing `/var/lib/urnetwork`, `/etc/urnetwork` and the
`urnetwork` group. It removes `/etc/geolocation` unconditionally **if URnetwork wrote it** —
leaving a faked location behind would itself be a bug.

---

## 13. The result sheet — please fill this in

| # | Question | Result | Evidence |
|---|---|---|---|
| 1 | `--selftest-egress` verdict + exit code | | full output |
| 2 | Installer layout: standard or immutable? | | §4 dry-run |
| 3 | Unit loadable (`systemctl cat urnetworkd`) through the `/lib` symlink? | | §6.2 |
| 4 | SELinux/AppArmor block skipped silently? | | §6.1 |
| 5 | `nftables` / `iproute2` present out of the box? | | §2.1 |
| 6 | Track A: which **tier** did `--diagnose` report, and did tier 3 actually hold? | | §5.2, §7.6 A/B |
| 7 | Track A: does dnsleaktest show your ISP (it should NOT, now that tier 3 applies)? | | §7.6 E |
| 8 | Track B (resolved enabled): tier 1, `dns_applied=1`? | | §7.6 A |
| 9 | Kill switch ON before connect on Track A — connects normally now? | | §8.1 |
| 10 | Kill switch toggled ON while connected — DNS still pinned? | | §8.2 |
| 10b | After disconnect, is `/etc/resolv.conf` **byte-identical** to what you recorded in §2.2 (incl. symlink vs file)? | | §0.2 |
| 10c | Did the session ever drop with *"Your DNS stopped going through the tunnel"*? (NetworkManager rewrite) | | §0.3 |
| 11 | Public IP changed; both tun counters moving | | §7.4 |
| 12 | Exactly 31 routes in table 51821 | | §7.4 |
| 13 | IPv6 blocked, with a non-zero reject counter | | §7.5 |
| 14 | AppImage runs? `fuse2` needed? `GSK_RENDERER=cairo` needed? | | §9 |
| 15 | Flatpak runs and reaches the host daemon? Survives a daemon restart? | | §10 |
| 16 | Recovery (`sudo nft delete table inet urnetwork`) works as described? | | §1, §8.3 |

---

## Appendix — reference values

| Thing | Value | Source |
|---|---|---|
| tun interface | `urnet0` | `TunnelConfig::name` |
| tun MTU | `1440` | `TunnelConfig::mtu` |
| fallback tun address | `169.254.2.1/24` (the device usually supplies its own) | `TunnelConfig::local_addr` |
| egress fwmark | `0x55524e57` | `kEgressMark` |
| capture route table | `51821` | `kTunnelRouteTable` |
| policy rule priority | `32763` | `kFwmarkRulePriority` |
| capture prefixes | 31 IPv4 prefixes, RFC1918 excluded | `CaptureV4Prefixes()` |
| nft table | `inet urnetwork` | `kNftTableName` |
| nft chains | `urnw_mark_out`, `urnw_probe`, `urnw_out`, `urnw_in`, `urnw_fwd` | `Tunnel.hpp` |
| nft counters | `urnw_out_total`, `urnw_out_daemon`, `urnw_probe_tun`, `urnw_probe_phy` | `Tunnel.hpp` |
| LAN permit set | `{ 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 }` | `kLanV4Set` |
| control socket | `/run/urnetwork/control.sock`, dir `0750 root:urnetwork` | `ControlProtocol.hpp` |
| device RPC | `127.0.0.1:12026–12125`, mTLS-pinned per session | `ControlProtocol.hpp` |
| armed marker | `/run/urnetwork/kill-switch-armed` | `Tunnel.cpp` |
| state / logs | `/var/lib/urnetwork` (0700), `/var/log/urnetwork` (0700) | unit + `main.cpp` |
| daemon binary | `/usr/lib/urnetwork/urnetworkd` (RPATH `$ORIGIN`) | `meson.build` |
| launcher | `/usr/bin/urnetwork` | `urnetwork-launcher` |
| egress witness re-check | every 30 s; two consecutive failures tear the tunnel down | `TunnelHost.cpp` |

**Related docs:** `docs/DISTRO-SUPPORT.md` (the cross-distro matrix) and
`packaging/distro-smoke.sh` (the automated smoke check) are owned by a separate workstream and
are being written in parallel with this file; read them alongside this one rather than instead
of it.
