# URnetwork GTK4/C++ Port — Implementation Spec: SUPPORT + DEVELOPER destinations

Source of truth read for this spec (code wins over docs):
- `urnetwork-windows/app/src/App/MainWindow.xaml` (SupportView lines ~2158–2281, DeveloperView lines ~2382–2432, nav lines ~598–689, ModeNoticeBar ~2436–2453)
- `urnetwork-windows/app/src/App/MainWindow.xaml.cpp` (ApplyBreakpoint Support/Developer branches ~633–660, ApplyAdvancedMode ~1024–1090, OnNavSelectionChanged ~1247–1305, ApplyUpdateChecker ~1571)
- `urnetwork-windows/app/src/App/SettingsPage.cpp` (support surface: ApplyStrings 120–148, UploadLogs 1111–1143, ShowPreviewSnackbar 1281–1283, OnSendFeedback 1287–1340)
- `urnetwork-windows/app/src/App/DeveloperPage.{h,cpp}` (complete)
- `urnetwork-windows/app/src/App/SdkHost.{h,cpp}` (ReliabilitySnapshot/Action ~300–369, bridge impls 3151–3434)
- `urnetwork-windows/app/src/App/App.xaml` (styles), `UrColors.h`, `UrComponents.h` (kit::Snackbar, kWideBreakpointDip), `BalanceSheets.cpp` (SetMarkdownLinkText), `PageContext.{h,cpp}` (Loc/LocBox/Adv), `Strings/en/Resources.resw`
- Cross-checked against `urnetwork-linux/docs/linux_agent_help.md` §7.1, §7.12, §7.13, §8 (disagreements in FLAGS)

Localization convention in this spec: `Loc("key")` = store string (en given). `Dev("key", "English")` = `pages::Adv(key, english)`: `Localized(key)`; if the lookup returns the key itself (the store-miss signal), use the English fallback. **None of the `dev_*` keys are in the store today** — the English literals ARE the shipped strings; the port must carry the same key+fallback pairs so the screen localizes with no code change when keys land. Extraction idiom (keep it greppable): `grep -oE '"dev_[a-z0-9_]+"' DeveloperPage.cpp | sort -u`.

Shared design tokens used below (doc §8, verified in App.xaml/UrColors.h):
- Colors: page `#101010`, sheet `#151515`, card `#1C1C1C`, card hover `#242424`, pressed `#2A2A2A`, text `#F8F8F8`, muted `#989898`, faint `#5A5A5A`, hairline `#1FFFFFFF` (white@12%), strong border `#38FFFFFF`, accent (primary action) `#EFF7BB`, danger `#F8523B`.
- `UrCardStyle` (Border): bg `#1C1C1C`, border `#1FFFFFFF` 1px, corner radius 12, padding 16.
- `UrDividerStyle` (Border): height 1, bg `#1FFFFFFF`, stretch.
- `UrCardLabelStyle` (card heading): PP NeueBit, 22px Bold, `#F8F8F8`.
- `UrSectionHeadingStyle`: PP Neue Montreal 15 SemiBold `#F8F8F8`; `UrPanelHeadingStyle` = same at 18.
- `UrLabelStyle`: Montreal 12 muted; `UrSupportingTextStyle` = same + wrap.
- `UrTextInputStyle` (TextBox): Montreal 16, corner radius 0 (underline field), padding 0,0,0,8, stretch.
- `UrRowIconStyle` (FontIcon): Segoe Fluent Icons, 16px, muted `#989898`, VCenter, **AccessibilityView=Raw** (decorative, not announced).
- `UrSnackbarStyle` (InfoBar): body font, IsClosable=true, corner radius 8.
- `UrCardButtonStyle`/`UrCardRowButtonStyle` (Button): card bg, hover `#242424` + strong border, pressed `#2A2A2A`, disabled opacity 0.38, visual-state transition 150ms; row variant: radius 8, padding 12,8, border 0.
- `AccentButtonStyle` theme overrides: bg `#EFF7BB` (hover `#F5FBD1`, pressed `#D9E3A3`, disabled `#33EFF7BB`), fg `#101010` (disabled `#66101010`), no border.
- `UrSwitchToggleStyle` (ToggleSwitch): OnContent/OffContent empty, MinWidth 0, right-aligned, VCenter; on-state fill is BlueMedium `#638BFC` via ToggleSwitchFillOn* theme keys. Every instance MUST get `AutomationProperties.LabeledBy` = its row's title TextBlock or it is nameless to a screen reader.
- `UrTitleTextStyle` (page header): ABC Gravity Extended 28/36 `#F8F8F8`.
- App-wide breakpoint: `kWideBreakpointDip = 1000.0` (UrComponents.h), compared against window content width in **DIPs**.
- kit::Snackbar: default duration 4000 ms; severity gate — Informational and Success auto-dismiss after the duration, **Warning and Error persist** until the user closes them (bar IsClosable=true); a second Show restarts the timer; explicit duration 0 = persistent.
- Monospace used in tables: `Consolas` (pick the platform mono on Linux).
- Empty vs loading vs failed: never a bare dash for state; each surface below spells out its three-way story explicitly.

---

# 1. SUPPORT destination

## 1.1 Navigation & chrome
- Footer nav item `SupportNavItem`, tag `support`, glyph **E897** (Help), FontIcon 20epx, label `Loc("support")` = "Support". Footer order: Support, [Developer when Advanced], Settings.
- Support is a **card-model page**: unlike the pane-shell destinations it KEEPS the NavigationView header — the destination title "Support" rendered by the header template in `UrTitleTextStyle` (ABC Gravity Extended 28/36), margin 4,4,0,0. (Code comment: "a card page with no title reads as content that started halfway down.")
- Page transition: 250 ms crossfade between the outgoing and incoming page grids (motion `kBaseMs`).
- The whole page lives in a ScrollViewer with **Margin 24** (the card-model page gutter).

## 1.2 Layout (exact)
Outer grid `SupportLayout`, three columns weighted `*` : `1000*` : `*` with the width cap on the middle (`SupportCapColumn`). The greedy middle star always reaches its MaxWidth when there is room, the two hairline stars split the rest — so the column is **centred at wide widths and fills the viewport at narrow ones**, with no extra breakpoint or branch for the centring itself. (Do NOT put MaxWidth on the panel — on WinUI that mis-centres because the arrange offset comes from desired width; on GTK use an equivalent centering container.)

Inside the capped column: a 2-column (star + `SupportSideColumn`) × 2-row (Auto, Auto) grid.

Responsive rule (single gate, `wide = width >= 1000 dip`, applied in `ApplyBreakpoint`):
- **Wide (>= 1000 dip)**: `SupportCapColumn.MaxWidth = 1080`; `SupportSideColumn` = 1*; `SupportSideStack` placed at row 0, column 1, rowSpan 1, margin **20,16-equivalent: exactly `{left 20, top 0, right 0, bottom 24}`** — i.e. form and contact card sit side by side, 20 dip gutter, both top-aligned.
- **Narrow (< 1000)**: `SupportCapColumn.MaxWidth = 560`; `SupportSideColumn` width 0; `SupportSideStack` placed at row 1, column 0, margin `{0, 16, 0, 24}` — contact card stacks under the form with a 16 dip gap.
Rationale fixed in code comments: one form, no data; a feedback box 540 dip across is already generous.

## 1.3 Element order (top to bottom)

**Main stack** (`SupportMainStack`, StackPanel, Spacing 12, VerticalAlignment Top, row 0 col 0):
1. Section header — horizontal StackPanel, Spacing 8:
   - FontIcon glyph **E9CE** (feedback/comment glyph), `UrRowIconStyle` (16px muted, decorative/not announced)
   - `FeedbackHeading` TextBlock, `UrPanelHeadingStyle` (Montreal 18 SemiBold), text `Loc("feedback")` = **"Feedback"**
