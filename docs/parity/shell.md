# URnetwork Window Shell — GTK4/C++ Port Spec (from the WinUI 3 client)

Sources read in full: `app/src/App/MainWindow.xaml` (2485 ln), `MainWindow.xaml.cpp` (2048 ln), `Startup.h`, `PageContext.h`; supporting: `App.xaml`, `UrComponents.h/.cpp`, `UrMotion.h/.cpp`, `WindowReveal.h`, `WindowShell.h/.cpp`, `UrColors.h`, `LoginPage.cpp` (account menu + mode notice), `AuthSheets.cpp` (`ShowAccountMenu`), `DeveloperPage.cpp` (`OnModeNotice`). Cross-checked against `docs/linux_agent_help.md` §7/§8 — disagreements are in FLAGS; **the code wins**.

All lengths are **dip** (device-independent px). All strings are store keys via `Loc("key")`; keys that do not exist yet ship as `Adv("key", L"English")` — render the English until the store key lands (a store miss renders the key id itself, never blank).

---

## 0. Window frame

- Default window size **1120 × 820** dip (`WindowShell.h kDefaultWidthDips/kDefaultHeightDips`) — deliberately opens *wide* of the 1000-dip breakpoint. Minimum **400 × 480** dip (`kMinWidthDips/kMinHeightDips`).
- Window `Title` = `Loc("app_name")`.
- Content extends into the title bar (`ExtendsContentIntoTitleBar(true)`); `SetTitleBar(AppTitleBar)` makes the 48-dip strip the drag region. The window frame itself **never animates** (motion rule).
- Close hides to tray; the window is (re)shown on tray click. Presentation gate = shown && not minimized; `MainWindow::SetPresentationActive(active)` relays to connect/developer/network/login pages, and on `false` calls `reveal_.CancelToFinal()` (a hidden window must never keep a hero pinned mid-animation).

## 1. Root structure (WindowPlate / RevealRoot)

```
Window
└─ Grid  x:Name=WindowPlate    Background=#101010 (UrBackgroundBrush) — opaque, NEVER moves/animates
   └─ Grid x:Name=RevealRoot   — carries everything; the reveal machine's legacy-restore target
      RowDefinitions: [0]=Auto  [1]=*  [2]=Auto  [3]=Auto
      ├─ Row 0: Grid  AppTitleBar          (Height=48, Background=Transparent — the DRAG region)
      │         └ StackPanel Horizontal, Margin=16,0, Spacing=8, VCenter, IsHitTestVisible=False
      │           ├ Image  BrandIcon   20×20  (app icon, ms-appx:///Assets/app.ico)
      │           └ TextBlock BrandText — PP NeueBit (UrWordmarkFontFamily), 24, Bold, Text=Loc("app_name")
      ├─ Row 0: Button AccountMenuButton   (SIBLING of AppTitleBar, not a child — a drag region
      │         swallows clicks from children; siblinghood is load-bearing)
      │         Visibility=Collapsed until signed in; HAlign=Right, VAlign=Center,
      │         Margin=0,0,150,0 (clears system caption buttons), Padding=2, 32×32,
      │         CornerRadius=16, Background=Transparent, BorderThickness=0, Click=OnAccountMenu
      │         └ Grid
      │           ├ Ellipse AccountProRing 30×30, StrokeThickness=2, Stroke=#FFC400 (kProGold),
      │           │        Visible IFF the JWT says Pro — gold is spent on Pro and NOTHING else
      │           └ PersonPicture AccountAvatar 26×26 (initials from DisplayName)
      ├─ Row 1: ScrollViewer LoginRoot     (sign-in flow; FontFamily=UrBodyFontFamily inherited)
      ├─ Row 1: NavigationView HomeNav     (signed-in shell; Visibility=Collapsed until login)
      ├─ Row 1: InfoBar AccountSnackbar    (window snackbar — see §9)
      ├─ Row 2: InfoBar ModeNoticeBar      (standing session notice — see §8)
      └─ Row 3: Border StatusStrip         (persistent status strip — see §4)
```

Root swap: `ShowLoginRoot()` / `ShowHomeRoot()` toggle LoginRoot⇄HomeNav visibility (if a swap actually changes visibility mid-reveal, `reveal_.CancelToFinal()` first), then `ApplyStatusStrip()`. The auth relay (`ApplyAuthState`) does the same swap with `showHome = loggedIn || previewUi_`.

### Account menu button behavior
- `LoginPage::ApplyAccountIdentity(networkName, guest, pro, signedIn)` — visibility follows `showHome` (so `--preview-ui` shows it); a11y name `Loc("account")` (a button whose content is an avatar gets NO automatic name); `AccountAvatar.DisplayName` = network name, or `Loc("guest")` when empty; ProRing visible iff `pro`.
- Click → `ShowAccountMenu(anchor, ...)` — a MenuFlyout anchored to the button:
  1. Disabled identity item: network name, or `Loc("guest")` — *not actionable*, names which network the actions apply to.
  2. Separator.
  3. Guest only: `Loc("create_account")` → `login().BeginGuestUpgrade()` (create step in GuestUpgrade mode over LoginRoot).
  4. `Loc("share_urnetwork")` → guard `Sdk().IsLoggedIn()` (NOT `apiReady()` — apiReady is true with no token) → `Sdk().api().getNetworkReferralCode(cb)`; on the UI thread, if a code came back, put `Format("referral_share_message", code)` on the clipboard and show the window snackbar `Loc("bonus_referral_code_copied_to_clipboard")` at Success severity. Empty code → do nothing (never share a sentence with a hole).
  5. Separator.
  6. `Loc("sign_out")` → `Sdk().Logout()`.

## 2. Left NavigationView (HomeNav)

