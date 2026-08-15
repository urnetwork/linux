# URnetwork Settings Destination — Implementation Spec (GTK4/C++ port)

Source of truth: `urnetwork-windows/app/src/App/SettingsPage.cpp/.h`, `SettingsSheets.cpp/.h`, `StatsSheets.cpp/.h` (AppRulesSheet), `InstalledApps.h`, `UrComponents.cpp/.h`, `App.xaml`, `MainWindow.xaml(.cpp)`, `SdkHost.cpp/.h`, `UpdateChecker.cpp/.h`. Where `docs/linux_agent_help.md` §7/§8 disagrees with code, the code wins (see FLAGS).

Localization convention in this spec: `Loc("key")` = key exists in the shared store (English given). `Adv("key", "English")` = the key does NOT exist yet; render the English fallback until the store grows the key (`Localized()` returns the key id on a miss; that equality is the fallback test). `Missing()` in SettingsPage.cpp is the identical mechanism, used for `about`, `advanced`, `app_version`.

---

## 1. Destination shape

Three equal full-bleed panes ("preferences in three columns"), all rows CODE-BUILT into three empty host panels; the markup supplies only the shell:

```
SettingsView (Grid, UrPaneStyle: Background #101010)
  col0 *     SettingsPaneA   (LandmarkType=Main)  — pane header + ScrollViewer > StackPanel SettingsSections
  col1 Auto  SettingsPaneBRule (1px vertical rule, #1FFFFFFF)
  col2 *     SettingsPaneB                        — pane header + ScrollViewer > StackPanel SettingsSectionsRight
  col3 Auto  SettingsPaneCRule (1px vertical rule)
  col4 *     SettingsPaneC                        — pane header + ScrollViewer > StackPanel SettingsAboutHost
```

Each pane: `RowDefinition Auto` (header strip) + `*` (its own vertical ScrollViewer, horizontal disabled). Panes scroll independently.

Pane header strip (UrPaneHeaderStyle): Height **40**, Background **#151515**, bottom hairline 1px **#1FFFFFFF**, Padding **12,0**. Title (UrPaneTitleStyle): PP Neue Montreal, **12px SemiBold, CharacterSpacing 60** (letterspaced), #F8F8F8, vertically centered, CharacterEllipsis.

Pane titles + a11y landmark names (set in ApplyStrings):
- Pane A: `Loc("general")` = "General"
- Pane B: `Loc("device")` = "Device"
- Pane C: `Missing("about", "About")` — **key `about` does not exist in the store**
Each pane Grid gets `AutomationProperties.Name` = the same string.

### 1.1 Responsive (MainWindow::ApplyBreakpoint, ~line 612)
- width **≥ 1400 dip** → three panes (General | Device | About)
- **900 ≤ width < 1400** → two panes (About folds: PaneC + its rule collapse, star→0)
- width **< 900** → one pane (PaneB + rule also collapse)
Rationale comment: at the 2062dip reference window each pane is ~660dip — the "constrained settings column" measure. Column fold = set star width to 0 AND Visibility Collapsed on pane + its rule.

### 1.2 Content grouping (what lives in which pane — CODE, not the doc)
- **Pane A (col 0, "what the app DOES")**: General group (no header), then Connections group (`site_app_connections` = "Connections" — the "VPN & privacy" group).
- **Pane B (col 2, "what this machine IS")**: Device group (no header), then Post Quantum Identity group, then Advanced group.
- **Pane C (col 4, "what the app is")**: Version group (no header), then Stay in touch group.

R4 override: the ACCOUNT-SUBJECT sections (Security = login methods/auth code/client id, Referrals, Manage Subscription, Danger = Sign out/Delete account) are still BUILT AND OWNED by SettingsPage but their host panels are on the ACCOUNT destination (`AccountSecurityHost`, `AccountReferralHost`, `AccountPlanExtraHost`, `AccountDangerHost`). Consequences: `LoadSettings()` fires on navigation to Account AND to Settings; `ResetForSignOut()` clears them from SettingsPage.

---

## 2. Row kit in PANE MODE (rows:: namespace, `SetPaneMode(true)` around every Settings section build)

The same builder calls emit pane vocabulary instead of cards. It is a thread-unsafe global flag read only during UI-thread construction.