2. Card (`UrCardStyle`) containing StackPanel Spacing 12:
   a. `SupportIntroText` — `UrSupportingTextStyle` (12 muted, wraps), text `Loc("site_app_support_intro")` = **"Send us your feedback directly, or join the community for direct support."**
   b. Divider (`UrDividerStyle`)
   c. `FeedbackRating` — star RatingControl (5 stars max, clearable — WinUI defaults; nothing is customized), with Caption `Loc("how_are_we_doing")` = **"How are we doing?"** rendered after the stars. No initial Value is set (WinUI default −1 = "unset").
   d. Divider
   e. `FeedbackText` — multi-line TextBox: AcceptsReturn, TextWrapping Wrap, `UrTextInputStyle` (Montreal 16, underline, radius 0), **MinHeight 96**, Header (label above the field) `Loc("anything_else")` = **"Anything else?"**
   f. `FeedbackIncludeLogs` — CheckBox, Content `Loc("feedback_include_logs")` = **"Attach logs to feedback (optional)"** (the content is also its accessible name; it shipped once with NO content — never repeat that)
   g. Divider
   h. `SendFeedbackButton` — Button, HorizontalAlignment **Right**, Click → send flow. Content = horizontal StackPanel Spacing 8: FontIcon glyph **E724** (send) in `UrRowIconStyle` with FontSize **14**, + `SendFeedbackText` TextBlock VCenter, text `Loc("send")` = **"Send"**. Because the content is a panel the button gets an explicit `AutomationProperties.Name = Loc("send")`.
3. `SupportInfo` — InfoBar in `UrSnackbarStyle`, IsOpen false. This is the support snackbar surface; it sits in-flow directly below the card.

**Side stack** (`SupportSideStack`, StackPanel, Spacing 12, VerticalAlignment Top — the "reach a human" card):
1. Section header — horizontal StackPanel Spacing 8: FontIcon glyph **E939** (person/contact), `UrRowIconStyle`; `SupportContactHeading` `UrPanelHeadingStyle`, text `Loc("support")` = **"Support"**.
2. Card (`UrCardStyle`) containing StackPanel Spacing 12:
   a. `SupportContactText` — TextBlock, wrap, filled by `urnw::SetMarkdownLinkText(textBlock, Localized("if_the_problem_persists_contact_us_at_support_ur"), fontSize=14)`. The store English: **"If the problem persists, contact us at [support@ur.io](mailto:support@ur.io) or [join our Discord](https://discord.com/invite/RUNZXMwPRK) for direct support."** SetMarkdownLinkText parses `[label](url)` spans into REAL inline Hyperlink runs (tab-order focusable, open in default browser via the URL — including the `mailto:`), plain runs between them; a malformed URL degrades to plain styled text; sets FontSize 14 and wrap on the block.
   b. Divider
   c. `SupportProtocolLink` — HyperlinkButton, NavigateUri **https://ur.xyz**, FontSize **12**, Padding 0, HorizontalAlignment Left, Content `Loc("learn_more_protocol_page")` = **"Learn more at the protocol page"**.

## 1.4 Behavior (send flow — exact)
Handler: MainWindow `OnSendFeedback` forwards to `SettingsPage::OnSendFeedback` (SettingsPage owns the support surface: its strings, the snackbar, and the log upload).

1. **Session guard first**: if `!Sdk().IsLoggedIn()` → snackbar `Loc("please_login_to_urnetwork")` = "Please login to URnetwork", severity **Warning** (persists), and return. (Historical bug being guarded: no guard + unconditional success once rendered a 401 as "Thanks for the feedback!".)
2. Build `urnet::FeedbackSendArgs`:
   - `star_count = (int64_t) FeedbackRating.Value()` — NOTE: an untouched RatingControl reports −1; that −1 is sent as-is (see FLAGS).
   - If the feedback text is non-empty: `args.needs = FeedbackSendNeeds{ other = text }` (UTF-8 narrowed). Empty text ⇒ `needs` omitted.
   - `attachLogs = FeedbackIncludeLogs.IsChecked() == true` (captured NOW, before the async call).
3. Disable `SendFeedbackButton` (the only in-flight gating; no spinner).
4. `Sdk().api().sendFeedback(args, callback)` — `urnet::Api::sendFeedback(std::optional<FeedbackSendArgs>, SendFeedbackCallback)`. The callback runs on an SDK thread; marshal to UI (dispatcher enqueue, weak window ref, values captured).
5. Success test: `FeedbackSendResult` carries **no error field, only `optional<string> feedback_id`** — so `ok = (no transport error) && result.has_value()`. Never report success unconditionally.
6. On UI thread:
   - Re-enable Send (on EVERY path).
   - **Failure**: snackbar shows the transport error string **verbatim** if non-empty, else `Loc("error_sending_feedback")` = "There was an error sending your feedback. Please try again later.", severity **Error → persists** until dismissed. Nothing is cleared; no upload happens. Also `LogWarn`.
   - **Success**: snackbar `Loc("thanks_for_the_feedback")` = "Thanks for the feedback!", severity **Success → auto-dismisses after 4000 ms**. Clear `FeedbackText` to empty; uncheck `FeedbackIncludeLogs`. The star rating is NOT reset. Then, iff `attachLogs && feedback_id` non-empty → `UploadLogs(feedbackId)`.
7. **UploadLogs(feedbackId)** — the include-logs contract (apple FeedbackView parity): logs go to the server ONLY from here, only when the box was ticked, and only keyed by the **server-issued feedback id** (a client-minted id correlates with nothing). Guard: empty id or `!Sdk().hasDevice()` → log a skip and return. Call `Sdk().device().uploadLogs(feedbackId, cb)` (`urnet::Device::uploadLogs(const std::string& feedback_id, UploadLogsCallback)`, result `UploadLogsResult{ optional<UploadLogsError> error }`). **Failure is silent by design here and only here** (the feedback itself WAS accepted; telling the user their report failed would be false) — failures are logged (`LogWarn`), never surfaced. `uploadLogs` can also throw synchronously (C-call failure): catch and log.

States, distinguishable by construction: idle form / in-flight (Send disabled) / success (green timed snackbar + cleared fields) / failure (red persistent snackbar, fields intact) / signed-out (yellow persistent warning). There is no loading state — the page loads nothing.

Preview harness parity (`--preview-ui=support`): navigating to support in a preview run raises the success snackbar once (`ShowPreviewSnackbar()` = thanks_for_the_feedback, Success) — deliberately the timing-out severity, paired with wallet's persistent-error preview so both snackbar behaviours are reviewable.

