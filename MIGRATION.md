# Linux migration to the APPIMAGE.md plan — implementation contract

Status: **in progress 2026-08-05.** This file is the coordination contract between the
three parallel workstreams (daemon split, packaging, build pipeline). It is normative:
where it names a path, a binary, an artifact filename or a wire verb, that name is
fixed and the other workstreams depend on it. Rationale lives in `APPIMAGE.md` §11 —
read that first; this is only the interface.

## Shape

```
  ┌─ unprivileged, desktop user ────────┐   ┌─ root, systemd ──────────────────┐
  │ urnetwork  (GUI, AppImage)          │   │ urnetworkd  (deb | install.sh)   │
  │   gtkmm4 + libadwaita               │   │   no GUI deps at all             │
  │   urnet::DeviceRemote ──────────────┼─ device rpc (loopback + mTLS) ──────▶│
  │   control client ───────────────────┼─ unix socket, SO_PEERCRED ──────────▶│
  └─────────────────────────────────────┘   │   urnet::DeviceLocal(rpc=true)   │
                                            │   Tunnel (/dev/net/tun, routes,  │
                                            │     resolvectl DNS)              │
                                            │   urnet::newIoLoop(device, fd)   │
                                            │   GeoClue /etc/geolocation write │
                                            └──────────────────────────────────┘
```

**Decision — device RPC stays on loopback TCP + mTLS (127.0.0.1:12025), mirroring
Windows.** `APPIMAGE.md` §11c lists a unix transport as the cleaner fix; it is an SDK
(Go) change touching every platform and the generated C ABI, so it is deliberately
deferred. The control socket is the real authorization boundary: it is unix-domain,
`SO_PEERCRED`-checked, and nothing starts a tunnel without passing it. Record this as
a known follow-up, do not silently pretend loopback is private.

## Installed paths (normative)

| Path | Owner | Notes |
|---|---|---|
| `/usr/lib/urnetwork/urnetworkd` | daemon pkg | the daemon binary |
| `/usr/lib/urnetwork/libURnetworkSdk.so` | daemon pkg | rpath `$ORIGIN` |
| `/usr/bin/urnetwork` | daemon pkg | **launcher script**, the stable `Exec=` target |
| `/lib/systemd/system/urnetworkd.service` | daemon pkg | `/lib`, in every release's load path |
| `/usr/share/applications/network.ur.urnetwork.desktop` | daemon pkg | filename **must** match `main.cpp`'s app id |
| `/usr/share/icons/hicolor/{48x48,256x256}/apps/urnetwork.png` | daemon pkg | |
| `/usr/share/urnetwork/world-110m.json` | daemon pkg | globe land outlines |
| `/usr/share/urnetwork/icons/urnetwork-tray-*.png` | daemon pkg | tray art |
| `/usr/share/locale/<l>/LC_MESSAGES/urnetwork.mo` | daemon pkg | gettext catalogs |
| `/etc/urnetwork/autostart/network.ur.urnetwork.desktop` | daemon pkg | **inert template**, GUI symlinks it |
| `/etc/NetworkManager/conf.d/95-urnetwork.conf` | daemon pkg | `unmanaged-devices=interface-name:urnet0` |
| `/etc/udev/rules.d/85-urnetwork-unmanaged.rules` | daemon pkg | `ENV{NM_UNMANAGED}="1"` |
| `~/.local/lib/urnetwork/URnetwork.AppImage` | **user** | never packaged; must be user-writable |
| `/run/urnetwork/control.sock` | daemon | dir `0750 root:urnetwork`, sock `0660` |

The GUI AppImage is **never** installed by a package. `/usr/bin/urnetwork` searches, in
order: `$URNETWORK_APPIMAGE`, `~/.local/lib/urnetwork/URnetwork.AppImage`,
`~/Applications/URnetwork*.AppImage`, `/usr/lib/urnetwork/URnetwork.AppImage`, then any
`urnetwork-gui` on `$PATH`; `exec`s the first hit with `"$@"`. If none is found it
prints a one-line install hint and exits 127.

## Artifact filenames (normative — the pipeline greps for these)

```
urnetwork-daemon_<version>_<arch>.deb          arch = amd64 | arm64
urnetwork-daemon-<version>-<arch>.install.tar.gz
URnetwork-<version>-<arch>.AppImage
URnetwork-<version>-<arch>.AppImage.zsync
```