`muxc:NavigationView`, `Grid.Row=1`, `Visibility=Collapsed` at start.
Properties: `AlwaysShowHeader=False`, `IsBackButtonVisible=Collapsed`, **`PaneDisplayMode=Auto`** (native adaptivity: expanded icon+label pane at ≥1008 epx, compact icon rail 641–1007, overlay pane behind the toggle at ≤640 — WinUI defaults, not pinned), **`OpenPaneLength=220`**, `IsPaneToggleButtonVisible=True`, **`IsSettingsVisible=False`** (load-bearing: the built-in settings gear's tag matches nothing and would strand the user on an empty page), `SelectionChanged=OnNavSelectionChanged`, `FontFamily=UrBodyFontFamily`.

Resources: `NavigationViewItemOnLeftMinHeight = 44` (item height).

Theme keys (App.xaml, all shell-critical):
- Content layer: `NavigationViewContentBackground=Transparent` (WinUI's default content layer is #1C1C1C — byte-identical to the card fill; Transparent lets the #101010 page through), content grid border transparent/0/0.
- Pane: `NavigationViewDefaultPaneBackground` / `ExpandedPaneBackground` / `TopPaneBackground` / `SplitViewPaneBackground` = **#151515** (the default is acrylic — a pale desktop-sampling slab; solid sheet color required).
- `NavigationViewItemSeparatorForeground = #1FFFFFFF` (the rail needs an edge against the page).
- Selection: `NavigationViewSelectionIndicatorForeground = #EFF7BB` (the brand accent bar — WinUI's built-in left indicator pill, recolored; the in-code comment calls it "3epx"); `ItemBackgroundSelected = #242424`; `SelectedPointerOver/SelectedPressed = #2A2A2A`; `ItemBackgroundPointerOver = #1C1C1C`; `ItemBackgroundPressed = #242424`.
- Foregrounds: unselected `#989898`; selected/hover/pressed `#F8F8F8`.

Header: `AlwaysShowHeader=False`; HeaderTemplate = `TextBlock` with `UrTitleTextStyle` (ABC Gravity Extended, 28, LineHeight 36, #F8F8F8), `Margin=4,4,0,0`. Header content is set per navigation to the selected item's own Content — **EXCEPT** pane-shell destinations, which set `Header = null` (see §5).

### Items, in order (tag → store key → Segoe Fluent glyph, FontIcon `UrIconFontFamily` FontSize=20)

MenuItems:
| # | x:Name | Tag | Label (store key) | Glyph |
|---|---|---|---|---|
| 1 | ConnectNavItem (IsSelected=True) | `connect` | `connect` (Home) | U+E80F Home |
| 2 | NetworkNavItem | `network` | `network` | U+E774 Globe |
| 3 | WalletNavItem | `wallet` | `earnings` (wallet + leaderboard merged) | U+E8C7 Wallet |
| 4 | AccountNavItem | `account` | `account` | U+E77B Contact |

FooterMenuItems:
| # | x:Name | Tag | Label | Glyph |
|---|---|---|---|---|
| 1 | SupportNavItem | `support` | `support` | U+E897 Help |
| 2 | DeveloperNavItem | `developer` | `Dev("dev_developer", "Developer")` (painted by DeveloperPage — no store key) | U+EBE8 DevTools |
| 3 | SettingsNavItem | `settings` | `settings` | U+E713 Settings |

PaneFooter: `Border Height=8` spacer — guarantees Settings never sits flush against the status strip's hairline.

### Advanced-only Developer item — INSERT/REMOVE, never Visibility
`MainWindow::ApplyAdvancedMode(bool on)`:
- `on && !present` → `footer.InsertAt(1, DeveloperNavItem)` → order **Support, Developer, Settings**.
- `!on && present` → `footer.RemoveAt(index)`.
- Rationale (port it): WinUI's NavigationView caches an explicit MaxHeight on the footer scroller from its last real measure; a Visibility flip does not invalidate it and a footer that grew while collapsed clips Settings. A **collection change** is what forces the re-measure. (GTK: prefer real add/remove of the row.)
- If the user is **standing on** `developer` when the mode turns off: `HomeNav.SelectedItem = ConnectNavItem` (never leave a page on screen with no nav item selected).
- `ApplyAdvancedMode` also: `BuildStatusStrip()` (rebuild, not toggle — separators belong to fields), `ApplyBreakpoint(force=true)` (the Home pane count is mode-gated and a toggle at unchanged width would otherwise never re-run it), then `connect_->ApplyAdvancedMode(on)`, `developer_->…`, `settings_->…` (Settings owns the toggle and must be told, or a disk-restored mode leaves the toggle reading Off).
- One apply path: the Settings toggle calls `MainWindow::SetAdvancedMode(on)` → `Sdk().SetAdvancedMode(on)` (persist first), and SdkHost publishes back through the handler → `ApplyAdvancedMode`. Ctor: bind handler (marshalled to UI thread, weak ref), then **synchronously replay** `ApplyAdvancedMode(Sdk().CurrentAdvancedMode())` (bind-then-replay; an async replay flashes the Normal reading for a frame).

## 3. MainWindow::ApplyBreakpoint — the ONE responsive switch (complete)

Trigger: `SizeChanged` on the window content root (fires on the first layout pass too, so it seeds initial state). Also called with `force=true` from `ApplyAdvancedMode`. Reads `ActualWidth` in **dip**. Guard: return if `width<=0`; compute `wide = width >= kWideBreakpointDip (1000.0)` and `ultra = width >= kUltraWideDip (1800.0)`; return unless `force` or a bucket changed. **`ultra` is computed and logged only — no layout keys off it today.** Ends with `LogInfo("layout: {ultra|wide|narrow} at {width}dip")`.

Helpers: `SetWidth(col, dips)` (0 collapses), `SetStar(col, weight)`, `Place(element, {row, col, rowSpan, margin})`.

Named columns and their XAML-declared initial widths:
- Connect: A=330 | Auto(rule) | B=* | Auto(rule) | C=380
- Network: A=* | Auto | B=400
- Account: A=360 | Auto | B=* | Auto | C=380
- Wallet: A=360 | Auto | B=* | Auto | C=380
- Settings: A=* | Auto | B=* | Auto | C=*
- Support: SupportCapColumn (Width=1000*, MaxWidth=1000) + SupportSideColumn (0)
- Developer: DeveloperCapColumn (Width=1000*, MaxWidth=1000) + DeveloperSideColumn (0)
- Login: LoginArtColumn=0 | LoginFormColumn=*

### 3a. Home (ConnectView) — mode question FIRST, then width
```
homeWide     = advancedMode_ && width >= 1000   → three panes: connect(330) | activity(*) | statistics(380)
homeTwoPanes = advancedMode_ && width >= 640    → two panes:  connect(330) | activity(*)
Simple (advancedMode_ == false), ANY width      → one pane:   connect only, capped + centred
```
- `ConnectPaneCColumn` = 380 if homeWide else 0; `ConnectPaneCRule` + `ConnectPaneC` Visible iff homeWide.
- If homeTwoPanes: `ConnectPaneAColumn`=330 fixed, `ConnectPaneBColumn`=star 1, `ConnectPaneB` + `ConnectPaneBRule` Visible. Else: `ConnectPaneAColumn`=star 1, `ConnectPaneBColumn`=0, B + rule Collapsed.
- `ConnectPaneAContent` (the scrolling column inside pane A): Advanced → `MaxWidth=∞`, HAlign=Stretch; Simple → **`MaxWidth=480`, HAlign=Center**.
- `ConnectCanvasHost` (hero) `MaxWidth`: Advanced **190**, Simple **320** (the canvas clamps its own side to [168,288] off host width).
- Statistics (C) folds first because it is the inspector — everything in it is reachable from sheets. Simple hides B and C entirely (hidden, never disabled).

### 3b. Network — the list takes the star, detail is fixed
- `NetworkPaneBColumn` = **400** if `wide (>=1000)` else 0; `NetworkPaneBRule` + `NetworkPaneB` Visible iff wide. (400 not 380: the longest row is a country name beside a provider count.) Below the breakpoint nothing is unreachable — selecting a row still connects.

### 3c. Earnings (WalletView)
```
earningsThree = width >= 1500   wallets(360) | ledger(*) | points(380)
earningsTwo   = width >= 900    wallets(360) | ledger(*)
else                            ledger(*) only
```
- `WalletPaneCColumn`=380 iff earningsThree; `WalletPaneCRule` + `WalletPaneC` Visible iff earningsThree.
- `WalletPaneAColumn`=360 iff earningsTwo; `WalletPaneBRule` + `WalletPaneA` Visible iff earningsTwo.
- The LEDGER survives to the smallest width (a payouts table is what the destination is for); the points rail folds first.

### 3d. Account
```
accountThree = width >= 1500    plan(360) | account(*) | codes(380)
accountTwo   = width >= 900     plan(360) | account(*)
else                            account(*) only
```
- `AccountPaneCColumn`=380 iff accountThree; `AccountPaneCRule` + `AccountPaneC` Visible iff accountThree.
- `AccountPaneAColumn`=360 iff accountTwo; `AccountPaneBRule` + `AccountPaneA` Visible iff accountTwo.
- Codes fold first (a record, not a control — Redeem lives in the plan pane); below 900 the PLAN pane folds, not the account pane (usage figures are also on the status strip and tray). 1500 not 1000 was **measured**: at 1240 with three panes, 220 nav + 360 plan + 380 codes left the account list 278 dip — labels rendered "…".

### 3e. Settings — three EQUAL star columns
```
settingsThree = width >= 1400   general | device | about
settingsTwo   = width >= 900    general | device        (About folds first — least operational)
else                            general only
```
- `SettingsPaneCColumn` star = 1 iff settingsThree else 0; `SettingsPaneCRule` + `SettingsPaneC` Visible iff settingsThree.
- `SettingsPaneBColumn` star = 1 iff settingsTwo else 0; `SettingsPaneBRule` + `SettingsPaneB` Visible iff settingsTwo.

### 3f. Support (card model)
- `SupportCapColumn.MaxWidth` = **1080** if wide else **560**.
- Wide: `SupportSideColumn` star 1; `SupportSideStack` placed at {row 0, col 1, span 1, margin 20,0,0,24} (the "reach a human" card BESIDE the form).
- Narrow: `SupportSideColumn` width 0; SideStack at {row 1, col 0, span 1, margin 0,16,0,24} (stacked below).

### 3g. Developer (card model)
- `DeveloperCapColumn.MaxWidth` = **1800** if wide else **1000**.
- Wide: `DeveloperSideColumn` star 1; `DeveloperSideStack` at {row 2, col 1, span 1, margin 20,16,0,24} (overrides beside measurements). Narrow: side column 0; SideStack at {row 3, col 0, span 1, margin 0,16,0,24}.
- (Row layout: row0 `DeveloperTopStack` full-width, row1 `DeveloperTablesStack` full-width Margin 0,16,0,0, row2 `DeveloperMainStack` Margin 0,16,0,24, row3 side stack.)

### 3h. Login — art beside the form (gate = kWideBreakpointDip, 1000)
- Wide: `LoginArtColumn` star 1; `LoginFormColumn` fixed **544** (= LoginPanel MaxWidth 512 + 16+16 margins); **reparent** `LoginCarouselHost` to `LoginArtPane` (index 0) via `ReparentTo` (search real Children vectors, NEVER `Parent()` — null before a layout pass; trusting it crashed every signed-in launch); `LoginArtPane` (Padding 24) Visible.
- Narrow: `LoginArtPane` Collapsed; reparent host back to `LoginPanel` index 0; art column 0, form column star 1.
- After BOTH directions: `login_->ApplyLoginLayout()` (single writer of the host's Visibility/Height — must be re-run explicitly; a SizeChanged is not guaranteed) then `login_->OnCarouselHostReparented()` (reparenting drops the realized surfaces behind the carousel's ImageBrushes; the carousel re-assigns them — after ApplyLoginLayout, never before).

### 3i. Status strip captions
- All seven captioned fields (`statusNetwork_, statusProvider_, statusTraffic_, statusMode_, statusRoutes_, statusRpcPort_, statusRaw_` — the state field has no caption): caption Visible iff `wide`. Below 1000 the values stand alone (the caption remains the value's accessible name — a screen reader loses nothing).
- Then `ApplyStatusStrip()` (also hides the Advanced fields entirely below the breakpoint — §4).

## 4. The status strip (D4/D5)

**Placement**: `Border StatusStrip`, root grid Row 3 (the LAST row on purpose — pinned under everything incl. the mode notice; Proton parity: the least urgent, most permanent thing on screen). Starts Collapsed.

**Style** (`UrStatusStripStyle`): Background **#151515**, top hairline **1px #1FFFFFFF** (`BorderThickness=0,1,0,0`), **Padding 16,7**. Child: `StackPanel StatusStripFields`, Horizontal, Spacing 0, VCenter. One row, never two: values trim (`CharacterEllipsis`), never wrap; no horizontal scroll.

**Field anatomy** (`kit::MakeStatusField(label, withDot, accessibleName)`): horizontal StackPanel, Spacing 6, VCenter; optional 8×8 Ellipse dot (a11y Raw — the word beside it says the same thing); caption TextBlock (`UrStatusFieldLabelStyle`: body face, **11 px**, #5A5A5A faint, a11y **Raw**); value TextBlock (`UrStatusFieldValueStyle`: body face, **12 px**, #989898 muted, trim/NoWrap). `SetStatusFieldValue` writes the text AND the value's automation Name = `"<label>, <value>"` (the caption being Raw, the value is the field's only accessible node). Separator (`MakeStatusSeparator`): Border **1 wide × 14 tall**, Margin **14,0,14,0**, fill #1FFFFFFF.

Strip landmark: `AutomationProperties.Name(StatusStrip) = Loc("urnetwork_status")`.

**Built in code** (`BuildStatusStrip()`), rebuilt wholesale on each Advanced-mode change (separators belong to their fields). Field order:

Normal (3 fields):
1. **State** — `withDot=true`, NO caption (the dot + word are self-naming); accessible name `Loc("urnetwork_status")`. Value foreground overridden to **#F8F8F8** on every render (the one never-muted line); dot fill = the pushed state color.
2. ─ separator ─ **Provider**, caption `Loc("selected_provider")`.
3. ─ separator ─ **Traffic**, caption `Loc("data")`.

Advanced appends **FIVE** more (each with its own separator; all five in `statusAdvancedParts_`):
4. **Network**, caption `Loc("network")` (moved out of the Normal set in Phase B — context, not one of Simple's three questions).
5. **Session**, caption `Adv("adv_session_mode", "Session")`.
6. **Routes**, caption `Adv("adv_routes", "Routes")`.
7. **RPC**, caption `Adv("adv_rpc", "RPC")`.
8. **Raw**, caption `Adv("adv_raw_status", "Raw")`.

(NOT present, deliberately: the egress interface index — `proto::TunnelStatus` does not carry it; a field that would be invented is not a field.)

After a rebuild, replay the last state render: `if (!statusStateText_.empty()) RenderStatusState(statusStateText_, statusStateDot_)` (a rebuild is not a state push; under a pinned preview sample no push is coming).

### ApplyStatusStrip() — the one renderer
1. Bail if not built (`statusProvider_.value` null — Provider exists in both readings; Network does not).
2. Strip Visible iff `HomeNav` is Visible (the strip belongs to the signed-in shell; on the login flow it would describe a session that does not exist). Return when hidden.
3. **Signed-out** (`!statusSignedIn_` — what plain `--preview-ui` shows): `RenderStatusState(Loc("sign_in"), kTextFaint #5A5A5A)`; hide every session part (separators + fields 2–8). Return. (The store has no "Not signed in" string; it uses the instruction it has.)
4. Signed-in: show session parts; then the five Advanced parts Visible iff `wideLayout_` (below 1000 they are DROPPED entirely, not shortened — eight fields do not fit; the same values live on the Developer destination).
5. Values:
   - **Network**: `statusGuest_ || name empty ? Loc("guest") : name`.
   - **Provider**: `locationName empty ? Loc("best_available_provider") : locationName`.
   - **Traffic**: `!statusConnected_ ? Loc("site_app_no_traffic")` : Advanced → `"↓ <FormatBitRate(down)>   ↑ <FormatBitRate(up)>"` : Simple → `Adv("conn_carrying_traffic", "Carrying traffic")`. ("Connected" and "carrying traffic" are different claims; rpc-only sessions clamp `connected` false so this reads "No traffic yet" instead of two honest-looking zeroes.)
   - Return here if `statusMode_.value` null (Normal mode — null field means Normal, not error).
   - `haveSession = Sdk().HasSession()` (lock-free: "there is a DeviceRemote AND its service is still there").
   - **Session**: `!haveSession → Adv("adv_none","none")`; mode==RpcOnly → `Adv("adv_mode_rpc_only","rpc-only")`; else `Adv("adv_mode_tunnel","tunnel")`. (No session must NOT read "rpc-only" — naming a state the app is not in is worse than naming none.)
   - **Routes** (5-way):
     * `!routesInstalled && wfp=="off"` → `Loc("off")`
     * `!routesInstalled && wfp!="off"` → `Adv("adv_routes_kill_switch_armed", "off, kill switch armed")` (the kill switch armed on a drop: machine blocked on purpose — name the cause)
     * `routesInstalled && !dnsApplied` → `Adv("adv_routes_dns_degraded", "on, dns not applied")` (a DEGRADED tunnel: lookups leak or fail closed)
     * `routesInstalled && dnsApplied && wfp=="off"` → `Adv("adv_routes_no_firewall", "on, no leak guard")`
     * else → `Loc("on")`
   - **RPC**: `!haveSession || hostport empty → Adv("adv_none","none")` else host:port (a stale endpoint under "Session: none" reads as a live listener).
   - **Raw**: `raw empty → Adv("adv_none","none")` else the raw string. The raw string is chosen in `OnStatsChanged`: `stats.rpcOnly ? stats.rawConnectionStatus : stats.connectionStatus` (rawConnectionStatus is populated ONLY in rpc-only sessions; elsewhere the clamped value IS the raw one).

### Feeds into the strip (one writer per fact)
- **Connection half** (`ApplyStatusStripConnection(text, dotColor)`): pushed by `ConnectPage::ApplyConnectStatus` — the SAME function that draws the hero/status word, so strip and connect screen cannot disagree. Guards: return if `statusSamplePinned_`; return if `!statusSignedIn_` (the idle wording "Ready to connect" must not stand under a signed-out shell).
- **Identity** (`ApplyAuthState`): unless sample-pinned — `statusSignedIn_/statusNetworkName_/statusGuest_` from the parsed JWT; on logout also clears location/connected/bps; then `ApplyStatusStrip()`.
- **Stats** (`OnStatsChanged(LiveStats)`): first `connect_->ApplyStats(stats)`; unless pinned — cache `connected` (CLAMPED field), `locationName`, `downBitsPerSecond`, `upBitsPerSecond`, raw (per rpcOnly rule); `ApplyStatusStrip()`.
- **Tunnel** (`OnTunnelStateChanged(proto::TunnelStatus)`): cache `rpc_listen_hostport`, `mode`, `routes_installed`, `dns_applied`, `wfp_state`, `stop_reason`, `failsafe_armed` — **cached unconditionally** (turning Advanced on mid-session must show current values) and **BEFORE** `connect_->SetConnectedUi(state==Up)` (that re-render reads them); then `if (advancedMode_) ApplyStatusStrip()`.

## 5. Navigation behavior (OnNavSelectionChanged)

On selection change (tag from item Tag, default `"connect"`):
1. **Header**: `paneShell = tag ∈ {connect, network, wallet, leaderboard, account, settings}` → `Header = null` (a display-face page title above a pane layout stops the panes reaching the ceiling; the 40-px pane headers name the columns and the selected nav item names the destination). `support`/`developer` keep `Header = item.Content` (card pages; a card page with no title reads as content starting halfway down).
2. **Page swap** — there is NO Frame: the 7 sibling grids are `{connect→ConnectView, network→NetworkView, account→AccountView, wallet→WalletView, support→SupportView, settings→SettingsView, developer→DeveloperView}`. Find whichever is currently Visible (at most one) = outgoing; the tag's = incoming; `motion::CrossfadePageSwap(outgoing, incoming)`. `tag=="connect"` sets `homeRevealed_=true`.
3. `developer_->SetSelected(tag=="developer")` — gates its 5 s poll (also needs presentation active).
4. `network_->SetSelected(tag=="network")` — opens/closes the SDK locations/peers view controllers ("a destination that is not on screen does not hold a feed open").
5. **Preview branch** (`previewUi_`): log + skip all API loads (no token → 401s); settle the destination navigated TO so nothing sits on "Loading…" forever: `wallet` → `wallet_->ShowPreviewWalletState()` + `ShowPreviewLeaderboardState()`; `developer` → `developer_->ShowPreviewModeNotice()` + `ShowPreviewSnapshot()`. Return.
6. Otherwise `LoadCurrentDestination()`.

### LoadCurrentDestination() — tag → API loads (fired on nav selection AND on auth change)
Guards: return if `previewUi_`; return if `!Sdk().IsLoggedIn()` (NOT `apiReady()` — that is `api_.has_value()`, set at SDK init, true with no token). Reads the CURRENTLY selected item's tag:
- `account` → `account_->LoadAccount()`; `Balance().Refresh()` (macOS AccountRootView onAppear parity); `settings_->LoadSettings()` (login methods / auth code / client id / bonus code / referral network live on Account but SettingsPage loads them).
- `wallet` → `wallet_->LoadWallet()`.
- `leaderboard` → `wallet_->LoadLeaderboard()` (vestigial — no nav item carries this tag anymore; see FLAGS).
- `settings` → `settings_->LoadSettings()`.
- `connect` / `network` / `support` / `developer` → no API loads here (their data comes from SDK feeds / RPC, not the HTTP API).

The **auth-change caller is the load-bearing one**: the selection survives a sign-out (Sign out is ON Settings), so signing out of A and into B would otherwise show A's data against B's session — for delete-account that is destroying the WRONG network (`Api::networkDelete` acts on the current JWT, no arguments).

### ApplyAuthState(state, error) — the auth relay (full order)
1. `showHome = loggedIn || previewUi_`; if the root swap changes visibility mid-reveal → `reveal_.CancelToFinal()` first; swap LoginRoot/HomeNav.
2. Non-empty error → `login_->ShowErrorOnCurrentStep(H(error))` (errors surface on the step the user is looking at).
3. `loggedIn` → `login_->ClearGuestUpgrade()`.
4. Parse identity once per auth change from `Sdk().ParsedJwt()` → `{NetworkName, GuestMode, Pro}`.
5. `connect_->SetNetworkIdentity(name, guest)` (re-renders the status line).
6. Unless sample-pinned: write strip identity fields (clearing session facts on logout) + `ApplyStatusStrip()`.
7. `login_->ApplyAccountIdentity(name, guest, pro, showHome)` — avatar visibility keys off `showHome`, not `loggedIn` (so preview shows it).
8. `loggedIn && !wasVisible` (Home just appeared): `connect_->ResyncDrawer()`; `account_->LoadReferralInfo()`; fallback first-entrance `CrossfadePageSwap(nullptr, ConnectView())` if `!homeRevealed_` and ConnectView visible (a sign-in landing on the default-selected Connect never raises SelectionChanged); `MaybeShowOnboardingTip()`; `LoadCurrentDestination()` (re-read whatever destination is still selected from the previous session).
9. `!loggedIn && wasVisible`: `login_->ResetToInitialStep()`; `settings_->ResetForSignOut()`; `account_->ResetForSignOut()` (stale auth state is dangerous, not merely ugly).
10. `!loggedIn && !wasVisible && login_->IsGuestUpgrade() && !Sdk().IsLoggedIn()`: clear guest upgrade + reset to initial (guest session invalidated server-side under the upgrade form).

## 6. CrossfadePageSwap (`UrMotion`)

- No Frame — 7 sibling grids toggled by Visibility; this is the page transition AND every page's first reveal (`outgoing == nullptr` = first entrance; the old drawer entrance was this with no outgoing).
- `outgoing == incoming` → ensure Opacity 1 + Visible, done. `!ShouldAnimate()` → instant swap (outgoing Collapsed + Opacity restored to 1; incoming Opacity 1 + Visible).
- Animated: incoming Opacity 0 → Visible; **ONE shared Storyboard**: fade-in 0→1 over **kBaseMs = 250 ms** on Standard `(0.10, 0.90, 0.20, 1.00)`; fade-out 1→0 over **kFastMs = 150 ms** on Exit `(0.70, 0.00, 1.00, 0.50)` (exits one step faster, always). Completion: collapse outgoing **only if its opacity ≤ 0.01** (guards a second swap re-showing it) and restore its Opacity to 1 for its next entrance. Independent timelines ending frames apart read as flicker — keep one timeline.
- `ShouldAnimate()` = OS "show animations" setting; any read failure → true. Off means motion GONE, not slower.

## 7. `--preview-ui` (Startup.h/.cpp + MainWindow)

- **Tags** (`PreviewUiDestination()`): `--preview-ui` (bare = `connect`) or `--preview-ui=<tag>`, or env `URNETWORK_PREVIEW_UI=<tag>`. Known: `connect, network, account, wallet, leaderboard, support, settings, developer, seedphrase`. Unknown → **falls back to `connect` and logs an error** (never silently show the wrong screen). Empty string returned in every real run — grants nothing; there is no account to read.
- **Semantics**: does NOT authenticate — no token read/written, no session, SDK asked for nothing. Only forces the LoginRoot→HomeNav swap and picks a destination; panels render against the empty local snapshots (= a real signed-in user's first frame, before any response lands).
- `EnterPreviewUi(destination)`: set `previewUi_=true`; `ShowHomeRoot()`; `PreviewSampleStatusStrip()`; `seedphrase` → `login_->ShowPreviewSeedphraseSheet()` (a MODAL over the connect drawer, BIP-39 all-"abandon" test vector) and return; `developer` with mode off → `ApplyAdvancedMode(true)` **in memory only, not persisted** (a collapsed item cannot be selected); select the matching nav item from the seven (the SelectionChanged relay shows the panel); `connect_->ResyncDrawer()`; `connect_->ApplyPreviewSample()` (AFTER ResyncDrawer, which would overwrite it); if sample requested `network_->ApplyPreviewSample()`; fallback entrance `CrossfadePageSwap(nullptr, ConnectView())` guarded on `homeRevealed_` (Connect is the default selection so no SelectionChanged fires); `wallet` → `wallet_->ShowPreviewSnackbar()` (Error — must PERSIST), `support` → `settings_->ShowPreviewSnackbar()` (acknowledgement — must time out).
- **API loads are skipped in `OnNavSelectionChanged`** (see §5.5) — a dev switch shipping in Release must not talk to production; skipping alone is half of it, the preview settle-states are the other half (a fetch that never runs looks like a hang). The drawer's own `EnsureLocations` still runs, exactly as on a normal signed-out launch.
- **`URNETWORK_PREVIEW_SAMPLE=1`** (`PreviewSampleRequested()` — BOTH gates required: `previewUi_` AND env exactly `"1"`): opts into SYNTHETIC content. `PreviewSampleStatusStrip()` renders the strip signed-in: network `sample-network`, not guest, connected, provider `Frankfurt, Germany`, ↓24,600,000 bps ↑3,100,000 bps, state `Loc("connected")` with dot **kUrGreen #87FB67**, then sets `statusSamplePinned_=true` LAST (every later real push is from a session that does not exist — pin wins). Also gates `connect_->ApplyPreviewSample()` / `network_->ApplyPreviewSample()`. Without it, panels show the REAL empty states — worth looking at, and not the populated ones.
- The window still only appears on a tray click, as in a normal run.

## 8. ModeNoticeBar + the mode-notice channel

Two consumers of one SdkHost channel ("this app is not carrying traffic, and here is why" — rpc-only grants, session-start failures). Messages are complete sentences composed by SdkHost — NOT store keys; render verbatim.
- **Transient**: `SdkHost::SetModeNoticeHandler` (bound in the MainWindow ctor, marshalled; SdkHost invokes it with its mutex HELD — never touch UI or call back inline) → `login_->ShowModeNotice(message, failed)` → the window snackbar (AccountSnackbar), severity Error if `kind==SessionFailed` else Informational. After binding, `Sdk().RefreshModeNotice()` **replays** standing state (a failure before the window existed must still surface).
- **Standing**: `InfoBar ModeNoticeBar`, root Row 2, `IsOpen=False`, `IsClosable=False` (a property of the session, not an event — stays until replaced), `Margin=0`, body font. Written ONLY by `DeveloperPage::OnModeNotice` (via `SdkHost::SetModeNoticeObserver`, delivered over the RPC bridge): active → Visible, `Title=""` (the message is self-describing), `Message=<verbatim>`, Severity Error (SessionFailed) / Warning (RpcOnly), `IsOpen=true`. Inactive → `IsOpen=false` AND `Visibility=Collapsed` explicitly (a closed InfoBar still measures its Margin/Padding; the row must contribute 0 px so the pane rules terminate on the status strip's hairline). Once a preview notice is raised (`ShowPreviewModeNotice`, synthetic rpc-only text) it owns the bar for the rest of the process.

## 9. AccountSnackbar (window-level snackbar)

`InfoBar AccountSnackbar`: root **Row 1** (floats over whichever panel is showing), `Style=UrSnackbarStyle` (body font, `IsClosable=True`, `CornerRadius=8`), `VerticalAlignment=Bottom`, `HorizontalAlignment=Center`, **`Margin=24`**, **`MaxWidth=480`**, `IsOpen=False`. Driven by `kit::Snackbar` (owned by LoginPage): default auto-dismiss **4000 ms** (`kDefaultDurationMs`); `kPersistent=0`; severity-gated — Informational/Success time out, **Warning/Error stay until dismissed** (an error string is often the user's only diagnostic). Uses today: the account menu's "referral link copied" (Success) and the transient mode notice (§8).

## 10. Signed-in Hero Bloom (window reveal) — the RevealState table

Machine: `WindowReveal` (`Bind` once in ctor; `Arm(enabled)` writes start poses synchronously pre-Activate; `Start()` post-Activate; `CancelToFinal()` snaps the UNION of both tables to settled). Show sequencing (AppController): latch `wasIconic` → SW_RESTORE if iconic → `ArmReveal(!wasIconic)` → activate → `StartReveal()`. **Never plays on un-minimize.** Arm picks the signed-in vs signed-out table from HomeNav/LoginRoot visibility itself. Tokens: `kFastMs 150, kBaseMs 250, kSlowMs 400, kHeroMs 500, kStaggerMs 40, kBrandBeatMs 120, kHeroHoldMs 240, kHeroScaleFrom 0.92`, reveal spring ζ=0.86 / period 60 ms. Ring = {element, riseDip (+ = starts BELOW final, settles UP; − = above, settles DOWN; 0 = opacity-only), delayMs, fadeMs, riseMs}. Total choreography ≈ 760 ms (last rise 360+400).

**Signed-in table** — hero = `ConnectCanvasHost` (scale 0.92→1 spring + 500 ms opacity); rings, in declared order:

| Element | riseDip | delay | fade | rise |
|---|---|---|---|---|
| HomeNav (STAGE — hero's alpha ancestor; opacity-only, NEVER translated) | 0 | 0 | 150 | 0 |
| AppTitleBar | +8 | 120 | 250 | 400 |
| AccountMenuButton | +8 | 120 | 250 | 400 |
| StatusDot | +8 | 240 | 250 | 400 |
| StatusText | +8 | 240 | 250 | 400 |
| ProtectionText | +8 | 240 | 250 | 400 |
| TrafficHeldText | +8 | 240 | 250 | 400 |
| StatusReasonText | +8 | 240 | 250 | 400 |
| LocationRow | −8 | 240 | 250 | 400 |
| ConnectButton | −8 | 280 (240+40) | 250 | 400 |
| StatusStrip | −12 | 360 (240+3×40) | 250 | 400 |
| ConnectPaneB (opacity-only — large surfaces never slide) | 0 | 360 | 400 | 0 |
| ConnectPaneC (opacity-only) | 0 | 360 | 400 | 0 |

(The 1 px pane rules stay unringed as skeleton. Signed-out table, for completeness: hero=LoginCarouselHost; AppTitleBar +8@120; EmailGroup −8@240; GetStartedButton, OrDivider −8@280; Google/Bittensor/Solana/AuthCode buttons −12@320; SecondaryAuthRow, NetworkServerLink −12@360 — all 250/400. LoginRoot/LoginPanel are hero alpha-ancestors and are never listed.)

Invariants to port: every armed pose is animated-to-settled or explicitly restored (`SettleRing`: opacity 1, translation 0 — inert on hidden elements, so the union restore is always safe); stop-the-animation-then-write; scale pivot = live element center (AnchorPoint (0,0) + center expression — the (0.5,0.5) AnchorPoint displaced content by half its size and SHIPPED as a bug); `Start()` re-checks reduced motion and settles if it flipped since Arm; Collapsed-between-Arm-and-Start routes through the same restore; visibility is NEVER touched by choreography.

## 11. Pane-shell chrome tokens the shell owns (App.xaml)

- `UrPaneStyle` (Grid): Background #101010 (solid — the ConnectCanvas mask samples the surface behind it).
- `UrPaneVRuleStyle`: Width 1, #1FFFFFFF, stretch vertical — the rule BETWEEN panes (never a gap).
- `UrPaneHeaderStyle`: Height **40**, Background #151515, bottom 1 px hairline, Padding 12,0. Title `UrPaneTitleStyle`: body face, **12** SemiBold, CharacterSpacing **60**, #F8F8F8, trim. Meta `UrPaneMetaStyle`: 11, #989898, right-riding, trim/NoWrap.
- `UrGroupHeaderStyle`: Height **28**, #151515, hairlines top+bottom, Padding 12,0. Text `UrGroupHeaderTextStyle`: **11**, CharacterSpacing **90**, #989898.
- `UrPaneRowStyle` (Border): MinHeight **40**, bottom hairline, Padding 12,0. `UrPaneRowButtonStyle` (Button): same metrics, transparent bg, CornerRadius 0, hover = fill step (not outline). Row heights: key-value 34, list/table 36, standard 40, two-line 44 (kit builders).
- Fonts: `UrHeadingFontFamily` = "ABC Gravity Extended"; `UrHeadingCondensedFontFamily` = "ABC Gravity Extra Condensed"; `UrBodyFontFamily` = "PP Neue Montreal"; `UrWordmarkFontFamily` = "PP NeueBit" (no space in NeueBit; Bold-only file — state the weight, don't synthesize). Internal family names are load-bearing; a wrong name falls back SILENTLY.
- `UrTitleTextStyle`: Gravity Extended 28 / LineHeight 36, #F8F8F8; `UrTitleLargeTextStyle` 40/52.

## 12. Onboarding tip (shell-owned, step 3 of 3)

`MaybeShowOnboardingTip()` (called from ApplyAuthState on a real sign-in reaching Home): skip if inactive/shown; skip while the ServiceSetup banner is actionable (NotInstalled/Stopped/VersionMismatch — the banner IS step 2 and they must not compete); else TeachingTip `OnboardingTip` → Target `ConnectButton`, Title `Adv("onb_tip_title", "You're ready")`, Subtitle `Adv("onb_tip_subtitle", "Press Connect to start protecting your traffic.")`, open once.

## 13. Shell string inventory (key → English via store; Adv() = key + fallback)

Store keys (Loc): `app_name`, `connect`, `network`, `earnings`, `account`, `support`, `settings`, `urnetwork_status`, `selected_provider`, `data`, `guest`, `best_available_provider`, `site_app_no_traffic`, `off`, `on`, `sign_in`, `connected`, `become_supporter`, `create_an_account`, `upgrade`, `supporter`, `free`, `create_account`, `share_urnetwork`, `sign_out`, `bonus_referral_code_copied_to_clipboard`, `referral_share_message` (Format, takes the code), `something_went_wrong`, `total_referrals_lld` (Format), `referral_bonus` (Format).
Adv()/Dev() fallbacks: `adv_session_mode`→"Session", `adv_routes`→"Routes", `adv_rpc`→"RPC", `adv_raw_status`→"Raw", `adv_none`→"none", `adv_mode_rpc_only`→"rpc-only", `adv_mode_tunnel`→"tunnel", `adv_routes_kill_switch_armed`→"off, kill switch armed", `adv_routes_dns_degraded`→"on, dns not applied", `adv_routes_no_firewall`→"on, no leak guard", `conn_carrying_traffic`→"Carrying traffic", `onb_tip_title`→"You're ready", `onb_tip_subtitle`→"Press Connect to start protecting your traffic.", `dev_developer`→"Developer".

## 14. Empty vs loading vs failed (shell-level)

- Status strip: signed-out = single `sign_in` field (faint dot) with everything else hidden; signed-in-but-no-session: Session/RPC read "none" (never a stale mode/endpoint); connected-but-clamped = "No traffic yet". Three distinguishable truths by construction.
- Preview: skipped loads are SETTLED per destination (never an eternal "Loading…" — indistinguishable from a hang); page-level Loading/Failed/Empty states are the pages' own contract.
- ModeNoticeBar contributes exactly 0 px when silent (explicit Collapse).


## SDK surface referenced
- urnw::SdkHost& Sdk() — pages::Sdk() (PageContext.h)
- Sdk().IsLoggedIn() — auth gate for loads, share menu, strip
- Sdk().apiReady() — explicitly NOT an auth gate (api_.has_value(), set at SDK init); only used with IsLoggedIn for the ctor's LoadReferralInfo
- Sdk().HasSession() — lock-free 'DeviceRemote exists and its service is alive'; drives strip Session/RPC 'none'
- Sdk().ParsedJwt() → {NetworkName, GuestMode, Pro} — read once per auth change
- Sdk().Logout() — account menu Sign out
- Sdk().api().getNetworkReferralCode(cb(std::optional<urnet::GetNetworkReferralCodeResult>, std::optional<std::string>)) — account menu Share (result->referral_code)
- Sdk().SetLocationsObserver(cb(std::optional<urnet::FilteredLocations>, std::string state)) — Network page's second subscriber, bound at window level, marshalled
- Sdk().SetPeersObserver(cb(std::optional<urnet::NetworkPeerList>)) — Network page peers feed
- Sdk().SetModeNoticeHandler(cb(SdkHost::ModeNotice)) + Sdk().RefreshModeNotice() — bind-then-replay; handler invoked with SdkHost mutex held (must marshal)
- Sdk().SetModeNoticeObserver(...) — DeveloperPage's standing ModeNoticeBar consumer (bridge thread)
- Sdk().SetAdvancedModeHandler(cb(bool)) + Sdk().CurrentAdvancedMode() + Sdk().SetAdvancedMode(bool) — persist-first publish-second, bind-then-synchronous-replay
- Balance().Current() / Balance().CurrentPoll() / Balance().Refresh() — SubscriptionBalanceStore; OnBalanceChanged(snapshot, poll) relay
- urnw::App().balance().StartConfirmationPolling() — after redeem
- Updates().SetHandler(cb(UpdateChecker::Snapshot)) / Updates().Current() / Updates().BeginApply() — bind-then-replay; UpdateChecker::RevealInExplorer(zipPath) for ManualUnzip
- urnw::proto::TunnelStatus {state, mode (StartMode::RpcOnly), rpc_listen_hostport, routes_installed, dns_applied, wfp_state, stop_reason, failsafe_armed} — OnTunnelStateChanged
- urnw::LiveStats {connected (clamped), locationName, downBitsPerSecond, upBitsPerSecond, rpcOnly, rawConnectionStatus, connectionStatus} — OnStatsChanged
- Page loads the shell fires: account_->LoadAccount(), account_->LoadReferralInfo(), account_->LoadBalanceCodes(), wallet_->LoadWallet(), wallet_->LoadLeaderboard(), settings_->LoadSettings(), connect_->ResyncDrawer(), network_->SetSelected(bool), developer_->SetSelected(bool)
- urnw::ServiceSetup::Classify() / RunElevatedVerb(L"install"|L"uninstall", timeoutMs) / AwaitState(State, ms) — service banner (install wait 60000 ms, Running poll 15000 ms, uninstall 30000 ms, NotInstalled poll 5000 ms)

## Flags (doc-vs-code drift / risks)
- Doc §7.5 says 'Advanced adds 4' status-strip fields, and the task prompt says 'normal 3 + Advanced 4' — the CODE adds FIVE (Network + Session + Routes + RPC + Raw; Phase B moved Network from the Normal set into the Advanced set). The doc's own list even names five. Code wins: Normal = 3 fields, Advanced = 8 total. Comments inside MainWindow.xaml.cpp/UrComponents.h still say 'four more fields' — internal comment drift, same cause.
- MainWindow.xaml.cpp's Account breakpoint header comment reads '>=1000dip three panes / <1000 two / <640 one' but the code directly beneath uses 1500/900 (with a paragraph explaining the 1500 change, measured at 1240). Code (1500/900) wins; doc §7.1 already agrees with the code.
- kUltraWideDip (1800) is computed and logged ('ultra (Home in three columns)') but drives NO layout — Home's third pane keys off advancedMode_ && >=1000. The log string is a stale label from the deleted W1 card layout. Doc §7.1 flags this correctly.
- --preview-ui=leaderboard is still an accepted tag (Startup.cpp kKnown, doc §7.17) and LoadCurrentDestination still has a 'leaderboard' branch, but no NavigationView item carries that tag anymore (leaderboard is a tab inside Earnings), so EnterPreviewUi's item loop matches nothing and the launch lands on the default Connect selection. The branch is unreachable from the nav. Port decision needed: map the tag to wallet or drop it.
- Doc §7.5 'avatar 32px' is the BUTTON size; exact metrics are Button 32×32 (CornerRadius 16, Padding 2, Margin 0,0,150,0), PersonPicture 26×26, Pro ring Ellipse 30×30 with StrokeThickness 2 (#FFC400).
- Doc §7.4 'card-fill pill' for the selected nav item: the actual selected fill is #242424 (kCardHover), not the #1C1C1C card color; hover-on-selected/pressed are #2A2A2A. The '3epx accent bar' width is asserted only in a MainWindow.xaml comment — App.xaml recolors WinUI's built-in selection indicator (#EFF7BB) without setting an explicit width; GTK should draw a ~3px accent bar.
- ApplyBreakpoint's status-strip comment says 'Measured at the 720x520 minimum' while WindowShell.h sets the real minimum at 400×480 — stale figure in the comment; the shipped minimum is 400×480.
- MainWindow.xaml's title-bar comment block still describes 'Height 40' from an earlier revision; the shipped strip is 48 (the R1 comment right below corrects it). Use 48.
- Nav labels 'home / help / diagnostics' have no store keys — the shipped stand-ins are connect/support (Loc) and Dev("dev_developer","Developer"); reported as needed store additions. The Linux port must carry the same Adv()/Dev() fallback mechanism (store-miss renders the key id).
- The mode-notice channel has TWO consumers: every notice hits the transient window snackbar (SetModeNoticeHandler → LoginPage::ShowModeNotice → AccountSnackbar), while the standing ModeNoticeBar is written only by DeveloperPage via SetModeNoticeObserver (and in practice its real-run replay is documented as preview-oriented). Doc §7.5 describes only the standing bar. The code itself calls the snackbar 'transient where a standing banner would be better' — a sanctioned improvement point for the port, but ship the shipped behavior first.
- Doc §7.1's status-strip row says 'the 4 Advanced fields drop entirely below 1000' — five drop (statusAdvancedParts_ includes Network).
- The egress interface index named in the Advanced-strip spec is deliberately ABSENT: proto::TunnelStatus does not carry it. Do not invent it on Linux either.
- UrComponents.h's header comment still describes the pre-R3 card/VisualState layout ('Every destination instantiates ... a two-state VisualStateGroup named Narrow/Wide') — the shipped mechanism is ApplyBreakpoint in code; no VisualStateGroups function in this shell. Follow §3 of this spec, not that comment.
- Open question for GTK: WinUI NavigationView's Auto pane thresholds (expanded ≥1008 epx, compact 641–1007, overlay ≤640) are platform defaults, not values in this repo; the port must implement them explicitly (they are distinct from the app's own 1000-dip content breakpoint).
