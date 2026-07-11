# URnetwork for Linux

Native Ubuntu 22.04+/24.04 (GNOME/Wayland, amd64+arm64) client, distributed via
the **Snap Store**. Unlike macOS/Windows (privileged daemon + RPC), Linux uses
the **single-process, Android-like model**: one process runs the SDK's
`DeviceLocal` in-process and pumps packets through `/dev/net/tun` via the SDK's
`IoLoop` — no daemon, no RPC, no wintun. See `../PLAN.md`.

## Stack

**C++17 + GTK4 (gtkmm-4.0) + libadwaita**, consuming the shared **cgo SDK**
(`libURnetworkSdk.so` + the header-only `urnetwork_sdk.hpp` C++ wrapper) — the
same SDK surface the Windows app uses. We use GTK/C++ directly (not gotk4) to
avoid a Go GUI binding in the hot path; the SDK-host logic (`SdkHost`) is shared
in spirit with the Windows app, and only the UI toolkit + tun layer differ.

```
urnetwork (one process, strict snap + network-control)
  GTK4 + libadwaita (C++)  ── SNI tray (raw GDBus) + window
        │  C++ calls over the cgo C ABI (no RPC)
  SdkHost → urnet::NetworkSpaceManager → NetworkSpace → Api/LocalState
                                         DeviceLocal (in-process)
        │  in-process fd loop
  urnet::newIoLoop(device, tunFd) ⇄ /dev/net/tun   (CAP_NET_ADMIN via snap)
        │  ip route (0.0.0.0/1 + 128.0.0.0/1) + resolvectl DNS
```

## Layout

| Path | What |
|---|---|
| `src/SdkHost.{hpp,cpp}` | single-process core: init, auth, tunnel, connect, provide |
| `src/Tunnel.{hpp,cpp}` | `/dev/net/tun` RAII + `ip`/`resolvectl` config (wg-quick style) |
| `src/MainWindow.{hpp,cpp}` | GTK4 window: sign-in / home (connect + provide) views |
| `src/Tray.{hpp,cpp}` | StatusNotifierItem + `com.canonical.dbusmenu` over raw GDBus |
| `src/main.cpp` | entry: wires SdkHost + window + tray, hide-to-tray keep-running |
| `meson.build`, `meson_options.txt` | build (`-Dsdk_arch=amd64\|arm64`) |
| `snap/snapcraft.yaml` | strict snap (core24, `gnome` extension, `network-control`) |
| `packaging/` | `.desktop` (+ `urnetwork://` SSO scheme) + icon |
| `scripts/fetch-deps.sh` | vendor the cgo SDK into `third_party/urnetwork-sdk/<arch>/` |

## Build (local dev, on Linux)

```bash
# 1. vendor the SDK (from a local zip or SDK_ZIP_URL) — produces
#    third_party/urnetwork-sdk/{amd64,arm64}/{libURnetworkSdk.so,urnetwork_sdk.hpp}
scripts/fetch-deps.sh /path/to/URnetworkSdkLinux.zip

# 2. configure + build
meson setup build -Dsdk_arch=$(dpkg --print-architecture)
meson compile -C build
./build/urnetwork
```

Dev deps: `libgtkmm-4.0-dev libadwaita-1-dev libglib2.0-dev nlohmann-json3-dev
meson ninja-build g++ pkg-config`.

## Build (release snap)

The release pipeline builds this in a Docker container, not on the macOS host —
see `build/all/linux/` and `build/all/run.sh`. Each arch runs
`snapcraft --destructive-mode` in an Ubuntu-24.04 container that first stages the
matching cgo SDK into `third_party/urnetwork-sdk/<arch>/`. Output `.snap` files
are uploaded to the GitHub release; Snap Store submission is **manual** for now.

## Confinement notes

- `network-control` grants `/dev/net/tun` + `CAP_NET_ADMIN` — the crux that lets
  the confined process open + configure the tunnel itself (verified in a
  container with `--cap-add=NET_ADMIN --device=/dev/net/tun`).
- `resolvectl` DNS is best-effort; if systemd-resolved is unavailable the tunnel
  still routes (DNS just isn't captured on that link).
- The `gnome` extension supplies the GTK4/libadwaita runtime + portals; we stage
  only `libgtkmm`/`libadwaita` (the C++ binding libs the platform snap lacks).

## Not yet at macOS parity

This is the functional core (sign in via password/code, connect best-available,
provide toggle, hide-to-tray). Sign-up, Google/Apple browser SSO, the
location/provider picker, connection detail, and account/wallet/leaderboard
surfaces are the next slice — the `SdkHost`/`Api`/`ConnectViewController`
surfaces they need are already wired. See `../NEXTSTEPS.md`.
