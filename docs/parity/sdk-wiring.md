# URnetwork Linux port — THE SDK WIRING MAP (implementation contract)

**Subject:** what every destination needs from the SDK, and what the Linux SdkHost already has.

**Windows sources read:** `urnetwork-windows/app/src/App/SdkHost.h` (full, 1774 lines), `SubscriptionBalance.h` (full), `UpdateChecker.h` (full), `PageContext.h`, `Common/Protocol.h` (TunnelStatus), plus per-page grep of every `Sdk().` / `Balance().` / `Updates().` / `api().` / `device().` call site in ConnectPage, LocationSheets, AccountPage, WalletPage, SettingsPage, DeveloperPage, StatsSheets, SettingsSheets, WalletSheets, BalanceSheets, AuthSheets, ProviderLocationsSheet, LoginPage, MainWindow.xaml.cpp, AppController, SubscriptionBalance.cpp, and the SdkHost.cpp method bodies (to resolve each public method to its underlying `urnet::` call).

**Linux sources read:** `urnetwork-linux/app/src/SdkHost.hpp` (full), `SubscriptionBalance.hpp` (full), `ControlClient.hpp` (public surface), `ControlProtocol.hpp` (spot-checked).

**SDK verification:** every `urnet::` symbol below was grep-verified against the vendored `urnetwork-linux/app/third_party/urnetwork-sdk/amd64/urnetwork_sdk.hpp`. **Result: ZERO missing symbols — the Linux vendored SDK is at parity with the Windows S1 SDK**, including `WindowStatus{StallReason, Failed}` (hpp:2224), the seven advanced-mode fault-injection/probe methods on `DeviceRemote` (hpp:10785-10824: dropExit, stallExit, shuffleExits, migrateExit -> int64_t, probeAllExits -> int64_t, startProbeSuite, stopProbeSuite, probeSuiteRunning, getProbeResults, sync), `urnet::getDefaultProbeSuiteConfig`, `urnet::getFilteredLocationsFromResult` (hpp:19754), `newNetworkNameValidationViewController(api)` (hpp:19911), `urnet::setEgressInterfaceIndex`, `urnet::version()` (hpp:19567). All Device getters used by ReadStats live on the shared `Device` base class (hpp:10398-10437), which `DeviceRemote` inherits — so they work over the daemon device RPC.

## 0. Gap classification legend

| Class | Meaning |
|---|---|
| EXISTS | Linux SdkHost/store already exposes an equivalent (named in the table; names often differ) |
| NEEDS-SDKHOST-METHOD | the `urnet::` symbol is in the vendored hpp (grep-verified) but the Linux SdkHost does not surface it — add the method |
| NEEDS-VC-EXPOSURE | the Linux SdkHost already holds the view controller but does not surface the needed method on it |
| MISSING-IN-SDK | symbol absent from the vendored hpp — **none found anywhere in this audit** |
| PROTOCOL-GAP | needs a daemon control-protocol verb/field the Linux protocol does not have yet (doc §9.2 gap note — confirmed accurate against `ControlProtocol.hpp`/`ControlClient.hpp`) |
| APP-SIDE | no SDK involvement; port the Windows app-side file (e.g. ConnectionHealth.h, AppPrefs.h) |

`api().X` rows: Windows pages call `Sdk().api().X(...)` directly; the Linux SdkHost exposes the same `urnet::Api& api()` accessor, so every such row is EXISTS(api()) provided the symbol is in the vendored hpp — all are (hit counts in §8).

---

## 1. Process shape (context for every row below)

Windows: SdkHost owns NetworkSpace/NetworkSpaceManager/Api/AsyncLocalState/LocalState in-process; the tunnel lives in the service; a `urnet::DeviceRemote` binds over loopback mTLS; a named-pipe control channel carries `proto::` verbs and PUSHED `TunnelStatus` events. Linux (already built the same way): `SdkHost::StartTunnel()` = ControlClient connect -> hello (protocol + exact `urnet::version()` match both ways) -> `start_tunnel` -> bind DeviceRemote to `127.0.0.1:12025` (`kDeviceRpcPort`). Linux control verbs today: `hello, status, start_tunnel, stop_tunnel, set_provide, location_override_available, location_override_write, location_override_clear`. Linux `status` reply carries ONLY `tunnel_state, rpc_port, client_id, error` — see §7 for what Windows pages read that this cannot yet carry.

View controllers held by BOTH SdkHosts (same set, same SDK): ConnectViewController, ContractViewController, ContractDetailsViewController (client feed), BlockActionViewController, LocationsViewController, PeerViewController, ProviderLocationsViewController, NetworkNameValidationViewController (api-scoped, survives logout). Linux additionally holds PostQuantumIdentityViewController (Windows implements PQI differently in PostQuantumIdentity.cpp).

Event model difference (deliberate, both fine): Windows pushes typed payloads through per-feed handler slots (SetThroughputHandler, SetContractRowsHandler, SetBlockActionsHandler, SetBlockStatsHandler, SetSplitRulesHandler, SetDnsSettingsHandler, SetBlockerEnabledHandler, SetLocationsHandler/Observer, SetPeersHandler/Observer, SetRemoteChangedHandler, SetProviderLocationsHandler, SetProviderIdentitiesHandler, SetProviderSelectionHandler) plus snapshot getters. Linux publishes a single `DrawerEventHandler` with a `DrawerEvent` enum (DeviceLifecycle, Throughput, BlockActions, BlockStats, Overrides, DnsSettings, Blocker, RouteLocal, Contracts, Location, Profile, Locations, Peers, ProviderIdentities, ProviderLocations, ProviderSelection) and the UI re-reads snapshot accessors. The Linux model already solves the Windows R4 two-consumer problem (one slot, two subscribers) by design — keep it; the GTK UI fans events out itself.

---

## 2. WIRING TABLE — window level / AppController / tray

| Windows call | Underlying call | Linux equivalent | In vendored hpp | Class |
|---|---|---|---|---|
| `Sdk().Initialize()` | builds NetworkSpaceManager/NetworkSpace/Api/AsyncLocalState/LocalState; resume via BootstrapSession(attachOnly) | `SdkHost::Initialize(storageDir, logDir)` | yes | EXISTS |
| `Sdk().IsLoggedIn()` (x5 MainWindow) | cached atomic over `localState_->getByClientJwt` presence | `IsLoggedIn()` | yes | EXISTS |
| `Sdk().apiReady()` | `api_.has_value()` | none (only `api()` deref) | — | NEEDS-SDKHOST-METHOD (trivial guard) |
| `Sdk().HasSession()` | atomic `hasSession_` | `Connected()` is connect-status, NOT session presence; `hasDevice()` is closest | — | NEEDS-SDKHOST-METHOD (session/no-session must be distinguishable from rpc state) |
| `Sdk().ServiceConnected()` | PipeClient atomic | `Control().EnsureSession()` probes; no cheap atomic | — | NEEDS-SDKHOST-METHOD |
| `Sdk().SetAuthStateHandler` | fired from every auth path | `SetAuthStateHandler(bool loggedIn)` (Windows passes `AuthState` enum + error) | — | EXISTS (shape differs) |
| `Sdk().SetAuthInvalidHandler` | `device_->addAuthLogoutListener` | `SetAuthInvalidHandler` | yes | EXISTS |
| `Sdk().SetJwtRefreshedHandler` | `device_->addJwtRefreshListener` | `SetJwtRefreshedHandler` | yes | EXISTS |
| `Sdk().SetTunnelStateHandler(proto::TunnelStatus)` | pipe-pushed status events + synthesised `SessionStatus()` | `SetConnectionStatusHandler(std::string)` only; the daemon does not push (client polls `status`) | n/a | PROTOCOL-GAP (no pushed events; no status fields — §7) |
| `Sdk().SetStatsHandler` / `CurrentStats()` | ReadStats (see §3 field list) | `SetStatsHandler` / `CurrentStats()` | yes | EXISTS (fewer fields — §6) |
| `Sdk().RepublishStats()` | `PublishStats()` on demand (health reeval clock, window re-show) | none (`PublishStats` private) | — | NEEDS-SDKHOST-METHOD (one-liner) |
| `Sdk().SetModeNoticeHandler` / `SetModeNoticeObserver` / `RefreshModeNotice()` | standing ModeNotice{RpcOnly, SessionFailed} channel, bind-then-replay | none; `TunnelStartResult` + `LastTunnelError()` is the partial analogue | — | NEEDS-SDKHOST-METHOD (standing-state channel; MIGRATION.md requires distinct degraded renders) |
| `Sdk().CurrentAdvancedMode()` / `SetAdvancedMode` / `SetAdvancedModeHandler` / `RefreshAdvancedMode` | app_prefs.json read-modify-write + atomic + replay contract | none | n/a | APP-SIDE (port AppPrefs + the D5 standing-state contract) |
| `Sdk().SetLocationsObserver` / `SetPeersObserver` | second slot for the Network pane | `DrawerEvent::Locations` / `Peers` fan-out | yes | EXISTS |
| `Sdk().SetPresentationActive(bool)` | presentation worker opens/closes presentation VCs off the UI thread | `SetPresentationActive(bool)` | yes | EXISTS (verify Linux is also non-blocking on the GTK thread — Windows D4 lesson) |
| `Sdk().ConnectBestAvailable()` (tray) | session worker -> BootstrapSession if needed -> `connectVc_->connectBestAvailable()` | `ConnectBestAvailable()` (VC only; caller must `StartTunnel()` first) | yes | EXISTS (session-bring-up semantics differ — flag) |
| `Sdk().Disconnect()` (tray) | `connectVc_->disconnect()` (+ gesture plan may stop tunnel) | `Disconnect()` | yes | EXISTS |
| `Sdk().StopServiceTunnel()` (tray "turn off") | one blocking pipe rpc `stop_tunnel` | `Control().StopTunnel(&err)` | n/a | EXISTS (via ControlClient) |
| `Sdk().CurrentStats()` (tray tooltip) | ReadStats | `CurrentStats()` | yes | EXISTS |
| `Sdk().HandleDeepLink(url)` | wallet bridge callback routing | `HandleDeepLink(url)` | n/a | EXISTS |
| `Sdk().Logout()` | teardown + `localState` wipe + service logout | `Logout()`; also Linux-only `Shutdown()` (quit is not sign-out — keep) | yes | EXISTS |
| `Sdk().SetKillSwitch` (tray quick action) | see Settings row | `SetRouteLocal` soft leg only | yes | PROTOCOL-GAP (no `set_kill_switch` verb) |
| `Sdk().EnsureSession(reason)` (resume/watchdog) | attach-only bootstrap, D8: never cold-starts | none (Linux `StartTunnel` always starts) | — | NEEDS-SDKHOST-METHOD (attach-only reattach path + service-reconnect watchdog) |
| `Balance().Initialize/Start/Stop/SetVisible/SetChangeHandler/OnJwtRefreshed/Refresh/Current/CurrentPoll` | `api().subscriptionBalance` (+ referral ride-along on Linux) | ctor(host) / `Start` / `Stop` / `SetWindowVisible` / `SetChangedHandler` / `OnJwtRefreshed` / `FetchNow` / accessor getters (`IsPro, IsGuest, IsPolling, PurchaseConfirmationTimedOut, HasFetched, UsedByteCount, PendingByteCount, AvailableByteCount, StartBalanceByteCount, TotalReferrals, ReferralCode`) + Linux-only `DidDetectUpgradeToPro`, `ClearPurchaseConfirmationTimeout` | yes | EXISTS (names differ; timings identical: 30 s background poll, 5 s confirmation poll, 2-minute ACTIVE-time budget that pauses with window visibility) |
| `Updates().SetHandler/Current/BeginApply/SetRelaunchHandler/Start/Stop` | GitHub releases HTTP + SHA-256 + rename-swap | none on Linux | n/a | MISSING component (packaging decision: AppImage zsync / repo updates; if an in-app banner is wanted, port the Snapshot/Phase model in §5) |

