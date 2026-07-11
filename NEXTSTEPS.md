# URnetwork Linux — Next Steps

Concrete, ordered pickup list. Context: `PLAN.md` (architecture/risks),
`app/README.md` (build/layout). Everything under `linux/app/` is written and
uncommitted.

## Where it stands (2026-07-10)

- **Stack decided + written: C++17 + GTK4 (gtkmm-4.0) + libadwaita**, consuming
  the cgo SDK (`libURnetworkSdk.so` + `urnetwork_sdk.hpp`). The earlier Go/gotk4
  tree is deleted. All `src/` is written: `SdkHost` (init/auth/tunnel/connect/
  provide), `Tunnel` (`/dev/net/tun` + `ip`/`resolvectl`), `MainWindow`
  (gtkmm4 login/home), `Tray` (StatusNotifierItem + dbusmenu over raw GDBus),
  `main.cpp` (hide-to-tray keep-running). Plus `meson.build`, `snapcraft.yaml`
  (meson plugin), `fetch-deps.sh`.
- **Functional core:** sign in (password / code / **guest**), connect
  best-available, provide toggle, hide-to-tray. NOT yet full macOS parity.
- **NOT yet compiled** — needs a Linux box with GTK4 + the SDK (all macOS-host
  LSP errors are environmental). The tun/`IoLoop` data plane itself was proven
  earlier in a container (`--cap-add=NET_ADMIN --device=/dev/net/tun`), and that
  confidence carries over (same `urnet::newIoLoop`).
- **Release build wired:** `build/all/linux/` (snapcraft-rock Docker container)
  + `build/all/run.sh` build the snap per arch. Store submission is manual.

## 1. Compile on an Ubuntu box (the iteration surface)

```sh
sudo apt install libgtkmm-4.0-dev libadwaita-1-dev libglib2.0-dev \
  nlohmann-json3-dev meson ninja-build g++ pkg-config
cd linux/app
scripts/fetch-deps.sh /path/to/URnetworkSdkLinux.zip     # stages third_party/
meson setup build -Dsdk_arch=$(dpkg --print-architecture)
meson compile -C build && ./build/urnetwork
```

Expect to iterate on: gtkmm4/libadwaita widget details written blind; the raw
GDBus **SNI tray** (does the icon appear on Ubuntu GNOME; is the dbusmenu layout
right); `PostToMain` (g_idle_add) marshaling; the `Tunnel` ioctl/`ip` calls on a
real kernel.

## 2. Full end-to-end connection test (BLOCKED on backend health)

> Paused 2026-07-10: production is having an issue; hold the auth-code/connect
> test until it's healthy. The auth code is in `linux/AUTH1.txt` (single-use —
> refresh if expired). Don't echo the code into logs.

Once compiled and prod is healthy: run `./build/urnetwork` in a GTK session (or
under `--cap-add=NET_ADMIN --device=/dev/net/tun`), sign in, connect, and confirm
real traffic egresses the tunnel (curl a what-is-my-ip service → provider IP).

## 3. Build + confine the snap, then confirm R4 *inside* the snap

The container build (`build/all/linux/build.sh`) produces the `.snap`; then:

1. `sudo snap install --dangerous ./urnetwork_*.snap`
2. `sudo snap connect urnetwork:network-control` (not auto-connected — R3).
3. **R4 probe confined:** launch the app, connect, confirm the tun opens AND
   routes/DNS apply under strict confinement — the one thing research left
   unverified (a snap may need extra interfaces or classic confinement).

## 4. DNS + desktop integration inside the snap (R5/R7 — unverified)

- **R5 DNS:** confirm `resolvectl` per-link DNS + `~.` domain work confined via
  systemd-resolved; else fall back to `/etc/resolv.conf` or the resolved D-Bus
  API. Validate no DNS leak. (22.04 vs 24.04 DoH differs.)
- **R7:** `urnetwork://` OAuth callback via `xdg-desktop-portal`; autostart via
  the Background portal; notifications via the portal; libadwaita theming.

## 5. Split tunneling (M3.5 — R6)

- Per-app exclusion: cgroup v2 + `SO_MARK`/nftables, Mullvad-style. **Confirm it
  works inside a strict snap** (research left this open).
- App self-exclusion (the SDK's own sockets), like Android's `VpnService.protect`.

## 6. Full-parity UI + iOS-parity additions (the bulk of v1)

v1 = full macOS functionality. The UI currently has sign-in + connect + provide.
Build out, wired to the cgo `Api`/view controllers (`SdkHost::api()` / `device()`
already expose them; add accessors for VCs as needed):

- **Full auth:** sign up (`Api::networkCreate`), reset password, **Google + Apple
  via system-browser OAuth** (`urnetwork://` via portal).
- **Location/provider picker:** `findProviders`/`ConnectViewController` grid — not
  only best-available.
- **Connect detail sheets:** contracts, split/block rules, DNS, throughput
  (Contract/BlockAction view controllers).
- **Provide:** control mode never/always/auto/manual + network mode.
- **Account / Wallet / Leaderboard / Support** to full parity (Stripe via `xdg-open`).

### iOS-parity additions from `apple/DESKTOP2.md` (verified SDK surfaces)

Bring these to Linux (same cross-platform SDK the apple app uses):

- [x] **Guest mode** (DESKTOP2 §2) — `SdkHost::LoginAsGuest` via
  `Api::networkCreate{guest_mode,terms}`; "Try Guest Mode" button on the login
  view. **DONE.** Still to add: guest→full-account **upgrade**
  (`Api::upgradeGuest` / `upgradeGuestExisting`).
- [ ] **Onboarding/Introduction flow** (DESKTOP2 §1) — welcome → plan/paywall →
  participate-to-earn → refer, gated by `isPro`/introduction-complete; "get more
  data" opens it. Gating uses `Api::subscriptionBalance`.
- [ ] **Account menu** (DESKTOP2 §3) — logout (SdkHost::Logout exists), referral
  share link (`Api::getNetworkReferralCode`), create-account. Needs an account view.
- [ ] **Copy client ID** (DESKTOP2 §4) — `device().getClientId()` → clipboard,
  in a contract-details context (needs that view).
- [x] **Solana wallet connect / Sign-in-with-Solana** (DESKTOP2) — **DONE & fully
  wired**: `src/WalletConnect.{hpp,cpp}` (Phantom/Solflare via the ur.io/wallet-connect
  bridge; SDK crypto `generateWalletKeyPair`/`generateSharedSecret`/`encrypt|decryptData`),
  `SdkHost::SignInWithSolana`/`HandleDeepLink`/`AuthLoginWithWallet`, "Phantom/Solflare"
  buttons on the login view, and the `urnetwork://` scheme via GApplication
  `HANDLES_OPEN` + `signal_open`. Needs the ur.io/wallet-connect page **deployed** +
  a real-wallet round-trip test (on-device, same as macOS). The connect-wallet-in-
  account path (`createAccountWallet`) awaits the account UI.
- [ ] **Country search** on the add-blocked-location surface (DESKTOP2 remaining).
- n/a **In-app review prompt** — platform-specific; Snap Store has no in-app
  review API. Omit (like the macOS `requestReview` note).

## 7. Snap Store submission

- Request the `network-control` auto-connect exception (store review — R3).
  Proton VPN is the precedent the category + snap confinement are accepted.
