# HOME/CONNECT — Implementation Spec for the GTK4/C++ Port
## Pane A (CONNECT rail) · Pane B (ACTIVITY) · Pane C (STATISTICS/INSPECTOR)

Source of truth (read for this spec, in full): `urnetwork-windows/app/src/App/ConnectPage.cpp` (2644 lines), `ConnectPage.h`, `MainWindow.xaml` lines 700–1443 (the `ConnectView` grid), `App.xaml` (styles/tokens), `UrColors.h`, `UrComponents.h/.cpp` (pane row builders), `Common/ConnectAction.h` (shared connect/disconnect predicate), `MainWindow.xaml.cpp` (InfoBar action wiring ~215–284, `UpdateBalanceWarning` ~1449, `ApplyBreakpoint` ~463), `StatsFormat.h`, `Strings/en/Resources.resw`. The hero canvas (`ConnectCanvas`) and the `TransferChart` internals are separate specs; this spec covers only their host slots, sizes, and feed wiring.

Notation: “Loc(key)” = shared localization store lookup (`Localized(key)`; a missing key renders the key id itself). “Adv(key, "English")” = store-first fallback: renders `Loc(key)` if the key ever lands in the store, else the given English. Every Adv key AND its exact English are listed. “dip” = device-independent px. All colors are the app’s single dark theme (no light theme).

---

## 0. Shell structure, breakpoints, Simple vs Advanced

`ConnectView` is a Grid (background `#101010`, style `UrPaneStyle`) of five columns:

| Col | Name | Width | Content |
|---|---|---|---|
| 0 | `ConnectPaneAColumn` → `ConnectPaneA` | 330 (fixed rail) — or `*` when it is the only pane | CONNECT |
| 1 | (auto) `ConnectPaneBRule` | 1px vertical rule, `UrPaneVRuleStyle` (Width 1, background `#1FFFFFFF`, stretch) | — |
| 2 | `ConnectPaneBColumn` → `ConnectPaneB` | `*` | ACTIVITY |
| 3 | (auto) `ConnectPaneCRule` | 1px vertical rule | — |
| 4 | `ConnectPaneCColumn` → `ConnectPaneC` | 380 (fixed) | STATISTICS / INSPECTOR |

Panes are floor-to-ceiling, edge-to-edge, separated by 1px rules, **never** gaps. No page padding, no card radius, no margins between panes.

**Breakpoints** (`MainWindow::ApplyBreakpoint`, the single responsive switch; widths in dip, measured on the root element’s `ActualWidth`):
- Advanced Mode, width ≥ 1000 (`kWideBreakpointDip = 1000.0`): **three panes** — connect 330 | activity `*` | statistics 380.
- Advanced Mode, width ≥ 640: **two panes** — connect 330 | activity `*` (Pane C column width → 0, rule + pane Collapsed).
- Advanced Mode, width < 640: **one pane** — connect takes `*`, Pane B column → 0 and Collapsed; connect content full width (MaxWidth ∞, Stretch).
- **Simple Mode, any width: one pane always.** `ConnectPaneAContent` gets `MaxWidth 480.0` and `HorizontalAlignment Center` (“one pane, 480dip cap, centred”). Panes B and C never exist on screen in Simple.
- Hero host (`ConnectCanvasHost`) `MaxWidth`: **190.0 in Advanced**, **320.0 in Simple** (the canvas clamps its own side to [168,288] off host width).
- `kUltraWideDip = 1800.0` exists but Home’s pane count keys off 1000 + Advanced only (doc self-notes this).
- Window default 1120×820 dip; min 400×480.

**Simple vs Advanced is structural, not cosmetic** (`ConnectPage::ApplyAdvancedMode(bool)`, pushed from MainWindow off SdkHost’s standing `advanced_mode` pref; the page never reads the pref itself; idempotent, no RPC):
- Normal/Simple: activity rows are static; Pane C is the statistics pane; session figures are the five a user cares about; Pane A shows the **“More options” disclosure** row gating provide/options/peers (collapsed by default; `moreOptionsExpanded_` resets per Simple reading).
- Advanced: every activity row is SELECTABLE; Pane C leads with a **connection inspector**; session figures gain the raw pre-clamp status and the exits count; contract rows show full client ids (selectable text); MoreOptionsToggle is Collapsed and MoreOptionsHost unconditionally Visible.
- On mode change: `if (!on) selectedConnectionId_.clear()`; rebuild `ApplyConnectionsList` (row TYPE changes Border↔Button), `ApplySessionRows`, `ApplyContractsList`, `ApplyInspector`, `ApplyMoreOptionsVisibility`, `ApplyConnectStatus`; if turning on, immediately `RefreshExitRouting()`.

---

## 1. Shared pane-shell tokens (exact values)

Metrics (App.xaml resource doubles):
- `UrPaneHeaderHeight` **40** — every pane’s header strip.
- `UrGroupHeaderHeight` **28** — group headers.
- `UrPaneRowHeight` **40** — standard row MinHeight.
- `UrPaneListRowHeight` **36** — list/table rows.
- `UrPaneRowTallHeight` **44** — two-line rows (not used on Home’s three panes).
- Key/value rows default **34** (builder default); connections/contracts/split-rule list rows **36**; peers list rows **34**.