## 1.5 Support localization keys (complete)
| Key | English | Where |
|---|---|---|
| `support` | Support | nav item, contact-card heading |
| `feedback` | Feedback | form section heading |
| `site_app_support_intro` | Send us your feedback directly, or join the community for direct support. | intro line |
| `how_are_we_doing` | How are we doing? | rating caption |
| `anything_else` | Anything else? | textbox header |
| `feedback_include_logs` | Attach logs to feedback (optional) | checkbox |
| `send` | Send | button label + a11y name |
| `if_the_problem_persists_contact_us_at_support_ur` | If the problem persists, contact us at [support@ur.io](mailto:support@ur.io) or [join our Discord](https://discord.com/invite/RUNZXMwPRK) for direct support. | contact sentence (markdown links) |
| `learn_more_protocol_page` | Learn more at the protocol page | link → https://ur.xyz |
| `thanks_for_the_feedback` | Thanks for the feedback! | success snackbar (times out) |
| `error_sending_feedback` | There was an error sending your feedback. Please try again later. | failure snackbar fallback (persists) |
| `please_login_to_urnetwork` | Please login to URnetwork | signed-out warning snackbar (persists) |

---

# 2. DEVELOPER destination

## 2.1 Existence, navigation, gating
- **Advanced Mode only.** The nav item is **inserted into / removed from the footer collection** (never Visibility-toggled): on → insert at footer index 1 (order: Support, Developer, Settings); off → remove; if the user is STANDING on developer when the mode turns off, selection is moved to Connect. Item: tag `developer`, glyph **EBE8** (DevTools) 20epx, label `Dev("dev_developer", "Developer")`. Header shows "Developer" (card-model page keeps the display-face page title, like Support).
- `--preview-ui=developer` flips Advanced on for the process (not persisted).
- Page content is **built lazily on first selection** (`EnsureBuilt`), so a user who never opens it pays nothing for its ~40 controls.

## 2.2 Layout (exact)
ScrollViewer: Margin 24, HorizontalScrollBarVisibility Disabled, VerticalScrollBarVisibility Auto. Inside: `DeveloperLayout` grid, columns `*` : `1000*` (cap `DeveloperCapColumn`) : `*`. Inner grid: 2 columns (star + `DeveloperSideColumn`), 4 rows (Auto ×4). Four named hosts the page appends its cards into:
- Row 0, colspan 2: `DeveloperTopStack` — StackPanel Spacing 16 (intro card).
- Row 1, colspan 2: `DeveloperTablesStack` — Spacing 16, Margin 0,16,0,0 (Exits, Destinations, Probe suite — the tables take the FULL composition width at every size).
- Row 2, col 0: `DeveloperMainStack` — Spacing 16, VAlign Top, Margin 0,16,0,24 (Measurements card).
- `DeveloperSideStack` — Spacing 16, VAlign Top (the five override sections).

Responsive rule (same `wide >= 1000` gate):
- **Wide**: `DeveloperCapColumn.MaxWidth = 1800`; side column = 1*; `DeveloperSideStack` at row 2, col 1, margin `{20, 16, 0, 24}` — overrides sit BESIDE measurements ("change a threshold on the right, watch a count on the left").
- **Narrow**: cap MaxWidth = 1000; side column width 0; side stack at row 3, col 0, margin `{0, 16, 0, 24}` — overrides stack below.

## 2.3 Builder kit (exact metrics)
- `MakeCard(heading)`: Border `UrCardStyle`; outer StackPanel Spacing 12; heading TextBlock `UrCardLabelStyle` (PP NeueBit 22 Bold) when non-empty; body StackPanel Spacing 10.
- `MakeSettingRow(label, detail, trailing)`: Grid, columns star + Auto. Left: StackPanel Spacing 2, Margin 0,0,16,0 with title TextBlock 14px `#F8F8F8` wrap and (optional) detail TextBlock 11px faint `#5A5A5A` wrap. Right: the trailing control. The title block is returned so toggles/number boxes can be `LabeledBy` it (a11y).
- `MakeActionButton(text, primary)`: primary → `AccentButtonStyle` (pale-yellow pill); otherwise `UrCardRowButtonStyle` with Foreground = accent `#EFF7BB`. HorizontalAlignment Left. (The 48px URButton pills are the sign-in CTA role — never used here.)
- `MakeTableRow(widths)`: one independent Grid per row; widths spelled star (negative numbers = star weight) or fixed pixels — **never Auto** (Auto would size per-row and misalign columns across rows). Header row and data rows share the SAME width array constant.
- `kActionPadding` = Thickness{8, 6, 8, 6} — compact padding for buttons inside table cells (vs the style's 12,8).
- `ShortId(id)`: exits/destination client ids are ULIDs sharing leading time bytes — render the **last 8 characters** (iOS parity).
- `FormatDurationMillis(ms)`: 0 → `Loc("off")` = "Off"; < 1000 → `"{n}ms"`; < 60000 → whole seconds `"{n}s"`, else one decimal `"{n.1}s"`; ≥ 60000 → `"{n}m"` (integer minutes).
- Blast radius: `"{:.1f}"`.

## 2.4 Card 1 — Intro (host: top)
`MakeCard(Dev("dev_developer","Developer"))`, body order:
1. `Dev("dev_intro", "Tools for diagnosing connection freezes. These act on the live connection.")` — 13px muted, wrap.
2. Version stamp: `Dev("dev_app_version","App version") + " " + version::kString` — 11px faint, wrap ("0.0.0-dev" on non-CI builds; transcripts pasted from this screen should carry it).
3. `connectHint_` — 14px text color, wrap. THREE distinguishable absent-states (set in ApplySettings; a diagnostic screen must not collapse them):
   - no device: `Dev("dev_no_device", "No session. Sign in and connect to use these tools.")`
   - device but service detached (`!remoteConnected`): `Dev("dev_service_detached", "The URnetwork service is not attached, so the live connection cannot be read.")`
   - connected but no override in force (settings == nullopt): `Dev("dev_nothing_in_force", "No reliability override is in force, so the settings sections are hidden rather than shown at zero. The measurements and exit readout below are live.")`
   - settings in force: hint hidden (Collapsed). Visibility rule: visible iff `!inForce`.
4. Actions row — horizontal StackPanel Spacing 8:
   - **Refresh** — `Dev("dev_refresh","Refresh")`, PRIMARY (accent pill). Never gated: it only re-reads, and it is how a user retries after starting the service. Click → `Poll()`.
   - **Simulate network change** — `Dev("dev_simulate_network_change","Simulate network change")`. Starts **disabled**; enabled iff snapshot `haveDevice` (NOT gated on settings-in-force). Click → `RunAction(SimulateNetworkChange, DevW("dev_simulate_network_change",…))`.
   - **Sync** — `Dev("dev_sync","Sync")`, same device gating. Click → `RunAction(Sync, DevW("dev_sync","Sync"))`.
   - **Check for updates** — `Dev("dev_check_updates","Check for updates")`. NEVER gated (unauthenticated HTTP GET; needs no session/device/service; the only way to exercise the check path on a dev build where the periodic checker is disabled, kCode==0). Click → `Updates().CheckNow()`.
5. `lastAction_` — 12px muted wrap, Collapsed when empty. The action report line (`SetLastAction`): text set + shown, cleared → collapsed.
6. `updateCheckText_` — 12px muted wrap, Collapsed until an outcome exists.
7. `Dev("dev_actions_are_requests", "Actions are requests: confirm them in the measurements and exit rows, which refresh underneath.")` — 11px faint, wrap.

**Update-check report line** (`ApplyUpdateCheck(UpdateChecker::Snapshot)`; pushed by MainWindow's fan-out whenever the checker publishes; replayed at the end of Build() so a check that ran before the screen was opened is not lost):
- NeverRan → line stays collapsed.
- InFlight → `Dev("dev_update_checking","Checking for updates…")`
- NoUpdate → `Dev("dev_update_none","No update: this build is current")` + ` (` + kString + `; ` + `Dev("dev_update_newest_seen","newest release seen:")` + ` ` + (`v`+newestVersion if newestCode else `Dev("dev_update_none_seen","none")`) + `)`
- UpdateFound → `Dev("dev_update_found","Update found:")` + ` v` + newestVersion
- DevBuild → `Dev("dev_update_dev_build","Newest release is")` + ` v` + newestVersion + ` — ` + `Dev("dev_update_dev_build_note","this is a dev build (code 0), which never self-updates")`
- Failed → `Dev("dev_update_check_failed","The update check failed — see the app log.")`

## 2.5 Two-gate visibility model
Two INDEPENDENT card groups, because the things behind them fail independently:
- `deviceCards_` (Measurements, Exits, Destinations, Probe suite): visible iff `snapshot.haveDevice`. They need a SESSION and nothing more — they are what a developer opens this screen for during a freeze. (Bug being guarded: gating them on settings hid a perfectly readable exits table whenever getReliabilitySettings returned null, which is the ORDINARY state.)
- `settingsCards_` (the five override sections): visible iff `snapshot.settings.has_value()` ("in force"). **A nil settings read means "nothing is in force", NOT "everything is off"** — rendering a zeroed struct would present defaults as if they were a configuration.

## 2.6 Card 2 — Measurements (host: main; deviceCard)
Heading `Dev("dev_measurements","Measurements")`. Body order:
1. `Dev("dev_measurements_detail","What a provider failure costs. Reset, run a test, read back.")` — 11 faint wrap.
2. **13 metric rows**, each a `MakeSettingRow(label14, detail11-faint, value)` where value = TextBlock 14px text-color, right-aligned, **monospace (Consolas)**, initial "0". Row order IS the apply-index order — never reorder one side without the other:

| # | key | label | detail | value format (ReliabilityMetrics) | visibility |
|---|---|---|---|---|---|
| 0 | dev_flows_opened | Flows opened | Total since reset, so runs of different lengths compare | `{FlowsOpened}` | always |
| 1 | dev_provider_connect_failures | Provider connect failures | Times a provider reported it could not open the upstream connection | `{DialFailuresIntercepted}` | always |
| 2 | dev_moved_to_another_exit | Moved to another exit | How many of those failures were quietly moved instead of hanging | `{FlowsReraced}` | always |
| 3 | dev_probes | Probes | Qualification probes this session | `{ProbesSent} sent / {ProbesAnswered} answered` | always |
| 4 | dev_busy_probes | Busy probes | Liveness pings fired at stalled exits; acquitted ones answered and were kept | `{BusyProbesSent} sent / {BusyProbesAcquitted} acquitted` | always |
| 5 | dev_verdicts_held | Verdicts held | Convictions withheld because this machine's own uplink, not the provider, was silent (uplink / transport) | `{VerdictsHeldUplinkStale} / {VerdictsHeldTransportDown}` | always |
| 6 | dev_removals_deferred | Removals deferred | Removals the storm breaker held back after a correlated burst | `{RemovalsDeferred}` | only if > 0 (zero is noise, row hidden) |
| 7 | dev_suspends_caught | Suspends caught | Host suspends the detector caught, each one a batch of verdicts held instead of executed on a just-resumed machine | `{SchedulerPausesDetected}` | only if > 0 |
| 8 | dev_quic_flows_rebound | QUIC flows rebound | Flows moved to a warm exit inside a removal; accepted means the server took the path change without a re-dial | `{FlowsRebound} ({RebindsAccepted} accepted / {RebindsRedialed} re-dialed)` | only if FlowsRebound > 0 |
| — | dev_no_provider_failures | *No provider failures yet.* | (13px muted wrap line, sits between row 8 and row 9) | — | visible iff ExitLossEvents == 0 |
| 9 | dev_blast_radius | Blast radius | Connections lost per provider failure. Lower is better | `{MeanFlowsLostPerExitLoss:.1f} per failure` | only if ExitLossEvents > 0 |
| 10 | dev_worst_single_failure | Worst single failure | The one the user actually feels | `{MaxFlowsLostInOneEvent} connections` | ditto |
| 11 | dev_recovery_time | Recovery time | From an exit dying to that site answering again | `avg {FormatDurationMillis(RecoveryMeanMillis)}, worst {FormatDurationMillis(RecoveryMaxMillis)}` | ditto |
| 12 | dev_never_came_back | Never came back | Sites abandoned rather than recovered | `{RecoveryMissed} of {FlowsLostToExit}` | ditto |

3. Button `Dev("dev_reset_measurements","Reset measurements")` → `RunAction(ResetMetrics, DevW("dev_reset_measurements",…))`.

ApplyMetrics uses `metrics.value_or({})` — an absent metrics object renders zeros (rows 0–5) with the no-failures line; the "not loaded at all" story is carried by the connectHint/deviceCards gate, not by dashes.

## 2.7 Card 3 — Exits table (host: tables; deviceCard)
Heading `Dev("dev_exits","Exits")`. Column widths (header AND every row share the constant): `kExitColumns = {-2, -2, -1, -1, -2, -2, 270}` — six star columns weighted 2/2/1/1/2/2 plus a FIXED 270 px trailing actions cell (fixed width comes off the top before stars divide; judged at 1100 dip AND maximized).

Header (11px faint): col 0 `Dev("dev_col_exit","Exit")`, 1 `dev_col_window` "Window", 2 `dev_col_tier` "Tier", 3 `dev_col_flows` "Flows", 4 `dev_col_failed_dials` "Failed dials", 5 `dev_col_state` "State", 6 `dev_col_actions` "Actions". Body StackPanel Spacing 6.

**Identity-keyed rebuild** (the load-bearing pattern; applies to all three tables here): row set is rebuilt ONLY when the identity string changes; cell TEXT is written on every poll. Exits identity = concatenation of each exit's ClientId + ";" (exits with **no/empty ClientId are dropped entirely** — no stable identity ⇒ nothing to migrate, nothing to name the row with; never mint a label). Identity is stored as `std::optional<wstring>` — an empty table has empty identity, and with a bare string the FIRST apply of an empty list would compare equal to the initial value and skip the rebuild, so the empty-state row would never be created (that state is reachable: connected, settings in force, no exits yet).
- Empty state row: `Dev("dev_no_exits","No exits. Connect first.")` — 13px muted wrap.

Per-row cells:
| col | content | style |
|---|---|---|
| 0 | ShortId(clientId) (last 8 chars) | 13px, text `#F8F8F8`, monospace |
| 1 | WindowType, or literally `auto` when empty | 12px muted |
| 2 | `{Tier}` normally; `{Tier}→{EffectiveTier}` (U+2192 arrow) when Tier < EffectiveTier (live demotion) | 12px muted |
| 3 | `{FlowCount}` | 12px muted |
| 4 | `{DialFailureCount}` | 12px; foreground **danger `#F8523B` when > 0**, else muted |
| 5 | state parts joined by ` · ` (U+00B7): `benched` (if Quarantined) ELSE (if Warning) WarningCause **verbatim** or `warned` when cause empty; then `done` (Done); `p2p` (P2pOnly); `proven` (Proven — absence means "not yet proven", never "bad") | 12px wrap; foreground danger when Quarantined or Warning else muted (state keys: dev_state_benched/warned/done/p2p/proven) |
| 6 | actions cluster — horizontal StackPanel Spacing 4, VCenter | all buttons `UrCardRowButtonStyle`, Padding {8,6,8,6} |

Actions per row (D6):
- **Migrate** `Dev("dev_migrate","Migrate")`, accent foreground (NOT destructive — it moves flows off an exit). Click → `RunAction(MigrateExit, DevW("dev_migrate_exit","Migrated exit") + " " + ShortId(clientId), clientId)`.
- **Drop** `dev_drop` "Drop", **Stall** `dev_stall` "Stall", **Unstall** `dev_unstall` "Unstall" — foreground **danger `#F8523B`** (destructive, colored as such; NO confirm modal — Advanced Mode's audience acts deliberately, the report line + SdkHost log naming the exit are the record). Click → `RunFaultAction(Drop|Stall|Unstall, clientId)`. Both Stall and Unstall are ALWAYS shown and enabled because `urnet::Exit` carries no stalled flag — the client cannot know which one is meaningful, and a toggle would have to lie about its state.

Below the table body:
- `Dev("dev_exits_actions_detail","Drop, Stall and Shuffle degrade the live connection on purpose, immediately. Nothing here is queued or retried — an action that misses is reported, not replayed.")` — 12px muted wrap.
- Button `Dev("dev_shuffle_exits","Shuffle exit window")` → `RunShuffleExits()` (acts on the whole window, so it lives under the table, not in a row).

## 2.8 Card 4 — Destinations table (host: tables; deviceCard)
Not on iOS (Windows-only readout). Heading `Dev("dev_destinations","Destinations")`. Detail: `Dev("dev_destinations_detail","Which exit each destination's flows are landing on. A site spread over several rows is a site that lost its single egress IP.")` — 11 faint wrap. Columns `{-3, -2, -1}`; header 11 faint: `dev_col_destination` "Destination", `dev_col_exit` "Exit", `dev_col_flows` "Flows". Body spacing 6.
- Identity = DestinationIp sequence (optional-wrapped, same first-empty-apply rule). Empty state: `Dev("dev_no_destinations","No destinations yet.")` 13 muted.
- Cells: destination IP 13px text-color monospace (identity/rebuilt); exit = ShortId(ClientId) 12px muted monospace, or em-dash `—` (U+2014) when ClientId is absent/empty; flows `{FlowCount}` 12px muted. Cells rewritten every poll (this table churns faster than exits; rebuilding per-poll would make it unreadable). No buttons.

## 2.9 Card 5 — Probe suite (host: tables; deviceCard) [D6]
Distinct from "Probe all exits now" (which qualifies EXITS against the embedded health-site list and reports a count): this dials a suite of TARGETS through the live connection and reports a per-stage timing breakdown — what tells "slow" apart from "slow to RESOLVE".

Heading `Dev("dev_probe_suite","Probe suite")`. Body order:
1. `Dev("dev_probe_suite_detail","Dials real targets through the live connection and times each stage. Runs with the SDK's own default configuration.")` — 11 faint wrap.
2. `probeSuiteState_` — 13px wrap: running → `Dev("dev_probe_suite_running","Running.")` in **accent** `#EFF7BB`; not running → `Dev("dev_probe_suite_idle","Not running.")` muted.
3. Controls row Spacing 8: **Start probe suite** (`dev_probe_suite_start`) — enabled iff NOT running → `RunProbeSuite(true)`; **Stop** (`dev_probe_suite_stop`) — enabled iff running → `RunProbeSuite(false)`. (The pair reads as one control; half of two always-live buttons would no-op.)
4. Results table, columns `kProbeColumns = {-3, -1, -1, -1, -1, -1, -2}`; header 11 faint: `dev_col_target` "Target", `dev_col_kind` "Kind", `dev_col_dns` "DNS", `dev_col_connect` "Connect", `dev_col_ttfb` "TTFB", `dev_col_total` "Total", `dev_col_outcome` "Outcome". Body spacing 6.
- Identity = `Name + "/" + Kind` sequence (a target legitimately appears once per probe kind); rebuild only when the TARGET SET changes so a repeating suite rewrites latencies in place. Empty state: `Dev("dev_no_probe_results","No probe results yet. Start the suite.")` 13 muted.
- Cells: target Name 13px text-color monospace; Kind 12 muted; each stage cell (DnsMillis/ConnectMillis/TtfbMillis/TotalMillis): `ms <= 0` → em-dash `—` (a zero stage means "did not happen / not measured" — an HTTP probe handed an address records no DNS time; rendering 0ms claims a measurement never taken), else `{n}ms`; Outcome: Ok → `Dev("dev_probe_ok","ok")` plus ` · {FormatByteCountCompact(ByteCount)}` when ByteCount > 0, muted; not Ok → the SDK `Error` string **verbatim** (new go-side failures render without an app update), or `Dev("dev_probe_failed","failed")` when empty, **danger** color.

`RunProbeSuite(start)` on the bridge: start → `Sdk().StartProbeSuite()` — SdkHost resolves the nullopt config through `urnet::getDefaultProbeSuiteConfig()` (NEVER a default-constructed `ProbeSuiteConfig`, which would be a suite with zero concurrency and zero timeout — same bug class as writing zeroed ReliabilitySettings); stop → `Sdk().StopProbeSuite()`. Then re-read + reapply snapshot, then report: stop → `Dev("dev_probe_suite_stopped","Probe suite stopped")`; start ok → `Dev("dev_probe_suite_started","Probe suite started")`; start declined → `Dev("dev_probe_suite_not_started","Probe suite did not start: no live session, or one is already running.")`. Running state + results come back on the ordinary poll (one consistent read), not from the action.

## 2.10 The five override sections (host: side; settingsCards, hidden unless in-force)
Each is `MakeCard(sectionHeading)`; rows appended in order. Row kinds:
- **boolRow** — `MakeSettingRow` + ToggleSwitch `UrSwitchToggleStyle`, `LabeledBy` the title; Toggled → `OnBoolToggled(index)`.
- **millisRow / countRow** — trailing horizontal StackPanel Spacing 8, VCenter: `effective` TextBlock 12px muted, MinWidth **72**, right-aligned text; then NumberBox Width **120**, Minimum 0, **Maximum = 2147483647 for int32 fields / 1e12 for int64 millis fields** (without a max, a typed value above INT32_MAX wraps NEGATIVE through the cast into the live reliability stack; 1e12 ms ≈ 35 years, past meaning and short of double→int64 UB), SpinButtonPlacement Compact, ValidationMode InvalidInputOverwritten (a rejected edit snaps back to the value in force rather than showing an unapplied number), `LabeledBy` title; ValueChanged → `OnNumChanged(index)`.
- **Effective-value label**: on every apply, `value == 0 && zeroLabel non-empty` → the zeroLabel; else millis → `FormatDurationMillis(value)`; else plain `{value}`.

**Section order in code (Observability LAST):**

### Detection — `Dev("dev_detection","Detection")` (how an exit is judged to be failing, and how fast)
| key | label | type / field (ReliabilitySettings) | zero label | detail |
|---|---|---|---|---|
| dev_drop_stalled_exits_fast | Drop stalled exits fast | millis `SendStallTimeoutMillis` (i64) | (Off via duration fmt) | How long an exit may stop delivering before it is dropped, in ms. Off waits 30s |
| dev_probe_stalled_exits | Probe stalled exits before dropping | bool `BusyProbe` | — | When an exit stalls, ping it once before convicting. A congested but alive exit answers and keeps its flows; a dead one is still dropped |
| dev_busy_probe_wait | Busy probe wait | millis `BusyProbeBudgetMillis` | — | How long the stall probe waits for an answer before convicting, in ms. Off derives half the stall bar |
| dev_suspend_detector | Suspend detector | millis `SchedulerPauseToleranceMillis` | — | How much timer overshoot reads as the machine being suspended rather than an exit stalling, in ms, so a resumed machine does not convict every exit at once. Off disables it |
| dev_suspend_recovery_window | Suspend recovery window | millis `SchedulerPauseRecoveryTimeoutMillis` | — | How long verdicts stay held after a detected suspend, in ms, giving transports time to re-register. Off uses the built-in 5s |
| dev_cut_dead_connects_early | Cut dead connects early | millis `BlackholeConnectComparativeTimeoutMillis` | — | Drop an exit that has established nothing sooner when two sibling exits are receiving, in ms. Off waits the full 30s connect bar |
| dev_keep_quiet_providers_longer | Keep quiet providers longer | millis `BlackholeReceiveTimeoutMillis` | — | How long a provider still acknowledging traffic may return nothing before it is dropped, in ms. Off keeps them until they stop acknowledging |
| dev_demote_before_removing | Demote before removing | bool `SoftVerdictDemote` | — | Ambiguous verdicts bench an exit instead of tearing down its flows; removal needs sustained evidence or an empty exit |

### Placement — `Dev("dev_placement","Placement")` (which exit a flow lands on, and how the pool is shaped)
| key | label | type / field | zero label | detail |
|---|---|---|---|---|
| dev_live_tier_demotion | Live tier demotion | bool `EffectiveTierSelection` | — | Failing dials and survived verdicts push a provider down the ranking within a second; promotion back needs clean minutes and a proven connect |
| dev_max_connections_per_exit | Max connections per exit | count `MaxFlowsPerExit` (i32) | **Unlimited** | Losing an exit kills every connection on it. Lower spreads the damage; a site may then use more than one exit IP |
| dev_sticky_site_affinity | Sticky site affinity | bool `AffinityStickyPastCap` | — | A site's new connections stay on the exit its earlier ones already use, even past the flow cap, so a busy site keeps one egress IP |
| dev_follow_benched_exits | Follow benched exits | bool `QuarantineGroupFollow` | — | A quarantined exit keeps its own sites' new connections through the early bench, when the verdict is least proven. New sites still avoid it |
| dev_follow_window | Follow window | millis `GroupFollowWindowMillis` | — | How long into a bench a site's new connections keep following their exit, in ms. Off scatters immediately |
| dev_removal_storm_limit | Removal storm limit | count `RemovalBudgetCount` (i32) | **Off** | How many verdict removals are allowed per window before the rest are deferred; a burst is more likely one local cause than many failures |
| dev_removal_storm_window | Removal storm window | millis `RemovalBudgetWindowMillis` | — | The window the removal limit is counted over, in ms. Off (like a limit of 0) turns the breaker off |
| dev_keep_a_spare_exit_warm | Keep a spare exit warm | bool `StandingReserve` | — | Size each window one exit beyond its target so a replacement is already connected. Off waits until a loss to backfill |
| dev_load_corroboration | Load corroboration | count `BlackholeLoadCorroboration` (i32) | **Off** | Extra silent destinations required per this many flows before a busy exit can be benched on soft evidence. Off keeps the flat minimum |
| dev_corroborate_silent_exits | Corroborate silent exits | count `MinBlackholeDestinations` (i32) | **Off** | How many distinct destinations must be silent before an exit is convicted on no-receive, so one dead site cannot remove a working exit |
| dev_group_ips_by_site | Group IPs by site | bool `ClusterAffinityFallback` | — | Keeps a site on one exit when its hostname is not visible |
| dev_converge_late_named_flows | Converge late-named flows | bool `ServerNameAffinityBridge` | — | Moves later connections onto the exit the first one already uses |

### Recovery — `Dev("dev_recovery","Recovery")` (getting a flow moving again after its exit fails)
| key | label | type / field | detail |
|---|---|---|---|
| dev_rebind_quic_on_exit_loss | Rebind QUIC on exit loss | bool `QuicRebindOnExitLoss` | Re-pin established QUIC flows to a live exit inside the removal instead of tearing them down |
| dev_retry_refused_connects_elsewhere | Retry refused connects elsewhere | bool `DialFailureRerace` | When a provider can't reach a site, move the connection to another exit instead of letting it hang |
| dev_signal_udp_teardown | Signal UDP teardown | bool `UdpTeardownSignal` | Tells DNS and QUIC the path is gone instead of going silent |
| dev_release_stuck_retransmits | Release stuck retransmits | millis `TcpCollapseMaxHoldMillis` | How long retransmits are held before one is released, in ms. Off waits 30s |
| dev_longer_tcp_idle_timeout | Longer TCP idle timeout | millis `TcpSequenceIdleTimeoutMillis` | How long a TCP connection may sit idle, in ms. Off uses the UDP bound |
| dev_udp_idle_timeout | UDP idle timeout | millis `SequenceIdleTimeoutMillis` | How long a non-TCP flow may sit idle before it is reaped, in ms |
| dev_uplink_silence_gate | Uplink silence gate | millis `UplinkStalenessGateMillis` | How long the whole tunnel may be silent before provider verdicts are held as inadmissible, in ms. 0 convicts as before |
| dev_fast_first_exit_poll | Fast first-exit poll | millis `FormationPollTimeoutMillis` | How often a connecting flow re-checks an empty window, in ms, so the first request leaves right after the first exit lands. Off waits the 2s retry pace |

### Probing — `Dev("dev_probing","Probing")` (proving an exit can actually reach real destinations)
| key | label | type / field | zero label | detail |
|---|---|---|---|---|
| dev_probe_providers | Probe providers | bool `ProviderProbe` | — | Qualify exits by dialing real sites through them. An answer proves the exit; silence never counts against it |
| dev_probe_wait | Probe wait | millis `ProbeTimeoutMillis` | — | How long a qualification probe waits for an answer, in ms. Off uses the built-in 4s. It only bounds waiting for proof, it never convicts |
| dev_probe_hosts_per_pass | Probe hosts per pass | count `ProbeSampleHostCount` (i32) | **All** | How many health sites one qualification pass dials through an exit. 0 probes the entire embedded list; a smaller number rotates through it in blocks |
| dev_probe_silence_streak | Probe silence streak | count `ProbeSilenceWarnStreak` (i32) | **Off** | How many consecutive probe passes an exit may answer with total silence before it is warned out of new-flow placement. Placement only |
| dev_candidates_per_slot | Candidates evaluated per slot | count `EvaluationPoolMultiple` (i32) | **1 (min)** | How many providers a window expansion pings and ranks per slot it needs, keeping the best. 1 evaluates exactly what it needs |
Then a button: `Dev("dev_probe_all_exits_now","Probe all exits now")` → `RunAction(ProbeAllExits, DevW("dev_probed_exits","Probed exits"))` (returns a count).

### Observability — `Dev("dev_observability","Observability")` (what the session writes to the log)
| key | label | type / field | detail |
|---|---|---|---|
| dev_state_heartbeat | State heartbeat | millis `HeartbeatIntervalMillis` | How often one line summarizing live state is written to the log for later forensics, in ms. Off silences it |
Then a button: `Dev("dev_reset_to_shipped_defaults","Reset to shipped defaults")` → `RunAction(ResetSettings, …)`.

**Totals**: 12 toggles + 22 number boxes = 34 override rows; 13 metric rows. (ReliabilitySettings also carries fields the UI does NOT expose: SharedFateMinExits, SharedFateWindowMillis, ScoredPlacement, PlacementHysteresisPct, PlacementDemoteConsecutive, RewardInstrumentation, QuarantineDampening — read-modify-write preserves them untouched.)

## 2.11 Edit semantics (the three shipped-bug invariants)
1. **Read-modify-write of the WHOLE struct from a FRESH read.** An edit never writes from the snapshot on screen (one poll interval stale — would revert concurrent changes). `EditSettings(mutate)` submits to the bridge: `Sdk().UpdateReliabilitySettings(mutate)` which, under SdkHost's lock: `getReliabilitySettings()` fresh → **nullopt ⇒ NO-OP** (log + return nullopt; never substitute a default-constructed struct on the write path) → `mutate(*current)` → `setReliabilitySettings(current)` → return `getReliabilitySettings()` (what the device APPLIED, not what was asked). Then a full `ReadReliability()` snapshot is re-applied to the UI; if the write was a no-op, `lastAction_` = `Dev("dev_not_applied","Not applied: there is no reliability override in force to change.")` — never a silent spring-back.
2. **nil = "nothing in force", never write zeros.** Writing a zeroed struct installs an all-zero override that disables the entire reliability stack, and sync() latches it. (Shipped once.) Display may substitute nothing — the sections HIDE instead.
3. **Echo guard**: `applying_` flag (RAII guard — ~46 property writes sit between set/clear; an exception must not leave it stuck, which would make the surface silently read-only) is true while a snapshot is being written into the controls; `OnBoolToggled`/`OnNumChanged` early-return while it is set. Without it every 5 s refresh would write every value back to the device (~40 rpcs/poll) and race user edits.
4. **Focused NumberBox is never overwritten by the poll** (`FocusState == Unfocused` check) — a 5 s poll must not stomp an in-progress edit; the effective label still updates.
5. `OnNumChanged`: NaN (cleared box) or negative → write nothing. Clamp: i32 fields to 2147483647, i64 millis to 1e12, then cast (a typed 1e30 cast straight to integer is UB, not a big number).

## 2.12 Actions & reporting (RunAction / fault injection)
`RunAction(action, describedText, exitClientId="")` on the bridge: `Sdk().RunReliabilityAction(action, exitClientId)` → fresh `ReadReliability()` → marshal both to UI. Report the OUTCOME after the fact (never "Requested: X" before submission — with no session that rendered a request line under "No session…"):
- `!result.issued` (no device, or rpc threw) → `Dev("dev_not_issued","Not issued: there is no live session to act on.")`
- `result.declined` (**negative return from migrateExit/probeAllExits is the SDK's not-found sentinel, NOT a count** — the first version rendered "affected -1") → `Dev("dev_action_declined","No change: the SDK declined that action.")`
- `result.hasCount` (MigrateExit and ProbeAllExits are int64 on DeviceRemote; the count crosses the ABI; **0 is a real answer** — "nothing needed moving" — the test is `< 0`, never `<= 0`) → `described + ": " + Dev("dev_count_affected","affected") + " " + count`
- else (the four genuinely-void actions ResetMetrics/ResetSettings/SimulateNetworkChange/Sync) → `Dev("dev_requested","Requested:") + " " + described`

SdkHost mapping: ResetMetrics→`resetReliabilityMetrics()`, ResetSettings→`resetReliabilitySettings()`, ProbeAllExits→`probeAllExits()` (int64), SimulateNetworkChange→`simulateNetworkChange()`, Sync→`sync()`, MigrateExit→`migrateExit(clientId)` (int64; empty id = skip).

**Fault injection (D6): immediate-or-nothing.** `RunFaultAction`: empty clientId → return. On the bridge, ONE call under the lock, **no client-side retry, nothing queued into sync state** (a drop replayed after an RPC reconnect hits a different, healthy exit — SDK pins this with TestDeviceRemoteAdvancedModeActionsAreNeverQueued; the client must not reintroduce it), **no confirmation modal** — the report line and the SdkHost log line naming the exit are the record:
- Drop → `Sdk().DropExit(id)` → bool. Report: `Dev("dev_dropped_exit","Dropped exit") + " " + shortId`.
- Stall → `Sdk().StallExit(id, true)`. Report: `Dev("dev_stalled_exit","Stalled exit") + " " + shortId`.
- Unstall → `Sdk().StallExit(id, false)`. Report: `Dev("dev_unstalled_exit","Cleared stall on exit") + " " + shortId`.
- Declined/false → `Dev("dev_declined_exit","No change: the SDK declined that exit") + " " + shortId` ("the SDK did it" is separated from "the SDK declined" — an action aimed at an exit that left the window says so instead of claiming success).
- Shuffle (`RunShuffleExits`): `Sdk().ShuffleExits()` (void on both Device forms, so "requested" is the ceiling) → report `Dev("dev_requested","Requested:") + " " + Dev("dev_shuffle_exits","Shuffle exit window")`; the exits table below is where the reshuffle is visible.
Every action path re-reads the snapshot and reapplies it before reporting.

## 2.13 Polling (exact cadence and gates)
- Interval: **5000 ms** (`kPollInterval`, iOS ReliabilityStore.pollInterval parity), dispatcher-queue timer on the UI thread whose tick submits work to the bridge.
- Gate (all four ANDed, reconciled on every change): `advancedMode_ && selected_ && presentationActive_ && built_`.
  - `selected_`: this destination is the selected nav item (`SetSelected` from OnNavSelectionChanged).
  - `presentationActive_`: window visible AND not minimized (AppController presentation lifecycle; focus loss deliberately does NOT stop it).
  - `advancedMode_`: seeded false; MainWindow::ApplyAdvancedMode pushes the real value before anything can poll. This screen has no Normal reading — what the mode buys here is that the poll (4+ synchronous rpcs into the service per tick) stops when the destination is unreachable.
- On gate-on: start timer AND run one immediate poll; on gate-off: stop timer. Log start/stop.
- **Coalescing**: `pollPending_` atomic CAS — one poll queued-or-running at a time; a read slower than the interval must not stack more behind SdkHost's lock. Cleared via RAII on EVERY path (a stuck flag wedges both the timer and the Refresh button for the process lifetime, and the screen looks merely stale rather than broken).
- The poll body runs on the bridge: `ReadReliability()` = ONE SdkHost lock hold, seven rpcs, so the parts cannot disagree about which session they describe: `device_->getRemoteConnected()`, `getReliabilitySettings()`, `getReliabilityMetrics()`, `getExits()`, `getDestinationExits()`, `probeSuiteRunning()`, `getProbeResults()` (list getters null-guarded — a suite that never ran returns a nil slice that must not throw on unwrap). Snapshot value-captured and marshaled to the UI → `ApplySnapshot` → ApplySettings / ApplyMetrics / ApplyExits / ApplyProbeSuite.

## 2.14 The serial FIFO bridge (port this thread model)
ONE serial worker thread carries EVERY rpc this screen makes (`Submit(job)` → deque + condvar; iOS ReliabilityStore.bridgeQueue port). Load-bearing three ways — a detached-thread-per-call design got all three wrong:
1. **ORDER**: every settings edit writes an ABSOLUTE value from a read-modify-write; two in-flight edits are serialised by SdkHost's lock but not ORDERED — FIFO on one thread fixes the device holding the older value while the screen shows the newer.
2. **STALE PAINT**: a poll that read before an edit committed must not be delivered to the UI after the edit's own snapshot; one queue delivers results in production order.
3. **LIFETIME**: destructor sets stop, **clears queued jobs**, notifies, and **joins** — after the dtor returns no job can be inside SdkHost (the TOCTOU at tray-quit landed inside a half-destroyed AppController before this). Honest cost: the join is not bounded by one rpc (a job can block on SdkHost::mutex_ held across BootstrapSession), so a tray-quit against a hung service can freeze seconds — still the right trade vs use-after-free; in practice the bridge is idle at quit because polling stops when not presenting.
- Nothing may escape a job: catch std::exception AND catch-all (winrt::hresult_error does not derive from std::exception; an uncaught exception on the thread is std::terminate).
- Edits and actions are never coalesced; only polls are.

## 2.15 The persistent session-mode notice (window-level, driven from this page)
- `ModeNoticeBar`: window-level InfoBar (root grid row 2, visible from EVERY destination — an rpc-only session carries no traffic whichever screen is showing). Driven by `DeveloperPage::OnModeNotice` via `SdkHost::SetModeNoticeObserver` (bound in the ctor; a SEPARATE observer slot from MainWindow's handler — one slot caused the two consumers to silently overwrite each other).
- Threading: the SDK-side handler may fire with SdkHost's mutex held — it must do exactly one thing: enqueue to the UI thread and return.
- Inactive → `IsOpen(false)` AND `Visibility(Collapsed)` explicitly (a closed InfoBar still measures its margins; the row must contribute 0px).
- Active → Visible, **Title empty** (the message is a complete sentence; a title would say it twice), Message = notice.message **verbatim**, Severity = Error for Kind::SessionFailed else Warning (rpc-only), **IsClosable(false) — never dismissible**: a standing property of the session, not an event; it stays until replaced.
- Ctor also submits `Sdk().RefreshModeNotice()` **on the bridge** (never inline on the UI thread: it takes SdkHost::mutex_, which bootstrap holds across several service rpcs — calling it directly stalls first window show).
- Preview: `ShowPreviewModeNotice()` raises a synthetic rpc-only notice (English literal in code, not localized) and latches `previewNotice_` so the ctor's async refresh (which lands ~20 ms later and would correctly clear it in a real run) cannot.

## 2.16 Preview snapshot (`--preview-ui=developer`)
`ShowPreviewSnapshot()` builds the page and applies one synthetic `ReliabilitySnapshot` (haveDevice, remoteConnected, settings exercising 900ms/1.5s/5s/2m durations and the zero-labels Unlimited/All/1 (min), metrics unlocking every conditional row, three exits covering healthy / demoted+warned(probe_silence) / benched+p2p+done, three destinations incl. a no-client-id em-dash row and an IPv6, probe results covering ok+bytes / dns-only em-dashes / verbatim error, suite running) then latches `previewData_` so the real (empty) async poll cannot wipe it; also sets lastAction to `"Probed exits: affected 3"`. Writes nothing (no device ⇒ UpdateReliabilitySettings no-ops).

## 2.17 Empty vs loading vs failed (Developer, summary)
- No device (signed out / no session): only the intro card visible; hint = dev_no_device; Simulate/Sync disabled; Refresh + Check-updates live.
- Device but service detached: device cards visible, hint = dev_service_detached.
- Connected, nothing in force: device cards live (measurements/exits/destinations/probe suite), override sections hidden, hint = dev_nothing_in_force.
- In force: everything visible, hint hidden.
- Per-table empties: dev_no_exits / dev_no_destinations / dev_no_probe_results — real sentences, never dashes.
- Failures: rpc failures are logged once (per-getter guards) and surface as the absence states above plus lastAction report lines (dev_not_issued / dev_not_applied / declined variants). The screen never fakes success.

## 2.18 A11y (Developer)
- Every ToggleSwitch and NumberBox `LabeledBy` its row title (unnamed switches are the stated failure mode of `UrSwitchToggleStyle`).
- Action buttons carry their text content as their name (plain text content — no explicit name needed).
- Decorative glyphs are AccessibilityView=Raw via `UrRowIconStyle`.

---

# 3. SDK / host surface used (complete)
See the `sdk_calls` output field for the flat list. Notable shapes:
- `FeedbackSendArgs { optional<FeedbackSendNeeds> needs; int64 star_count }`, `FeedbackSendNeeds { string other }`, `FeedbackSendResult { optional<string> feedback_id }` (no error field).
- `UploadLogsResult { optional<UploadLogsError> error }`.
- `ReliabilitySnapshot { bool haveDevice; bool remoteConnected; optional<ReliabilitySettings> settings; optional<ReliabilityMetrics> metrics; vector<Exit> exits; vector<DestinationExit> destinationExits; bool probeSuiteRunning; vector<ProbeResult> probeResults }`.
- `Exit { optional<string> ClientId; string WindowType; bool Warning, Quarantined; string WarningCause; bool Done, P2pOnly; int32 FlowCount, DialFailureCount, Tier, EffectiveTier; bool Proven; int64 ProbeAgeSeconds }`.
- `DestinationExit { string DestinationIp; optional<string> ClientId; int32 FlowCount }`.
- `ProbeResult { string Name, Kind; bool Ok; string Error; int64 DnsMillis, ConnectMillis, TtfbMillis, TotalMillis, ByteCount, StartOffsetMillis }`.
- `ProbeSuiteConfig { int32 Concurrency; int64 TimeoutMillis; int32 RepeatCount; bool IncludeDns, IncludeHttp, IncludeDownload; int64 DownloadByteCount }`.
- The SDK also has `FeedbackViewController::sendFeedback(msg, star_count)` — the Windows client does NOT use it; it uses `Api::sendFeedback`. Match Windows.


## SDK surface referenced
- urnet::Api::sendFeedback(std::optional<FeedbackSendArgs>, SendFeedbackCallback) -> FeedbackSendResult{feedback_id} (via Sdk().api())
- urnet::Device/DeviceRemote::uploadLogs(feedback_id, UploadLogsCallback) -> UploadLogsResult (via Sdk().device(); only after server accepts feedback, keyed by server feedback_id)
- urnet::DeviceRemote::getRemoteConnected() -> bool
- urnet::DeviceRemote::getReliabilitySettings() -> optional<ReliabilitySettings> (nullopt = nothing in force)
- urnet::DeviceRemote::setReliabilitySettings(settings) (whole struct, from a fresh read only)
- urnet::DeviceRemote::getReliabilityMetrics() -> optional<ReliabilityMetrics>
- urnet::DeviceRemote::getExits() -> optional<ExitList>
- urnet::DeviceRemote::getDestinationExits() -> optional<DestinationExitList>
- urnet::DeviceRemote::resetReliabilityMetrics()
- urnet::DeviceRemote::resetReliabilitySettings()
- urnet::DeviceRemote::probeAllExits() -> int64 (negative = declined sentinel)
- urnet::DeviceRemote::simulateNetworkChange()
- urnet::DeviceRemote::sync()
- urnet::DeviceRemote::migrateExit(client_id) -> int64 (flows moved; negative = not-found sentinel, 0 is a real answer)
- urnet::DeviceRemote::dropExit(client_id) -> bool
- urnet::DeviceRemote::stallExit(client_id, stalled) -> bool
- urnet::DeviceRemote::shuffleExits()
- urnet::DeviceRemote::startProbeSuite(optional<ProbeSuiteConfig>) -> bool
- urnet::DeviceRemote::stopProbeSuite()
- urnet::DeviceRemote::probeSuiteRunning() -> bool
- urnet::DeviceRemote::getProbeResults() -> optional<ProbeResultList>
- urnet::getDefaultProbeSuiteConfig() -> optional<ProbeSuiteConfig> (used instead of a zeroed struct when starting with no explicit config)
- SdkHost wrappers: Sdk().IsLoggedIn(), Sdk().hasDevice(), Sdk().api(), Sdk().device(), Sdk().ReadReliability(), Sdk().UpdateReliabilitySettings(mutate), Sdk().RunReliabilityAction(action, exitClientId), Sdk().DropExit(), Sdk().StallExit(), Sdk().ShuffleExits(), Sdk().StartProbeSuite(), Sdk().StopProbeSuite(), Sdk().ProbeSuiteRunning(), Sdk().RefreshModeNotice(), Sdk().SetModeNoticeObserver(handler)
- urnw::pages::Updates().CheckNow() and Updates().Current() (UpdateChecker; unauthenticated HTTP GET, no session/device needed)
- NOT used (exists in SDK): urnet::FeedbackViewController::sendFeedback(msg, star_count) - Windows uses Api::sendFeedback instead

## Flags (doc-vs-code drift / risks)
- Doc §7.13 says the Exits table's 7th column is a 'per-row Migrate button' — the code (post-D6) puts an Actions CLUSTER of four buttons (Migrate + Drop + Stall + Unstall) in a fixed 270px column, Drop/Stall/Unstall in danger color. Code wins.
- Doc §7.13 lists the probe result table as 6 columns (kind, dns, connect, ttfb, total, outcome) — the code table has SEVEN columns; column 0 is Target (dev_col_target), the probe target name in monospace. Code wins.
- Doc §7.13 calls 'Check for updates now, Simulate network change, Sync' the 'session-less actions' — in code only Check for updates (and Refresh, which the doc omits entirely) are ungated; Simulate network change and Sync are DISABLED unless the snapshot has a device (snap.haveDevice). Code wins.
- Doc §7.13 orders the override sections 'detection/observability/placement/recovery/probing' — the code builds Detection, Placement, Recovery, Probing, Observability, with Observability LAST and carrying the 'Reset to shipped defaults' button. Code wins.
- Doc §7.13's heading says '~50 controls'; the precise code inventory is 12 toggles + 22 number boxes = 34 override rows, plus 13 metric rows, 3 tables, and ~14 action buttons. Use the code counts.
- Doc §7.13 mentions identity-keyed rebuild only for the Exits table — the code applies the same identity/cells split to the Destinations table (identity = DestinationIp sequence) AND the probe-results table (identity = Name+'/'+Kind sequence). All three must be ported, each with the optional-identity first-empty-apply rule.
- Doc §7.12 omits: (a) the signed-out guard — Send with no session shows please_login_to_urnetwork as a persistent Warning snackbar and sends nothing; (b) the contact card's protocol link row (learn_more_protocol_page → https://ur.xyz); (c) that the failure snackbar shows the transport error string VERBATIM when non-empty — error_sending_feedback is only the fallback. Code wins on all three.
- RISK/OPEN QUESTION: FeedbackRating (WinUI RatingControl) has no initial Value; its default is -1 ('unset'), and OnSendFeedback sends star_count = -1 as-is when the user never rated. Neither doc nor code comments acknowledge this. Near-perfect parity means replicating -1; flag to the owner whether the GTK port should clamp instead.
- On a successful send only the text box and the include-logs checkbox are cleared — the star rating is deliberately (or at least actually) NOT reset. Replicate.
- The Support wide/narrow behaviour is ONE gate (kWideBreakpointDip = 1000 dip on window content width): it simultaneously switches the cap (560↔1080), collapses/expands the side column, and re-places the contact stack. There is no separate stacking breakpoint. The centring itself (1:1000*:1 star columns with MaxWidth on the middle) is breakpoint-free.
- None of the dev_* strings exist in the localization store (916-key audit; confirmed none used by DeveloperPage) — every Developer string ships as Dev(key, english) with the English literal as the real string. The port must carry the identical key+fallback table (this spec's tables are the authoritative list) or add the keys to the store.
- ReliabilitySettings carries 7 fields the UI never exposes (SharedFateMinExits, SharedFateWindowMillis, ScoredPlacement, PlacementHysteresisPct, PlacementDemoteConsecutive, RewardInstrumentation, QuarantineDampening) — the whole-struct read-modify-write MUST round-trip them untouched; a port that serializes only the 34 exposed fields would zero the rest, which is exactly the shipped-once bug.
- Support layout margins: the XAML default for SupportSideStack is Margin 0,16,0,0 but ApplyBreakpoint immediately overwrites it ({20,0,0,24} wide / {0,16,0,24} narrow) — implement the code-applied values, not the markup default.
- The mode-notice preview message and the two log-only diagnostics are English literals in code (not localized) — acceptable to keep as literals in the port.
- DeveloperPage dtor join cost: against a hung service a tray-quit can block for seconds because a bridge job can be waiting on SdkHost's lock across BootstrapSession's rpcs. Accepted trade on Windows (vs use-after-free); the port should keep the join and the queue-clear, and keep polling gated off when not presenting so the bridge is idle at quit.