`<version>` = `$EXTERNAL_WARP_VERSION`. The tarball's **single top-level directory** is
`urnetwork-daemon/`, containing `install.sh`, `uninstall.sh`, `VERSION`, and a
`payload/` tree mirroring the installed paths above.

### Packaging script names + invocation (pinned 2026-08-05)

The pipeline calls these three by exact path. **These names are normative**; the
original contract pinned only the output filenames, which left the pipeline guessing:

```
linux/packaging/make-deb.sh
linux/packaging/make-install-tarball.sh
linux/packaging/make-appimage.sh
```

Each is invoked with this environment and **must write its normative artifact
filename into `$OUT_DIR`**:

| Var | Meaning |
|---|---|
| `VERSION` | `$EXTERNAL_WARP_VERSION` |
| `ARCH` | `amd64` \| `arm64` |
| `STAGING_DIR` | the `meson install --destdir` tree |
| `OUT_DIR` | where the artifact must land |
| `APP_DIR` | `linux/app` |
| `SDK_DIR` | vendored `third_party/urnetwork-sdk/$ARCH` |

### Two ownership questions the pipeline cannot decide (settled here)

- **`libURnetworkSdk.so` is installed by meson, not the pipeline.** Under snapcraft
  this was the pipeline's job (`snapcraft.yaml`'s `override-build`), and that path is
  gone. `meson.build` is the single source of truth for layout — so it installs the
  vendored `.so` to `/usr/lib/urnetwork/libURnetworkSdk.so`. The AppImage bundles its
  own copy separately (workstream B); the two copies are expected and correct, and the
  `sdk_version` handshake below is what keeps them honest.
- **The app version must be threaded in at build time.** `meson.build` hardcodes
  `version : '0.0.1'` and nothing passes `$EXTERNAL_WARP_VERSION` in — the old
  pipeline stamped it into `snapcraft.yaml`, which no longer exists. Without a fix the
  `hello` reply's `daemon_version` and any about-box string report `0.0.1` forever.
  **Add a `-Dapp_version=` meson option** (default `0.0.0`), surface it as a compile
  define, and have the pipeline pass `$VERSION`.

## Control protocol (`src/ControlProtocol.hpp`, shared by both binaries)

- Transport: `AF_UNIX`/`SOCK_STREAM`, **newline-delimited JSON**, one request per line,
  one reply per line.
- `inline constexpr int kControlProtocolVersion = 1;` — **and unlike Windows, it is
  actually enforced.** `hello` carries `protocol_version` in **both** directions; the
  daemon rejects a client below `kMinSupportedClientProtocol`, the GUI rejects a daemon
  below `kMinSupportedDaemonProtocol`. Bump only on wire-format change.