Styles (all fonts are **PP Neue Montreal** = `UrBodyFontFamily`; SemiBold is synthesized):
- `UrPaneHeaderStyle` (Border): Height 40, Background `#151515`, BorderBrush `#1FFFFFFF`, BorderThickness 0,0,0,1, Padding 12,0.
- `UrPaneTitleStyle` (TextBlock): 12px SemiBold, CharacterSpacing **60** (1/1000 em units ≈ 0.06em letterspacing), Foreground `#F8F8F8`, VCenter, CharacterEllipsis.
- `UrPaneMetaStyle` (TextBlock): 11px, `#989898`, VCenter, CharacterEllipsis, NoWrap — the right-aligned count/figure in a header.
- `UrGroupHeaderStyle` (Border): Height 28, Background `#151515`, Border `#1FFFFFFF` 0,1,0,1 (rules top AND bottom), Padding 12,0.
- `UrGroupHeaderTextStyle`: 11px, CharacterSpacing **90**, `#989898`, VCenter, CharacterEllipsis.
- `UrPaneRowStyle` (Border): MinHeight 40, bottom hairline `#1FFFFFFF` (0,0,0,1), Padding 12,0, Stretch.
- `UrPaneRowButtonStyle` (Button): same metrics (MinHeight 40, Padding 12,0, bottom hairline, CornerRadius 0, transparent background); hover = fill `#1C1C1C` (`UrCardBrush`), pressed = `#2A2A2A`, disabled = Opacity 0.38; visual-state transition **150 ms**; system focus visuals on.
- `UrPaneActionButtonStyle` (icon-only header action, BasedOn UrPaneRowButtonStyle): 28w × 24h, BorderThickness 0, Padding 0, right-aligned, content centered. MUST carry an accessible name.
- `UrKeyTextStyle`: 13px `#989898`, VCenter, CharacterEllipsis, NoWrap (left half of a key/value row).
- `UrValueTextStyle`: 13px `#F8F8F8`, right-aligned (HorizontalAlignment Right + TextAlignment Right), CharacterEllipsis, NoWrap.
- `UrRowTitleStyle`: 13px `#F8F8F8`, VCenter, CharacterEllipsis, NoWrap.
- `UrSupportingTextStyle`: 12px `#989898`, wrapping (BasedOn UrLabelStyle + TextWrapping Wrap).
- `UrRowIconStyle` (FontIcon): family **Segoe Fluent Icons** (port note: substitute equivalent glyphs), 16px, `#989898`, VCenter, a11y Raw.
- `UrChevronIconStyle`: 14px, `#5A5A5A`, right-aligned.
- `UrEmptyGlyphStyle`: 28px, `#5A5A5A`, centered.
- `UrPaneActionPrimaryStyle` (the Connect button, filled form): Background `#638BFC` (`UrBlueMediumBrush`), Foreground `#101010` (`UrInverseTextBrush` — NOT white: 14px SemiBold needs 4.5:1; #101010 on #638BFC = 5.9:1), BorderThickness 0, **CornerRadius 4**, Height 40 (Min 40), **Margin 12,12,12,12**, Padding 12,0, font PP Neue Montreal **14px SemiBold**. Template = `UrButtonBaseStyle`’s Material state-layer: hover = translucent wash of the content color at opacity 0.08, pressed 0.12, disabled Root opacity 0.38, 150 ms state fades.
- `UrPaneActionSecondaryStyle` (outlined Disconnect form, BasedOn Primary): Background Transparent, Foreground `#F8F8F8`, BorderBrush `#38FFFFFF` (`UrBorderStrongBrush`), BorderThickness 1. Same geometry.
- `UrSwitchToggleStyle` (ToggleSwitch): OnContent/OffContent empty, MinWidth 0, right-aligned, VCenter. On-state fill `#638BFC` (hover `#7FA0FC`, pressed `#5378E0`, disabled `#33638BFC`). Each instance Width 44 in the markup, `LabeledBy` its row label.

Palette used on these panes: page `#101010`; pane/sheet/group-header surface `#151515`; hover/selected fill `#1C1C1C`; pressed `#2A2A2A`; hairline `#1FFFFFFF` (white@12%); strong border `#38FFFFFF`; text `#F8F8F8` / muted `#989898` / faint `#5A5A5A`; inverse text `#101010`; action blue `#638BFC`; accent lime `#EFF7BB` (selection marker only here — never the connect fill); green `#87FB67` (`kUrGreen`); coral `#FF6C58` (`kUrCoral`); amber `#F5C242` (`kUrAmber`); pink `#ED8FFF`; muted coral `#C8604F`; status-connecting yellow `#E6EA23` (`kStatusConnecting`); status-idle blue `#2A60FF` (`kStatusIdle`); coral-subtle pill bg `#26FF6C58` (coral @15%).

Formatting helpers (`StatsFormat`): `FormatByteCountCompact` IEC base-1024 (“996 B”, “1.2 KiB”, “3.4 MiB”, “1.1 GiB”); `FormatCountCompact` decimal base-1000 (“996”, “1.2k”, “34k”, “3.4M”); `FormatBitRate` decimal (“996 bps”, “1.2 Kbps”, “3.4 Mbps”, “… Gbps”); `RelativeTime` (“now”, “12s ago”, “3m ago”, “2h ago”). `Plural(key,n)` selects composite key `<key>.<CLDR category>` with `.other` fallback.

Dynamic row builders (port `UrComponents`):
- `MakePaneRow(height)`: Border, **fixed Height** (not MinHeight — uniformity is enforced), Padding 12,0, bottom 1px hairline.
- `MakePaneKeyValueRow(key, value, height=34)`: grid ColumnSpacing 8; key (`UrKeyTextStyle`, star) + value (`UrValueTextStyle`, auto). Key marked a11y-Raw; value’s accessible name = “{key}, {value}”.
- `MakePaneListRow(height=36)` (static) and `MakePaneListRowButton(height=36)` (selectable Button on `UrPaneRowButtonStyle`; Height AND MinHeight pinned): shared grid — ColumnSpacing 10; columns: **marker** (2px wide Border, Margin −10,0,0,0 to undo spacing, Opacity 0, a11y Raw), **dot** (Ellipse 7×7, VCenter, a11y Raw), **title** (`UrRowTitleStyle`, star), **meta** (`UrValueTextStyle` with Foreground forced muted `#989898`, auto). On the Button form title+meta are a11y Raw — the row’s own Name is the whole announcement, set by the caller.
- `SetPaneListRowSelected(row, selected)`: background `#1C1C1C` when selected else transparent; marker background `#EFF7BB` (accent), opacity 1/0. Selection carries **three channels**: fill + 2px leading accent bar (shape, not color-alone) + a11y name suffix.
- `SetTextOrCollapse(textblock, text)`: empty string ⇒ Collapsed (an empty TextBlock still spends StackPanel spacing).

Timers: one drawer clock (`chartTimer_`) at **100 ms** (~10 fps), started/stopped by presentation state (window shown && not minimized; focus loss does NOT stop it).

---

## 2. PANE A — CONNECT (x:Name `ConnectPaneA`, a11y landmark **Main**, named Loc("connect") = “Connect”)

Grid rows: header (auto) + body (star). Header: `UrPaneHeaderStyle` strip containing `PaneATitle` (`UrPaneTitleStyle`) = Loc("connect"). Body: vertical ScrollViewer (no horizontal) → StackPanel `ConnectPaneAContent` (Simple: MaxWidth 480 centred; Advanced: stretched).

Element order, top to bottom:

### 2.1 Status row
`Border` on `UrPaneRowStyle` with **MinHeight 0, Padding 12,14**. Inside: Grid ColumnSpacing 10:
- Col 0: `StatusDot` — Ellipse **10×10**, VerticalAlignment Top, **Margin 0,7,0,0** (pinned to the first 24px line, because the block can grow multi-line).
- Col 1: StackPanel Spacing 4, VCenter:
  1. `StatusText` — PP Neue Montreal, **FontSize 20, LineHeight 24, SemiBold**, wraps. (This is the “20sp status line”.)
  2. `ProtectionText` — `UrSupportingTextStyle` (12px muted, wraps), collapsed by default.
  3. `TrafficHeldText` — same style, collapsed by default.
  4. `StatusReasonText` — same style, collapsed by default.

**One writer**: `ConnectPage::ApplyConnectStatus()`. It renders the AGGREGATE health (`urnw::health::State`: NoService, Disconnected, Connecting, Evaluating, Connected, Degraded, Failed), not the raw SDK status. Inputs: `health_` (from LiveStats), `connectStatus_` (SDK connect controller: Disconnected/Connecting/DestinationSet/Connected/Failed, parsed case-insensitively from `getConnectionStatus()` strings `"CONNECTED"|"CONNECTING"|"DESTINATION_SET"|"CONNECT_FAILED"`, anything else ⇒ Disconnected), `connected_` (service tunnel), the window’s service facts (`statusRoutesInstalled()`, `statusWfpState()`, `statusStopReason()`, `statusFailsafeArmed()`, `statusSessionMode()`), and balance state (`balanceConfirming()`, `balanceBlocked()`).

`RenderHealth()` reconciliation (button label and status line must derive from ONE reading):
- If `connectStatus_` ∈ {Connecting, DestinationSet} AND health ∈ {Disconnected, NoService, Connected} ⇒ render Connecting. (Never overrides Evaluating/Degraded/Failed.)
- If `connectStatus_` == Disconnected AND health ∉ {NoService, Disconnected} ⇒ render Disconnected (a settled idle status outranks stale ACTIVE health).

Per-state rendering (text / dot color / hero state):

| render state | StatusText | dot | hero |
|---|---|---|---|
| Connected | Loc("connected") “Connected” | `#87FB67` kUrGreen | Connected |
| Evaluating | Adv("conn_finding_providers", "Finding providers…") | `#E6EA23` kStatusConnecting | Connecting |
| Degraded | Adv("conn_degraded", "Connection degraded — reconnecting") | `#FF6C58` kUrCoral | Connecting |
| Connecting | Loc("connecting_status_indicator") “Connecting to providers” | `#E6EA23` | Connecting |
| Failed | Adv("conn_failed", "Couldn't connect") | `#FF6C58` | Error |
| NoService / Disconnected, **machine still captured** (`statusRoutesInstalled()` true) | Adv("conn_disconnecting", "Disconnecting — your traffic is still going through the tunnel") | `#E6EA23` | Connecting |
| NoService / Disconnected, wfpState == "armed" | Adv("conn_blocked_kill_switch", "Blocked — kill switch on") | `#FF6C58` | Disconnected |
| NoService / Disconnected, plain | Loc("disconnected") “Disconnected” | `#2A60FF` kStatusIdle | Disconnected (sets `showNotProtected`) |

Seeded state at startup (ApplyStrings → ApplyConnectStatus): idle — “Disconnected”, blue dot, “Connect”.
NOTE (code wins over doc): the old idle copy “{network} is ready to connect” was **removed** (owner reconciliation R1). `SetNetworkIdentity(name, guestMode)` still caches the name and re-renders, but the current rendering doesn’t use it — the network name lives in the status strip.

**ProtectionText** (Simple only): shown iff `!advancedMode_ && showNotProtected` → Adv("conn_not_protected", "Your internet traffic is not protected."). Collapsed otherwise (Advanced omits it deliberately; also never shown on captured/kill-switch readings, which say something more precise).

**TrafficHeldText** (soft-kill-switch honesty, one composed string, collapsed when empty):
- If `connected_` && render ∈ {Evaluating, Degraded, Failed}:
  - wfpState ≠ "off" ⇒ Adv("conn_traffic_blocked", "No working provider right now — your traffic is blocked, not exposed. Disconnect to go back to your normal connection.")
  - wfpState == "off" ⇒ Adv("conn_traffic_blocked_unprotected", "No working provider right now — traffic sent into the tunnel is going nowhere, and leak protection is off, so some traffic may bypass it. Disconnect to go back to your normal connection.")
  - If `statusFailsafeArmed()` append `" "` + Adv("conn_failsafe_armed", "If nothing gets through shortly, URnetwork will turn the tunnel off automatically so you keep your internet.")
- Else if `!connected_` && `proto::IsFailsafeStop(statusStopReason())`:
  - wfpState ≠ "off" ⇒ Adv("conn_failsafe_blocked", "The tunnel could not carry traffic, so URnetwork shut it down. The kill switch is on, so nothing leaves this machine until you connect again or turn the kill switch off — nothing is leaking.")
  - else ⇒ Adv("conn_failsafe_restored", "URnetwork disconnected you to keep you online: the tunnel was up but nothing was getting through. Your traffic is going out normally now and is NOT protected. Press Connect to try again.")

**StatusReasonText** (stall diagnosis; collapsed when empty). While render ∈ {Connecting, Evaluating, Degraded, Failed}, map `LiveStats.windowStallReason`:
- "platform-unreachable" ⇒ Adv("conn_reason_platform", "Contacting the platform…")
- "providers-unresponsive" ⇒ Adv("conn_reason_providers", "Providers not responding — retrying…")
- "rate-limited" ⇒ Adv("conn_reason_rate_limited", "Rate limited — waiting…")
- "auth-failing" ⇒ Adv("conn_reason_auth", "Signing in to the platform is failing…")
- If render == Failed and no reason matched ⇒ Adv("conn_failed_detail", "No providers could be reached. Retry rebuilds the connection from scratch.")
- Plain "evaluating" renders nothing (headline already says Finding providers…).

The same text+dot is relayed to the window status strip via `w_.ApplyStatusStripConnection(text, dot)` — one derivation, two surfaces.

### 2.2 Hero slot (separate spec; slot facts only)
Border with bottom hairline (`#1FFFFFFF`, 0,0,0,1) and Padding 0,8, containing `ConnectHero` — a full-width Button, all backgrounds/borders transparent in every state, `UseSystemFocusVisuals False`, Click = `OnConnectToggle` (the hero IS a connect toggle). Content: `ConnectCanvasHost` Grid, **MaxWidth 190** (Advanced) / **320** (Simple), centred, a11y Raw. Hero’s accessible name is set on every status render to the current StatusText. Hover/focus wired: PointerEntered/Exited → `canvas_->SetHovered`, GotFocus (keyboard only: FocusState==Keyboard) → focus ring; LostFocus clears. Hero `IsEnabled` mirrors the Connect button’s enabled rule (below). Hero states set in the same function: Connected/Connecting/Disconnected/Error per table above, overridden by balance: `Processing` if `balanceConfirming()`, else `Error` if `balanceBlocked()` (processing wins).

### 2.3 Selected-provider row → LocationChooserSheet
`LocationRow` — Button, `UrPaneRowButtonStyle`, MinHeight **48**, Click → `ShowLocationChooserSheet()`. Grid ColumnSpacing 8:
- Col 0 (star): StackPanel Spacing 1, VCenter: `SelectedProviderLabel` FontSize 11, `#989898`, a11y Raw = Loc("selected_provider") “Selected provider”; `LocationText` FontSize 14, CharacterEllipsis, a11y Raw.
- Col 1 (auto): chevron FontIcon glyph U+E76C (ChevronRight), `UrChevronIconStyle`.

`LocationText` content (from `ApplyStats`): `stats.locationName`; if the selected location is a connected network peer (SelectedLocation has a non-empty `connect_location_id->client_id` found in `Sdk().ConnectedProvidePeers()`), show `PeerDisplayName(peer)` instead of the raw id. Empty ⇒ Loc("best_available_provider") “Best available provider”.
A11y: row Name = “Selected provider, {LocationText}” (`ApplyLocationRowName`, re-run on every stats push).
Opening the sheet: guard `w_.sheetOpen()` (one ContentDialog at a time), `Sdk().EnsureLocations()` (opens LocationsViewController + PeerViewController, idempotent), create `LocationChooserSheet`, seed with `Sdk().CurrentFilteredLocations()` + `Sdk().ConnectedProvidePeers()`, show modally; live updates via the locations/peers handlers while open.

### 2.4 The Connect button
`ConnectButton` — Button, Click = `OnConnectToggle`, style **swapped whole** by `ApplyConnectButtonStyle` (looked up from app resources so hover/pressed/disabled travel with it):
- `UrPaneActionSecondaryStyle` (outlined) iff the action is Disconnect; else `UrPaneActionPrimaryStyle` (filled blue) — including the Failed state (Retry is filled).

Label (boxed string): Failed ⇒ Loc("retry") “Retry”; else `ConnectActionIsDisconnect()` ⇒ Loc("disconnect") “Disconnect”; else Loc("connect") “Connect”.

**Shared predicate** (`urnw::gesture::ActionIsDisconnect(facts, health)` — same rule as the tray item):
`sdkActive = health ∉ {Disconnected, NoService}`; result = `sdkActive || (MachineIsCaptured && !BlockedByKillSwitch)` where MachineIsCaptured = routesInstalled || wfpState ∉ {"", "off"}; BlockedByKillSwitch = !routesInstalled && wfpState=="armed" (armed kill switch deliberately offers Connect, not Disconnect). ServiceFacts built as: pipeUp = health ≠ NoService; routesInstalled/mode/wfpState/stopReason from the window’s status cache; state = connected_ ? Up : Stopped.

**Click behavior** (`OnConnectToggle`):
1. `retry = RenderHealth()==Failed`. If NOT retry and predicate says Disconnect ⇒ `Sdk().Disconnect()`; return.
2. Else read `selected = Sdk().SelectedLocation()`; `bestAvailable = IsBestAvailableSelected(selected)` (nullopt, or `connect_location_id->best_available` true).
3. If retry: `Sdk().Disconnect()` first (tears down the failed session), then fall through to connect — Retry = disconnect + fresh connect.
4. **Optimistic local write BEFORE the RPC**: `connectStatus_ = Connecting; health_ = Connecting; ApplyConnectStatus();` (a press that produces no visible change is indistinguishable from a hang; the next SDK push corrects if it disagrees).
5. `Sdk().ConnectBestAvailable()` if bestAvailable, else `Sdk().Connect(*selected)` (the immediate entry points, NOT the coalesced row variants).

**Watchdog-disable** (in ApplyConnectStatus): `kConnectWatchdog = 8 s`. Only `connectStatus_ == Connecting` counts as transitional (DestinationSet and Connected are settled). On entering Connecting, stamp `connectingSince_`; after 8 s of continuous Connecting set `connectWatchdogFired_` and re-enable — a watchdog, not a latch (a hung connect must not trap the user). Leaving Connecting resets both. The tick loop re-runs ApplyConnectStatus each 100 ms frame while Connecting && !fired so the timeout takes effect without an SDK event.
**Balance gate**: `blocked = balanceConfirming() || balanceBlocked()` (processing wins over out-of-balance; matches iOS). `enabled = !blocked && (!transitional || watchdogFired)`. Applied to BOTH `ConnectButton` and `ConnectHero`.

### 2.5 Onboarding TeachingTip
`OnboardingTip` — declared here (between ConnectButton and the InfoBars) so it exists in the tree; content and target set in code (`MainWindow::MaybeShowOnboardingTip`; keys `onb_tip_title`/`onb_tip_subtitle`; skipped while the ServiceSetup banner is actionable). Renders as a popup; zero layout cost. (Out of this spec’s scope beyond position.)

### 2.6 The three InfoBars (stacked, in this order; all `IsClosable False`, Margin **12,8**)
All three are one-writer surfaces; MainWindow owns each snapshot and pushes every change through the ConnectPage renderer.

**(a) `BalanceWarning`** — Severity Warning. `IsOpen = insufficientBalance && !balance.isPro && !balancePoll.confirming` (from `MainWindow::UpdateBalanceWarning`; `insufficientBalance` arrives on LiveStats via `w_.SetInsufficientBalance(stats.insufficientBalance)`; auto-disconnect on empty balance happens in the SDK). Title = Loc("insufficient_balance") “Insufficient balance”. Message = Loc("insufficient_balance_message") “Add balance or a plan to keep connecting.” Action button (built once in MainWindow ctor): content Loc("become_supporter") “Get UR Pro”; click ⇒ guest ? `login().BeginGuestUpgrade()` : `ShowUpgradeSheet()`. UpdateBalanceWarning also re-runs ApplyConnectStatus so the hero’s error/processing states and this bar can never disagree.

**(b) `ServiceSetupBar`** — Severity Warning. Renderer `ApplyServiceSetup(snapshot)`. Show iff state ∈ {NotInstalled, Stopped, VersionMismatch}; Running / ConsoleMode / Unknown ⇒ `IsOpen false` (healthy needs no banner; a dev console must not be interfered with; no evidence, no claim). Per state:
- NotInstalled: title Adv("svc_setup_title","Set up the VPN service"); action Adv("svc_setup_action","Set up"); message Adv("svc_setup_message","URnetwork uses a Windows service to carry traffic. One click — Windows will ask for administrator permission.")
- Stopped: title Adv("svc_start_title","Start the VPN service"); action Adv("svc_start_action","Start"); message Adv("svc_start_message","The service is installed but not running.")
- VersionMismatch: title Adv("svc_update_title","Update the VPN service"); action **Loc("update")** “Update”; message Adv("svc_update_message","The installed service is a different version than this app."); if both versions known append `" (" + installed + " → " + sibling + ")"` (versions are data, never translated).
Overrides (message REPLACES the pitch, title stays): `snap.busy` ⇒ message = **Loc("site_ext_setting_up")** “Setting up…” and action button disabled; notice UacDeclined ⇒ Adv("svc_uac_declined","Windows asked for permission and the prompt was closed. Click again whenever you're ready."); notice ActionFailed ⇒ Adv("svc_action_failed","That didn't finish — the service is unchanged. Details are in the app log."). Action button click ⇒ `MainWindow::BeginServiceSetupAction()` (one elevated `urnetworkd install`; idempotent verb — Set up/Start/Update are three wordings of one action). The bar self-heals: window Activated re-probes the SCM.

**(c) `UpdateBar`** — default Severity Informational. Renderer `ApplyUpdateChecker(snapshot)`. Phase None ⇒ closed. Title in every phase = Adv("upd_available_title","Update available:") + `" v" + version`. Default action = Loc("update"), enabled. Per phase:
- Available: message Adv("upd_available_message","One click downloads the release, verifies it, applies it here and restarts the app. Updating the VPN service is a second click afterwards.")
- Applying: action disabled; stage messages — Downloading: Adv("upd_stage_downloading","Downloading the update…"); Verifying: Adv("upd_stage_verifying","Verifying the download…"); Extracting: Adv("upd_stage_extracting","Unpacking…"); Swapping: Adv("upd_stage_swapping","Applying the new files…")
- ManualUnzip: action Adv("upd_show_file","Show file"); message Adv("upd_manual_message","This folder isn't writable, so the app can't swap its own files. The verified download was shown in Explorer — quit the app and extract it over this folder.") + `" (" + zipPath + ")"` if known.
- Failed: **Severity Error**; messages — Download: Adv("upd_failed_download","The download didn't finish. Check the connection and click to try again."); Checksum: Adv("upd_failed_checksum","The download didn't match the release's checksums, so it was discarded. Click to try again."); Extract: Adv("upd_failed_extract","The downloaded update couldn't be unpacked. Details are in the app log."); SwapDirty: Adv("upd_failed_swap_dirty","The update couldn't be applied, and some previous files could not be put back — this folder may mix versions until a retry succeeds. Details are in the app log."); Swap: Adv("upd_failed_swap","The update couldn't be applied, and the previous files were put back. Details are in the app log.")
Action click ⇒ `MainWindow::OnUpdateBannerAction()` (checker lives on AppController; wiring is bind-then-replay so a banner found before the window existed still renders).

### 2.7 “More options” disclosure (Simple only)
`MoreOptionsToggle` — Button, `UrPaneRowButtonStyle`, MinHeight **36**, Click `OnMoreOptionsToggle`. Content: horizontal StackPanel Spacing 8, VCenter: `MoreOptionsChevron` FontIcon FontSize 12 (`UrRowIconStyle`) + `MoreOptionsLabel` FontSize 12 (`UrKeyTextStyle`) = Adv("conn_more_options","More options"); explicit a11y Name = same string.
One predicate (`ApplyMoreOptionsVisibility`, called only from ApplyAdvancedMode and the toggle; also seeded from ApplyStrings so a window that opens Simple collapses correctly): Toggle Visible iff Simple; `expanded = advancedMode_ || moreOptionsExpanded_`; `MoreOptionsHost` Visible iff expanded. Chevron glyph = **U+E70E (ChevronUp) when expanded, U+E70D (ChevronDown) when collapsed** — it says what pressing does next. Toggle click flips `moreOptionsExpanded_` and re-applies. Default collapsed in Simple; Advanced never shows the toggle row.

### 2.8 `MoreOptionsHost` contents (contiguous StackPanel; everything below is inside it)

**Provide group.** Group header (`UrGroupHeaderStyle`): horizontal StackPanel Spacing 8: the provide indicator — a 12×12 Grid holding `ProvideModeRing` (Ellipse 12×12, StrokeThickness **1.5**, Collapsed by default) over `ProvideModeDot` (Ellipse **7×7**) — then `ProvideModeLabel` (`UrGroupHeaderTextStyle`) = Loc("provide_mode") “Provide mode”.
Indicator semantics from `stats.provideMode` (an effective-tier bit set: 0 none, 1 network, 2 friends-and-family, 3 public) — per-case only:
- 3 (public): color = providePaused ? `#F5C242` amber : `#87FB67` green; **ring Visible** (dot + outer ring = Public tier; amber while paused because pause stops public provide only).
- 1 (network — also Auto while idle) or 2 (friends-and-family): green, solid dot, no ring.
- default/0: `#FF6C58` coral (not providing), no ring.
Ring stroke gets the same brush as the dot fill.

Provide mode picker row: Border `UrPaneRowStyle` **MinHeight 44, Padding 8,4** containing `ProvideModeBar` — a 4-item segmented control (WinUI SelectorBar; port as linked toggle group), VCenter, SelectionChanged `OnProvideModeChanged`. Items: `ProvideAutoItem` Loc("auto") “Auto” | `ProvideAlwaysItem` Loc("always") “Always” | `ProvideNetworkItem` Loc("network") “Network” | `ProvideNeverItem` Loc("never") “Never”.
- Seeding (`SeedConnectControls`, wrapped in `updatingControls_=true` echo guard): from `Sdk().CurrentProvideControlMode()` string — "auto"/"always"/"network" map to their items; **anything else (incl. "manual"/unknown) lands on Never** (conservative default).
- Change (guarded by `updatingControls_`): `Sdk().SetProvideControlMode(SelectedProvideMode())` where the reverse map is Auto→"auto", Always→"always", Network→"network", else "never". SDK side writes device (`device_->setProvideControlMode`) AND persists (`localState_->setProvideControlMode`).

`DiscoverableRow` — Border `UrPaneRowStyle` MinHeight **36**: Grid ColumnSpacing 8: FontIcon U+E704 FontSize 13 (`UrRowIconStyle`) + `DiscoverableText` (`UrKeyTextStyle`, FontSize 12). Content from stats: `provideEnabled && provideHasNetworkKey` ⇒ Loc("device_discoverable") “This device is discoverable” else Loc("device_not_discoverable") “Enable provide mode to make this device discoverable”. (A paused device stays discoverable — pause stops public provide only.)

`ProvideStatsRow` — Border `UrPaneRowStyle` MinHeight 36, **Collapsed by default**; `ProvideStatsText` (`UrKeyTextStyle` 12). Content: if `provideEnabled`: providePaused ? Loc("providing_paused") “Providing (paused)” : Plural("providing_client_count", provideClients) (“Providing to {} client/clients”); else empty. **The ROW collapses when the text is empty**, not just the text.

**Connect options group.** Group header: `ConnectOptionsLabel` = Loc("connect_options") “Connect options”.
Connection-mode row: Border `UrPaneRowStyle` MinHeight 44 Padding 8,4 → `ConnectionModeBar` segmented control, 3 items, SelectionChanged `OnConnectionModeChanged`: `ModeAutoItem` Loc("window_type_auto") “Auto” | `ModeWebItem` Loc("window_type_quality") “Web” | `ModeStreamingItem` Loc("window_type_speed") “Streaming”.
Then four toggle rows, each: Border `UrPaneRowStyle` (MinHeight 40) → Grid ColumnSpacing 10 {FontIcon 14px `UrRowIconStyle` | label (`UrRowTitleStyle`, star) | ToggleSwitch Width 44 `UrSwitchToggleStyle`, LabeledBy the label}:
1. glyph U+E71B, `FixedIpLabel` Loc("fixed_ip") “Fixed IP”, `FixedIpToggle` → `OnFixedIpToggled`
2. glyph U+EA18, `StrongAnonLabel` Loc("strong_anonymization") “Strong Anonymization”, `StrongAnonToggle` → `OnStrongAnonToggled`
3. glyph U+E72E, `PostQuantumLabel` Loc("post_quantum_encryption") “Post Quantum Encryption”, `PostQuantumToggle` → `OnPostQuantumToggled`
4. glyph U+EA39, `BlockerLabel` Loc("block_ads_and_trackers") “Block ads and trackers”, `BlockerToggle` → `OnBlockerToggled`

Behavior:
- **PerformanceProfile mapping**: `PushPerformanceSettings()` builds `{mode = selected segment, fixedIp = FixedIpToggle, allowDirect = !StrongAnonToggle (Strong Anonymization is the INVERSE of allow_direct), postQuantum = PostQuantumToggle}` and calls `Sdk().SetPerformanceSettings()`. SDK writes always send a profile (even Auto) so allow_direct/post_quantum persist in every mode: Auto ⇒ `window_type = WindowTypeAuto`, no window_size; Web ⇒ `WindowTypeQuality`; Streaming ⇒ `WindowTypeSpeed`; non-Auto sets `WindowSizeSettings{min,max}` = fixedIp ? {1,1} : {2,4}. Writes go to `localState_->setPerformanceProfile` (persistence) AND `device_->setPerformanceProfile` (live).
- Seeding: from `Sdk().CurrentPerformanceSettings()` (device profile first, else persisted; nil profile ≡ Auto/everything off; fixedIp derived as window_size min==1 && max==1; Web ⇔ WindowTypeQuality, Streaming ⇔ WindowTypeSpeed). FixedIpToggle.IsOn(settings.fixedIp); **FixedIpToggle enabled iff mode ≠ Auto**; StrongAnonToggle.IsOn(!allowDirect); PostQuantumToggle.IsOn(postQuantum); BlockerToggle.IsOn(`Sdk().CurrentBlockerEnabled()`).
- Mode change: if new mode == Auto and FixedIp is on, **quietly force FixedIp off** (inside the echo guard, single push after), set FixedIp enabled = (mode ≠ Auto), then one `PushPerformanceSettings()`.
- Each of the three profile toggles: guarded by `updatingControls_`, then `PushPerformanceSettings()`.
- Blocker toggle: guarded, then `Sdk().SetBlockerEnabled(IsOn)` (the device applies AND persists; the app stores nothing). Inbound echo: `SetBlockerEnabledHandler` → `ApplyBlockerUi(on)` — no-op if already equal, else set under the guard.
- **Echo guard**: a single `updatingControls_` bool wraps every programmatic segment/toggle write; every handler first checks it and returns.

**Network peers.**
`PeersLine` — Button, `UrPaneRowButtonStyle`, MinHeight **36**, Click → `ShowLocationChooserSheet()` (same sheet as the provider row). Content: horizontal StackPanel Spacing 8: `PeerDot` Ellipse **8×8** VCenter + `PeerCountText` FontSize 12 `#989898`, a11y Raw (the BUTTON carries the name).
`ApplyPeerCount(trigger)` (seeded at ApplyStrings with nullopt; called on every peers push and on remote attach/detach): a nullopt trigger re-renders the kept snapshot (does NOT clear it — only a real list replaces `peers_`). If `!Sdk().RemoteConnected()` (service RPC down): text AND button a11y name = Loc("peer_discovery_disabled") “Peer discovery disabled until connected”; dot = muted `#989898`. Else `count = Sdk().ConnectedPeerCount()` (ALL connected devices, provide or not); text/name = Plural("network_peer_count", count) (“You have {} other device/devices online”); dot **green `#87FB67` if count > 0 else amber `#F5C242`**.
Group header (`UrGroupHeaderStyle`): `PeersGroupLabel` = Loc("network_peers") “Network peers” + right-aligned `PeersGroupCount` (`UrPaneMetaStyle`) = `FormatCountCompact(count)` or empty at 0.
`PeersHost` — StackPanel of rows built by `ApplyPeersList()`: empty/none ⇒ one `MakePaneKeyValueRow(Loc("peer_discovery_disabled"), "", 34)`. Else per peer: `MakePaneListRow(34)` — dot green `#87FB67` if `peer.ProvideEnabled` else faint `#5A5A5A`; title = `PeerDisplayName(peer)`; meta = `peer.DeviceSpec` (what the device IS — distinguishes two phones with the same name). Rows are static, not clickable.

*(End of MoreOptionsHost.)*

---

## 3. PANE B — ACTIVITY (`ConnectPaneB`, a11y name Loc("activity") “Activity”)

Grid rows: header (auto), chart (auto), group header (auto), list (star).

1. **Header strip** (`UrPaneHeaderStyle`): Grid ColumnSpacing 12: `PaneBTitle` (`UrPaneTitleStyle`) = Loc("activity"); right-aligned `ThroughputText` (`UrPaneMetaStyle`). Content from every stats push: connected ⇒ `"↓ " + FormatBitRate(downBitsPerSecond) + "   ↑ " + FormatBitRate(upBitsPerSecond)` (three spaces between), else **collapsed** (SetTextOrCollapse).
2. **Chart slot**: Border bottom hairline, Padding 12,0 → `RemoteChartHost` Grid **Height 150** (remote TransferChart, legend Loc("remote"), colors green `#87FB67` bytes / pink `#ED8FFF` packets; separate spec). The chart is clipped to its host on every resize (a Canvas does not clip; unclipped it bleeds across the 1px rule into Pane C).
3. **Connections group header** (`UrGroupHeaderStyle`): `ConnectionsLabel` = Loc("connections") “Connections” + right `ConnectionsCount` (`UrPaneMetaStyle`) = Plural("host_count", blockActions.size()) (“{} host/hosts”) — always the FULL feed count, even though rendering caps at 200 rows.
4. **The list area** (star row) — one Grid holding both readings; they swap, never coexist:
   - `ConnectionsScroll` (vertical ScrollViewer) → `ConnectionsHost` StackPanel: the routing-decision list.
   - `SessionEmptyCard` — centred (H+V), IsHitTestVisible false, StackPanel Spacing 8: FontIcon glyph **U+E9D2** (`UrEmptyGlyphStyle`: 28px `#5A5A5A`) over `SessionEmptyText` (`UrSupportingTextStyle`, TextAlignment Center) = Loc("contracts_appear_connected") “Contracts appear here while connected.”
   - Rule (`ApplySessionCardsVisibility`): `showList = statsConnected && ConnectionsHost has ≥1 child`; list Visible/empty Collapsed iff showList, else inverse. The empty state is a centred line INSIDE the full-height pane, never a short card.

**Routing-decision list** (`ApplyConnectionsList`, rebuilt on every block-actions push and on resync; newest first as delivered; cap `kMaxRows = 200` — a cap, not a scroll budget):
Per `BlockActionItem`:
- Title precedence (`BlockActionTitle`, iOS parity): `matchedHosts.front()` → `hosts.front()` → `matchedIps.front()` → `ips.front()` → empty ⇒ Loc("unknown") “unknown”.
- **Verdict dot color** (three verdicts, not two): `block` ⇒ coral `#FF6C58` (blocked); else `local` ⇒ **amber `#F5C242`** (sent AROUND the tunnel — allowed and unprotected); else green `#87FB67` (tunnelled/allowed).
- Meta (right, muted): `FormatByteCountCompact(byteCount) + "   " + FormatCountCompact(packetCount) + " pkt"`.
- **Normal mode**: `MakePaneListRow(36)` — static, not focusable, not selectable (200 tab stops on the way to Connect is hostile). A11y Name on the row root = “{title}, {verdict}” where verdict = Loc("blocked")/Loc("local")/Loc("allowed") (“Blocked”/“Local”/“Allowed”) — the dot is Raw, so the name is the only place the color’s meaning exists for a screen reader.
- **Advanced mode**: `MakePaneListRowButton(36)` — a real Button: clickable, in tab order, Enter/Space invokable. Same dot/title/meta/name. Click → `SelectConnection(id)` with the **block-action id captured by value** (selection is held by id, NEVER index — the feed rebuilds and rows move; an index selection would silently start inspecting a different connection). Rows and ids are kept in parallel vectors for repainting.
- After rebuild: update ConnectionsCount, re-run visibility, `ApplyConnectionSelectionVisuals()`, `ApplyInspector()` (if the selection aged out of the feed the inspector must say so).

**Selection behavior**: clicking the selected row **clears** the selection (toggle). `ApplyConnectionSelectionVisuals` repaints WITHOUT rebuilding (a rebuild would destroy keyboard focus): per row `SetPaneListRowSelected` (fill `#1C1C1C` + 2px `#EFF7BB` leading marker) and the a11y name gains/loses the suffix `", " + Adv("adv_selected","selected")` (idempotent add/remove).

There is no separate loading/failed reading for this list — the two states are list vs empty-sentence (the connected flag gates which); flagged below.

---

## 4. PANE C — STATISTICS / INSPECTOR (`ConnectPaneC`, a11y name Loc("client_statistics") “Client statistics”)

Grid rows: header (auto) + body (star, vertical ScrollViewer → StackPanel). Header strip: `PaneCTitle` = Loc("client_statistics").

Body order, top to bottom:

### 4.1 Connection inspector (`InspectorGroup`) — Advanced Mode ONLY; sits ABOVE the charts (the selection is the most specific thing on screen)
Normal mode: the whole group is **Collapsed** (gone, not empty). Advanced: Visible; a custom a11y landmark named Adv("adv_inspector","Inspector").
- Group header (`UrGroupHeaderStyle`): `InspectorLabel` = Adv("adv_inspector","Inspector") + right `InspectorClearButton` (`UrPaneActionButtonStyle` 28×24, FontIcon U+E711 12px) — Visible only while a selection resolves to a live action; a11y name Adv("adv_clear_selection","Clear selection"); Click clears the selection and re-renders.
- `InspectorHeadline` — Border `UrPaneRowStyle`, MinHeight 0, Padding **12,10**: StackPanel Spacing 3: `InspectorTitle` (15px SemiBold, `#F8F8F8`, CharacterEllipsis NoWrap) then a horizontal StackPanel Spacing 7: `InspectorDot` Ellipse 8×8 (a11y Raw) + `InspectorVerdict` (12px, `UrKeyTextStyle`).
- `InspectorRowsHost` — StackPanel of `MakePaneKeyValueRow` rows (34px rhythm), fully rebuilt by `ApplyInspector()`.

**Empty/gone readings** (distinguishable): no selection ⇒ title Adv("adv_no_selection","No connection selected"); selection not in current feed ⇒ title Adv("adv_selection_gone","That connection is no longer listed"). Both: dot faint `#5A5A5A`; verdict line Adv("adv_select_a_row","Select a row in Activity to inspect it"); clear button Collapsed; no rows.

**Populated reading** — headline: title = BlockActionTitle (or Loc("unknown")); dot = verdict color (coral/amber/green as in Pane B); verdict text = block ⇒ Adv("adv_verdict_blocked","Blocked — no packets sent"); local ⇒ Adv("adv_verdict_local","Bypassed the tunnel — not protected"); else Adv("adv_verdict_tunnelled","Tunnelled through URnetwork").

Rows, in exact order (value falls back to Adv("adv_none","none") when the text is empty; “addText” rows use it):
1. Adv("adv_host","Host") — `join(hosts, ", ")`
2. Adv("adv_addresses","Addresses") — `join(ips)`
3. Adv("adv_matched","Matched") — matchedHosts joined, else matchedIps joined; row present only if non-empty
4. Adv("adv_protected","Protected") — block ⇒ Adv("adv_na","—"); local ⇒ Loc("off") “Off”; else Loc("on") “On”
5. Adv("adv_reason","Reason") — overrideId empty ⇒ Adv("adv_reason_default","Default policy"); else hasBlockOverride ⇒ Adv("adv_reason_block","Block override") / hasRouteOverride ⇒ Adv("adv_reason_route","Route override") / else Adv("adv_reason_override","Override")
6. Adv("adv_override_id","Override") — the overrideId itself (only when non-empty)
7. Adv("adv_packets_total","Packets (total)") — FormatCountCompact(packetCount) — TOTALS; there is no per-direction split on a block action and the label says so
8. Adv("adv_bytes_total","Bytes (total)") — FormatByteCountCompact(byteCount)
9. Adv("adv_last_decision","Last decision") — `RelativeTime(timeMillis, now)`; only if timeMillis > 0. (An age, NOT a duration — nothing records when a connection closed.)
10. **Exit routing** (only when NOT blocked), joined from the reliability snapshot via `RoutingForAddresses(action->ips)` — first ip matching a `DestinationExit.DestinationIp`, then its `ClientId` looked up in `exits_`:
    - Found: Adv("adv_via_exit","Via exit") = clientId; Adv("adv_exit_flows","Flows to this destination") = FormatCountCompact(dest.FlowCount); and if the exit record was found: Adv("adv_exit_tier","Exit tier") = `"{effectiveTier} / {tier}"`; Adv("adv_exit_flows_total","Exit flows") = FormatCountCompact; Adv("adv_exit_dial_failures","Dial failures") = FormatCountCompact; Adv("adv_exit_state","Exit state") = Quarantined ⇒ Adv("adv_exit_quarantined","Quarantined") / Warning ⇒ Adv("adv_exit_warning","Warning") / Proven ⇒ Adv("adv_exit_proven","Proven") / else Adv("adv_exit_ok","OK"); if warning && cause non-empty: Adv("adv_exit_warning_cause","Warning cause") = cause; if probeAgeSeconds > 0: Adv("adv_probe_age","Probe age") = `"{n}s"`.
    - Not found: Adv("adv_via_exit","Via exit") = Adv("adv_unknown_exit","Not in the routing table") — absent-not-guessed is the rule.
    - Then, if the SESSION’s countryName is non-empty: Adv("adv_session_exit_country","Session exit country") = countryName — labelled as the session’s, because per-exit geo is not bridged.
11. Adv("adv_action_id","Action id") — `ShortId(id)` (≤12 chars else first 12 + “…”), with **IsTextSelectionEnabled(true)** on the value (copyable).

Deliberately NOT shown (nothing on any reachable feed): protocol, port, per-direction counters, ASN/org, per-connection duration, per-connection RTT.

**Exit-routing cache refresh** (`RefreshExitRouting`): `Sdk().ReadReliability()` is several SYNCHRONOUS service RPCs — run on a background thread, marshal back to UI. Guards: `exitRefreshInFlight_` (single-flight); cadence from the drawer clock — every 10 ticks (~1 s) increment `exitRefreshTick_`, refresh when `% 5 == 0` ⇒ **every 5 s**, and ONLY while `advancedMode_ && ConnectView visible && window presenting`. Also fired immediately when Advanced Mode turns on. On completion store `exits_`/`destinationExits_` and re-run ApplyInspector only if something is selected. Failures log a warning and leave empty tables (the inspector then reads “Not in the routing table”).

### 4.2 Two chart slots
Border bottom hairline Padding 12,0 → `BlockedChartHost` Grid **Height 132** (legend Loc("blocked"), coral `#FF6C58` bytes / muted-coral `#C8604F` packets), then identically `LocalChartHost` **Height 132** (legend Loc("local"), green/pink). Same clip-to-bounds treatment. (Chart internals: separate spec.)

### 4.3 `data_usage` group
Group header: `DataUsageLabel` = Loc("data_usage") “Data usage”.
- `LiveStatsGroup` — StackPanel, **Visible iff stats.connected** (Collapsed otherwise so no blank rows). Contains `ProviderCountLine` — Button `UrPaneRowButtonStyle` MinHeight **34**, Click `OnProviderCountClick` → holds `ProviderCountText` (`UrKeyTextStyle`). Text (SetTextOrCollapse): shown iff `stats.connected && stats.health == Connected` (gated on the AGGREGATE — “Connected to N providers” must not appear under “Finding providers…”): Plural("connected_provider_count", providerCount) “Connected to {} provider/providers”. Click: `if (!connected_) return;` then `ShowProviderLocationsSheet()` — the globe sheet, seeded from the page’s cached `providerLocations_` (the feed is signal-only pushes) + `Sdk().RemoteConnected()`, identities seeded from `Sdk().CurrentProviderIdentities()`.
- `SessionRowsHost` — StackPanel rebuilt by `ApplySessionRows()` (every stats push, block-stats push, advanced-mode change). Rows (`MakePaneKeyValueRow`, 34):
  1. Loc("remote") “Remote” — connected ? `"↓ " + FormatBitRate(downBitsPerSecond)` : **"—"** (em dash: “no session”, not “zero”)
  2. Loc("local") “Local” — connected ? `"↑ " + FormatBitRate(upBitsPerSecond)` : "—"
  3. Loc("allowed") “Allowed” — FormatCountCompact(allowedCount)
  4. Loc("blocked") “Blocked” — FormatCountCompact(blockedCount)
  5. Loc("connections") “Connections” — FormatCountCompact(blockActions.size())
  Advanced adds:
  6. Adv("adv_raw_status","Raw status") — the PRE-CLAMP connection status: `stats.rpcOnly ? stats.rawConnectionStatus : stats.connectionStatus` (LiveStats clamps to "RPC_ONLY" in rpc-only sessions; Advanced sees through the clamp). Empty ⇒ Adv("adv_none","none").
  7. Adv("adv_exits","Exits") — FormatCountCompact(exits_.size()) — the denominator behind every inspector “via” line.

### 4.4 Contracts group
Group header: `ClientStatsLabel` = Loc("client_contracts") “Client contracts” + right trailing action `ClientStatsCard` (`UrPaneActionButtonStyle`, FontIcon U+E8A7 12px, a11y name Loc("client_contracts")) → opens **ClientContractsSheet** (seeded with cached contractRows_, live-updated via the contract-rows handler; on close `Sdk().SetContractsAtTop(true)` so the VC doesn’t stay frozen collecting a pending count).
`ContractsHost` — rebuilt by `ApplyContractsList()` on every contract push/resync/mode change:
- Empty ⇒ one `MakePaneKeyValueRow(Loc("contracts_appear_connected"), "", 34)` (still a ROW on the grid — never a hole).
- Else per peer: `MakePaneListRow(36)`; dot green `#87FB67` if `0 < lastActivityMillis && !closing` else faint `#5A5A5A`; title = **Advanced: full clientId, IsTextSelectionEnabled, CharacterEllipsis; Normal: ShortId(clientId)** (12 chars + …); meta = `"↑ " + FormatByteCountCompact(sendByteCount) + "   ↓ " + FormatByteCountCompact(receiveByteCount)`; row a11y Name = Loc("contract") “Contract” + `", " + clientId` (always full).

### 4.5 Split rules group
Group header (3 columns): `LocalStatsLabel` = Loc("split_rules") “Split rules”; middle `SplitRuleCountText` (`UrPaneMetaStyle`) = Plural("split_rule_count", n) “{} split rule/rules”; trailing `LocalStatsCard` (action button, U+E8A7, a11y name Loc("split_rules")) → **SplitRulesSheet** (seeded with splitRules_, blockActions_, allowed/blocked counts; live-updated; sheet has its own 1 s “Ns ago” refresh driven off the drawer clock).
`SplitRulesHost` — `ApplySplitRulesList()`:
- Empty ⇒ `MakePaneKeyValueRow(Loc("app_split_active_none"), "", 34)` “No app split — all apps use the VPN”.
- Else per rule: `MakePaneListRow(36)`; dot **amber `#F5C242` if routeLocal** (sent around the tunnel — same “not protected, on purpose” color as the connections table) else green; title = `hosts.front()` or Loc("unknown"); meta = hosts.size() > 1 ? Plural("host_count", hosts.size()) : Loc(routeLocal ? "local" : "remote") (“Local”/“Remote”).

### 4.6 Custom DNS group
Group header: `DnsCardLabel` = Loc("custom_dns") “Custom DNS” + trailing `DnsCard` (action button, FontIcon **U+E70F** 12px, a11y name Loc("custom_dns")) → **DnsEditorSheet** created with `(XamlRoot, Sdk(), dnsSettings_, countryCode_, countryName_)` (draft edits apply together; live pushes don’t reset the open editor).

**Unapplied-recommendation pill** `DnsRecPill` — Border `UrPaneRowStyle` MinHeight **34**, Background `#26FF6C58` (coral@15%), Collapsed by default; content: horizontal StackPanel Spacing 6: `DnsRecDot` Ellipse **7×7** (Collapsed unless country variant) + `DnsRecText` 12px `#F8F8F8` CharacterEllipsis.
`ApplyDnsRecommendationPill()` — recomputed on dns-settings changes AND on connected-country changes (country compared on each stats push; only refreshed when it changed). Priority (iOS DnsRecommendationPill parity):
1. No applied settings ⇒ hidden.
2. Connected country (code lowercased) has `urnet::getRecommendedDnsResolverSettings(code)`: if NOT `DnsSettingsEquivalent(applied, rec)` ⇒ show text `Format("dns_pill_recommended", countryDisplay)` “There are unapplied recommended settings for {}” (countryDisplay = countryName, else UPPERCASED code) with the dot Visible, filled from `urnet::getColorHex(code)` (hex parser accepts RRGGBB/#RRGGBB/AARRGGBB; fallback muted gray `#989898`). If the recommendation IS applied ⇒ hidden. **Never fall through to the default nudge when the country has a recommendation.**
3. Otherwise if `urnet::getDefaultDnsResolverSettings()` exists and differs from applied ⇒ show Loc("dns_pill_default") “The default safe settings are not applied”, dot Collapsed.
4. Else hidden.
`DnsSettingsEquivalent` = field-for-field: the five Enable* booleans plus all eight server lists (absent list ≡ empty list).

**Four status rows** `DnsRowsPanel` (Visible iff settings present) — each: Border `UrPaneRowStyle` MinHeight **34** → Grid ColumnSpacing 8 {dot Ellipse **6×6** VCenter | label (`UrKeyTextStyle`, star) | state (`UrValueTextStyle`, FontSize 12)}:
| Row | Label key / English | ON iff |
|---|---|---|
| `DohDot/DohLabel/DohState` | "dns_over_https" “DNS over HTTPS” | EnableRemoteDoh \|\| EnableLocalDoh |
| `UdnsDot/UdnsLabel/UdnsState` | "unencrypted_dns" “Unencrypted DNS” | EnableRemoteDns \|\| EnableLocalDns |
| `LdnsDot/LdnsLabel/LdnsState` | "local_dns" “Local DNS” | EnableLocalDoh \|\| EnableLocalDns |
| `FallbackDot/FallbackLabel/FallbackState` | "local_dns_fallback" “Local DNS fallback” | EnableFallback |
Dot semantics: on ⇒ green `#87FB67`; off ⇒ faint `#5A5A5A` **at alpha 102** (40%). State text: Loc("on")/Loc("off") “On”/“Off”; foreground green when on, muted `#989898` when off. Seeded to “Off” at ApplyStrings.

**Unavailable row** `DnsUnavailableRow` — Border `UrPaneRowStyle` MinHeight 34, Collapsed by default; `DnsUnavailableText` (`UrKeyTextStyle`, Foreground faint `#5A5A5A`) = Loc("dns_settings_unavailable") “DNS settings unavailable”. `ApplyDnsCard(settings)`: settings present ⇒ rows Visible/unavailable Collapsed; absent ⇒ inverse (the ROW swaps, not just text). The pill re-evaluates in both paths (collapses with the rows).

---

## 5. Feeds, caches, and the clock (wiring the implementer must reproduce)

**All SDK callbacks arrive on SDK threads**; marshal to the UI thread with the payload by value; resolve the weak window ref on the UI side only; no UI touched off-thread.

Handler → action map (`WireDrawerFeeds`):
- `SetThroughputHandler(points, windowSeconds)` → all three charts `SetPoints`.
- `SetContractRowsHandler` → re-read `Sdk().CurrentContractRows()` on the UI thread (settled snapshot), `ApplyContractsList()`, update open contracts sheet.
- `SetBlockActionsHandler(actions)` → cache, `ApplyConnectionsList()`, update open split-rules sheet.
- `SetBlockStatsHandler(allowed, blocked)` → cache, `ApplySessionRows()`, update open split-rules sheet.
- `SetSplitRulesHandler(rules)` → cache, `ApplySplitRuleCount()` (count text + rebuild list), update open sheet.
- `SetDnsSettingsHandler(optional settings)` → cache, `ApplyDnsCard`.
- `SetBlockerEnabledHandler(on)` → `ApplyBlockerUi` (echo-guarded).
- `SetLocationsHandler` → update open LocationChooserSheet only.
- `SetPeersHandler(optional list)` → `ApplyPeerCount` (+ open chooser update).
- `SetProviderLocationsHandler(rows)` → cache `providerLocations_`, update open globe sheet.
- `SetProviderIdentitiesHandler` → cache, update open globe sheet badges.
- `SetProviderSelectionHandler` → open globe sheet `RefreshSelection()` (selection read back on UI thread).
- `SetRemoteChangedHandler` → `ApplyPeerCount(nullopt)` (flips the disabled reading) + open globe sheet availability (an empty window while RPC is down is “unavailable”, not “no providers”).

**Resync** (`ResyncDrawer`, on login/tab entry): `Sdk().EnsureLocations()`; seed charts from `CurrentThroughputPoints(windowSeconds=60)`; seed `contractRows_/blockActions_/allowed/blocked/splitRules_/dnsSettings_` from the `Current*` getters; re-apply all lists; `SeedConnectControls()`.

**ApplyStats(LiveStats)** — the per-push relay (also carries: locationName/peer resolution → LocationRow; `connectStatus_`; `windowStallReason_`; `health_`; `healthReevalAtMillis_`; grid → hero; `ApplyConnectStatus()`; `ApplyPeerCount(peers)`; country → dns pill; ProviderCountText; ThroughputText; LiveStatsGroup visibility; down/up/providerCount/statsConnected caches → `ApplySessionRows()`; `ApplySessionCardsVisibility(connected)`; `SetInsufficientBalance`; ProvideStats row; provide indicator + discoverable line).

**OnChartTick** (every 100 ms): skip if window not visible. If `healthReevalAtMillis_` > 0 and now ≥ it (steady-clock millis): zero it and `Sdk().RepublishStats()` — a pending degrade hold expires on the CLOCK, before the per-pane gate, because the status strip renders health on every destination. If ConnectView visible: tick 3 charts + hero; while `connectStatus_ == Connecting && !watchdogFired` re-run `ApplyConnectStatus()`. Tick open contracts/provider sheets. Every 10th tick (~1 s): split-rules sheet RefreshTimes; and every 5th of those (5 s), gated on Advanced && ConnectView visible: `RefreshExitRouting()`.

**Sheets**: all ContentDialogs on the `#151515` sheet surface, one at a time via a window-level `sheetOpen` guard; each Show routine takes a strong ref, sets the guard, seeds from caches, awaits, resets.

---

## 6. Complete string inventory for these panes

**Store keys (Loc) — key = English:**
connect = Connect · activity = Activity · client_statistics = Client statistics · connections = Connections · data_usage = Data usage · network_peers = Network peers · selected_provider = Selected provider · best_available_provider = Best available provider · insufficient_balance = Insufficient balance · insufficient_balance_message = Add balance or a plan to keep connecting. · become_supporter = Get UR Pro · update = Update · site_ext_setting_up = Setting up… · connect_options = Connect options · window_type_auto = Auto · window_type_quality = Web · window_type_speed = Streaming · provide_mode = Provide mode · auto = Auto · always = Always · network = Network · never = Never · fixed_ip = Fixed IP · strong_anonymization = Strong Anonymization · post_quantum_encryption = Post Quantum Encryption · block_ads_and_trackers = Block ads and trackers · client_contracts = Client contracts · split_rules = Split rules · custom_dns = Custom DNS · contracts_appear_connected = Contracts appear here while connected. · dns_over_https = DNS over HTTPS · unencrypted_dns = Unencrypted DNS · local_dns = Local DNS · local_dns_fallback = Local DNS fallback · on = On · off = Off · dns_settings_unavailable = DNS settings unavailable · dns_pill_default = The default safe settings are not applied · dns_pill_recommended (Format, 1 arg) = There are unapplied recommended settings for {} · connected = Connected · connecting_status_indicator = Connecting to providers · disconnected = Disconnected · retry = Retry · disconnect = Disconnect · providing_paused = Providing (paused) · device_discoverable = This device is discoverable · device_not_discoverable = Enable provide mode to make this device discoverable · peer_discovery_disabled = Peer discovery disabled until connected · unknown = unknown · blocked = Blocked · allowed = Allowed · local = Local · remote = Remote · contract = Contract · app_split_active_none = No app split — all apps use the VPN

**Plural keys** (composite `<key>.one` / `.other`): connected_provider_count = “Connected to {} provider(s)” · providing_client_count = “Providing to {} client(s)” · network_peer_count = “You have {} other device(s) online” · host_count = “{} host(s)” · split_rule_count = “{} split rule(s)”

**Adv() keys (NOT in the store yet — ship key + English fallback; prefer the store the moment the key lands):**
conn_more_options=More options · conn_finding_providers=Finding providers… · conn_degraded=Connection degraded — reconnecting · conn_failed=Couldn't connect · conn_disconnecting=Disconnecting — your traffic is still going through the tunnel · conn_blocked_kill_switch=Blocked — kill switch on · conn_not_protected=Your internet traffic is not protected. · conn_traffic_blocked=No working provider right now — your traffic is blocked, not exposed. Disconnect to go back to your normal connection. · conn_traffic_blocked_unprotected=No working provider right now — traffic sent into the tunnel is going nowhere, and leak protection is off, so some traffic may bypass it. Disconnect to go back to your normal connection. · conn_failsafe_armed=If nothing gets through shortly, URnetwork will turn the tunnel off automatically so you keep your internet. · conn_failsafe_blocked=The tunnel could not carry traffic, so URnetwork shut it down. The kill switch is on, so nothing leaves this machine until you connect again or turn the kill switch off — nothing is leaking. · conn_failsafe_restored=URnetwork disconnected you to keep you online: the tunnel was up but nothing was getting through. Your traffic is going out normally now and is NOT protected. Press Connect to try again. · conn_reason_platform=Contacting the platform… · conn_reason_providers=Providers not responding — retrying… · conn_reason_rate_limited=Rate limited — waiting… · conn_reason_auth=Signing in to the platform is failing… · conn_failed_detail=No providers could be reached. Retry rebuilds the connection from scratch. · svc_setup_title=Set up the VPN service · svc_setup_action=Set up · svc_setup_message=URnetwork uses a Windows service to carry traffic. One click — Windows will ask for administrator permission. · svc_start_title=Start the VPN service · svc_start_action=Start · svc_start_message=The service is installed but not running. · svc_update_title=Update the VPN service · svc_update_message=The installed service is a different version than this app. · svc_uac_declined=Windows asked for permission and the prompt was closed. Click again whenever you're ready. · svc_action_failed=That didn't finish — the service is unchanged. Details are in the app log. · upd_available_title=Update available: · upd_available_message=One click downloads the release, verifies it, applies it here and restarts the app. Updating the VPN service is a second click afterwards. · upd_stage_downloading=Downloading the update… · upd_stage_verifying=Verifying the download… · upd_stage_extracting=Unpacking… · upd_stage_swapping=Applying the new files… · upd_show_file=Show file · upd_manual_message=This folder isn't writable, so the app can't swap its own files. The verified download was shown in Explorer — quit the app and extract it over this folder. · upd_failed_download=The download didn't finish. Check the connection and click to try again. · upd_failed_checksum=The download didn't match the release's checksums, so it was discarded. Click to try again. · upd_failed_extract=The downloaded update couldn't be unpacked. Details are in the app log. · upd_failed_swap=The update couldn't be applied, and the previous files were put back. Details are in the app log. · upd_failed_swap_dirty=The update couldn't be applied, and some previous files could not be put back — this folder may mix versions until a retry succeeds. Details are in the app log. · adv_inspector=Inspector · adv_clear_selection=Clear selection · adv_no_selection=No connection selected · adv_selection_gone=That connection is no longer listed · adv_select_a_row=Select a row in Activity to inspect it · adv_selected=selected · adv_verdict_blocked=Blocked — no packets sent · adv_verdict_local=Bypassed the tunnel — not protected · adv_verdict_tunnelled=Tunnelled through URnetwork · adv_host=Host · adv_addresses=Addresses · adv_matched=Matched · adv_protected=Protected · adv_na=— · adv_reason=Reason · adv_reason_default=Default policy · adv_reason_block=Block override · adv_reason_route=Route override · adv_reason_override=Override · adv_override_id=Override · adv_packets_total=Packets (total) · adv_bytes_total=Bytes (total) · adv_last_decision=Last decision · adv_via_exit=Via exit · adv_unknown_exit=Not in the routing table · adv_exit_flows=Flows to this destination · adv_exit_tier=Exit tier · adv_exit_flows_total=Exit flows · adv_exit_dial_failures=Dial failures · adv_exit_state=Exit state · adv_exit_quarantined=Quarantined · adv_exit_warning=Warning · adv_exit_proven=Proven · adv_exit_ok=OK · adv_exit_warning_cause=Warning cause · adv_probe_age=Probe age · adv_session_exit_country=Session exit country · adv_raw_status=Raw status · adv_none=none · adv_exits=Exits

---

## 7. Accessibility summary
- Three panes are named landmarks: Pane A = LandmarkType Main named “Connect”; B named “Activity”; C named “Client statistics”; the InspectorGroup is a Custom landmark named “Inspector”.
- Rule: a Button whose content is a Panel gets NO automatic name — every such button carries an explicit name: ConnectHero (= current status text), LocationRow (“Selected provider, {provider}”), PeersLine (= its text, kept in sync), MoreOptionsToggle (“More options”), the three group trailing actions (their group label), InspectorClearButton (“Clear selection”), each connections row (“{title}, {verdict}” [+ “, selected”]), each contracts row (“Contract, {clientId}”).
- Decorative marks are a11y Raw: row icons, chevrons, all state dots, selection markers, canvas host, and any label already restated by its row’s name (SelectedProviderLabel, LocationText, PeerCountText, key cells of key/value rows, title/meta of Button rows). Key/value rows name their value “{key}, {value}”.
- Toggle switches are labeled via LabeledBy their row label. Selection is exposed on three channels (fill, 2px bar, name suffix). Copyable values (inspector action id; Advanced contract client ids) enable text selection.

---

## 8. Empty vs loading vs failed, per surface (as the code distinguishes them)
- Activity list: connected+rows ⇒ list; else the centred glyph+sentence empty state. (No separate loading/failed reading exists for this list — see flags.)
- Inspector: three explicit readings — no selection / selection aged out / populated; plus per-field absence (“none”, “Not in the routing table”).
- DNS: rows (per-resolver On/Off) vs “DNS settings unavailable” (row swap), pill independent.
- Peers: list vs “Peer discovery disabled until connected” (RPC down is “unavailable”, rendered gray — a stale zero is never presented as fact).
- Contracts / split rules: populated list vs a one-row empty sentence on the same grid (never a hole).
- Session figures: “—” (em dash) while disconnected — “no session”, not zero.
- Provide stats / provider count / throughput: rows and lines COLLAPSE entirely when empty (SetTextOrCollapse / row Visibility) — never blank rows.


## SDK surface referenced
- Sdk().Disconnect()  [connect button/retry; -> SessionRequest kind=Disconnect through the session worker]
- Sdk().Connect(urnet::ConnectLocation)  [connect button with an explicit selection]
- Sdk().ConnectBestAvailable()  [connect button with best-available selection]
- Sdk().SelectedLocation()  -> connectVc_->getSelectedLocation() / device_->getConnectLocation()
- urnw::IsBestAvailableSelected(std::optional<urnet::ConnectLocation>)  [connect_location_id->best_available]
- Sdk().ConnectedProvidePeers()  -> peerVc_->getPeers()  [provider-row peer-name resolution; chooser seeding]
- Sdk().ConnectedPeerCount()  -> peerVc_->getConnectedCount()  [peers line]
- Sdk().RemoteConnected()  -> device_->getRemoteConnected()  [peers line disabled reading; globe sheet availability]
- Sdk().EnsureLocations()  -> device_->openLocationsViewController()+addFilteredLocationsListener()+start(); device_->openPeerViewController()+addPeersListener()+start()
- Sdk().CurrentFilteredLocations()  -> locationsVc_->getFilteredLocations()  [chooser seeding]
- Sdk().CurrentThroughputPoints(windowSeconds=60)  [chart resync]
- Sdk().CurrentContractRows()  [contracts list seed + settled re-read per push]
- Sdk().CurrentBlockActions()  [connections list seed]
- Sdk().CurrentBlockCounts(allowed, blocked)  [session rows seed]
- Sdk().CurrentSplitRules()  [split-rules list seed]
- Sdk().CurrentDnsSettings()  [dns card seed]
- Sdk().CurrentPerformanceSettings()  -> device_->getPerformanceProfile() / localState_->getPerformanceProfile()  [urnet::PerformanceProfile: window_type WindowTypeAuto/Quality/Speed; window_size min/max; allow_direct; post_quantum_encryption]
- Sdk().SetPerformanceSettings(PerformanceSettings)  -> localState_->setPerformanceProfile() + device_->setPerformanceProfile()  [fixedIp => WindowSizeSettings{1,1} else {2,4}]
- Sdk().CurrentProvideControlMode()  -> device_->getProvideControlMode() / localState_->getProvideControlMode()
- Sdk().SetProvideControlMode(mode)  -> device_->setProvideControlMode() + localState_->setProvideControlMode()
- Sdk().CurrentBlockerEnabled()  -> device_->getBlockerEnabled()
- Sdk().SetBlockerEnabled(on)  -> device_->setBlockerEnabled(on)  [device persists]
- Sdk().ReadReliability()  -> device_->getRemoteConnected(), device_->getReliabilitySettings(), device_->getReliabilityMetrics(), device_->getExits(), device_->getDestinationExits(), device_->probeSuiteRunning(), device_->getProbeResults()  [inspector exit-routing cache, 5 s cadence, background thread]
- Sdk().RepublishStats()  [degrade-hold clock expiry nudge]
- Sdk().CurrentStats()  [LiveStats incl. connectionStatus via connectVc_->getConnectionStatus(), providerCount via providerGrid.getWindowCurrentSize(), rawConnectionStatus, rpcOnly, health, healthReevalAtMillis, windowStallReason, gridPoints via getProviderGridPointList]
- Sdk().SetContractsAtTop(true)  [contracts sheet close]
- Sdk().CurrentProviderIdentities()  [globe sheet badge seed]
- Sdk().SetThroughputHandler(...)
- Sdk().SetContractRowsHandler(...)
- Sdk().SetBlockActionsHandler(...)
- Sdk().SetBlockStatsHandler(...)
- Sdk().SetSplitRulesHandler(...)
- Sdk().SetDnsSettingsHandler(...)
- Sdk().SetBlockerEnabledHandler(...)
- Sdk().SetLocationsHandler(...)
- Sdk().SetPeersHandler(...)
- Sdk().SetProviderLocationsHandler(...)
- Sdk().SetProviderIdentitiesHandler(...)
- Sdk().SetProviderSelectionHandler(...)
- Sdk().SetRemoteChangedHandler(...)
- urnet::getRecommendedDnsResolverSettings(lowercaseCountryCode)  [dns pill]
- urnet::getDefaultDnsResolverSettings()  [dns pill default comparison; preview seeding]
- urnet::getColorHex(lowercaseCountryCode)  [dns pill country dot]
- urnet types consumed: DnsResolverSettings, StringList, ConnectLocation, FilteredLocations, NetworkPeerList/NetworkPeer, Exit, DestinationExit, ThroughputPoint/ThroughputSample, ProviderGridPoint, PerformanceProfile, WindowSizeSettings
- urnw::gesture::ActionIsDisconnect(ServiceFacts, health::State)  [Common/ConnectAction.h — shared with the tray]
- MainWindow relays used by these panes: w_.SetInsufficientBalance(bool), w_.ApplyStatusStripConnection(text, dot), w_.balanceConfirming(), w_.balanceBlocked(), w_.statusRoutesInstalled(), w_.statusWfpState(), w_.statusStopReason(), w_.statusFailsafeArmed(), w_.statusSessionMode(), w_.sheetOpen()/SetSheetOpen(bool)
- Updates().SetHandler(...) + Updates().Current()  [MainWindow-side bind-then-replay feeding ApplyUpdateChecker]

## Flags (doc-vs-code drift / risks)
- DOC vs CODE (§7.7): the doc lists idle copy '{network} is ready to connect' among the status words. The code removed it (R1 owner reconciliation, ConnectPage.cpp ~644): the plain-disconnected headline is Loc('disconnected') with the Simple-only ProtectionText restatement; the network name renders only in the status strip. Code wins.
- DOC vs CODE (§7.7 Pane C): doc says the Advanced session group 'adds raw status + session mode'. The code adds 'Raw status' (adv_raw_status) and 'Exits' (adv_exits) — session mode is a status-strip field, not a session row. Code wins.
- DOC vs CODE (§7.7 Pane B): doc describes routing rows as '{verdict dot, host, bytes}'. The code's meta is bytes AND packets: FormatByteCountCompact + '   ' + FormatCountCompact + ' pkt'. Code wins.
- DOC imprecision (§7.3): 'list/table rows 36px' — the peers list rows (and every one-row empty sentence) are 34px in code (ApplyPeersList uses MakePaneListRow(34); empty rows use MakePaneKeyValueRow(...,34)); connections/contracts/split-rules are 36. Follow the code's per-list heights.
- DOC self-flagged (§7.1), confirmed in code: kUltraWideDip=1800 is not what gates Home's third pane — homeWide = advancedMode_ && width>=1000; homeTwoPanes = advancedMode_ && width>=640; Simple is always one pane capped 480.
- The task brief asked for 'empty vs loading vs failed (all three distinguishable)' — on these three panes the code does NOT have a three-way reading for the connections list (only list vs empty; the Network destination has the three-way Loading/Failed/Empty). Do not invent one for Home; parity means two states here.
- InstalledApps.h: confirmed NOT referenced by ConnectPage or the Home markup — the app-rules surface (AppRulesSheet + InstalledApps enumeration) is Settings-only. Home's split-rules group opens SplitRulesSheet (host-cluster rules), a different surface.
- Store-key reuse compromises the code itself flags: Pane B's empty state reuses Loc('contracts_appear_connected') ('Contracts appear here while connected.') because the spec's ideal copy is not in the store; the third pane's title is 'client_statistics' because 'Details'/'Session'/'Inspector' don't exist as keys. Port as-is; the exact-copy store additions are reported upstream.
- All adv_*/conn_*/svc_*/upd_* labels ship as English fallbacks via Adv(key, english) — the GTK port must either add these keys to the localization store or reproduce the same store-first fallback mechanism (Localized() returns the key id on a miss; Adv prefers the store the moment the key appears).
- Segoe Fluent Icons glyphs (E76C chevron-right, E70D/E70E disclosure chevrons, E704 discoverable, E71B fixed-ip, EA18 anon, E72E post-quantum, EA39 blocker, E9D2 empty-state, E711 clear, E8A7 open-sheet, E70F dns) need GTK-equivalent substitutes; the doc does not map them — pick per-glyph equivalents and keep the Raw/decorative a11y treatment.
- PP Neue Montreal ships regular-only; SemiBold is synthesized by DirectWrite on Windows. The GTK port must verify Pango synthesizes a comparable weight or license the SemiBold cut — the 20sp status line, pane titles, and the Connect button label all depend on it.
- Two SelectorBar behaviors to reproduce exactly: (1) Auto connection-mode forces Fixed IP off quietly (single push, echo-guarded) and disables the Fixed IP toggle; (2) unknown/'manual' provide mode seeds the Never segment. Both are conservative-default rules, not cosmetics.
- Concurrency invariants worth carrying to GTK: every SDK callback marshals to the UI thread with payload by value; ReadReliability runs on a worker (several synchronous RPCs) with a single-flight guard and re-checks its preview/sample gate ON ARRIVAL, not just on entry; selection is by block-action id, never index; repaint-not-rebuild on selection change (keyboard focus survives).
