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
| `/usr/bin/urnetwork-exclude` | daemon pkg | per-app split tunnel launcher (`urnetwork-exclude <command>`) |
| `/lib/systemd/system/urnetworkd.service` | daemon pkg | `/lib`, in every release's load path |
| `/usr/share/applications/com.bringyour.network.desktop` | daemon pkg | filename **must** match `main.cpp`'s app id |
| `/usr/share/icons/hicolor/{48x48,64x64,128x128,256x256,512x512}/apps/com.bringyour.network.png` | daemon pkg | one 1024 master, downscaled |
| `/usr/share/urnetwork/world-110m.json` | daemon pkg | globe land outlines |
| `/usr/share/urnetwork/icons/urnetwork-tray-*.png` | daemon pkg | tray art |
| `/usr/share/locale/<l>/LC_MESSAGES/urnetwork.mo` | daemon pkg | gettext catalogs |
| `/etc/urnetwork/autostart/com.bringyour.network.desktop` | daemon pkg | **inert template**, GUI symlinks it |
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
urnetwork-daemon_<version>_<arch>.deb              arch = amd64 | arm64
urnetwork-daemon-<version>-<arch>.install.tar.gz
urnetwork-daemon-<version>-<pacmanarch>.pkg.tar.zst   pacmanarch = x86_64 | aarch64
URnetwork-<version>-<arch>.AppImage
```

No `.AppImage.zsync` sidecar: the AppImage embeds no update information (GitHub
Releases cannot serve zsync's multi-range requests), and the GUI updates itself
through the in-app checker (`app/src/UpdateChecker.cpp`, README "Updates").

The pacman package is named with pacman's own arch spelling, for the same reason the
`.rpm` is named with rpm's: a package whose filename disagrees with the arch in its own
metadata is the confusing artifact. Its `<version>` is the release version verbatim —
pacman does not parse filenames (`pacman -U ./file` reads `.PKGINFO`), so the file is
free to be named for the release while the metadata carries the folded, pacman-legal
`pkgver` (see `pkg_fields()` in `packaging/make-arch.sh`). `make-arch.sh` prints the
canonical `<pkgname>-<pkgver>-<pkgrel>-<arch>.pkg.tar.zst` on every build and emits it
instead under `UR_ARCH_CANONICAL_NAME=1`, which is what a `repo-add` repository expects.

**The `.rpm` still has no row here.** rpm forbids `-` in both Version and Release while
this line's `<version>` contains two, so no legal rpm filename can carry the version
string verbatim. Its asset name is therefore settled by the release workflow rather
than by this contract, and that remains an open item.

`<version>` = `$EXTERNAL_WARP_VERSION`. The tarball's **single top-level directory** is
`urnetwork-daemon/`, containing `install.sh`, `uninstall.sh`, `VERSION`, and a
`payload/` tree mirroring the installed paths above.

### Packaging script names + invocation (pinned 2026-08-05)

The pipeline calls these by exact path. **These names are normative**; the
original contract pinned only the output filenames, which left the pipeline guessing:

```
linux/packaging/make-deb.sh
linux/packaging/make-install-tarball.sh
linux/packaging/make-appimage.sh
linux/packaging/make-rpm.sh       (added later; Fedora/RHEL/openSUSE)
linux/packaging/make-arch.sh      (added later; Arch/CachyOS/EndeavourOS/Manjaro)
```

The two later ones take the SAME four-variable environment as the original three and
are deliberately not special: each is one more wrapper around the single staging tree,
so the daemon inside every package is the same bytes by construction rather than by
review. The pipeline may treat either as optional (`UR_REQUIRE_RPM` /
`UR_REQUIRE_ARCH_PKG`) — a new package that cannot build must never take the
already-contracted assets off a release with it.

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
| `status` | — | `tunnel_state`, `rpc_port`, `client_id`, `error`, `stop_reason`, `failsafe_armed`, `provider_running`, `provider_mode`, `provider_client_count`, `network_country_code`, `log_upload_id`, `log_upload_state`, `log_upload_carrier` |
| `start_tunnel` | `by_jwt`, `instance_id`, `app_version` | `ok`, `rpc_port`, `instance_id`, `rpc_session_id` |
| `attach_tunnel` | `instance_id`, `rpc_session_id` | `ok`, `rpc_port`, `instance_id`, `rpc_session_id` |
| `stop_tunnel` | — | `ok` |
| `set_provide` | `mode` | `ok` |
| `start_provider` | `by_jwt`, `instance_id`, `app_version`, `network_space_json`, `provide_mode`, `provider_transport_settings_json` | `ok` + the status |
| `provider_stats` | `poll_status` | `running`, `has_provider_stats`, `provider_throughput_points_json`, `provider_transport_distribution_json`, `status_open`, `status_loaded`, `status_last_fetch_error`, `provider_status_json`, `extender_provide_status_json`, `extender_throughput_points_json`, `provide_extender`, `provide_extender_writable` |
| `set_provide_extender` | `provide_extender` | `ok` |
| `reset_extenders` | `host_name`, `env_name`, `extender_reset_id`, `interactive` | `ok`, `reset` |
| `location_override_available` | — | `available`, `reason` |
| `location_override_write` | `lat`, `lon`, `accuracy_m` | `ok` |
| `location_override_clear` | — | `ok` |
| `upload_logs` | `feedback_id`, `by_jwt`, `instance_id`, `app_version`, `network_space_json` | `ok`, `carrier`, `upload_id` |
| `logout` | `network_space_json` | `ok` + the status |

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

`start_provider` keeps providing while the user is disconnected (support inbox 1521):
the daemon builds a provider-only `DeviceLocal` from the persisted identity and the
request's credentials, network space and provide mode, with no tun, no capture routes,
no DNS change, no nftables change, no egress marker and no device-RPC listener, so the
machine's own traffic is routed as if URnetwork were not running. It is refused while a
tunnel session exists (`tunnel_session_active`) or is being built (`start_in_progress`),
while the kill-switch floor is armed (`kill_switch_armed`), and for a mode that does
not provide while disconnected (`provide_mode_off`; `app/src/ProvideLifecycle.hpp` is
the rule both halves apply). Every teardown, and so every `start_tunnel`, retires it
first; `set_provide` with a mode that does not provide retires it; `stop_tunnel` stops
it with everything else. It is authorized like `set_provide`, without a prompt, because
the GUI's health poll sends it as well as Disconnect. A request that differs from the
running device's (`ctl::SameProviderDevice`) replaces it, which is how a saved network
space value (the bootstrap DoH servers, VLESS, the private extender) reaches a running
provider: the GUI sends `start_provider` again when one is saved. `status` publishes the
device: `provider_running`, its control mode, live tier and network key, and
`provider_client_count`, its connected network peers (the count a tunnel session's device
gives the GUI), which the provide line ("Providing to N clients") reads while
disconnected. The count is -1 until it is read, and absent from a daemon that predates it,
which parses -1 too; the GUI then shows no count rather than 0.

`provider_stats` is what the GUI's provider statistics read while there is no tunnel
session, because the provider-only device has no `DeviceRemote`: the daemon runs the
SDK's contract and provider status view controllers on that device and answers with its
provider series and transport distribution (the Earnings plots and the "no traffic yet"
line), whether it reports provider packet stats (the plots' gate), its provider
status (the reason line, the demand histogram and "Why?"), and its extender role's
status and series (the read-only extender row, the running state behind the extender
statistics, and their plot). The SDK payloads travel as the SDK's own JSON. It is
polled about once a second while the Earnings destination is on screen, so it is
answered like `status`: never gated, and empty for a caller whose status would be
redacted. `poll_status` keeps the provider status controller polling
`GET /network/provider-status`; the daemon stops it 15 s after the last request that
asked, so nothing polls the API once no GUI shows it. A daemon that predates the verb
answers `unknown verb`, and the GUI then shows no provider statistics while
disconnected, as before; one that predates the two extender fields sends neither, and
the GUI keeps the extender row and plot hidden while disconnected, as before.

`upload_logs` is "send feedback with logs" with the tunnel down (support inbox 2090). The
logs support reads are the daemon's (the SDK's `UploadLogs` zips the glog files of the
process it runs in), and the GUI's `DeviceRemote` reaches the daemon's `DeviceLocal` only
while a tunnel session runs, so a report sent while disconnected, held by the kill switch
or failing to connect carried none. The GUI now asks the daemon, after the server has
accepted the feedback and only when the box is ticked, with the server's `feedback_id`
and the credentials `start_provider` carries. The daemon calls the SDK's `UploadLogs` on
the tunnel session's device, else on the provider-only device, else on a standalone
device built for the upload exactly as the provider-only device is (provide mode never,
no tun, routes, DNS, nftables or listener), retired once the upload reports, after 30
minutes, or before any other device under the identity is built; a request that finds a
bring-up running is queued until it ends. `carrier` names the device (`tunnel`,
`provider`, `standalone`, `queued`), and the daemon writes it into the uploaded log. The
daemon answers once the upload is admitted: the SDK's call zips the log directory, up to
the upload's cap read from disk, so it runs on a thread of its own and never on the main
loop that serves every other request, the reaper and the kill switch; the device it runs
on stays alive until the call returns. One upload at a time: a request while one is in
flight (queued or running) is refused with `log_upload_busy`, and the GUI does not fall
back for it, since the server would refuse a second upload. The outcome reaches the GUI
through `status`: `log_upload_id` is the reply's `upload_id`, and `log_upload_state` says
where that upload is (`queued`, `running`, `uploaded`, `refused`, `failed`; an upload that
never reports is failed after 30 minutes). The GUI follows it from its health poll and logs
it. These status fields are additive within v1 (absent parses 0 and "") and dropped from a
redacted status. The upload itself is the SDK's, unchanged: the same zip,
`POST /log/{feedback_id}/upload`, the server's 100 MB cap and its one upload per network
per 5 minutes; the server keeps one file per feedback, so the GUI's own logs do not ride
along. It is gated like `log_tail`:
`read-log`, without a prompt, and refused (`auth_not_tunnel_owner`) when the log belongs
to another uid. A daemon that predates the verb answers `unknown verb`, and the GUI falls
back to the `DeviceRemote`'s `UploadLogs` while a tunnel session is bound, as before.

`status.network_country_code` is the country of the mobile network this machine is on
(P052): the daemon reads it from ModemManager on the system bus while the default
route leaves through a registered modem's data interface, maps the operator's MCC to an
ISO 3166-1 alpha-2 code, and reports `""` on every other network (Wi-Fi, ethernet, a
tethered phone, no ModemManager). The locale and the timezone are never used. The daemon
applies it with `urnet::setNetworkCountryCode` before it builds a device and in place
after, so the extender dials of the tunnel's device and the provider-only device front
with that country's spoof list while the extender hint cannot be fetched; the GUI
applies the same value to its own process from its health poll. Absent (an older
daemon) parses `""`, and a redacted status carries none. ModemManager is asked only
while its bus name has an owner and never with auto-start, so a disabled ModemManager
stays stopped.

The daemon ends a tunnel that is up but carries nothing (the dead-tunnel failsafe,
`app/src/TunnelWatchdog.hpp`, Windows' TunnelWatchdog). A thread of its own asks the
session's device for its proven exits every 2 s and tells it of a network change, so the
main loop makes no call on that device, and the reaper reads the tun's packet counters
once a second: 8 or more packets in, nothing back and no proven exit for 20 s, no proven
exit for 90 s, or no completed answer from the SDK for 30 s ends the session, unless a
packet came back out of the tunnel in the last 20 s. A suspend or a stalled daemon
rebases every window, and a new destination generation gets its own clocks while its
window forms. The session ends as the other protective teardowns do: the armed floor
when the kill switch was asked for, no table otherwise, and nothing reconnects. An
account out of balance is never stopped by the first two rules: the sampler also reads
`ContractStatus.InsufficientBalance` off the same device, and from the first refusal
until a contract goes through the traffic stays captured, the app's held alert and
notification tell the user, and `failsafe_armed` stays false for them. The 30 s rule
stays, since a hung SDK is not about traffic (`docs/linux_agent_help.md` 6.4). The
veto has a known hole (`docs/linux_agent_help.md` 6.4, task #44): the tun's counters
cannot tell a provider's packet from one the SDK writes itself, and the SDK answers DNS
on the tun and resets DoT locally, so a machine whose apps keep resolving can keep a
dead tunnel up; closing it needs an SDK count of the packets no provider sent.
`status.tunnel_state` is `error`, `stop_reason` names the rule (`failsafe_no_inbound`,
`failsafe_no_exit`, `failsafe_sdk_unresponsive`; `ctl::IsFailsafeStop`), `error_code`
is `tunnel_dead` and `error` says whether the machine is now blocked.
`status.failsafe_armed` is true while a countdown on the live session is within 30 s of
firing, so the GUI can warn first. Both are additive within v1 (an older daemon sends
neither, and `failsafe_armed` parses false), and a redacted status carries no countdown.

For `failsafe_sdk_unresponsive` the daemon cannot finish the teardown: every call left on
the session's device would wait on the lock the SDK is stuck behind. It lands the machine
as above, writes the stop to `/run/urnetwork/last-stop.json` and exits with status 75
(`app/src/daemon/SelfRestart.hpp`); `Restart=on-failure` starts a clean daemon 2 s later,
which publishes that stop as its `status` (`tunnel_state` `error`, the same
`stop_reason`, `error_code` and `error`) until the next start, and deletes the file. The
GUI sees its control connection drop and come back, as after any service restart. What
is left: a stop that reaches a wedged device before the failsafe's 30 s verdict (a
Disconnect, the IoLoop's death, the daemon's own SIGTERM) still waits on it on the main
loop, since the bounded, abandonable SDK teardown of `docs/linux_agent_help.md` 6.5 is
not ported.

`reset_extenders` is Account > Extenders' Reset extenders (connect `EXTENDER.md` E7). The
GUI resets its own network space with the SDK's `NetworkSpace::resetExtenders`, which
clears what the space learned about extenders and what the user added (manual hosts, the
private extender, the dns name, gossip url and root key overrides), persists the cleared
values with the reset's id, and restarts the space's extender client and node. It then
sends the space's key (`host_name`, `env_name`) and that id, and the daemon applies the
same reset to the space it holds under the key (`applyExtenderReset`). The tunnel
session's device and the provider-only device both run in that space, so one call covers
both; their live extender paths keep running, and new dials draw from the fresh directory.
`reset` is false when the daemon holds no space for the key (no device has run) or the
space applied that reset, or a newer one, already. The verb only makes the reset
immediate: the id travels in the space's values, so a daemon that was not reachable or
never asked applies it at its next `start_tunnel` or `start_provider` import, and an id
the space applied already changes nothing. A daemon whose session a bring-up owns refuses
it (`start_in_progress`, never waited behind), and that bring-up imports the space as it
was before the reset, so the GUI sends the same request again, once, when its health
poll reads a status that shows the bring-up settled. Nobody pressed anything for that
one, so it carries `interactive: false` and never raises a dialog: beside another uid's
live session, which only `take-over-tunnel` reaches, the daemon refuses it before any
check (`auth_not_tunnel_owner`), and otherwise it checks it without interaction and
refuses (`auth_required`) where a dialog would be needed. The GUI then drops it, and the
next import applies the id. Every field but `interactive` (a boolean; absent is a press)
is required, at most 256 bytes and free of control bytes, since the daemon logs the key
it reset. One
daemon serves every user of the machine and its spaces are theirs in common, so one
user's reset of a key resets the daemon's state for that key for everyone; that is
intended (extender knowledge is per installation), and the verb is gated like
`set_provide_extender`: `control-tunnel` while the caller owns the live session or none
is live, `take-over-tunnel` while another uid's tunnel or provider session is live, a
press, so interactive. Another user's own GUI space is untouched, and the older id their
next import carries applies nothing. A daemon that predates the verb answers `unknown
verb`.

`set_provide_extender` is the connect page's Extender switch while there is no tunnel
session. The provider extender setting belongs to the network space
(`.provide_extender` in the daemon's storage), and every device reads it from its space
when it starts, so the daemon writes it through the device that runs (the provider-only
device, which persists it and applies it at once, or a tunnel session's device that came
up meanwhile), or with neither into the space the last device ran in, which the next
start imports (`provide::ExtenderSettingTargetFor`). A change made while disconnected
therefore holds after a Connect, and one made while connected holds after a Disconnect.
It is authorized like `set_provide` (a press, so interactive), refused while a bring-up
owns the session, and a request without a boolean `provide_extender` is refused rather
than written as a default. `provider_stats` carries the setting (`provide_extender`) and
`provide_extender_writable`, and the GUI shows the switch over the provider-only device
only when that is true, polling `provider_stats` while the connect destination is on
screen too (without `poll_status`). A daemon that predates the verb sends neither field,
so the switch stays hidden while disconnected, as before.

`logout` is the account signing out of the app (owner decision 2026-10-05: "logout should
not cross contaminate other networks. Each network should start fresh"). The GUI sends it
after `stop_tunnel` in every sign-out delivery (`app/src/SignOut.hpp`), with the account's
network space. The daemon ends what runs as an explicit stop does (the session, the
provider-only device, a log upload's standalone device and a queued upload, whose
credentials would otherwise make an identity), forgets the provide mode and the kill switch
the account asked for, deletes the device identity it keeps (`client_key_seed.bin`,
`provide_cert.pem`, `provide_key.pem` in the state directory) and logs out what its SDK
stored in that network space (`LocalState.logout`: the client credential and instance a
device persists when it starts, among the rest), so the next account starts on a new
identity in a clean space. The space's extender state (the extender directory, the gossip
role, the extender identity and the provider extender setting) stays, as every sign-out
leaves it: the SDK's `LocalState.logout` keeps it, and Account > Extenders has its own
reset. It mirrors the Windows service's `logout`. A daemon runs one
identity for every user of it, so the verb respects the multi-user daemon: while another
uid's session (a tunnel or a provider-only device) is live, it is refused with
`auth_not_tunnel_owner` and nothing is cleared, whatever was authorized; it is authorized
as `control-tunnel` and never as `take-over-tunnel`, without a prompt, because the health
poll re-sends an owed sign-out. It is refused while another client of the same uid owns the
live tunnel (`tunnel_owned_by_other_client`) and while a bring-up owns the session
(`start_in_progress`). The GUI keeps a refused sign-out owed and sends it again; it skips
both requests when the status it reads is redacted for its uid, and counts a logout refused
with `auth_not_tunnel_owner` (a group-mode daemon, whose status is not redacted, or a
session that started meanwhile) as delivered, since what the daemon keeps is then the other
user's. A daemon that predates the
verb answers `unknown verb`, which the GUI counts as done: that daemon keeps its identity,
as before, and a sign-out kept owed to it would hold every start.

**`sdk_version` must match EXACTLY, and this is a second, independent check.**
`protocol_version` guards *our* JSON control socket; the **device RPC has no version
negotiation at all** — `sdk/device_rpc.go`'s `DeviceRemoteSyncRequest` carries only
`InstanceId` (pairing, not versioning) and listener id lists. That was harmless on
every other platform because both halves ship in one artifact, so the two SDK copies
are byte-identical. **Linux is the first platform where they can drift**: the GUI
AppImage bundles its own `libURnetworkSdk.so` and self-updates through the in-app
checker, while the daemon's copy updates via apt or `install.sh`.

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