## 2.1 WIRING TABLE — Login (LoginPage.cpp + AuthSheets.cpp)

| Windows call | Underlying call | Linux equivalent | hpp | Class |
|---|---|---|---|---|
| `Sdk().StartLogin(userAuth, done)` | `api_->authLogin{user_auth}` -> LoginRouting{Login/Password/Create/Verify/IncorrectAuth/Error} | `StartLogin` (same LoginRouting shape) | yes | EXISTS |
| `Sdk().LoginWithPassword` | `api_->authLoginWithPassword` -> RegisterNetworkClient (`api_->authNetworkClient` + `localState_->setByJwt/setByClientJwt`) | `LoginWithPassword` | yes | EXISTS |
| `Sdk().LoginWithCode` | `api_->authCodeLogin` | `LoginWithCode` | yes | EXISTS |
| `Sdk().LoginWithSeedphrase` | `api_->authLogin{seedphrase}` (normalized lowercase/trim/single-space) | `LoginWithSeedphrase` | yes | EXISTS |
| `Sdk().CreateNetwork(params)` | `api_->networkCreate` (3 modes: password / pending wallet auth / pending Google auth-jwt) | `CreateNetwork(name,userAuth,password,referral)` + `CreateNetworkWithPendingWallet(name,referral)`; **no auth-jwt mode** | yes | EXISTS for password+wallet; NEEDS-SDKHOST-METHOD for the auth-jwt (Google) mode |
| `Sdk().CreateInstantAccount / ConfirmInstantAccount / DiscardInstantAccount` | `networkCreate{terms}` (no auth) -> held jwt -> confirm registers | same three methods | yes | EXISTS |
| `Sdk().VerifyCode` / `ResendVerifyCode` | `api_->authVerify` / `api_->authVerifySend` | `VerifyCode` / `ResendVerifyCode(done(ok,error))` | yes | EXISTS (Linux resend also returns error text — keep) |
| `Sdk().SendPasswordResetLink` | `api_->authPasswordReset` | `SendPasswordResetLink(done(ok,error))` | yes | EXISTS |
| `Sdk().CheckNetworkName` (caller debounces) | `networkNameVc_->networkCheck` (`newNetworkNameValidationViewController(api)`) | `CheckNetworkName` | yes | EXISTS |
| `Sdk().api().validateReferralCode` | Api direct | `ValidateReferralCode(done(ok,valid,capped))` wrapper (better) or `api()` | yes | EXISTS |
| `Sdk().SsoGoogleEnabled()` / `SignInWithGoogle` / `HasPendingAuthJwt` | system-browser OAuth+PKCE loopback -> `authLogin{auth_jwt_type:"google"}` | none | yes (authLogin) | NEEDS-SDKHOST-METHOD (whole Google flow; gate on compiled client id like Windows Config.h — hidden, not broken, when absent) |
| `Sdk().SignInWithSolana(provider)` / `SignInWithBittensor()` | ur.io/wallet-connect bridge -> `authLogin{wallet_auth}` (SOL base64 / TAO sr25519 hex) | `SignInWithSolana` / `SignInWithBittensor` | yes | EXISTS |
| `Sdk().HasPendingWalletAuth()` | retained `urnet::WalletAuthArgs` | `HasPendingWalletAuth()` | yes | EXISTS |
| `Sdk().UpgradeGuest(name,userAuth,password)` | `api_->upgradeGuest` -> re-register device under new jwt | `UpgradeGuest` | yes | EXISTS |
| AuthSheets: `sdk_.LoginAsGuest` (GuestModeSheet) | `networkCreate{guest_mode, terms}` | `LoginAsGuest` | yes | EXISTS |
| AuthSheets: `sdk_.CurrentNetworkServer` / `ApplyNetworkServer` (NetworkServerSheet, signed-out only) | NetworkSpaceManager update; tears down Api/LocalState | `CurrentNetworkServer` / `ApplyNetworkServer` (+ Linux-only `NetworkSpaceJson()` for start_tunnel) | yes | EXISTS |
| AuthSheets: `api().getNetworkReferralCode` | Api direct (guest-upgrade sheet referral) | `api()` | yes | EXISTS |

## 2.2 WIRING TABLE — Home / Connect (ConnectPage.cpp + ConnectCanvas + StatsSheets.cpp + ProviderLocationsSheet.cpp + LocationChooserSheet in LocationSheets.cpp)