| Builder | Pane-mode output |
|---|---|
| `Card(host)` | plain StackPanel, spacing 0 (no island, no radius) |
| `Heading(host, text, glyph)` | `MakePaneGroupHeader(text)` — **28px** strip, Background #151515, hairlines top+bottom 1px #1FFFFFFF, Padding 12,0; title 11px, CharacterSpacing **90**, #989898, Montreal; a11y HeadingLevel3. Glyph is DROPPED in pane mode |
| `Row(host, label, note, trailing)` | `MakePaneTwoLineRow(label, note)` — FIXED Height **44** (not MinHeight), Padding 12,0, bottom hairline; inner Grid ColumnSpacing 10, col0 * = text stack (Spacing 1: title UrRowTitleStyle 13px #F8F8F8 trimmed NoWrap; note UrRowNoteStyle 11px #989898 trimmed NoWrap, COLLAPSED when empty), col1 Auto = right-aligned trailing host. Note is TRIMMED to one line — the price of the fixed height |
| `ToggleRow` | Row with a `UrSwitchToggleStyle` ToggleSwitch trailing (platform switch, On/Off content empty, MinWidth 0, right-aligned; on-state fill = brand blue **#638BFC**) |
| `ButtonRow(host,label,note,action)` | Row with a default Button whose Content = action verb |
| `ValueRow(host,label)` | Row whose trailing is a TextBlock: 14px, #989898, CharacterEllipsis, MaxWidth **260** |
| `ValueActionRow(host,label,action,&btn)` | Row whose trailing is a horizontal StackPanel spacing 8: value TextBlock (14px, muted, MaxWidth **220**, centered) + Button |
| `NavRow(host,label,&outValue)` | `MakePaneTwoLineRowButton(label)` — Button on UrPaneRowButtonStyle (transparent bg; hover fill **#1C1C1C**, pressed **#2A2A2A**, transitions **150 ms**; disabled opacity 0.38; bottom hairline; CornerRadius 0), Height AND MinHeight **44**, Padding 12,0; Grid ColumnSpacing 10: col0 text stack, col1 value TextBlock (UrValueTextStyle 13px, right-aligned, forced #989898, MaxWidth **240**), col2 chevron FontIcon (Segoe Fluent Icons — on Linux use an equivalent chevron-right glyph, 11px, #989898, a11y Raw). Button a11y Name = title; note (if any) = FullDescription; title/note/value marked Raw |
| `Supporting(host,text)` | 12px wrapping TextBlock #989898 wrapped in a Border: Padding **12,10,12,10**, bottom hairline 1px #1FFFFFFF — prose allowed to be taller than 44 but on the pane grid |
| `Divider(host)` | NOTHING in pane mode (each row carries its own hairline; a divider would draw a 2px rule). In card mode (dialogs): 1px Border, Background #1FFFFFFF |
| `MakePaneRow(h)` | Border: fixed Height h, Padding 12,0, bottom hairline — used directly for the protocol-link row (h=40) |

Trailing-control a11y (`DescribeTrailing`): a ToggleSwitch gets `AutomationProperties.Name = row label` (it is otherwise nameless — empty On/Off content); a Button keeps its verb and gains `FullDescription = row label`; a TextBlock is NEVER touched (its name is its text). For ValueActionRow's panel, each child is described individually.

### 2.1 FieldState — the six terminal states of every async field
`enum FieldState { NoSession, NoDevice, Loading, Loaded, Empty, Failed }`

`ApplyFieldState(valueTextBlock, state, loadedText)`:
| State | Text | Foreground |
|---|---|---|
| Loaded | caller's text | #989898 (muted) |
| Loading | `Loc("loading")` "Loading..." | #5A5A5A (faint) |
| Empty | `Loc("none")` "None" | faint |
| NoSession | `Loc("please_login_to_urnetwork")` "Please login to URnetwork" | faint |
| NoDevice | `Loc("site_app_device_attaching")` "Attaching device controls…" | faint |
| Failed | `Loc("something_went_wrong")` "Something went wrong." | **#F8523B** (danger) |

NoDevice exists because "signed in but service not up" must not say "please login" (that is a lie). Every load in this destination terminates in exactly one of these; log the underlying error at the call site on Failed.

### 2.2 Snackbar
SettingsPage owns `snackbar_` bound to the `SupportInfo` InfoBar (UrSnackbarStyle: CornerRadius 8, closable) — note this InfoBar physically sits in the SUPPORT destination's form column (see FLAGS). Timer contract (`kit::Snackbar`): default auto-dismiss **4000 ms**; severity-gated — Informational/Success auto-dismiss, **Warning/Error persist until dismissed**; `kPersistent = 0` pins open.

---

## 3. Pane A — GENERAL (SettingsSections host)

### 3.1 General group — `BuildGeneralSection` (NO group header: pane title already says "General")
Element order, top→bottom:

**Row 1 — Send product updates (44px ToggleRow)**
- Label `Loc("send_product_updates")` = "Send me product updates". No note.
- ACCOUNT preference (server-side). Toggle starts **IsEnabled(false)** until the current value is read.
- Under it, its state line: a 12px wrapping TextBlock (`productUpdatesState_`) inside a Border Padding **12,8,12,8** + bottom hairline. Initial state `NoSession`.
- Load (`LoadPreferences`, part of LoadSettings): set state line Loading → `api().accountPreferencesGet(cb)`. Failure test: `!result || err` → state line **Failed**, toggle stays disabled ("we do not know what the server holds"). Success: `allow = result->product_updates && *product_updates`; write with echo guard (`applyingPreference_=true; toggle.IsOn(allow); applyingPreference_=false`), set `preferencesLoaded_=true`, enable toggle, CLEAR state line text ("the toggle itself is the state now").
- Toggled handler (`OnProductUpdatesToggled`): return if `applyingPreference_ || !preferencesLoaded_` (echo guard + not-yet-loaded guard). Else: disable toggle; `AccountPreferencesSetArgs{product_updates=IsOn}` → `api().accountPreferencesUpdate(args, cb)`. Success test: result present AND no transport err (result struct carries no error field). On UI thread: re-enable; on failure snap the toggle back (`applyingPreference_` guard around `IsOn(!allow)`) and snackbar `Loc("couldnt_update_preferences")` = "Couldn't update your preferences. Please try again." severity Error (persistent).

**Row 2 — Check for updates automatically (44px ToggleRow)**
- Label `Adv("upd_auto_check", "Check for updates automatically")`; note `Adv("upd_auto_check_note", "Look for new releases shortly after launch and every six hours. Nothing is ever installed without a click.")` (note trims to one line in the 44px row).
- LOCAL preference — persisted as key `"check_updates_automatically"` in `app_prefs.json` (whole-file read-modify-write JSON beside `advanced_mode`). Default **true**. No session gate, no FieldState, no echo guard (one writer — this toggle; nothing writes IsOn back).
- Init: `IsOn(UpdateChecker::AutoCheckEnabled())` (static read of the prefs file).
- Toggled: `Updates().SetAutoCheckEnabled(IsOn())` — persists, and turning it ON fires a check immediately ("answer now, not in six hours"). Checker cadence facts (for the note's honesty): launch check after **30 s** delay, then every **6 h**.

### 3.2 Connections group — `BuildConnectionsSection`
Group header: `Heading(Loc("site_app_connections"))` = "Connections" (28px strip).

**Row 1 — Kill switch (44px ToggleRow)**
- Label `Loc("kill_switch")` = "Kill switch"; note `Adv("adv_kill_switch_note", "If the tunnel drops unexpectedly, block this device's traffic instead of letting it out unprotected.")`.
- DELIBERATE: the shipped store key `site_app_kill_switch_note` ("Block browser traffic when URnetwork is disconnected") is WRONG twice and must NOT be used (it is not browser-only, and "when disconnected" misdescribes a bug as a feature — the switch guards only UNEXPECTED loss).
- Init (in `ApplyLocalDeviceState`, which runs at build time and on every LoadSettings): `applyingKillSwitch_=true; IsOn(Sdk().CurrentKillSwitch()); applyingKillSwitch_=false`. Readable with no session/tunnel: preference lives in app LocalState.
- Toggled (`OnKillSwitchToggled`): return if `applyingKillSwitch_` (echo guard). `wanted = IsOn(); applied = Sdk().SetKillSwitch(wanted); actual = Sdk().CurrentKillSwitch();` — READ BACK rather than trusting the return (this is the toggle where a wrong state costs privacy). If `applied && actual==wanted` done. Else log warn, snap toggle to `actual` under the guard, snackbar `Loc("something_went_wrong")` Error.
- SdkHost semantics (replicate): kill switch == **!routeLocal** (inversion lives in SdkHost, never in views). `SetKillSwitch`: write LocalState FIRST (`localState_->setRouteLocal(!on)` — persistent truth), then device (`device_->setRouteLocal(!on)`), then the enforcement leg — service RPC `SetKillSwitch(on)` (Windows: WFP policy; **Linux: nftables policy in the daemon**). Any leg failing → returns false. `CurrentKillSwitch`: prefer `!device_->getRouteLocal()`, else `!localState_->getRouteLocal()`, else **false** (claim the permissive default, not the strict one).

**Row 2 — Supporting prose #1** (`Supporting`, 12px muted, hairline): `Adv("adv_kill_switch_deliberate", "Pressing Disconnect always restores your internet straight away. The kill switch only applies to drops you did not ask for - a tunnel that stops carrying traffic, a network change, or the URnetwork service stopping.")`

**Row 3 — Supporting prose #2**: `Adv("adv_kill_switch_dns_window", "While it is on and nothing is connecting, nothing leaves this device - name lookups included. During a connection attempt, name lookups from any app on this device can leave in the clear so URnetwork can reach its servers; everything else stays blocked.")` — the DNS-window honesty disclosure; keep it on Linux if the daemon's connect path opens an equivalent DNS permit.

**Row 4 — Blocked locations (44px NavRow)**
- Label `Loc("blocked_locations_2")` = "Blocked locations", no value. Click → `ShowBlockedLocationsSheet()` (§6.2). This method is PUBLIC: the Network destination's detail pane is a second door to the same sheet.

**Row 5 — App split rules (44px ButtonRow)**
- Label `Loc("app_split_rules")` = "App split rules"; note `Loc("apps_listed_bypass_vpn")` = "Apps listed here bypass the VPN."; action button `Loc("manage_apps")` = "Manage apps". Click → `OnManageAppSplitTunnel` → `ShowAppRulesSheet()` (§6.4).

**Row 6 — Uninstall VPN service (44px ButtonRow inside its own host StackPanel `serviceRowHost_`)**
- Label `Adv("svc_service_label", "VPN service")`; note `Adv("svc_uninstall_note", "Remove the Windows service URnetwork uses to carry traffic.")`; action `Adv("svc_uninstall_action", "Uninstall")`. **Linux port: reword note for the systemd unit** (see FLAGS).
- Wrapped in a host panel so VISIBILITY collapses the WHOLE row (ButtonRow returns only the button; hiding the button alone would leave a caption pointing at nothing). Starts **Collapsed**.
- `ApplyServiceSetup(snapshot)` is the ONLY writer of that visibility (pushed by MainWindow, the one writer of the snapshot): visible iff `state ∈ {Running, Stopped, VersionMismatch}` (registered in any form); hidden — NOT disabled — for NotInstalled / ConsoleMode / Unknown (an affordance for removing what is not there is noise; in ConsoleMode it would fight the developer's console run). Button `IsEnabled(!snap.busy)` — disabled while EITHER elevated verb (install/uninstall) runs, so two elevation prompts can't be in flight.
- Click → `ConfirmUninstallService()`: guarded by the window's one-sheet-at-a-time `sheetOpen_` flag. ContentDialog via `MakeSheet` (sheet surface **#151515**, close button "Close" replaced): Title `Adv("svc_service_label","VPN service")`; Primary `Adv("svc_uninstall_action","Uninstall")`; CloseButton `Loc("cancel")` = "Cancel"; **DefaultButton = Close** (Enter must not remove). Body TextBlock 14px wrap MinWidth 320: `Adv("svc_uninstall_confirm", "This stops the URnetwork service and removes it from Windows. The app can't connect until it is set up again. Windows will ask for administrator permission.")` — says the two costs so the elevation prompt is expected. On Primary → `MainWindow::BeginServiceUninstall()`.
- `BeginServiceUninstall` semantics: re-check busy/state (a stale window can still click); set busy, clear notice, re-apply; on background thread `ServiceSetup::RunElevatedVerb(L"uninstall", timeout 30000 ms)` — Windows: UAC-elevated `urnetworkd uninstall`; **Linux analogue: privileged systemd unit removal (e.g. pkexec `urnetworkd uninstall` → systemctl disable/stop + unit file removal)**. User-declined elevation = SILENCE (the user changed their mind), not an error. Otherwise failure = `!launched || !exited || exitCode != 0` → log + snackbar `something_went_wrong` Error. Success: `AwaitState(NotInstalled, 5000 ms)` re-probe; failure/declined: `Classify()` fresh probe. Then publish the new snapshot (which hides/shows the row and the Connect page's setup banner).

---

## 4. Pane B — DEVICE (SettingsSectionsRight host)

### 4.1 Device group — `BuildDeviceSection` (NO group header — pane title already says "Device"; a 28px strip repeating it reads as a stutter)

**Row 1 — Device name (44px NavRow)**: label `Loc("device_name_label")` = "Name"; value TextBlock ← FieldState. Click → `ShowDeviceNameSheet()` (§6.1).
**Row 2 — Device spec (44px ValueRow)**: label `Loc("device_spec_label")` = "Spec". Server-assigned, read-only.
Both start `NoSession`.

Load (`LoadDeviceInfo`): requires `clientId_` (read in `ApplyLocalDeviceState` from `Sdk().device().getClientId()` when `hasDevice()`); with none: state = `IsLoggedIn() ? NoDevice : NoSession` for both rows. Else both → Loading; `api().getNetworkClients(cb)`: find `info.client_id == clientId_` in `result->clients`; name = `device_name` falling back to `description`; spec = `device_spec`. Transport failure OR "call worked but this client is not in its own network's list" (log warn — that means something is wrong, not empty) → both **Failed**. Success → `deviceName_` cached; both rows Loaded (empty string → Empty="None").

### 4.2 Post Quantum Identity group — `BuildIdentitySection`
Header: `Heading(Loc("post_quantum_identity"))` = "Post Quantum Identity".
**Row 1 — Provider Identities (44px NavRow)**: label `Loc("provider_identities")` = "Provider Identities", no value. Click → `ShowIdentitySheet()` (§6.3).
**Row 2 — Supporting prose**: `Loc("post_quantum_identity_explanation")` = "Your identity key is stored locally on this device. If any peer's key appears different than their locally stored key, it means the network operator cannot be trusted."

### 4.3 Advanced group — `BuildAdvancedSection`
Header: `Heading(Missing("advanced", "Advanced"))` — **key `advanced` not in store**.

**Row 1 — Advanced mode (44px ToggleRow, inside `advancedModeHost_` StackPanel — the D5 drop point, FIRST in the group)**
- Label `Adv("adv_advanced_mode", "Advanced mode")`; note **`Adv("adv_advanced_mode_note", "Show raw values, identifiers, the connection inspector and the reliability tuning surface across the app.")`**.
- Init: `IsOn(Sdk().CurrentAdvancedMode())`.
- Toggled: `if (applyingAdvancedMode_) return;` (re-entrancy/echo guard — IsOn assignment raises Toggled) then `w_.SetAdvancedMode(IsOn())`. **THE TOGGLE IS THE ONE WRITER AND IT ONLY WRITES — IT NEVER APPLIES.** Full contract in §8.

**Row 2 — Save logs (44px ButtonRow)**
- Label `Loc("save_logs")` = "Save logs"; note `Loc("export_logs")` = "Export Logs"; action `Loc("save")` = "Save". Click → `SaveLogsToFile()`:
  - `source = LogFilePath()` (the app's own log file). Missing/empty → snackbar `Loc("no_log_files_found")` = "No log file found" Warning (persistent).
  - Native save-file dialog (Windows FileSavePicker needs an owner HWND; GTK: GtkFileDialog.save). **SuggestedFileName = the log file's stem** (basename without extension); file-type choice labeled `Loc("export_logs")` with extension `.log`.
  - Cancel = silent no-op (not a failure). Success: copy_file overwrite → snackbar `Loc("save_logs")` ("Save logs") Success. Any throw → log + snackbar `something_went_wrong` Error.
- There is deliberately NO "Share/upload logs" row here. Log upload happens ONLY inside the feedback flow (Support destination `OnSendFeedback`): after the server accepts feedback, if the user ticked `feedback_include_logs`, call `Sdk().device().uploadLogs(serverFeedbackId, cb)` with the SERVER-issued feedback_id; attach failure is silent-by-design (logged only) because the feedback itself succeeded.

---

## 5. Pane C — ABOUT (SettingsAboutHost)

### 5.1 Version group — `BuildVersionSection` (no header)
**Row 1 — Version and build info (44px ValueRow)**: label `Loc("version_info")` = "Version and build info". Value written in BuildSections: `sdkVersion = urnet::version()`; if empty (it IS empty in the current SDK build) fall back to `Sdk().appVersion()` (a constant the app reports to the SDK/server; "0.0.1" on Windows today).
**Row 2 — App version (44px ValueRow)**: label `Missing("app_version", "App version")` — key not in store. Value = the build's compile-time stamp `urnw::version::kString` (release tags / update banner / OS version resource all speak this exact string; **"0.0.0-dev" outside CI is the correct answer, not a bug**). Applied at build time — compile-time constant, no round trip.

### 5.2 Stay in touch group — `BuildStayInTouchSection`
Header: `Heading(Loc("stay_in_touch"))` = "Stay in touch".

**Rows 1–2 — community links**: each is the store's own markdown sentence rendered with the link INLINE and clickable (`SetMarkdownLinkText`, 13px), wrapping allowed, inside a Border Padding **12,10,12,10** + bottom hairline (shares the pane's left edge/hairline grid):
- `Loc("join_the_community_on_discord_https_discord_com")` = "Join the community on [Discord](https://discord.com/invite/RUNZXMwPRK)"
- `Loc("verified_project_on_depin_hub_https_depinhub_io")` = "Verified project on [DePIN Hub](https://depinhub.io/projects/urnetwork)"
The canonical URLs (what the affordance actually opens; keep in step with the strings): `https://discord.com/invite/RUNZXMwPRK`, `https://depinhub.io/projects/urnetwork`.

**Row 3 — protocol link**: a `MakePaneRow(40)` (fixed 40px, hairline) containing a HyperlinkButton: content `Loc("uses_ur_protocol")` = "Uses the UR Protocol", FontSize 13, Padding 0, → `https://ur.xyz`.

---

## 6. Sheets (ContentDialogs)

All sheets: created through `MakeSheet(root, title)` — surface **#151515** (sheets sit ABOVE the #101010 page), CloseButtonText `Loc("close")` = "Close" unless overridden. One sheet at a time via the window's `sheetOpen_` guard (open attempts while one shows are dropped). Lifetime: window holds the shared_ptr while showing; control handlers capture the sheet weakly; SDK callbacks capture DispatcherQueue + weak_from_this and marshal to UI thread before touching anything.

### 6.1 DeviceNameSheet (opened from Device name row)
- Title `Loc("edit_device_name")` = "Edit device name". Primary `Loc("save")` = "Save"; Close `Loc("cancel")` = "Cancel"; **DefaultButton = Primary**.
- Content: StackPanel MinWidth **360** Spacing **8**: TextBox (UrTextInputStyle: 16px, no corner radius, underline look, Padding 0,0,0,8) Header `Loc("device_name")` = "Device name", prefilled with the page's cached `deviceName_` (display only); error TextBlock 12px wrap danger, Collapsed.
- Not logged in: box disabled, primary disabled, errorText shows FieldState NoSession, visible.
- PrimaryButtonClick: always `args.Cancel(true)` — Submit decides whether the sheet closes. Submit: no-op if saving_ or !IsLoggedIn; `name = Trim(text)`; **empty name is a silent no-op**. Set saving_, disable primary, hide error; `DeviceSetNameArgs{device_name=name}` — **device_id left unset: the server names the client this JWT was issued to ("this device")** → `api().deviceSetName(args, cb)`. Error via ServerError (server `result->error->message` first, then transport err, then generic).
- ApplyResult: clear saving_, re-enable primary. OK → invoke `onSaved(name)` (page updates `deviceName_`, sets the row Loaded with the new name, snackbar `Loc("device_name_updated")` = "Device name updated" Success) then `dialog.Hide()`. Fail → errorText = server message if any else `Loc("error_updating_device_name")` = "There was an error updating the device name.", visible.

### 6.2 BlockedLocationsSheet (from Blocked locations row; also the Network destination's second door)
- Title `Loc("blocked_locations")` = "Blocked Locations". Content StackPanel MinWidth **420** Spacing **12**:
  1. `blockedPanel_` (StackPanel spacing 4) — current blocked rows.
  2. `blockedEmpty_` TextBlock 12px faint, initial `Loc("no_blocked_locations")` = "No blocked locations".
  3. Divider (1px — card mode inside dialogs).
  4. Label `Loc("select_country_to_block")` = "Select country to block" (UrLabelStyle 12px muted).
  5. Search TextBox (UrTextInputStyle) Placeholder `Loc("search_placeholder")` = "Search for all locations"; TextChanged → RenderCountries (client-side filter).
  6. ScrollViewer MaxHeight **240**, auto vscroll → `countryPanel_` (StackPanel spacing 2).
  7. errorText 12px danger wrap, Collapsed.
- Create() → Build, LoadBlocked, LoadCountries. BOTH lists carry their own FieldState (`blockedState_`, `countriesState_`, init Loading) — without them a 401 arrived as an empty list and rendered as the reassuring "No blocked locations".
- LoadBlocked: gate on `IsLoggedIn()` (NOT apiReady — that is set at SDK init, not login) → NoSession. Else Loading → `api().getNetworkBlockedLocations(cb)`; result has NO error field, so failure = `!result || err`. Sort by `location_name`. State = Failed / Empty / Loaded. Then RenderBlocked + RenderCountries (already-blocked countries drop out of the picker).
- RenderBlocked: clear panel. Non-Loaded-or-empty: blockedEmpty_ visible, ApplyFieldState with the state (Loaded+empty→Empty), and for the genuine-empty case override text to the specific `no_blocked_locations` line (beats generic "None"). Loaded: hide empty line; per blocked location a card-mode `Row(name, —, RemoveButton)`: Remove button content `Loc("remove")` = "Remove", Foreground **#F8523B**, disabled if the entry has no location_id; click → Unblock(id).
- LoadCountries: same login gate (getProviderLocations is actually unauthenticated, but a signed-out dev switch must not talk to production); re-entry guard `loadingCountries_`. `api().getProviderLocations(cb)` → keep only `location_type == LocationTypeCountry`; id = `location_id` else `country_location_id`; drop empty id/name; sort by name.
- RenderCountries: clear; filter = lowercased trimmed search; skip already-blocked ids; each hit = full-width Button (UrCardRowButtonStyle, content = country name, left-aligned) → Block(id). If 0 shown: one 12px line that SAYS WHICH case — Loaded+nonempty ⇒ `Loc("no_locations_found")` = "No locations found" (faint; search matched nothing); otherwise ApplyFieldState(countriesState_) (Loading/NoSession/Failed/Empty each distinct).
- Block(id): gate IsLoggedIn + non-empty id; hide error; `NetworkBlockLocationArgs{location_id}` → `api().networkBlockLocation`. Error → `Loc("blocked_location_could_not_be_added_please_try")` = "Blocked location could not be added. Please try again later." Success → **re-fetch LoadBlocked()** (never insert a locally-built row; the server owns name/type).
- Unblock(id): mirror with `networkUnblockLocation`; error line `Loc("blocked_location_could_not_be_removed_please_try")` = "Blocked location could not be removed. Please try again later."; **LoadBlocked() re-sync either way**.

### 6.3 PostQuantumIdentitySheet (from Provider Identities row)
- Title `Loc("post_quantum_identity")`. Content StackPanel MinWidth **420** Spacing **12**:
  1. Identicon Image **80×80**, centered, Collapsed until decoded.
  2. Hash TextBlock: 13px monospace (Consolas → Linux: monospace), wrap, centered, text-selectable.
  3. Copy button, centered, content `Loc("copy")` = "Copy", **IsEnabled(false) until a hash exists** (an enabled Copy over nothing lies). Click: copy the RAW hash (never the grouped display form); status line ← `Loc("identity_key_hash_copied")` = "Identity key hash copied", muted, visible.
  4. Supporting `Loc("post_quantum_identity_explanation")` (12px muted wrap).
  5. Divider (1px).
  6. Label `Loc("provider_identities")` (UrLabelStyle).
  7. ScrollViewer MaxHeight **200** → providerPanel_ (spacing 6).
  8. statusText 12px wrap faint.
- Load (N3): DeviceRemote calls = synchronous RPCs — **show the dialog FIRST, run the three reads on a background thread**, marshal back. Gate: `!hasDevice()` → hash line AND status line get `IsLoggedIn() ? NoDevice : NoSession`; Copy stays disabled; return. Else both Loading, then off-thread: `device.getPublicIdentityKeyHash()`, `device.getPublicIdentityKey()`, `device.getProviderIdentities()`; any throw ⇒ failed.
- ApplyIdentity: failed → hash+status Failed. hash empty → Empty. Else hash line = `GroupHash(hash, elide=false)` — 4-char groups space-joined; enable Copy. Identicon: `urnet::renderIdenticonPng(key, 160)` — rendered at **2× the 80px display size** so it stays crisp; byte-identical across platforms for the same key (that is the point — out-of-band glyph comparison); decode async, sheet-dismissal-safe.
- Provider rows: per identity a 2-line stack (spacing 2): ClientId 12px monospace trimmed; `GroupHash(PublicKey, elide=true)` 11px monospace faint trimmed — elision: when >6 groups show first 4 groups + "..." + last 2 groups.
- Status line: `Plural("connected_provider_count", count)` → "Connected to {} provider(s)" (store keys `connected_provider_count.one` / `.other`), faint.

### 6.4 AppRulesSheet (per-app split tunnel; lives in StatsSheets.cpp)
- Title `Loc("app_split_rules")` = "App split rules" (MakeDialog: same #151515 surface, Close button). Content = ScrollViewer **MaxHeight 520, MinWidth 440** wrapping a StackPanel spacing 8.
- On open: `installed_ = EnumerateInstalledApps()` — Windows: registry Uninstall keys (HKLM 64+32-bit views + HKCU), exe derived from DisplayIcon, skip SystemComponent/no-name/no-exe/nonexistent path, de-dup by lowercased exe path, exclude self, sort case-insensitively by display name. **Linux analogue required: scan .desktop entries → resolve Exec to a real binary path — whatever path the Linux split-tunnel driver (cgroup/fwmark) matches on** (see FLAGS). Struct: `{name, exePath}`.
- **Summary banner** (card-in-sheet): Border CornerRadius **12**, Padding **12**, Background **#1C1C1C**. Inside, StackPanel spacing 6:
  - Status row (Grid Auto+Star, spacing 8): `statusDot_` Border **8×8**, CornerRadius 4 (a circle), centered + `statusText_` 13px wrap.
  - Precedence note: 12px muted wrap `Loc("app_split_precedence_note")` = "Included apps take precedence: when any app is included, only included apps use the VPN and exclusions have no effect."
- **App list** (`appsList_` StackPanel spacing 2), built by RenderList():
  - Read `sdk.CurrentAppRules()` → map lower(imagePath)→includeInTunnel.
  - **Ruled apps pinned on top in rule order** (section header `Loc("rules")` = "Rules" — 12px muted, top margin 8), then remaining installed apps under header `Loc("apps")` = "Apps". A ruled app that wasn't enumerated still gets a row (name = image basename) so an existing rule stays visible and changeable.
  - Both empty → single line `Loc("no_installed_apps_found")` = "No installed apps found. (Store apps aren't listed.)" 12px faint wrap; still RefreshRuleState (summary reflects empty rules).
  - Per-app row: Grid cols Star/Auto/Auto, ColumnSpacing 8, Padding 0,4,0,4. Col0: name 13px wrap + exePath 10px faint wrap. Col1: chip slot (Border, filled by RefreshRuleState). Col2: **ComboBox MinWidth 130** with exactly three items: index 0 `Loc("default_option")` = "Default" (no rule), 1 `Loc("include_in_vpn")` = "Include in VPN", 2 `Loc("bypass_vpn")` = "Bypass VPN". Initial index: ruled → include?1:2; unruled → 0.
  - SelectionChanged: idx ≤ 0 → `sdk.RemoveAppRule(path)`; else `sdk.SetAppRule(path, idx==1)` (1=include→Local=false, 2=bypass→Local=true). Then `RefreshRuleState()` — chips + summary update IN PLACE; **pinned order only re-sorts on the next open** (rows keep position while editing).
- **RefreshRuleState()** (the active-mode logic):
  - `includeMode` = any rule with includeInTunnel. `excludeMode` = !includeMode && rules non-empty.
  - Dot color: includeMode → **#638BFC** (kToggleAccent); excludeMode → **#87FB67** (kUrGreen); none → **#989898** (kTextMuted).
  - Status text: `app_split_active_include` = "Include active — only included apps use the VPN" / `app_split_active_exclude` = "Exclude active — excluded apps bypass the VPN" / `app_split_active_none` = "No app split — all apps use the VPN".
  - Chips per ruled row (`MakeChip`: capsule CornerRadius 9, Padding 7,3, label 10px weight 500; highlighted ⇒ solid color fill + **#101010** text; idle ⇒ color @ alpha 36/255 tint + colored text): included → `Loc("included")` = "Included" in #638BFC; bypass → `Loc("local")` = "Local" in #87FB67. **A local (bypass) rule has no effect while include mode is active** → its chip renders muted (#989898, not highlighted). Unruled rows: empty slot.
- **SdkHost rule storage** (replicate semantics): a rule is a `BlockActionOverride` whose `AppIds=[imagePath]` and `RouteOverride.Local = !includeInTunnel`. Writes go to BOTH `localState_` (offline source of truth — sheet works disconnected) and `device_` (live; `setBlockActionOverrides` fires the SDK change listener which re-drives the split-tunnel driver from `getLocalOverrideAppIds` — that getter already inverts: Included = Local/bypass, Excluded = tunneled). `CurrentAppRules` reads localState only; entries without AppIds are host-based split rules (a different sheet), skip them. Include-precedence/allowlist inversion is Android parity.

---

## 7. Sign out / danger placement + ResetForSignOut

- Sign out and Delete account are built by `SettingsPage::BuildDangerSection()` onto **the ACCOUNT destination's** `AccountDangerHost` — last on that page at every width — under a 28px group-header strip whose text `SettingsAccountHeading` is painted by Settings' ApplyStrings with `Loc("account")` = "Account". (Doc §7.11 "Sign out lives here" is stale — see FLAGS.)
- Both are whole-row 44px NavRows (never "Sign out [Sign out]" label+button): `Loc("sign_out")` = "Sign out" → `OnSignOut` → `Sdk().Logout()` (fires the auth-state relay; MainWindow calls ResetForSignOut on every page). `Loc("delete_account_2")` = "Delete account" → DeleteAccountSheet. **NOT red in the row** — red belongs to the destructive confirmation context only (the sheet's typed-name gate).
- **`ResetForSignOut()` — the exact field-clearing list** (identity first — every one describes the account that just left; `networkName_` is the dangerous one: the delete gate compares against it and `networkDelete` acts on the CURRENT JWT):
  1. Clear: `clientId_`, `referralCode_`, `networkName_`, `deviceName_`, `authTypes_`; `preferencesLoaded_ = false`.
  2. Visible state → NoSession: clientIdValue, referralCodeValue, referralNetworkValue, deviceNameValue, deviceSpecValue; `RenderAuthMethods(NoSession)`.
  3. `clientIdCopy_.IsEnabled(false)`; `referralCodeCopy_.IsEnabled(false)`.
  4. Product updates: `applyingPreference_=true; IsOn(false); applyingPreference_=false; IsEnabled(false);` state line → NoSession.
  (Advanced-mode toggle, kill switch and auto-update-check are NOT reset — they are local/app state, not account state.)
- DeleteAccountSheet (context): takes NO cached name — reads the current session's network name itself via `getNetworkUser` and fails closed; typed-name gate enables the primary only on exact match; DefaultButton=Close; on success `networkDelete` → `dialog.Hide(); sdk->Logout()`.

## 8. D5 Advanced Mode standing-state contract (as implemented)

- **Storage**: key `"advanced_mode"` in `app_prefs.json` (`%LOCALAPPDATA%\URnetwork\app\` → Linux: XDG state/config dir), whole-file read-modify-write. Loaded at SdkHost::Initialize into an atomic (`advancedMode_`, acquire/release) — the value exists BEFORE any view (the window may be built ~25 s after startup).
- **API**: `CurrentAdvancedMode()` — the authority, valid at ANY time. `SetAdvancedMode(bool)` — **PERSIST FIRST, PUBLISH SECOND**: store atomic → `SaveAppPref` → log → `RefreshAdvancedMode()`. The publish is best-effort; the recorded value is what reaches a late-built surface. `SetAdvancedModeHandler(fn)` — an optimisation for changes AFTER a view binds. `RefreshAdvancedMode()` — replays the standing value (reads one atomic, copies one std::function, NO SdkHost mutex on this path — a UI-thread call must not queue behind session bootstrap).
- **BIND-THEN-REPLAY** (MainWindow ctor): bind the handler (marshals to UI queue → `ApplyAdvancedMode`), then SYNCHRONOUSLY call `ApplyAdvancedMode(Sdk().CurrentAdvancedMode())` — waiting for a change notification provably loses the disk-restored value (nothing changed, no event is coming), and enqueue-instead-of-sync causes a visible one-frame Normal→Advanced flash.
- **ONE APPLY PATH**: Settings toggle → `MainWindow::SetAdvancedMode` → `Sdk().SetAdvancedMode` (persist only) → handler → `MainWindow::ApplyAdvancedMode(on)` — the SAME path a disk-restored value takes, so toggle-now and on-at-launch cannot render differently. ApplyAdvancedMode: sets `advancedMode_`; **inserts/removes the Developer nav item in the footer collection** (never Visibility — insert at index 1: Support, Developer, Settings; on turn-off, if the user is STANDING on developer, navigate them to Connect); rebuilds the status strip; `ApplyBreakpoint(force=true)`; then calls each page's `ApplyAdvancedMode(on)` — including Settings itself (the page that OWNS the toggle must be told, or a disk-restored mode leaves the very control that sets it reading Off).
- **RE-ENTRANCY GUARD**: `SettingsPage::ApplyAdvancedMode(on)`: no-op if the section isn't built or IsOn already equals `on`; else `applyingAdvancedMode_=true; IsOn(on); applyingAdvancedMode_=false`. The Toggled handler returns immediately under the guard, so the apply-path write never echoes back out through SdkHost as a user edit.

## 9. Load pipeline & preview

`LoadSettings()` (fires on navigation to Settings AND Account, and on auth change; skipped entirely by `--preview-ui`):
1. `ApplyLocalDeviceState()` — no round trips: client id off `device().getClientId()` (NoDevice/NoSession per §2.1 when absent; copy button enabled iff non-empty), kill-switch IsOn under echo guard.
2. If `!IsLoggedIn()`: every server-backed field → NoSession (referral code/network, device name/spec, auth methods; product-updates toggle disabled + NoSession line) and RETURN — no field may sit on a dash or an unresolving spinner.
3. Else: `LoadNetworkUser()` (auth methods + networkName cache), `LoadDeviceInfo()`, `LoadReferral()` (two parallel calls; note: for `getReferralNetwork`, "No referral network found" arrives on the error channel of a SUCCESSFUL lookup — only transport failure is Failed; a structured response means the server answered, empty name ⇒ "None"), `LoadPreferences()`.
All callbacks: capture DispatcherQueue + weak window ref; marshal to UI thread; resolve weak; terminal FieldState always set. `ShowPreviewSnackbar()` (--preview-ui only) raises `thanks_for_the_feedback` Success to demo the auto-dismissing severity.

## 10. Localization key table (Settings destination + its sheets)

Store keys (exist): general=General · device=Device · account=Account · settings=Settings · send_product_updates=Send me product updates · couldnt_update_preferences=Couldn't update your preferences. Please try again. · kill_switch=Kill switch · site_app_connections=Connections · blocked_locations=Blocked Locations · blocked_locations_2=Blocked locations · app_split_rules=App split rules · apps_listed_bypass_vpn=Apps listed here bypass the VPN. · manage_apps=Manage apps · app_split_precedence_note=Included apps take precedence: when any app is included, only included apps use the VPN and exclusions have no effect. · app_split_active_include=Include active — only included apps use the VPN · app_split_active_exclude=Exclude active — excluded apps bypass the VPN · app_split_active_none=No app split — all apps use the VPN · no_installed_apps_found=No installed apps found. (Store apps aren't listed.) · default_option=Default · include_in_vpn=Include in VPN · bypass_vpn=Bypass VPN · included=Included · local=Local · rules=Rules · apps=Apps · post_quantum_identity=Post Quantum Identity · provider_identities=Provider Identities · post_quantum_identity_explanation=(see §4.2) · identity_key_hash_copied=Identity key hash copied · connected_provider_count.one/.other=Connected to {} provider(s) · device_name_label=Name · device_spec_label=Spec · device_name=Device name · edit_device_name=Edit device name · device_name_updated=Device name updated · error_updating_device_name=There was an error updating the device name. · save=Save · save_logs=Save logs · export_logs=Export Logs · no_log_files_found=No log file found · version_info=Version and build info · stay_in_touch=Stay in touch · join_the_community_on_discord_https_discord_com=Join the community on [Discord](https://discord.com/invite/RUNZXMwPRK) · verified_project_on_depin_hub_https_depinhub_io=Verified project on [DePIN Hub](https://depinhub.io/projects/urnetwork) · uses_ur_protocol=Uses the UR Protocol · sign_out=Sign out · delete_account_2=Delete account · select_country_to_block=Select country to block · search_placeholder=Search for all locations · no_locations_found=No locations found · no_blocked_locations=No blocked locations · blocked_location_could_not_be_added_please_try=Blocked location could not be added. Please try again later. · blocked_location_could_not_be_removed_please_try=Blocked location could not be removed. Please try again later. · remove=Remove · cancel=Cancel · close=Close · copy=Copy · loading=Loading... · none=None · please_login_to_urnetwork=Please login to URnetwork · site_app_device_attaching=Attaching device controls… · something_went_wrong=Something went wrong.

Adv()/Missing() fallbacks (key does NOT exist yet; key + exact English):
- upd_auto_check → "Check for updates automatically"
- upd_auto_check_note → "Look for new releases shortly after launch and every six hours. Nothing is ever installed without a click."
- adv_advanced_mode → "Advanced mode"
- adv_advanced_mode_note → "Show raw values, identifiers, the connection inspector and the reliability tuning surface across the app."
- adv_kill_switch_note → "If the tunnel drops unexpectedly, block this device's traffic instead of letting it out unprotected."
- adv_kill_switch_deliberate → "Pressing Disconnect always restores your internet straight away. The kill switch only applies to drops you did not ask for - a tunnel that stops carrying traffic, a network change, or the URnetwork service stopping."
- adv_kill_switch_dns_window → "While it is on and nothing is connecting, nothing leaves this device - name lookups included. During a connection attempt, name lookups from any app on this device can leave in the clear so URnetwork can reach its servers; everything else stays blocked."
- svc_service_label → "VPN service"
- svc_uninstall_note → "Remove the Windows service URnetwork uses to carry traffic." (Linux: reword for systemd)
- svc_uninstall_confirm → "This stops the URnetwork service and removes it from Windows. The app can't connect until it is set up again. Windows will ask for administrator permission." (Linux: reword)
- svc_uninstall_action → "Uninstall"
- about → "About" · advanced → "Advanced" · app_version → "App version"

## 11. Key numbers cheat-sheet
Pane header 40px / group header 28px / two-line row 44 fixed / single-line pane row 40 / list row 36 / key-value row 34 / protocol-link row 40. Inset 12px; hairline 1px #1FFFFFFF. Row title 13px #F8F8F8; note 11px #989898; value 13px right #989898 (NavRow value MaxWidth 240; ValueRow 260; ValueActionRow 220); supporting 12px #989898, padding 12,10; state-line box padding 12,8. Hover fill #1C1C1C, pressed #2A2A2A, state transition 150 ms; disabled opacity 0.38. Sheet surface #151515; danger #F8523B; faint #5A5A5A; toggle-on/include #638BFC; exclude-active #87FB67. Sheet MinWidths: device name 360, add-auth/auth-code 380, referral/delete 400, blocked/PQ 420; AppRules scroll 440×max520; country scroll MaxHeight 240; PQ provider scroll 200; identicon 80×80 @2x render 160. Chip: radius 9, pad 7×3, 10px/500, idle tint alpha 36/255. Status dot 8×8 r4. Combo MinWidth 130. Snackbar 4000 ms (Warning/Error persistent). Breakpoints 1400/900 dip. Update cadence: +30 s launch, 6 h interval. Elevated uninstall verb timeout 30000 ms; post-uninstall AwaitState(NotInstalled) 5000 ms. Body face PP Neue Montreal; monospace for hashes/ids.


## SDK surface referenced
- urnet::Api::getNetworkUser (LoadNetworkUser; DeleteAccountSheet fresh-name gate)
- urnet::Api::getNetworkClients (LoadDeviceInfo — find this client's device_name/description + device_spec)
- urnet::Api::getNetworkReferralCode (LoadReferral)
- urnet::Api::getReferralNetwork (LoadReferral + ReferralNetworkSheet::Load; 'No referral network found' on the error channel = Empty, not Failed)
- urnet::Api::accountPreferencesGet (product-updates toggle load)
- urnet::Api::accountPreferencesUpdate(AccountPreferencesSetArgs{product_updates}) (product-updates toggle write)
- urnet::Api::deviceSetName(DeviceSetNameArgs{device_name; device_id unset = this JWT's client}) (DeviceNameSheet)
- urnet::Api::getNetworkBlockedLocations (BlockedLocationsSheet)
- urnet::Api::getProviderLocations (BlockedLocationsSheet country picker; filter LocationTypeCountry)
- urnet::Api::networkBlockLocation(NetworkBlockLocationArgs{location_id}) (BlockedLocationsSheet)
- urnet::Api::networkUnblockLocation(NetworkUnblockLocationArgs{location_id}) (BlockedLocationsSheet)
- urnet::Api::removeAuth(RemoveAuthArgs{auth_type}) (ConfirmRemoveAuth → RemoveAuth; Account-hosted)
- urnet::Api::addAuth(AddAuthArgs{user_auth,password}) (AddAuthSheet; Account-hosted)
- urnet::Api::authCodeCreate(AuthCodeCreateArgs{duration_minutes=5,uses=1}) (AuthCodeSheet; Account-hosted)
- urnet::Api::setNetworkReferral(SetNetworkReferralArgs{referral_code}) (ReferralNetworkSheet)
- urnet::Api::unlinkReferralNetwork (ReferralNetworkSheet)
- urnet::Api::stripeCreateCustomerPortal(StripeCreateCustomerPortalArgs{}) (Manage Subscription; Account-hosted)
- urnet::Api::networkDelete (DeleteAccountSheet; no args, acts on current JWT)
- urnet::Api::sendFeedback(FeedbackSendArgs{star_count,needs.other}) (Support half of SettingsPage)
- urnet::DeviceRemote::getClientId (ApplyLocalDeviceState)
- urnet::DeviceRemote::getPublicIdentityKeyHash (PostQuantumIdentitySheet, background thread)
- urnet::DeviceRemote::getPublicIdentityKey (PostQuantumIdentitySheet)
- urnet::DeviceRemote::getProviderIdentities (PostQuantumIdentitySheet)
- urnet::DeviceRemote::uploadLogs(feedbackId, cb) (feedback log attach only, server-issued id)
- urnet::DeviceRemote::getRouteLocal / setRouteLocal (kill switch = !routeLocal, via SdkHost)
- urnet::DeviceRemote::getBlockActionOverrides / setBlockActionOverrides (app rules live leg, via SdkHost)
- urnet::DeviceRemote::getLocalOverrideAppIds (SDK change listener re-drives the split-tunnel driver; already-inverted Included=Local)
- urnet LocalState::getRouteLocal / setRouteLocal (kill switch offline truth, via SdkHost)
- urnet LocalState::getBlockActionOverrides / setBlockActionOverrides (app rules offline truth, via SdkHost)
- urnet::version() (SDK version row; empty in current build)
- urnet::renderIdenticonPng(publicKey, 160) (PQ identicon, 2x of 80px display)
- SdkHost::IsLoggedIn / hasDevice / device / api (gates everywhere)
- SdkHost::Logout (Sign out row; DeleteAccountSheet success path)
- SdkHost::CurrentKillSwitch / SetKillSwitch (+ service RPC SetKillSwitch enforcement leg)
- SdkHost::CurrentAppRules / SetAppRule(imagePath, includeInTunnel) / RemoveAppRule(imagePath) (AppRulesSheet)
- SdkHost::CurrentAdvancedMode / SetAdvancedMode (persist-first-publish-second) / SetAdvancedModeHandler / RefreshAdvancedMode (D5)
- SdkHost::appVersion (version row fallback)
- urnw::UpdateChecker::AutoCheckEnabled (static) / Updates().SetAutoCheckEnabled(bool) (auto-update toggle; on-enable immediate check; 30 s launch delay, 6 h cadence; pref key check_updates_automatically)
- MainWindow::SetAdvancedMode → Sdk().SetAdvancedMode (one apply path) / MainWindow::ApplyAdvancedMode (footer insert/remove of Developer item, status strip rebuild, ApplyBreakpoint(force), per-page ApplyAdvancedMode)
- MainWindow::BeginServiceUninstall → ServiceSetup::RunElevatedVerb(L"uninstall", 30000) / ServiceSetup::Classify / ServiceSetup::AwaitState(NotInstalled, 5000) (Linux: systemd analogue)
- urnw::LogFilePath (Save logs source file)
- urnw::EnumerateInstalledApps (AppRulesSheet; Windows registry — Linux .desktop analogue required)

## Flags (doc-vs-code drift / risks)
- DOC vs CODE (§7.11): the doc puts 'client id copy' in Settings' Device pane. Code: client id + copy is in BuildSecuritySection, hosted on the ACCOUNT destination (AccountSecurityHost); Settings' Device pane has only Name + Spec. Code wins.
- DOC vs CODE (§7.11): the doc puts the Uninstall-service row under Device. Code: it is the LAST row of the Connections ('VPN & privacy') group in pane A (BuildConnectionsSection). Code wins.
- DOC vs CODE (§7.11): the doc puts the post-quantum identity row in 'VPN & privacy'. Code: it is its own group in pane B (Device), between the Device rows and the Advanced group. Code wins.
- DOC vs CODE (§7.11): 'Sign out lives here [Settings]'. Code (R4): Sign out and Delete account are built onto the ACCOUNT destination's AccountDangerHost under a group header labeled Loc("account"); SettingsPage still owns the handlers, ResetForSignOut, and the header string. Code wins.
- TASK-PROMPT drift: the prompt frames 'three equal panes: General / VPN & privacy / Device / Advanced / About' as five panes. Code renders five GROUPS in three PANES: A=General+Connections, B=Device+PostQuantumIdentity+Advanced, C=Version+StayInTouch.
- Group heading string: the 'VPN & privacy' group's actual on-screen heading is Loc("site_app_connections") = 'Connections' — 'VPN & privacy' is only the doc's conceptual name.
- Kill-switch note: store key site_app_kill_switch_note EXISTS ('Block browser traffic when URnetwork is disconnected') but is deliberately NOT used — it is wrong twice. The port must use the Adv("adv_kill_switch_note") English, not the store key.
- Linux analogue — service uninstall: RunElevatedVerb(L"uninstall") is a UAC-elevated `urnetworkd uninstall` on Windows. Linux must map to privileged systemd unit removal (e.g. pkexec urnetworkd uninstall → systemctl stop/disable + unit removal), keep the declined-elevation-is-silence rule, and reword svc_uninstall_note/confirm ('Windows service', 'Windows will ask for administrator permission').
- Linux analogue — InstalledApps: enumeration is Windows-registry based (Uninstall keys → DisplayIcon exe). Linux needs a .desktop-entry scan resolving Exec to the real binary path the Linux split-tunnel driver matches on. The comment in InstalledApps.h/StatsSheets.h mentions 'the manual file picker in the sheet remains the fallback', but AppRulesSheet::Build contains NO file picker — code wins: no picker exists.
- Version rows: urnet::version() is EMPTY in the current SDK build, so row 1 falls back to Sdk().appVersion(), which is a hard-coded "0.0.1" (the version the app reports to the SDK). Row 2 (urnw::version::kString) is the real build identifier and reads "0.0.0-dev" outside CI — by design, not a bug. The Linux port should wire its own compile-time stamp.
- Snackbar anchor quirk: SettingsPage's snackbar_ is bound to the SupportInfo InfoBar, which physically lives on the SUPPORT destination's form column — kill-switch/preference errors raised while the user stands on Settings render on an InfoBar the user may not see. Replicating vs fixing (window-level bar) is a port decision; the window-level AccountSnackbar pattern already exists.
- The uninstall row's visibility contract depends on a ServiceSetup::Snapshot classifier with states {Running, Stopped, VersionMismatch, NotInstalled, ConsoleMode, Unknown} and a busy flag — the Linux port needs the systemd equivalent of that classifier before this row can behave.
- Store keys missing upstream (rendered via Adv()/Missing() fallback English, localize the moment the key lands): upd_auto_check, upd_auto_check_note, adv_advanced_mode, adv_advanced_mode_note, adv_kill_switch_note, adv_kill_switch_deliberate, adv_kill_switch_dns_window, svc_service_label, svc_uninstall_note, svc_uninstall_confirm, svc_uninstall_action, about, advanced, app_version.
- a11y note that must survive the port: pane-mode toggles are nameless without AutomationProperties.Name = row label (empty On/Off content); NavRow buttons with panel content are nameless without an explicit name; trailing buttons use FullDescription (append), never LabeledBy (replace) — LabeledBy destroyed ValueRow value readability once already.
- Doc §7.3/§8 tokens match code exactly (40/28/44/36/34 heights, 12px inset, #101010/#151515/#1C1C1C/#1FFFFFFF, #638BFC toggle accent, 1400/900 Settings breakpoints) — no drift found there.