- Auth: `SO_PEERCRED` on `accept()`, **before parsing any frame**. Allow uid 0 and
  members of the `urnetwork` group. Never authorize on pid. Never attempt peer-binary
  attestation (root cannot read an AppImage's FUSE mount — `APPIMAGE.md` §11c).

Verbs (request `{"verb":…,"id":N,…}` → reply `{"id":N,"ok":bool,…}`):

| Verb | Payload | Reply |
|---|---|---|
| `hello` | `protocol_version`, `sdk_version` | `protocol_version`, `sdk_version`, `daemon_version` |
| `status` | — | `tunnel_state`, `rpc_port`, `client_id`, `error` |
| `start_tunnel` | `by_jwt`, `instance_id`, `app_version` | `ok`, `rpc_port`, `instance_id`, `rpc_session_id` |
| `attach_tunnel` | `instance_id`, `rpc_session_id` | `ok`, `rpc_port`, `instance_id`, `rpc_session_id` |
| `stop_tunnel` | — | `ok` |
| `set_provide` | `mode` | `ok` |
| `location_override_available` | — | `available`, `reason` |
| `location_override_write` | `lat`, `lon`, `accuracy_m` | `ok` |
| `location_override_clear` | — | `ok` |

`attach_tunnel` re-adopts a tunnel that is already up by NAMING the live session
(`instance_id` + `rpc_session_id`) instead of re-describing it, and answers with the
same reply body `start_tunnel` does. It is authorized exactly as `start_tunnel` is —
`control-tunnel` for your own uid's tunnel, `take-over-tunnel` when the live tunnel
belongs to another uid. **On this fork it cannot succeed yet**: the daemon regenerates
the device-RPC mTLS material every session instead of persisting it, so
`status.rpc_session_id` is always empty and the verb answers
`rpc_session_not_persisted` — deliberately a different code from
`rpc_session_mismatch`, so a client can tell "this daemon does not do attach" from
"your saved credentials are stale". Reattachment meanwhile happens inside
`start_tunnel`, which adopts a live session whose pinning material matches byte for
byte.

**`sdk_version` must match EXACTLY, and this is a second, independent check.**
`protocol_version` guards *our* JSON control socket; the **device RPC has no version
negotiation at all** — `sdk/device_rpc.go`'s `DeviceRemoteSyncRequest` carries only
`InstanceId` (pairing, not versioning) and listener id lists. That was harmless on
every other platform because both halves ship in one artifact, so the two SDK copies
are byte-identical. **Linux is the first platform where they can drift**: the GUI
AppImage bundles its own `libURnetworkSdk.so` and self-updates via zsync, while the
daemon's copy updates via apt or `install.sh`.

The RPC is gob-encoded Go structs, and gob fails *quietly* in the cases that matter —
adding or removing a field is tolerated, but **a renamed field silently decodes as
zero** (a feature stops working with no error), and a changed *meaning* is invisible.
So: read each side's build with `urnet::version()`
(`sdk/cgo/include/urnetwork_sdk.hpp:17273`), exchange it in `hello`, and refuse a
mismatch **before constructing `DeviceRemote`**. Exact match is correct rather than a
floor — both halves come out of the same pipeline stamped `$EXTERNAL_WARP_VERSION`, so
any difference is a genuinely mismatched pair. Keep the two checks separate:
`protocol_version` bumps only on wire-format change, `sdk_version` must simply agree.

The GUI must render **"daemon unreachable"**, **"daemon too old"**, and **"GUI/daemon
SDK builds differ"** as distinct, actionable states — never a blank or a zero.

## Workstream ownership (do not cross these lines)

| Workstream | Owns | Must not touch |
|---|---|---|
| **A — daemon split** | `linux/app/src/**`, `linux/app/meson.build`, `linux/app/tests/**`, `po/POTFILES.in` | `packaging/**`, `scripts/**`, `build/**` |
| **B — packaging** | `linux/app/packaging/**`, `linux/app/scripts/**`, `linux/packaging/**` | `src/**`, `meson.build`, `build/**` |
| **C — build pipeline** | `build/all/linux/**`, `build/all/build-linux.sh`, `build/all/run.sh` | everything in `linux/` |

B consumes A's output only through `meson install --destdir`, so B never needs to read
`meson.build`. C consumes B's output only through the artifact filenames above.

### Static integration files live in `app/packaging/` ONLY (resolved 2026-08-05)

The systemd unit, desktop file, autostart template, NM `conf.d` snippet, udev rule and
hicolor icons are **shared** between the two halves: `meson.build` installs them *and*
`linux/packaging/lib/common.sh` copies them straight out of `app/packaging/` when
assembling the package root. The ownership split above briefly produced **two copies**
— one under `app/src/dist/` for meson, one under `app/packaging/` for the assembly —
and they diverged before anyone noticed: the two `urnetworkd.service` files disagreed
on `Type=notify` vs `Type=exec` (the daemon implements `READY=1`, so `Type=exec` made
that dead code and marked the unit ready before the control socket existed) and on
whether the unit orders against `network-pre.target` at all.

`app/src/dist/` is deleted. **`app/packaging/` is the single source**; meson installs
from there. If you add an integration file, add it in one place. This is the failure
mode parallel workstreams produce most reliably — two plausible copies, no error, and
the wrong one ships.

## Verification floor

Nothing here can be fully verified on macOS — the app needs GTK4 and the SDK is a Linux
ELF. What **must** pass before calling a workstream done:

- **A**: `meson setup` configures; the pure-logic test binary (`meson test`, 52 cases)
  still passes; every new pure module is testable without GTK/SDK and has tests.
  Homebrew has gtkmm-4.0/libadwaita/nlohmann_json, so most objects do compile here —
  `src/Tunnel.cpp` fails on `linux/if.h` on macOS both before and after, which is
  expected and pre-existing.
- **B**: every generated script passes `shellcheck` and `bash -n`; the deb builds with
  `dpkg-deb` **or** is produced by `nfpm`; the tarball extracts to exactly one top
  level dir; `install.sh --dry-run` runs on macOS without touching anything.
- **C**: `bash -n` on every changed script; the artifact-name globs match the names
  above exactly; `run.sh` keeps its existing non-blocking `if …; then upload; else
  warn; fi` shape so a flaky desktop build cannot sink a mobile release.