| Windows call | Underlying call | Linux equivalent | hpp | Class |
|---|---|---|---|---|
| `Sdk().ConnectBestAvailable()` / `Sdk().Connect(location)` (connect button) | session worker (bring-up if needed) -> `connectVc_->connectBestAvailable()` / `connectVc_->connect(loc)` | `ConnectBestAvailable()` / `Connect(optional<ConnectLocation>)` — no-ops with tunnel down; UI must call `StartTunnel()` first | yes | EXISTS (bring-up ordering is UI's job on Linux today; consider folding the Windows "connect starts the tunnel" worker into Linux SdkHost) |
| `Sdk().Disconnect()` (x2) | `connectVc_->disconnect()` | `Disconnect()` | yes | EXISTS |
| `Sdk().CurrentStats()` (x3) + `SetStatsHandler` | ReadStats: `connectVc_->getConnectionStatus/getConnected/getGrid` (grid -> `getWindowCurrentSize`, `getProviderGridPointList`), `device_->getContractStatus` (InsufficientBalance), `getProvideEnabled/getProvidePaused/getProvideMode`, `getNetworkPeers` (provideClients), `getConnectLocation` (x2: name + country), `getWindowStatus` (StallReason/Failed), `contractVc_->getThroughputPoints` (rates), provide-key atomic, health Tracker fold-in, rpc-only clamp | `CurrentStats()` reads a SUBSET (§6) | yes | EXISTS for the subset; NEEDS-VC-EXPOSURE for gridPoints/gridWidth/gridHeight (connectVc `getGrid` held but unexposed); NEEDS-SDKHOST-METHOD for locationName/countryCode/countryName, windowStallReason/windowFailed, rawConnectionStatus/rawConnected/rpcOnly; APP-SIDE for health/provenProviderCount/healthReevalAtMillis (port ConnectionHealth.h Tracker) |
| `Sdk().RepublishStats()` (1 s tick while a health degrade hold is pending) | PublishStats | none | — | NEEDS-SDKHOST-METHOD |
| `Sdk().SelectedLocation()` (x2) | `connectVc_->getSelectedLocation()` (fallback `device_->getConnectLocation`) | `SelectedLocation()` | yes | EXISTS |
| `Sdk().ConnectedPeerCount()` | `peerVc_->getConnectedCount()` | `ConnectedPeerCount()` | yes | EXISTS |
| `Sdk().ConnectedProvidePeers()` (x3) | `peerVc_->getPeers()` | `ConnectedProvidePeers()` | yes | EXISTS |
| `Sdk().RemoteConnected()` (x4 — peers "discovery disabled" state, drawer gating) | `device_->getRemoteConnected()` | none public (only raw `device()`) | yes | NEEDS-SDKHOST-METHOD (locked wrapper; false must render "unavailable", never zero) |
| `Sdk().CurrentProvideControlMode()` / `SetProvideControlMode(mode)` (Auto/Always/Network/Never segmented) | `device_->get/setProvideControlMode` with `localState_` fallback/persist | `GetProvideControlMode` / `SetProvideControlMode` (+ `ProvideEnabled`, `ResetProvideToNever` for the Pro-upgrade side effect) | yes | EXISTS (Linux daemon also has `set_provide` verb for tray-independent provide — keep) |
| `Sdk().CurrentPerformanceSettings()` / `SetPerformanceSettings` (mode Auto/Web/Streaming, fixedIp, strong-anonymization=!allowDirect, postQuantum) | `device_->get/setPerformanceProfile` + `localState_->get/setPerformanceProfile` (app decomposes profile <-> 4 controls) | `GetPerformanceProfile` / `SetPerformanceProfile` (raw `urnet::PerformanceProfile`) | yes | EXISTS (port the PerformanceSettings decomposition app-side; Auto writes a profile with window_type "auto" so allowDirect/postQuantum persist in every mode) |
| `Sdk().CurrentBlockerEnabled()` / `SetBlockerEnabled` | `device_->get/setBlockerEnabled` | `GetBlockerEnabled` / `SetBlockerEnabled` | yes | EXISTS |
| `Sdk().CurrentContractRows()` + `SetContractRowsHandler` | `contractDetailsVc_->getContractRows` (VC owns ordering/freeze/pending) via `addContractRowsListener` | `ContractRows()` + `DrawerEvent::Contracts` | yes | EXISTS |
| `Sdk().SetContractsAtTop(bool)` / `ContractsPendingCount()` | `contractDetailsVc_->setAtTop` / `pendingCount` | same names | yes | EXISTS |
| `Sdk().CurrentThroughputPoints(&windowSeconds)` + `SetThroughputHandler` (TransferChart, 60 s window) | `contractVc_->getThroughputPoints` via `addThroughputListener` | `ThroughputPoints()` + `ThroughputWindowSeconds()` + `DrawerEvent::Throughput` | yes | EXISTS |
| `Sdk().CurrentBlockActions()` / `CurrentBlockCounts` + handlers (activity list + blocked/local charts) | `blockVc_->addBlockActionsListener` / `addBlockActionStatsListener` | `BlockActions()` / `BlockStatsSnapshot()` + `DrawerEvent::BlockActions/BlockStats` | yes | EXISTS (Windows flattens to BlockActionItem incl. matchedHosts/matchedIps green chips — flatten app-side from `urnet::BlockAction`) |
| `Sdk().CurrentSplitRules()` + handler | `device_->getBlockActionOverrides` via change listener | `BlockActionOverrides()` + `DrawerEvent::Overrides` | yes | EXISTS |
| `Sdk().CurrentDnsSettings()` + handler (4 dot rows + recommendation pill) | `device_->getDnsResolverSettings` via change listener | `GetDnsResolverSettings()` + `DrawerEvent::DnsSettings` | yes | EXISTS |
| `Sdk().ReadReliability()` (Pane C inspector exit-join; off-thread, low cadence, only Advanced+presenting) | see Developer table | none | yes | NEEDS-SDKHOST-METHOD (shared with Developer) |
| `Sdk().CurrentProviderIdentities()` (x2) + handler (verified-e2e badges) | `device_->getProviderIdentities` (joined onto location rows by egress client id) | `ProviderIdentities()` + `DrawerEvent::ProviderIdentities` (Linux PQI VC also gives `PublicIdentityKeyHash/Key` — richer, keep) | yes | EXISTS |
| `Sdk().CurrentProviderLocations()` + handler ("Connected to N providers" sheet rows, SDK display order west-to-east) | `providerLocationsVc_` getter re-read on signal, value-compared | `ConnectedProviderLocations()` + `DrawerEvent::ProviderLocations` | yes | EXISTS |
| ProviderLocationsSheet: `sdk_.SelectedProviderClientId()` (x3) / `SetSelectedProviderClientId` / `StepProviderSelection(steps)` | `providerLocationsVc_->getSelectedClientId / setSelectedClientId / stepSelection` (clamps at ends) | same three names + `DrawerEvent::ProviderSelection` | yes | EXISTS |
| ProviderLocationsSheet: `sdk_.RemoveConnectedProvider(clientId)` | `providerLocationsVc_->removeProvider` (Windows also has `device_->removeConnectedProvider` path) | `RemoveConnectedProvider` | yes | EXISTS |
| StatsSheets: `sdk_.CreateSplitRule(hosts)` / `UpdateSplitRule(id,hosts)` / `RemoveSplitRule(id)` | `device_->addBlockActionOverride` / read-modify `getBlockActionOverrides`+`setBlockActionOverrides` / `removeBlockActionOverride` (+ `urnet::newId`) with `localState_` fallback | `AddBlockActionOverride` / `SetBlockActionOverrideHosts` / `RemoveBlockActionOverride` | yes | EXISTS |
| StatsSheets: `sdk_.ApplyDnsSettings(settings)` (DnsEditorSheet) | `device_->setDnsResolverSettings` | `SetDnsResolverSettings` | yes | EXISTS |
| StatsSheets: `sdk_.SetAppRule(imagePath, includeInTunnel)` / `RemoveAppRule` / `CurrentAppRules()` (x2) (AppRulesSheet — opened from Settings) | per-app BlockActionOverride keyed by exe path + `device_->getLocalOverrideAppIds` -> `PushLocalOverrideAppsToDriver` -> pipe `set_split_tunnel{paths, allowlist_mode}` | none | yes (`getLocalOverrideAppIds` present) | NEEDS-SDKHOST-METHOD + PROTOCOL-GAP (no `set_split_tunnel` verb; Linux mechanism is cgroup/fwmark per doc §6.7 — design differs, the SdkHost surface should not) |
| LocationChooserSheet (LocationSheets.cpp): `sdk_.SetLocationFilter(query)` / `CurrentFilteredLocations` / `CurrentFilteredLocationState` / `EnsureLocations` | dual source: `locationsVc_->filterLocations/getFilteredLocations/getFilteredLocationState/addFilteredLocationsListener/start` when VC exists; otherwise in-process `api_->getProviderLocations` (empty query) / `api_->findProviderLocations{query}` bucketed by `urnet::getFilteredLocationsFromResult` — states `urnet::LocationsLoading/LocationsLoaded/LocationsError`; single-writer gate `deviceFeedOpen_` | `FilterLocations` / `GetFilteredLocations` / `GetFilteredLocationState` exist but are **VC-only: nullopt with tunnel down** | yes (incl. `getFilteredLocationsFromResult`, `FindLocationsArgs/Result`) | EXISTS for VC path; NEEDS-SDKHOST-METHOD for the class-A api fallback + `EnsureLocations` re-arm (Windows contract: THE PROVIDER LIST IS ALWAYS AVAILABLE, no session/no auth needed; doc §7.8's Loading/Failed/empty tri-state depends on the state string) |
| `sdk_.ConnectFromRow(location)` (x2) / `ConnectBestAvailableFromRow()` (row click = connect) | last-request-wins slot, ~1.2 s settle coalescing, idempotent re-click (uses `SameId/IsBestAvailableSelected/IsLocationSelected` vs `connectVc_->getSelectedLocation`) | plain `Connect/ConnectBestAvailable` only | yes | NEEDS-SDKHOST-METHOD (port the coalescer + the three selection-identity predicates — free functions in SdkHost.h; without it click bursts wedge the SDK dial staircase) |

## 2.3 WIRING TABLE — Network destination (LocationSheets.cpp pane + MainWindow observers)

Same SDK feeds as the chooser sheet, second consumer. Every row of 2.2's chooser block applies. Additionally:

| Windows call | Underlying | Linux | hpp | Class |
|---|---|---|---|---|
| `Sdk().SetLocationsObserver` / `SetPeersObserver` (MainWindow binds for the pane) | copies of the chooser pushes | `DrawerEvent::Locations/Peers` fan-out | yes | EXISTS |
| Row click -> `sdk_.ConnectFromRow` | as above | as above | yes | NEEDS-SDKHOST-METHOD |
| Selection check glyphs -> `SameId / IsBestAvailableSelected / IsLocationSelected` + `Sdk().SelectedLocation()` | `connectVc_->getSelectedLocation` | `SelectedLocation()` exists; predicates do not | yes | NEEDS-SDKHOST-METHOD (predicates) |
| Blocked locations second door (SettingsSheets) | see Settings table | `api()` | yes | EXISTS |

## 2.4 WIRING TABLE — Account destination (AccountPage.cpp + BalanceSheets.cpp + account sheets in SettingsSheets.cpp)

| Windows call | Underlying | Linux | hpp | Class |
|---|---|---|---|---|
| `api().getNetworkUser` (network name, auth line) | Api | `api()` | yes (4 hits) | EXISTS |
| `api().changeNetworkName` (24 h cooldown server-side) / `api().claimNetworkName` (no cooldown) | Api | `api()` | yes | EXISTS |
| `api().authPasswordReset` (change-password mail) | Api | `api()` | yes | EXISTS |
| `api().getNetworkRedeemedBalanceCodes` (Pane C codes table) | Api | `api()` | yes | EXISTS |
| `api().getNetworkReferralCode` (bonus code copy row) | Api | `api()` | yes | EXISTS |
| `Balance().Current()/CurrentPoll()` (plan card, UsageBar, confirmation ring) via MainWindow relay; `Balance().Refresh()` on nav to account | `api().subscriptionBalance` -> `urnet::SubscriptionBalanceResult` | store accessors / `FetchNow()`; Linux store also carries `TotalReferrals`+`ReferralCode` on the same poll | yes | EXISTS |
| `Sdk().IsLoggedIn` (x6) | atomic | `IsLoggedIn` | — | EXISTS |
| BalanceSheets: `api().createStripeCheckoutSession` (UpgradeSheet) + `balance_.StartConfirmationPolling` (x3) | Api + store | `api()` + `StartConfirmationPolling` (Linux UpgradeSheet.cpp exists) | yes | EXISTS |
| BalanceSheets: `api().redeemBalanceCode` (RedeemCodeSheet, 26-char) | Api | `api()` (RedeemCodeSheet.cpp exists) | yes | EXISTS |
| SettingsSheets: `api().addAuth` (AddAuthSheet) | Api | `api()` | yes | EXISTS |
| SettingsSheets: `api().authCodeCreate` (AuthCodeSheet, 5-min expiry) | Api | `api()` | yes | EXISTS |
| SettingsPage (renders on Account per R4): `api().removeAuth` (login-methods remove confirm) | Api | `api()` | yes | EXISTS |
| SettingsSheets: `api().networkDelete` (DeleteAccountSheet typed confirm) | Api | `api()` | yes | EXISTS |
| SettingsSheets: `api().getReferralNetwork` / `setNetworkReferral` / `unlinkReferralNetwork` (ReferralNetworkSheet) | Api | `api()` | yes | EXISTS |
| NOTE | Account destination ALSO triggers `settings_->LoadSettings()` (MainWindow::LoadCurrentDestination) because login methods / auth code / client id / referral rows are owned by SettingsPage but render on Account | replicate the load fan-out | — | APP-SIDE |

## 2.5 WIRING TABLE — Earnings destination (WalletPage.cpp + WalletSheets.cpp)

| Windows call | Underlying | Linux | hpp | Class |
|---|---|---|---|---|
| `api().getAccountWallets` / `api().getPayoutWallet` (wallet cards + DEFAULT chip) | Api | `api()` | yes | EXISTS |
| `api().createAccountWallet` (connect-a-wallet) | Api | `api()` | yes | EXISTS |
| `api().walletValidateAddress` (per-chain debounced SOL/MATIC/TAO) | Api | `api()` | yes | EXISTS |
| `api().walletBalance` | Api | `api()` | yes | EXISTS |
| `api().getAccountPayments` (payouts tab + PayoutDetailSheet) | Api | `api()` | yes | EXISTS |
| `api().getAccountPoints` (points breakdown card) | Api | `api()` | yes | EXISTS |
| `api().getLeaderboard` / `api().getNetworkLeaderboardRanking` / `api().setNetworkLeaderboardPublic` (echo-guarded toggle) | Api | `api()` | yes | EXISTS |
| `api().getNetworkReliability` (reliability card) | Api | `api()` | yes | EXISTS |
| `api().getTransferStats` (unpaid bytes) | Api | `api()` | yes | EXISTS |
| `api().getNetworkReferralCode` (referrals row) | Api | `api()` | yes | EXISTS |
| `api().verifySeekerHolder` + `Sdk().SignWithSolanaWallet(provider, message, done)` (Verify Seeker: bridge signs, api verifies; kBridgeTimeoutMs 180 s, plain api 20 s) | WalletConnect bridge bare-sign (no auth) + Api | `api()` yes; bare-sign wrapper none — but Linux `WalletConnect::SignMessage` primitive EXISTS | yes | NEEDS-SDKHOST-METHOD (`SignWithSolanaWallet`: single-flight rule with the sign-in flow — starting one cancels the other) |
| `Sdk().ParsedJwt()` (x2 — guest/Pro gating of upgrade button) | `localState_->parseByJwt` | `ParseByJwt()` | yes | EXISTS |
| WalletSheets: `api().setPayoutWallet` (Make default) / `api().removeWallet` (two-press confirm; 20 s watchdog + generation guard) | Api | `api()` | yes | EXISTS (watchdog/generation is APP-SIDE) |
| `Sdk().apiReady()` gate (`CanCallApi()`) | — | add trivially | — | NEEDS-SDKHOST-METHOD (trivial) |

## 2.6 WIRING TABLE — Settings destination (SettingsPage.cpp + SettingsSheets.cpp + AppRulesSheet in StatsSheets.cpp)

| Windows call | Underlying | Linux | hpp | Class |
|---|---|---|---|---|
| `api().accountPreferencesGet` / `api().accountPreferencesUpdate` ("send_product_updates" echo-guarded toggle) | Api | `api()` | yes | EXISTS |
| `Updates().SetAutoCheckEnabled(bool)` + static `UpdateChecker::AutoCheckEnabled()` seed ("Check for updates automatically", persisted app_prefs.json, default ON; turning ON checks immediately) | app prefs + worker | none | n/a | MISSING component (see §5) |
| `Sdk().CurrentKillSwitch()` (x2) / `SetKillSwitch(on)` (kill switch toggle + notes) | THREE legs: `localState_->setRouteLocal(!on)` + `device_->setRouteLocal(!on)` + pipe `service_.SetKillSwitch(on)`; returns false if enforcement leg failed — caller must re-read and show the real state | `GetRouteLocal()` / `SetRouteLocal(!on)` = soft legs only | yes | EXISTS (soft) + PROTOCOL-GAP (no `set_kill_switch` verb / nftables enforcement; the Windows honesty rule — never leave the toggle On over a failed apply — must survive) |
| Blocked locations: `api().getNetworkBlockedLocations` / `networkBlockLocation` / `networkUnblockLocation` / `getProviderLocations` (BlockedLocationsSheet) | Api | `api()` | yes | EXISTS |
| Per-app split tunnel "manage_apps" -> AppRulesSheet (`sdk_.CurrentAppRules/SetAppRule/RemoveAppRule`) | see 2.2 row | none | yes | NEEDS-SDKHOST-METHOD + PROTOCOL-GAP |
| Post-quantum identity row + sheet | Windows: PostQuantumIdentity.cpp joins `device_->getProviderIdentities` | Linux RICHER: `ProviderIdentities()` + `PublicIdentityKeyHash()` + `PublicIdentityKey()` via pqiVc_ | yes | EXISTS |
| Device name row -> DeviceNameSheet: `api().deviceSetName` + `api().getNetworkClients` (find own row by client id) | Api | `api()` | yes | EXISTS |
| `Sdk().device().getClientId()` (client id copy field; `Sdk().hasDevice()` guard x3) | Device | `ClientId()` (wrapped — better) | yes | EXISTS |
| "Uninstall service" (registered-only; elevated verb) | ServiceSetup, not SDK | packaging owns it on Linux | n/a | APP-SIDE/packaging |
| Advanced Mode toggle (the one writer; MainWindow calls `Sdk().SetAdvancedMode`, Settings seeds from `CurrentAdvancedMode`) | app_prefs.json | none | n/a | APP-SIDE (D5 contract) |
| Export/Save logs | app-side file ops | app-side | n/a | APP-SIDE |
| `api().stripeCreateCustomerPortal` (Manage Subscription; also reachable from Account plan pane) | Api | `api()` | yes | EXISTS |
| `api().getNetworkUser` / `getNetworkReferralCode` / `getReferralNetwork` (R4 rows rendered on Account) | Api | `api()` | yes | EXISTS |
| `Sdk().Logout()` (sign out row; `ResetForSignOut` wipes page state) | teardown + auth wipe | `Logout()` | yes | EXISTS |
| Version row: `Sdk().appVersion()` (urnet::version() empty in this SDK build) | app constant | none (add accessor; NB Linux hello already carries `urnet::version()` — non-empty there, verify before assuming) | yes | NEEDS-SDKHOST-METHOD (trivial) |

## 2.7 WIRING TABLE — Support destination (lives in SettingsPage.cpp; SupportView grid, card model)

| Windows call | Underlying | Linux | hpp | Class |
|---|---|---|---|---|
| `api().sendFeedback` (star rating + text + include-logs checkbox) | Api; `FeedbackSendResult` carries ONLY `feedback_id` (no error field) | `api()` | yes (4 hits) | EXISTS |
| `Sdk().device().uploadLogs(feedbackId, ...)` — ONLY after the server accepted the report, ONLY when the box is ticked, keyed by the SERVER's feedback id; silent-failure is deliberate here and only here; `Sdk().hasDevice()` guard | Device (throws synchronously on C-call failure) | via `device()` accessor + `hasDevice()` | yes | EXISTS (consider a wrapped `UploadLogs(feedbackId)` on Linux SdkHost for the lock discipline) |
| Snackbars: `thanks_for_the_feedback` (Success, auto-dismiss 4000 ms) / `error_sending_feedback` (Error, persists) | — | app-side | n/a | APP-SIDE |

## 2.8 WIRING TABLE — Developer destination (DeveloperPage.cpp; Advanced-only; all RPC on one serial FIFO bridge thread; 5 s poll only while destination selected AND presenting AND Advanced)

| Windows call | Underlying | Linux | hpp | Class |
|---|---|---|---|---|
| `Sdk().ReadReliability()` (x6: the poll + refreshes) -> `ReliabilitySnapshot{haveDevice, remoteConnected, settings, metrics, exits, destinationExits, probeSuiteRunning, probeResults}` — ONE read under one lock so the four getters cannot disagree | `device_->getRemoteConnected`, `getReliabilitySettings` (NULLOPT = "nothing in force", NOT all-off — display may substitute defaults, WRITE must not), `getReliabilityMetrics`, `getExits`, `getDestinationExits`, `probeSuiteRunning`, `getProbeResults` | none | yes (all, on DeviceRemote hpp:10769+) | NEEDS-SDKHOST-METHOD |
| `Sdk().UpdateReliabilitySettings(mutate)` (34 toggles + numeric boxes; every edit is RMW of the WHOLE struct from a FRESH read; nullopt read = NO-OP never a zero-write; returns the read-back applied settings) | `getReliabilitySettings` -> mutate -> `setReliabilitySettings` -> `getReliabilitySettings` | none | yes | NEEDS-SDKHOST-METHOD (port the nullopt-guard verbatim — the zero-write bug shipped once) |
| `Sdk().RunReliabilityAction(action, exitClientId)` -> `ReliabilityActionResult{issued, hasCount, declined, count}` | ResetMetrics -> `resetReliabilityMetrics`; ResetSettings -> `resetReliabilitySettings`; ProbeAllExits -> `probeAllExits` (int64_t count); SimulateNetworkChange -> `simulateNetworkChange`; Sync -> `sync`; MigrateExit -> `migrateExit` (int64_t; negative = not-found sentinel, test `< 0` never `<= 0`; zero is a real count) | none | yes | NEEDS-SDKHOST-METHOD |
| `Sdk().DropExit(id)` / `StallExit(id, stalled)` (x2: stall + unstall) / `ShuffleExits()` (D6 fault injection: IMMEDIATE-OR-NOTHING, never queued/retried; log must name the exit) | `device_->dropExit / stallExit / shuffleExits` | none | yes | NEEDS-SDKHOST-METHOD |
| `Sdk().StartProbeSuite(nullopt)` / `StopProbeSuite()` | `device_->startProbeSuite(urnet::getDefaultProbeSuiteConfig())` — nullopt config asks for the SDK default, never a zeroed struct / `stopProbeSuite` | none | yes | NEEDS-SDKHOST-METHOD |
| `Updates().Current()` / `Updates().CheckNow()` (check line: CheckOutcome + newestVersion; dev builds still run the check, never apply) | UpdateChecker worker | none | n/a | MISSING component |
| `Sdk().SetModeNoticeObserver` + `RefreshModeNotice()` (Developer InfoBar copy of the notice channel) | notice channel | none | n/a | NEEDS-SDKHOST-METHOD (shared with window level) |
| Session-less actions guarantee | Developer works over RPC with NO tunnel (rpc-only session) | Linux has no rpc-only session mode | n/a | PROTOCOL-GAP (no `rpc_only` start mode) |

---

## 3. LiveStats — full Windows field inventory vs Linux

Windows `urnw::LiveStats` (SdkHost.h:86-162). UI readers grep-verified in ConnectPage/MainWindow/AppController/TrayIcon.

| Field | Source | In Linux LiveStats? |
|---|---|---|
| connectionStatus (rpc-only clamps to literal "RPC_ONLY") | connectVc getConnectionStatus | YES (no clamp — no rpc-only mode) |
| connected (forced false in rpc-only) | connectVc getConnected | YES |
| providerCount | grid.getWindowCurrentSize | YES |
| downBitsPerSecond / upBitsPerSecond | contractVc getThroughputPoints | YES |
| rpcOnly / rawConnectionStatus / rawConnected (Advanced strip "Raw" field sees through the clamp) | session mode + pre-clamp values | NO — blocked on PROTOCOL-GAP (rpc_only mode) |
| insufficientBalance | device getContractStatus().InsufficientBalance | YES |
| provideEnabled / providePaused / provideClients / provideMode | device getters | YES |
| provideHasNetworkKey | addProvideSecretKeysListener-fed atomic (no getter exists on DeviceRemote — both hosts cache the listener) | YES |
| locationName / countryCode / countryName | device getConnectLocation | NO — NEEDS-SDKHOST-METHOD (status strip "selected_provider", DNS recommendations) |
| gridPoints / gridWidth / gridHeight (hero canvas dots; EMPTY list is a NORMAL state — nil slice marshals as document `null`, ReadSdkList -> nullopt -> bare lattice, never an error) | grid.getProviderGridPointList | NO — NEEDS-VC-EXPOSURE |
| health (State enum) / provenProviderCount (grid Added cells) / healthReevalAtMillis (clock-driven re-ask; ConnectPage 1 s tick -> RepublishStats) | app-side ConnectionHealth.h Tracker folded in ReadStats | NO — APP-SIDE (port ConnectionHealth.h) |
| windowStallReason ("evaluating" / "platform-unreachable" / "providers-unresponsive" / "rate-limited" / "auth-failing"; empty = nothing attempted) / windowFailed (both 45 s outcome deadlines blown) | device getWindowStatus().StallReason / .Failed | NO — NEEDS-SDKHOST-METHOD (getter verified on shared Device base) |

## 4. proto::TunnelStatus — fields the Windows UI reads vs the Linux control protocol

Windows readers: MainWindow status strip reads `state, mode, routes_installed, dns_applied, wfp_state, rpc_listen_hostport, stop_reason, failsafe_armed`; AppController/tray reads `state, wfp_state, stop_reason`; SdkHost internally consumes `egress_index4/egress_index6` (ApplySdkEgressBind — R1 self-exclusion of the GUI's own SDK sockets) and `rpc_listen_hostport`, `mode`.

Linux `status` reply today (ControlProtocol.hpp, doc §9.2 — CONFIRMED against code, no drift): `tunnel_state (stopped|starting|up|stopping|error)`, `rpc_port` (0 when down), `client_id`, `error`. **Lacks:** `mode`/rpc_only, `routes_installed`, `dns_applied`, `wfp_state` (nftables analogue), `stop_reason` ("" | "user" | "failsafe_no_exit" | "failsafe_no_inbound" | "failsafe_sdk_unresponsive"), `failsafe_armed`, `egress_index4/6`, `tunnel_local_up_millis`, and there are **no pushed status events at all** (client polls). Also missing verbs: `set_kill_switch`, `set_split_tunnel`, `logout`, rpc-only start mode. Extend the existing protocol (keep its two-way version + exact SDK-version enforcement); do not replace it. Until extended, the Advanced status strip's Routes field ("off" / "off, kill switch armed" / "on, dns not applied" / "on, no leak guard" / "on"), the stop-reason disclosure ("it turned itself off to keep you online"), the failsafe warning, and the R1 egress bind CANNOT be built.

## 5. UpdateChecker (Windows) — public surface inventory (no Linux counterpart exists)

- `Phase { None, Available, Applying, ManualUnzip, Failed }` — one phase = one banner rendering; None = no banner.
- `Stage { Idle, Downloading, Verifying, Extracting, Swapping }` — meaningful while Applying.
- `Failure { None, Download, Checksum, Extract, Swap, SwapDirty }` — SwapDirty = rollback itself failed; banner wording owns that honesty.
- `CheckOutcome { NeverRan, InFlight, NoUpdate, UpdateFound, DevBuild, Failed }` — the developer line, separate from the banner phase.
- `Snapshot { phase, stage, failure, version (v-less), code, zipPath (ManualUnzip), lastCheck, newestVersion, newestCode }`.
- Methods: `Start()` (cleanup, launch check after ~30 s, then every 6 h), `Stop()` (bounded join), `Current()` (valid any time), `SetHandler` (store-only; bind then replay Current), `SetRelaunchHandler`, `CheckNow()`, `BeginApply()`, static `AutoCheckEnabled()`, `SetAutoCheckEnabled(bool)` (ON triggers an immediate check), static `RevealInExplorer(file)`. Handlers fire on the WORKER thread; standing-value/bind-then-replay contract identical to Advanced Mode. Dev build (version code 0) never self-applies; manual check still runs and reports.
- Call sites: MainWindow (SetHandler, Current, BeginApply, RevealInExplorer), AppController (SetRelaunchHandler, Start/Stop), SettingsPage (AutoCheckEnabled seed, SetAutoCheckEnabled), DeveloperPage (Current, CheckNow), ConnectPage renders the UpdateBar from the relayed snapshot.
- Linux: no UpdateChecker file exists. Packaging (AppImage zsync / distro repo) is the expected answer (doc §10); if an in-app banner ships, port this exact state model so the banner phases stay honest.

## 6. SubscriptionBalanceStore — Windows <-> Linux name map (behaviour already at parity)

| Windows | Linux |
|---|---|
| `Initialize(DispatcherQueue)` | ctor + Glib timeouts |
| `Start()` / `Stop()` | `Start()` / `Stop()` |
| `SetVisible(bool)` | `SetWindowVisible(bool)` |
| `Refresh()` | `FetchNow()` |
| `OnJwtRefreshed()` | `OnJwtRefreshed()` |
| `StartConfirmationPolling()` / `ClearTimeout()` | `StartConfirmationPolling()` / `ClearPurchaseConfirmationTimeout()` |
| `Current() -> BalanceSnapshot{used,pending,available,startBalance,isPro,guest,loaded}` | getters `UsedByteCount/PendingByteCount/AvailableByteCount/StartBalanceByteCount/IsPro/IsGuest/HasFetched` |
| `CurrentPoll() -> {confirming, timedOut}` | `IsPolling()` / `PurchaseConfirmationTimedOut()` |
| `SetChangeHandler(snapshot, poll)` | `SetChangedHandler()` (re-read accessors) |
| — | Linux extras: `DidDetectUpgradeToPro()` (drives `ResetProvideToNever` once), `TotalReferrals()`, `ReferralCode()` (ride the same poll) |

Underlying call both sides: `urnet::Api::subscriptionBalance` -> `urnet::SubscriptionBalanceResult` (hpp-verified); jwt seed via `localState parseByJwt`; disagreement triggers `RefreshJwt()` (`device refreshToken`). Cadence both sides: 30 s background (not-Pro or Pro-without-balance), 5 s confirmation, 2-minute ACTIVE-polling budget banked across hide/show; re-show always fetches once.

## 7. Aggregate Linux gap list (build order recommendation)

1. **PROTOCOL-GAP (milestone):** pushed status events + fields `mode/rpc_only, routes_installed, dns_applied, wfp_state, stop_reason, failsafe_armed, egress_index4/6`; verbs `set_kill_switch, set_split_tunnel, logout`. Blocks: Advanced status strip (4 fields), kill-switch enforcement honesty, per-app rules, stop-reason/failsafe copy, R1 egress bind, rpc-only Developer mode, rawConnectionStatus/rpcOnly stats.
2. **NEEDS-SDKHOST-METHOD, reliability block:** `ReadReliability`, `UpdateReliabilitySettings` (nullopt-guarded RMW), `RunReliabilityAction` (+ ReliabilityActionResult semantics: hasCount only for migrateExit/probeAllExits; declined on negative), `DropExit/StallExit/ShuffleExits`, `StartProbeSuite/StopProbeSuite` (+ default config), serial bridge-thread discipline. All SDK symbols verified present. Blocks: Developer destination and Home Pane C exit-join.
3. **NEEDS-SDKHOST-METHOD, stats extension:** locationName/countryCode/countryName, windowStallReason/windowFailed, gridPoints/gridWidth/gridHeight (NEEDS-VC-EXPOSURE via held connectVc), RepublishStats; APP-SIDE port of ConnectionHealth.h (health, provenProviderCount, healthReevalAtMillis). Blocks: hero canvas, status line/strip, tray tooltip.
4. **NEEDS-SDKHOST-METHOD, chooser hardening:** class-A api locations fallback (`getProviderLocations`/`findProviderLocations` + `getFilteredLocationsFromResult` + Loading/Loaded/Error states + generation-guarded fetches + single-writer gate), `EnsureLocations`, `ConnectFromRow`/`ConnectBestAvailableFromRow` coalescer (~1.2 s settle, last-click-wins, idempotent re-click), `SameId/IsBestAvailableSelected/IsLocationSelected`, `RemoteConnected()` wrapper.
5. **NEEDS-SDKHOST-METHOD, auth/misc:** Google SSO trio (`SsoGoogleEnabled/SignInWithGoogle/HasPendingAuthJwt` + auth-jwt CreateNetwork mode), `SignWithSolanaWallet` (bare sign for Seeker), `apiReady()`, `appVersion()`, mode-notice channel (handler + observer + refresh, standing state), `EnsureSession(attach-only)` + service watchdogs, Advanced Mode block (APP-SIDE prefs).
6. **NEEDS-SDKHOST-METHOD, per-app split:** `SetAppRule/RemoveAppRule/CurrentAppRules` + `getLocalOverrideAppIds`-driven push (mechanism differs on Linux — cgroup/fwmark — but the SdkHost surface should match).
7. **MISSING component:** UpdateChecker (packaging decision first).
8. **MISSING-IN-SDK: nothing.** Every referenced symbol exists in the vendored hpp.

## 8. Vendored-hpp grep evidence (hit counts in `urnetwork-linux/app/third_party/urnetwork-sdk/amd64/urnetwork_sdk.hpp`)

Api: authLogin 2, authLoginWithPassword 2, authCodeLogin 2, authCodeCreate 2, authVerify 2, authVerifySend 2, authPasswordReset 2, authNetworkClient 2, networkCreate 2, upgradeGuest 2, validateReferralCode 2, getNetworkUser 4, changeNetworkName 2, claimNetworkName 2, getNetworkRedeemedBalanceCodes 2, getNetworkReferralCode 2, subscriptionBalance 2, redeemBalanceCode 2, createStripeCheckoutSession 2, stripeCreateCustomerPortal 2, createAccountWallet 2, getAccountPayments 4, getAccountPoints 2, getAccountWallets 2, getLeaderboard 2, getNetworkLeaderboardRanking 2, getNetworkReliability 2, getPayoutWallet 2, getTransferStats 2, setNetworkLeaderboardPublic 2, verifySeekerHolder 2, walletBalance 2, walletValidateAddress 4, removeWallet 4, setPayoutWallet 2, accountPreferencesGet 2, accountPreferencesUpdate 2, getNetworkClients 2, getReferralNetwork 2, removeAuth 2, sendFeedback 4, addAuth 2, deviceSetName 2, getNetworkBlockedLocations 2, networkBlockLocation 2, networkUnblockLocation 2, networkDelete 2, setNetworkReferral 2, unlinkReferralNetwork 2, getProviderLocations 4, findProviderLocations 2.

Device/DeviceRemote: uploadLogs 2, getClientId 2, refreshToken 2, getReliabilitySettings 4, setReliabilitySettings 4, getReliabilityMetrics 4, resetReliabilityMetrics 4, resetReliabilitySettings 4, getExits 4, getDestinationExits 4, getProbeResults 4, probeSuiteRunning 4, startProbeSuite 4, stopProbeSuite 4, migrateExit 4 (int64_t), probeAllExits 4 (int64_t), simulateNetworkChange 4, sync (DeviceRemote::sync at hpp:10824), dropExit 4, stallExit 4, shuffleExits 4, getWindowStatus 2 (Device base, hpp:10431), getContractStatus 2, getNetworkPeers 2, getProvideEnabled 2, getProvidePaused 2, getProvideMode 4, getConnectLocation 4, getRemoteConnected 2 (DeviceRemote hpp:10791), getBlockerEnabled 4, setBlockerEnabled 4, getDnsResolverSettings 4, setDnsResolverSettings 4, getPerformanceProfile 4, setPerformanceProfile 4, getRouteLocal 4, setRouteLocal 4, getProvideControlMode 4, setProvideControlMode 4, getBlockActionOverrides 4, setBlockActionOverrides 4, addBlockActionOverride 2, removeBlockActionOverride 2, getLocalOverrideAppIds 4, getProviderIdentities 4, removeConnectedProvider 2, getConnectedProviderLocations 2, setRpcServer 4; listeners addProvideSecretKeysListener 2, addTunnelChangeListener 2, addConnectLocationChangeListener 2, addContractStatusChangeListener 2, addProvideChangeListener 2, addProvidePausedChangeListener 2, addBlockActionOverridesChangeListener 2, addBlockerEnabledChangeListener 2, addConnectedProviderLocationChangeListener 2, addDnsResolverSettingsChangeListener 2, addProviderIdentityChangeListener 2, addAuthLogoutListener 4, addJwtRefreshListener 4, addRemoteChangeListener 2.

View controllers + methods: ConnectViewController 28 (getConnected 2, getConnectionStatus 2, getGrid 2, getSelectedLocation 2, connect/connectBestAvailable/disconnect, addConnectionStatusListener 2, addGridListener 2, addSelectedLocationListener 2), ContractViewController 25 (getThroughputPoints 2, addThroughputListener 2), ContractDetailsViewController 35 (getContractRows 2, setAtTop 2, pendingCount 2, addContractRowsListener 2), BlockActionViewController 30 (addBlockActionsListener 2, addBlockActionStatsListener 2), LocationsViewController 22 (filterLocations 2, getFilteredLocations 2, getFilteredLocationState 2, addFilteredLocationsListener 2, start), PeerViewController 21 (getPeers 2, getConnectedCount 2), ProviderLocationsViewController 24 (getSelectedClientId 2, setSelectedClientId 2, stepSelection 2, removeProvider 2, addSelectedProviderLocationChangeListener 2), NetworkNameValidationViewController 10 (via `newNetworkNameValidationViewController(api)` hpp:19911; networkCheck/start/stop/close), PostQuantumIdentityViewController 21; openers openConnectViewController 4, openContractViewController 4, openContractDetailsViewController 4, openBlockActionViewController 4, openLocationsViewController 4, openPeerViewController 4, openProviderLocationsViewController 4, openPostQuantumIdentityViewController 4, closeViewController 4.

Types/free fns: ProviderGridPoint 10, FilteredLocations 16, NetworkPeerList 11, NetworkPeers 14, ConnectLocation 55, ByJwt 14, parseByJwt 4, ReliabilitySettings 16, ReliabilityMetrics 12, ExitList 7, DestinationExit 7, ProbeResult 7, ProbeResultList 7, ProbeSuiteConfig 12, getDefaultProbeSuiteConfig 1, DnsResolverSettings 25, SubscriptionBalanceResult 11, WalletAuthArgs 18, ThroughputPoint 7, ContractPeerRowList 4, BlockActionOverride 9, FindLocationsResult 12, FindLocationsArgs 10, getFilteredLocationsFromResult 1, LocationsLoading/Loaded/Error 1 each ("LOCATIONS_LOADED" etc.), NetworkCreateResult 11, newId 1, TAO 1, ProviderIdentityList 7, ConnectedProviderLocationList 7, WindowStatus 14 (struct at hpp:2224 with StallReason + Failed), ContractStatus 14, setEgressInterfaceIndex 1, version() at hpp:19567, StringList, AsyncLocalState 18, NetworkSpaceManager 19.

## 9. Behavioural constants the wiring must carry (timers, debounces, watchdogs, guards)

- Balance: 30 s background poll; 5 s confirmation poll; 2-minute ACTIVE budget, banked across visibility; re-show always fetches once; Pro-with-balance stops the periodic poll but never the on-focus fetch.
- Developer: 5 s reliability poll, gated on (destination selected AND presenting AND Advanced); identity-keyed table rebuild; one serial FIFO bridge thread for ALL device RPC.
- Row-click connect coalescer: ~1.2 s settle, last click wins, cancelled by Disconnect, superseded by immediate connects; re-click of current+active row is a no-op that also cancels a pending intent.
- Sync watchdog: kSyncSettleDeadline 5000 ms, generation-guarded, one bounded look, reattach falls back to fresh session.
- UpdateChecker: launch check ~30 s, cadence 6 h, worker-thread handlers, bind-then-replay.
- Seeker verify: bridge timeout 180 s; plain api watchdog 20 s; wallet sheet actions: 20 s watchdog + generation guard; verify-code resend cooldown 15 s.
- Snackbar: 4000 ms default auto-dismiss; Warning/Error persist until dismissed.
- Echo guards required on: leaderboard-public toggle, send-product-updates toggle, Advanced Mode apply, kill-switch toggle (re-read after write), blocker/provide segmented controls (listener echo).
- Loading vs Failed vs Empty must be three distinguishable renders on every async surface (locations state string; balance HasFetched/error; per-page Load states) — 401 and "empty" must never look identical.
- Never call blocking SdkHost surfaces (ReadReliability, UpdateReliabilitySettings, RunReliabilityAction, fault injection, StopServiceTunnel-equivalent) on the GTK main thread; marshal SDK callbacks onto the main loop; handlers invoked under host locks must only marshal.

## SDK surface referenced
- urnet::Api::authLogin
- urnet::Api::authLoginWithPassword
- urnet::Api::authCodeLogin
- urnet::Api::authCodeCreate
- urnet::Api::authVerify
- urnet::Api::authVerifySend
- urnet::Api::authPasswordReset
- urnet::Api::authNetworkClient
- urnet::Api::networkCreate
- urnet::Api::upgradeGuest
- urnet::Api::validateReferralCode
- urnet::Api::getNetworkUser
- urnet::Api::changeNetworkName
- urnet::Api::claimNetworkName
- urnet::Api::getNetworkRedeemedBalanceCodes
- urnet::Api::getNetworkReferralCode
- urnet::Api::subscriptionBalance
- urnet::Api::redeemBalanceCode
- urnet::Api::createStripeCheckoutSession
- urnet::Api::stripeCreateCustomerPortal
- urnet::Api::createAccountWallet
- urnet::Api::getAccountPayments
- urnet::Api::getAccountPoints
- urnet::Api::getAccountWallets
- urnet::Api::getLeaderboard
- urnet::Api::getNetworkLeaderboardRanking
- urnet::Api::getNetworkReliability
- urnet::Api::getPayoutWallet
- urnet::Api::getTransferStats
- urnet::Api::setNetworkLeaderboardPublic
- urnet::Api::verifySeekerHolder
- urnet::Api::walletBalance
- urnet::Api::walletValidateAddress
- urnet::Api::removeWallet
- urnet::Api::setPayoutWallet
- urnet::Api::accountPreferencesGet
- urnet::Api::accountPreferencesUpdate
- urnet::Api::getNetworkClients
- urnet::Api::getReferralNetwork
- urnet::Api::removeAuth
- urnet::Api::sendFeedback
- urnet::Api::addAuth
- urnet::Api::deviceSetName
- urnet::Api::getNetworkBlockedLocations
- urnet::Api::networkBlockLocation
- urnet::Api::networkUnblockLocation
- urnet::Api::networkDelete
- urnet::Api::setNetworkReferral
- urnet::Api::unlinkReferralNetwork
- urnet::Api::getProviderLocations
- urnet::Api::findProviderLocations
- urnet::Api::setByJwt
- urnet::Device::uploadLogs
- urnet::Device::getClientId
- urnet::Device::refreshToken
- urnet::Device::getWindowStatus
- urnet::Device::getContractStatus
- urnet::Device::getNetworkPeers
- urnet::Device::getProvideEnabled
- urnet::Device::getProvidePaused
- urnet::Device::getProvideMode
- urnet::Device::getConnectLocation
- urnet::Device::getBlockerEnabled
- urnet::Device::setBlockerEnabled
- urnet::Device::getDnsResolverSettings
- urnet::Device::setDnsResolverSettings
- urnet::Device::getPerformanceProfile
- urnet::Device::setPerformanceProfile
- urnet::Device::getRouteLocal
- urnet::Device::setRouteLocal
- urnet::Device::getProvideControlMode
- urnet::Device::setProvideControlMode
- urnet::Device::getBlockActionOverrides
- urnet::Device::setBlockActionOverrides
- urnet::Device::addBlockActionOverride
- urnet::Device::removeBlockActionOverride
- urnet::Device::getLocalOverrideAppIds
- urnet::Device::getProviderIdentities
- urnet::Device::removeConnectedProvider
- urnet::Device::getConnectedProviderLocations
- urnet::DeviceRemote::getRemoteConnected
- urnet::DeviceRemote::getReliabilitySettings
- urnet::DeviceRemote::setReliabilitySettings
- urnet::DeviceRemote::getReliabilityMetrics
- urnet::DeviceRemote::resetReliabilityMetrics
- urnet::DeviceRemote::resetReliabilitySettings
- urnet::DeviceRemote::getExits
- urnet::DeviceRemote::getDestinationExits
- urnet::DeviceRemote::getProbeResults
- urnet::DeviceRemote::probeSuiteRunning
- urnet::DeviceRemote::startProbeSuite
- urnet::DeviceRemote::stopProbeSuite
- urnet::DeviceRemote::migrateExit
- urnet::DeviceRemote::probeAllExits
- urnet::DeviceRemote::simulateNetworkChange
- urnet::DeviceRemote::sync
- urnet::DeviceRemote::dropExit
- urnet::DeviceRemote::stallExit
- urnet::DeviceRemote::shuffleExits
- urnet::DeviceRemote::setRpcServer
- urnet::Device::addProvideSecretKeysListener
- urnet::Device::addTunnelChangeListener
- urnet::Device::addConnectLocationChangeListener
- urnet::Device::addContractStatusChangeListener
- urnet::Device::addProvideChangeListener
- urnet::Device::addProvidePausedChangeListener
- urnet::Device::addBlockActionOverridesChangeListener
- urnet::Device::addBlockerEnabledChangeListener
- urnet::Device::addConnectedProviderLocationChangeListener
- urnet::Device::addDnsResolverSettingsChangeListener
- urnet::Device::addProviderIdentityChangeListener
- urnet::Device::addAuthLogoutListener
- urnet::Device::addJwtRefreshListener
- urnet::Device::addRemoteChangeListener
- urnet::ConnectViewController::getConnected
- urnet::ConnectViewController::getConnectionStatus
- urnet::ConnectViewController::getGrid
- urnet::ConnectViewController::getSelectedLocation
- urnet::ConnectViewController::connect
- urnet::ConnectViewController::connectBestAvailable
- urnet::ConnectViewController::disconnect
- urnet::ConnectViewController::addConnectionStatusListener
- urnet::ConnectViewController::addGridListener
- urnet::ConnectViewController::addSelectedLocationListener
- urnet::ConnectGrid::getWindowCurrentSize
- urnet::ConnectGrid::getProviderGridPointList
- urnet::ContractViewController::getThroughputPoints
- urnet::ContractViewController::addThroughputListener
- urnet::ContractDetailsViewController::getContractRows
- urnet::ContractDetailsViewController::setAtTop
- urnet::ContractDetailsViewController::pendingCount
- urnet::ContractDetailsViewController::addContractRowsListener
- urnet::BlockActionViewController::addBlockActionsListener
- urnet::BlockActionViewController::addBlockActionStatsListener
- urnet::LocationsViewController::filterLocations
- urnet::LocationsViewController::getFilteredLocations
- urnet::LocationsViewController::getFilteredLocationState
- urnet::LocationsViewController::addFilteredLocationsListener
- urnet::LocationsViewController::start
- urnet::PeerViewController::getPeers
- urnet::PeerViewController::getConnectedCount
- urnet::ProviderLocationsViewController::getSelectedClientId
- urnet::ProviderLocationsViewController::setSelectedClientId
- urnet::ProviderLocationsViewController::stepSelection
- urnet::ProviderLocationsViewController::removeProvider
- urnet::ProviderLocationsViewController::addSelectedProviderLocationChangeListener
- urnet::NetworkNameValidationViewController::networkCheck
- urnet::PostQuantumIdentityViewController (Linux pqiVc_)
- urnet::LocalState::getByJwt / setByJwt / setByClientJwt / parseByJwt / getPerformanceProfile / setPerformanceProfile / getRouteLocal / setRouteLocal / getProvideControlMode / setProvideControlMode / getBlockActionOverrides / setBlockActionOverrides
- urnet::AsyncLocalState::set
- urnet::getFilteredLocationsFromResult
- urnet::getDefaultProbeSuiteConfig
- urnet::newId
- urnet::version
- urnet::setEgressInterfaceIndex
- urnet::LocationsLoading / LocationsLoaded / LocationsError
- urnw::SdkHost::Initialize / IsLoggedIn / apiReady / HasSession / ServiceConnected / StartLogin / LoginWithPassword / LoginWithCode / LoginAsGuest / LoginWithSeedphrase / CreateInstantAccount / ConfirmInstantAccount / DiscardInstantAccount / CreateNetwork / UpgradeGuest / VerifyCode / ResendVerifyCode / SendPasswordResetLink / CheckNetworkName / SsoGoogleEnabled / SignInWithGoogle / HasPendingAuthJwt / SignInWithSolana / SignInWithBittensor / SignWithSolanaWallet / HasPendingWalletAuth / HandleDeepLink / CurrentNetworkServer / ApplyNetworkServer / ParsedJwt / RefreshJwt / Logout / ConnectBestAvailable / Connect / Disconnect / StopServiceTunnel / EnsureSession / EnsureLocations / SetLocationFilter / CurrentFilteredLocations / CurrentFilteredLocationState / ConnectedProvidePeers / ConnectedPeerCount / RemoteConnected / SelectedLocation / ConnectFromRow / ConnectBestAvailableFromRow / SetPresentationActive / CurrentStats / RepublishStats / CurrentThroughputPoints / CurrentContractRows / SetContractsAtTop / ContractsPendingCount / CurrentBlockActions / CurrentBlockCounts / CurrentSplitRules / CurrentDnsSettings / CurrentBlockerEnabled / CurrentPerformanceSettings / SetPerformanceSettings / SetBlockerEnabled / CurrentKillSwitch / SetKillSwitch / CurrentProvideControlMode / SetProvideControlMode / ApplyDnsSettings / CreateSplitRule / UpdateSplitRule / RemoveSplitRule / SetAppRule / RemoveAppRule / CurrentAppRules / ReadReliability / UpdateReliabilitySettings / RunReliabilityAction / DropExit / StallExit / ShuffleExits / StartProbeSuite / StopProbeSuite / ProbeSuiteRunning / GetProbeResults / CurrentProviderLocations / CurrentProviderIdentities / RemoveConnectedProvider / SelectedProviderClientId / SetSelectedProviderClientId / StepProviderSelection / CurrentAdvancedMode / SetAdvancedMode / RefreshAdvancedMode / RefreshModeNotice / SameId / IsBestAvailableSelected / IsLocationSelected / api() / device() / hasDevice() / appVersion() / linkHostName() + all Set*Handler/Observer slots
- urnw::SubscriptionBalanceStore (Windows): Initialize / Start / Stop / SetVisible / Refresh / OnJwtRefreshed / StartConfirmationPolling / ClearTimeout / Current / CurrentPoll / SetChangeHandler
- urnw::SubscriptionBalanceStore (Linux): Start / Stop / SetWindowVisible / FetchNow / OnJwtRefreshed / StartConfirmationPolling / ClearPurchaseConfirmationTimeout / IsPro / IsGuest / DidDetectUpgradeToPro / IsPolling / PurchaseConfirmationTimedOut / HasFetched / UsedByteCount / PendingByteCount / AvailableByteCount / StartBalanceByteCount / TotalReferrals / ReferralCode / SetChangedHandler
- urnw::UpdateChecker: Start / Stop / Current / SetHandler / SetRelaunchHandler / CheckNow / BeginApply / AutoCheckEnabled / SetAutoCheckEnabled / RevealInExplorer (+ Phase/Stage/Failure/CheckOutcome/Snapshot)
- proto (Windows pipe): hello / get_state / start_tunnel / stop_tunnel / set_split_tunnel / set_kill_switch / logout + pushed TunnelStatus events
- ctl (Linux socket, ControlClient): hello / status / start_tunnel / stop_tunnel / set_provide / location_override_available / location_override_write / location_override_clear (poll-only, no push)

## Flags
- DOC-vs-CODE: doc §7.4 load mapping omits the `leaderboard` nav-tag branch — MainWindow::LoadCurrentDestination (MainWindow.xaml.cpp:1370) also handles tag=="leaderboard" -> wallet_->LoadLeaderboard(). Code wins; the Linux port must fire LoadLeaderboard on that tag too.
- DOC-vs-CODE (clarification, not contradiction): doc §7.12 presents Support as its own destination; in code the Support card logic lives inside SettingsPage.cpp (SupportView grid at window level, strings/actions loaded by LoadSettings, preview snackbar routed via settings_->ShowPreviewSnackbar()). Do not look for a SupportPage file; a Linux port may keep support logic in its settings controller or split it, but the load fan-out must match.
- DOC accuracy confirmed: §9.2's Linux control-protocol gap note is exact against ControlClient.hpp/ControlProtocol.hpp — verbs hello/status/start_tunnel/stop_tunnel/set_provide/location_override_*; status reply only tunnel_state/rpc_port/client_id/error; no push; kDeviceRpcPort=12025. No drift found.
- SdkHost.h contains self-corrections that supersede older briefs: (a) migrateExit and probeAllExits RETURN int64_t (the earlier 'all void' comment was wrong — 'Migrated N flows' is reportable); (b) dropExit/stallExit/shuffleExits + probe suite ARE on DeviceRemote (older comment said DeviceLocal-only); (c) the seedphrase 'only copy' claim was weakened deliberately. Trust the current header text quoted in this spec.
- GAP (build-blocking for Home): Linux LiveStats lacks 14 Windows fields — rpcOnly, rawConnectionStatus, rawConnected, locationName, countryCode, countryName, gridPoints, gridWidth, gridHeight, health, provenProviderCount, healthReevalAtMillis, windowStallReason, windowFailed. All underlying SDK getters grep-verified present in the vendored hpp; health block is an app-side port of ConnectionHealth.h.
- GAP (build-blocking for Network): Linux locations feed is tunnel-scoped (VC-only, nullopt when down) but Windows guarantees THE PROVIDER LIST IS ALWAYS AVAILABLE via the in-process Api fallback (getProviderLocations/findProviderLocations + getFilteredLocationsFromResult + Loading/Loaded/Error states, generation-guarded, single-writer gate). Doc §7.8's three distinguishable empty states depend on it. Must be added to the Linux SdkHost.
- GAP (protocol, milestone): no pushed status events; no mode/rpc_only, routes_installed, dns_applied, wfp_state, stop_reason, failsafe_armed, egress_index4/6 fields; no set_kill_switch / set_split_tunnel / logout verbs. Blocks the Advanced status strip Routes/Session/Raw fields, kill-switch enforcement honesty, per-app split rules, failsafe/stop-reason disclosures, and the R1 GUI-egress self-exclusion (doc §5.8 calls R1 EXISTENTIAL — urnet::setEgressInterfaceIndex exists in the vendored hpp, but the daemon must report the egress index for the GUI to bind to). Extend the existing versioned protocol; do not replace it.
- GAP: entire Windows reliability/developer bridge (ReadReliability, UpdateReliabilitySettings, RunReliabilityAction, DropExit/StallExit/ShuffleExits, probe suite) is absent from the Linux SdkHost while every SDK symbol exists — pure NEEDS-SDKHOST-METHOD work. Port the two safety rules verbatim: nullopt ReliabilitySettings read is 'nothing in force' and must NEVER be written back as zeros; fault injections are immediate-or-nothing, never queued/retried.
- GAP: row-click connect coalescer (ConnectFromRow/ConnectBestAvailableFromRow, ~1.2 s settle, last-click-wins, idempotent re-click) and the SameId/IsBestAvailableSelected/IsLocationSelected predicates are missing on Linux; without them click bursts wedge the SDK's shared dial-pacing staircase (stuck pending-yellow exits — a bug Windows already paid for).
- GAP: Google SSO (SsoGoogleEnabled/SignInWithGoogle/HasPendingAuthJwt + the auth-jwt CreateNetwork mode and AuthResult.auth_needs_network routing) and the bare-sign SignWithSolanaWallet (Seeker verify) are missing on Linux; WalletConnect::SignMessage primitive already exists so the Seeker gap is thin.
- GAP: no UpdateChecker component on Linux (Settings 'Check for updates automatically' row and the Developer check line have nothing to wire to). Packaging decision (AppImage zsync vs repos, doc §10) should precede any port of the Phase/Stage/Failure/CheckOutcome model.
- SEMANTIC DIFFERENCE to resolve: Windows Connect/ConnectBestAvailable/Disconnect bring the session up themselves (non-blocking session worker, idempotent, loud on failure, D8 attach-only for non-gesture paths); Linux Connect/Disconnect are VC-only no-ops with the tunnel down and the UI calls StartTunnel explicitly. Recommend porting the Windows session-worker semantics into the Linux SdkHost so every surface (window, tray) gets identical behavior — otherwise each GTK surface must reimplement bring-up ordering.
- LINUX EXTRAS to preserve (do not regress to Windows shape): NetworkSpaceJson() for start_tunnel space pinning, Shutdown() quit-path (quit is not sign-out — guest jwt destruction hazard), set_provide daemon verb, location_override_* verbs, PostQuantumIdentityViewController exposure (PublicIdentityKeyHash/Key), ValidateReferralCode wrapper, ResetProvideToNever + DidDetectUpgradeToPro pro-upgrade side effect, ResendVerifyCode/SendPasswordResetLink error strings, both-ways protocol + exact SDK-version hello enforcement.
- OPEN QUESTION: guest sign-in — doc §7.6 says NO guest button on the Initial step (superseded by instant accounts) yet LoginAsGuest/GuestModeSheet remain wired on both platforms. Decide whether the Linux UI exposes it anywhere before wiring a button.
- OPEN QUESTION: urnet::version() is stated empty in the Windows SDK build (appVersion() exists because of it) but the Linux hello enforcement requires a NON-empty exact match (SdkVersionsAgree fails closed on empty). Verify the Linux SDK build stamps a version before shipping, or hello will always fail.
- RISK: Windows LiveStats.connectionStatus can carry the deliberately unrecognized literal 'RPC_ONLY' after the clamp; any Linux status-word switch must treat unknown strings as disconnected (claim less), matching the Windows contract, even before rpc-only mode exists.
- RISK: several Windows SdkHost calls BLOCK (ReadReliability, UpdateReliabilitySettings, RunReliabilityAction, fault injection, StopServiceTunnel) — the GTK port must keep them off the main loop (Windows uses a serial FIFO bridge thread for the developer surface; SetPresentationActive must also stay non-blocking per the D4 AppHangB1 history).
