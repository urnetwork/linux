# URnetwork — THE NETWORK DESTINATION (provider locations browser)
## GTK4/C++ port spec, extracted from the WinUI 3 client (code is authoritative)

Sources read (Windows client, all paths under `urnetwork-windows/app/src/App/` unless noted):
`MainWindow.xaml` (lines 1445–1533: NetworkView shell; 647–651: nav item), `MainWindow.xaml.cpp`
(ctor wiring 177–215, presentation 383–392, breakpoint 463–560, nav select 1268–1310, preview
1104–1220, `ShowBlockedLocationsFromNetwork` 1117), `LocationSheets.h/.cpp` (in full —
`LocationChooserSheet` + `NetworkPage`), `SettingsSheets.h/.cpp` (`rows::` kit + `BlockedLocationsSheet`),
`SettingsPage.cpp` (sheet opener), `ConnectPage.cpp` (chooser sheet open/feed wiring), `SdkHost.h/.cpp`
(feeds, coalescer, selection predicates), `UrComponents.h/.cpp` (pane kit builders), `UrColors.h`,
`App.xaml` (UrPane* styles), `PageContext.h` (`pages::Loc/Adv/Sdk`), `Strings/en/Resources.resw`,
and the Go SDK `sdk/locations_view_controller.go` + generated wrapper `sdk/cgo/include/urnetwork_sdk.hpp`.

---

## 0. Design tokens used by this surface (exact values)

Colors (`UrColors.h`; ARGB given as #RRGGBB unless alpha noted):
- `kBackground` **#101010** — pane body (`UrPaneStyle` Grid background).
- `kSheet` **#151515** — pane/group header strips, every ContentDialog background.
- `kCard` **#1C1C1C** — row hover fill; `kCardPressed` **#2A2A2A** — row pressed fill; `kCardHover` **#242424** (card-model hover, used by `UrCardRowButtonStyle` in the blocked sheet).
- `kBorder` — **white @ 12% alpha (#1FFFFFFF)** — every hairline and the inter-pane vertical rule. Strong border #38FFFFFF (card-row hover edge).
- `kText` = `kOffWhite` **#F8F8F8**; `kTextMuted` **#989898**; `kTextFaint` **#5A5A5A**; `kDanger` **#F8523B**.
- `kToggleAccent` **#638BFC** — the selection CHECK glyph color.
- `kUrGreen` **#87FB67** — strong-privacy lock glyph and peer "providing" globe glyph.
- `kUrCoral` **#FF6C58** — the "Best available provider" row's dot (hardcoded, mobile parity).
- `kUnstable` (file-local in LocationSheets.cpp) **#F5C242** (== `kUrAmber`) — unstable warning glyph + detail-pane unstable sentence. Deliberately NOT brand yellow and NOT danger red.
- Location dots: `urnet::getColorHex(code)` (see §9). Fallback when no id at all: `kTextMuted` #989898.

Type: everything on this surface is the body face **PP Neue Montreal** (`UrBodyFontFamily`);
glyphs are **Segoe Fluent Icons** (`FontIcon` default / `UrIconFontFamily`) — the GTK port needs an
equivalent icon strategy (symbolic icons or the shipped glyph font). Sizes are given per element below.

Pane metrics (App.xaml, one source): `UrPaneHeaderHeight` **40**, `UrGroupHeaderHeight` **28**,
`UrPaneRowHeight` **40**, `UrPaneListRowHeight` **36**, `UrPaneRowTallHeight` **44**,
key-value row **34** (MakePaneKeyValueRow default). Row inset **12px horizontal, 0 vertical**.
All hairlines 1px `kBorder`. Nothing in a pane has a corner radius, margin, or shadow.
Row hover = background fill step to #1C1C1C, pressed #2A2A2A, disabled opacity 0.38; visual-state
transition duration **150ms** (`GeneratedDuration 0:0:0.15`). Page swap into/out of this destination:
crossfade (`motion::CrossfadePageSwap`, kBaseMs=250ms tier).

Breakpoints: `kWideBreakpointDip = 1000.0`, `kUltraWideDip = 1800.0` (UrComponents.h). Measured in
**DIPs of the content root's ActualWidth**, never physical px. Re-evaluated on every SizeChanged of
the window content root; no-op unless the wide/ultra booleans change (or `force`).

---

## 1. Where the destination lives

- Nav: left `NavigationView`, item `NetworkNavItem`, `Tag="network"`, icon Segoe Fluent **U+E774 (Globe), 20epx**, label `Loc("network")` = **"Network"**. Item min height 44.
- The pane shell suppresses the big page header: for tags `connect|network|wallet|leaderboard|account|settings` the NavigationView header is null (panes reach the ceiling). 
- Selecting the destination calls `network_->SetSelected(tag == "network")` after the crossfade.
- The window-level presentation gate: `WindowPresentationShouldRun(shown, minimized) = shown && !minimized` (AppController.cpp:38). On every transition `MainWindow::SetPresentationActive(active)` forwards to `network_->SetPresentationActive(active)` AND `SdkHost::SetPresentationActive(active)` (which owns opening/closing the SDK view controllers; it is non-blocking — records desired state, a worker applies it).

---

## 2. NetworkView pane shell (XAML structure, exact)

`Grid x:Name="NetworkView"`, initially `Visibility=Collapsed`, style `UrPaneStyle` (background #101010).
Three columns:

| Col | Name | Width |
|---|---|---|
| 0 | `NetworkPaneAColumn` | `*` (the LIST takes the star — inverse of Home) |
| 1 | (rule host) | `Auto` |
| 2 | `NetworkPaneBColumn` | `400` |

Responsive (MainWindow::ApplyBreakpoint, lines 547–560): when `width >= 1000` dip →
PaneB column width **400**, `NetworkPaneBRule` and `NetworkPaneB` Visible. When `< 1000` →
PaneB column width **0**, rule and pane Collapsed. Rationale comment: 400 not Home's 380 because the
longest row is a country name beside a provider count; below the breakpoint nothing is unreachable —
clicking a row still connects.

`NetworkPaneBRule`: `Border` with `UrPaneVRuleStyle` = Width 1, Background `kBorder`, VerticalAlignment Stretch.

### Pane A (`NetworkPaneA`, Grid.Column 0, a11y LandmarkType **Main**)
Rows: Auto / Auto / *.
1. **Header strip** — `Border` `UrPaneHeaderStyle`: Height **40**, Background **#151515**, bottom hairline 1px `kBorder`, Padding **12,0**. Inside, one Grid holding:
   - `NetworkPaneATitle` (`UrPaneTitleStyle`: Montreal **12px SemiBold, CharacterSpacing 60** (≈0.06em), color #F8F8F8, VCenter, CharacterEllipsis). Text = `Loc("available_providers")` = **"Available providers"**.
   - `NetworkPaneAMeta` (`UrPaneMetaStyle`: **11px** #989898, VCenter, NoWrap, ellipsis), HorizontalAlignment Right. Text = **total row count** as plain integer, or empty when total ≤ 0.
2. **`NetworkSearchHost`** (empty Grid; NetworkPage::Build injects the search row — §3).
3. **List area** (star row): a Grid overlaying
   - `ScrollViewer` (HScroll Disabled / VScroll Auto) containing `StackPanel NetworkListHost` (rows are appended here), and
   - `Grid NetworkListEmptyHost` with `IsHitTestVisible=False` — the full-height centered empty-state overlay (§5.4).

### Pane B (`NetworkPaneB`, Grid.Column 2)
Rows: Auto / *.
1. Header strip, same `UrPaneHeaderStyle` construction: `NetworkPaneBTitle` (`UrPaneTitleStyle`) = `Loc("selected_provider")` = **"Selected provider"**; `NetworkPaneBMeta` (`UrPaneMetaStyle`, right) = the selected location's display name, or "Best available provider".
2. `ScrollViewer` (HScroll Disabled/VScroll Auto) with `StackPanel NetworkDetailHost` (§6).

A11y: `AutomationProperties.Name` on `NetworkPaneA` = "Available providers", on `NetworkPaneB` = "Selected provider" (set in `ApplyStrings`; without them a screen reader announces two unnamed regions).

---

## 3. Pane A search row (`kit::MakePaneSearchRow`)

Built ONCE in `NetworkPage::Build()` (idempotent via `built_`), appended into `NetworkSearchHost`:
- Root: `MakePaneRow(UrPaneHeaderHeight=40)` = Border **fixed Height 40** (Height, not MinHeight — rows must never grow), Padding 12,0, bottom hairline 1px `kBorder`; background overridden to **#101010** (`BackgroundBrush`) — the search row sits on the page surface, not the header sheet surface.
- Inside: Grid, ColumnSpacing **8**; col0 Auto = FontIcon **U+E721 (Search)**, FontSize **13**, color #989898, VCenter, a11y Raw; col1 Star = `TextBox` with `UrPaneSearchStyle` (Montreal **13px**, CornerRadius 0, BorderThickness 0, Background Transparent, Padding 0, MinHeight 0, Stretch, VCenter) — squared off to the pane row metrics; the hairline comes from the host Border.
- PlaceholderText = `Loc("search_providers_input_placeholder")` = **"Search countries, states, cities..."**. `AutomationProperties.Name` = `Loc("search_providers_input_label")` = **"Search providers"** (a placeholder is NOT an accessible name).

**Behavior on TextChanged** (every keystroke, NO app-side timer debounce):
1. `query_ = Narrow(text)` (UTF-16 → UTF-8).
2. `Sdk().SetLocationFilter(query_)` — the SDK owns the search (see §7.3: device path proxies to `LocationsViewController::filterLocations`; no-device path runs the identical two-endpoint dispatch against the in-process Api). No app-side filtering, so page and modal sheet can never disagree about a query.
3. `Render()` immediately — the idle/searching branch (best-available row vs Top Matches) is app-side and must flip without waiting for a push (a no-op filter change produces none).

Staleness protection is by **sequence number** in the SDK (`FilterLocations` per-query `filterSequenceNumber`; late answers to superseded queries are dropped) and by **generation counter** on the app's Api fallback path — there is no timer anywhere.

---

## 4. The ONE row species (`NetworkPage::MakeRow`) — every list row in Pane A is exactly this

`Button`, style `UrPaneRowButtonStyle` (transparent bg, #F8F8F8 fg, bottom hairline 1px `kBorder`, CornerRadius 0, Padding **12,0**, stretch, content VCenter, system focus visuals; hover fill #1C1C1C, pressed #2A2A2A, disabled opacity 0.38, 150ms state transitions), then **Height AND MinHeight forced to 36** (overriding the style's MinHeight 40 — "Home's list rows are 36 and so are these").

Content Grid, ColumnSpacing **10**, four columns **Auto / Star / Auto / Auto**, in order:
1. **Dot** — Ellipse **8×8**, VCenter, Fill = row color (see per-row rules below). A11y Raw.
2. **Name** — TextBlock **13px** (constructed by MakeName: NoWrap + CharacterEllipsis, color #F8F8F8, FontSize overridden from 15 to 13), VCenter. A11y Raw.
3. **State glyphs** — horizontal StackPanel, Spacing **6**, VCenter; appended in this exact order, each a FontIcon **14px**:
   - providing → **U+E774 Globe**, #87FB67 (peers only)
   - unstable → **U+E7BA Warning**, #F5C242 (locations with `stable == false`)
   - strongPrivacy → **U+E72E Lock**, #87FB67
   - selected → **U+E73E CheckMark**, #638BFC
   A11y Raw (whole panel).
4. **Figure** — TextBlock **12px** #989898, NoWrap, VCenter, right column (the provider count / peer device spec).

Accessibility (the Button's Content is a Panel ⇒ NO automatic name; everything inside is Raw, so the Button carries the whole row): `AutomationProperties.Name` = `"{title}"` + `", {meta}"` (if meta non-empty) + `", " + Loc("unstable_providers_warning")` (if unstable) + `", " + Loc("strong_anonymization")` (if strongPrivacy) + `", " + Loc("network_peers")` (if providing). If selected, `FullDescription` = `Loc("selected_provider")`.

Group headers (`NetworkPage::AppendGroup` → `kit::MakePaneGroupHeader`): Border `UrGroupHeaderStyle` = Height **28**, Background **#151515**, hairlines **top AND bottom** 1px `kBorder`, Padding 12,0. Grid ColumnSpacing 8, cols Star/Auto/Auto: title TextBlock `UrGroupHeaderTextStyle` (**11px, CharacterSpacing 90**, #989898, VCenter, ellipsis) with a11y **HeadingLevel 3**; meta TextBlock `UrPaneMetaStyle` (11px #989898, right) = the bucket count as integer, **collapsed when count ≤ 0**; trailing action Grid (unused here).

---

## 5. Pane A content — exact element order (`NetworkPage::Render`)

Clear `NetworkListHost`; read `selected = Sdk().SelectedLocation()`, `searching = !query_.empty()`, `total = 0`.

**Group 1 — Network peers** (only when `peers_` non-empty; count = peers_->size(), added to total):
- Header: `Loc("network_peers")` = **"Network peers"** + count meta.
- One MakeRow per peer, in feed order: title = `PeerDisplayName(peer)` (**DeviceName, else DeviceSpec, else ClientId** — shared helper, exported), meta = `peer.DeviceSpec`, dot = `getColorHex(peer.ClientId or "")`, selected = `IsPeerSelected` (client_id equality vs current selection), unstable=false, strongPrivacy=false, **providing=true**.
- Click: build `urnet::ConnectLocation{ connect_location_id.client_id = peer.ClientId, name = PeerDisplayName(peer) }` → `Sdk().ConnectFromRow(location)` → `Render()`.

**Group 2 — idle vs searching:**
- *Idle* (`query_` empty): header `Loc("promoted_locations")` = **"Promoted Locations"** with **count 0 ⇒ no meta shown**; then ONE MakeRow: title `Loc("best_available_provider")` = **"Best available provider"**, meta empty, dot **#FF6C58 (kUrCoral, hardcoded)**, selected = `IsBestAvailableSelected(selected)`, no glyph flags. Click → `Sdk().ConnectBestAvailableFromRow()` → `Render()`. total += 1. (Both mobile apps ignore the SDK's `Promoted` bucket; the header is just a label over the synthetic row.)
- *Searching*: `AppendLocationSection(Loc("top_matches") /*"Top Matches"*/, locations_->BestMatches, selected, total)` (self-hides if bucket empty).

**Groups 3–6 — the SDK's own buckets, in the SDK's own order** (each via `AppendLocationSection`, which self-hides on empty, adds header with count, adds count to total):
- `Loc("countries")` **"Countries"** ← `locations_->Countries`
- `Loc("regions")` **"Regions"** ← `locations_->Regions` (SDK populates only while searching)
- `Loc("cities")` **"Cities"** ← `locations_->Cities` (ditto)
- `Loc("devices")` **"Devices"** ← `locations_->Devices`

Each location row: title = `location.name` (or ""), meta = `Plural("provider_count", n)` when `provider_count > 0` else empty (`provider_count.one` = "{} provider", `provider_count.other` = "{} providers" — CLDR plural from the store, never inflect in code), dot = `LocationColor(location)` (§9), selected = `IsLocationSelected(selected, location)`, unstable = `!location.stable`, strongPrivacy = `location.strong_privacy`, providing = false. Click: `Sdk().ConnectFromRow(copy)` → `Render()`. Comment-level intent: **the click is select AND connect** — deliberately no separate "highlight" concept; the detail pane shows the SELECTED provider, not a hover state.

`bucketRows` = Countries+Regions+Cities+Devices counts (+ BestMatches when searching).

### 5.1 Click-to-connect semantics (SdkHost row-click coalescer — MUST be reproduced)
`ConnectFromRow` / `ConnectBestAvailableFromRow` (SdkHost.cpp 3804–3882):
- **Idempotence**: if `connectVc_` exists AND `getConnectionStatus()` ∈ {`CONNECTED`,`CONNECTING`,`DESTINATION_SET`} AND the clicked target matches `getSelectedLocation()` (via `IsLocationSelected` / `IsBestAvailableSelected`), the click is a **no-op** except it **cancels any newer pending row intent** ("click away, regret, click back" ends with zero rebuilds). After a Disconnect the selection survives only as a preference — re-clicking it then IS a real connect.
- **Coalescing**: otherwise a `SessionRequest{kind=Location|BestAvailable, coalesced=true, notBefore = now + kRowClickSettle}` is posted, **`kRowClickSettle = 1200ms`**. The session worker sleeps on a condition variable until `notBefore`; a later row click replaces the slot and restarts the clock; an immediate request (Connect button, tray toggle, Disconnect) has an epoch deadline and supersedes instantly; a bare "ensure session" (kind None — resume path, watchdog) does NOT replace a pending connect. There is deliberately **no UI timer** — the settle lives in the session worker so it behaves identically from every surface/thread.
- Rationale to preserve in comments: every real connect tears down the provider window and the SDK's dial staircase charges 100ms–1s of shared budget per cold dial with no refund; a click burst without the settle runs the staircase minutes ahead ("stuck pending-yellows").
- Note: `Render()` runs immediately after the click, but the SDK persists selection when the settled intent fires — the check glyph may visibly move only once the intent lands / status pushes arrive.

Selection predicates (SdkHost, shared by page, sheet, and coalescer — one identity question, one implementation):
- `SameId(a,b)`: both engaged, non-empty, equal.
- `IsBestAvailableSelected(sel)`: `!sel` **or** `sel->connect_location_id->best_available == true`.
- `IsLocationSelected(sel, loc)`: both have `connect_location_id`; true if `location_id`s match OR `client_id`s match OR `location_group_id`s match.
- `IsPeerSelected(sel, peer)` (LocationSheets.cpp anon ns): `SameId(sel->connect_location_id->client_id, peer.ClientId)`.

### 5.2 Feed-state machine (the three distinguishable empty states)
`FeedState CurrentFeedState()` — **state string checked BEFORE the snapshot** (a `LOCATIONS_LOADING` push carries JSON `null`, which parses to an engaged FilteredLocations whose six buckets are all nullopt — byte-identical to "loaded, zero providers"; only the state string separates them):
1. `locationsState_ == "LOCATIONS_ERROR"` → **Failed**
2. `locationsState_ == "LOCATIONS_LOADING"` → **Loading**
3. `!locations_` (no snapshot yet; state empty/unrecognized) → **Loading**
4. otherwise → **Loaded**

There is deliberately NO "no session" state: the provider list is public (`GET /network/provider-locations` answers 200 unauthenticated) and SdkHost serves it from the in-process Api when there is no device. A state that cannot occur must not be representable.

### 5.3 The inline "why the buckets are empty" line
After the groups, when `bucketRows <= 0 && !samplePinned_`, append `kit::MakePaneEmptyLine(line)` inline where the buckets would be (the list is never structurally empty when idle — the Best-available row is always there — so the full-height overlay alone could not fire):
- Loading → `Loc("loading")` = **"Loading..."**
- Failed → `pages::Adv("adv_providers_load_failed", L"Could not load the provider list.")` — **key `adv_providers_load_failed` is NOT in the store; English fallback "Could not load the provider list."** No retry promise in the copy: the SDK schedules none; the list reloads on destination re-entry, window return, or next search.
- Loaded → `Loc("no_locations_found")` **"No locations found"** when searching, else `Loc("no_providers_found")` **"We could not find any providers."**

`MakePaneEmptyLine`: TextBlock, `UrSupportingTextStyle` (12px, wraps) with Foreground overridden to **#5A5A5A (FaintBrush)**, TextAlignment Center, MaxWidth **320**, centered H+V, Margin **16** all sides. No glyph, no card.

### 5.4 The full-height overlay (`NetworkListEmptyHost`)
Cleared every render; if `NetworkListHost` ends the render with **zero children**, append `MakePaneEmptyLine(searching ? Loc("no_locations_found") : Loc("connecting_status_indicator") /*"Connecting to providers"*/)` into the hit-test-transparent overlay grid so it centers in the full remaining pane height. NOTE (flagged): with the §5.3 inline line present, the host can no longer be empty in any reachable state — port this overlay anyway for exact parity (it is the markup-level guarantee that an empty pane centers its message rather than showing a short card at the top).

Finally: `NetworkPaneAMeta.Text = total > 0 ? to_string(total) : ""` and `RenderDetail()`.

---

## 6. Pane B — detail (`NetworkPage::RenderDetail`) — exact element order

Clear `NetworkDetailHost`. `selected = Sdk().SelectedLocation()`; `best = IsBestAvailableSelected(selected)`.

**Group header** `MakePaneGroupHeader(Loc("selected_location"))` = **"Selected location"** (28px strip, no meta).

Key/value rows are `kit::MakePaneKeyValueRow(key, value, height=34)`: Border fixed Height **34**, Padding 12,0, bottom hairline; Grid ColumnSpacing 8, cols Star/Auto; key TextBlock `UrKeyTextStyle` (**13px #989898**, ellipsis, NoWrap), value TextBlock `UrValueTextStyle` (**13px #F8F8F8**, right-aligned, ellipsis, NoWrap). Key is a11y Raw; value's `AutomationProperties.Name` = `"{key}, {value}"`. Rows with an **empty value are skipped entirely** (the `value(...)` lambda).

*Branch A — best-available or nothing selected:*
- Row: key `Loc("name_label")` **"Name"** / value `Loc("best_available_provider")` **"Best available provider"**.
- `NetworkPaneBMeta` = "Best available provider".

*Branch B — a concrete location selected* (fields come from `urnet::ConnectLocation` ONLY — the SDK carries **no latency and no load anywhere**, so there are no such rows; do not invent them):
1. `name_label` **"Name"** → `location.name` (skipped if empty). `NetworkPaneBMeta` = that name.
2. `available_providers` **"Available providers"** → `Plural("provider_count", n)` — only when `provider_count > 0`.
3. `country` **"Country"** → `location.country` (skipped if empty).
4. `regions` **"Regions"** → `location.region` (skipped if empty). *(Deliberate key reuse: the store has no singular "Region" key; bucket-header keys used rather than inventing strings — reported for the store.)*
5. `cities` **"Cities"** → `location.city` (skipped if empty; same reuse note).
6. `strong_anonymization` **"Strong Anonymization"** → `Loc("yes")` "Yes" / `Loc("no")` "No" (always shown).
7. `promoted` **"Promoted"** → Yes/No from `location.promoted.value_or(false)` (always shown).
8. If `!location.stable`: a `kit::MakePaneRow(34)` Border containing a TextBlock **12px, color #F5C242**, VCenter, text `Loc("unstable_providers_warning")` = **"* (may be unstable)"** — a sentence, not a "Stable: No" row (the store has no "Stable" label; this is the shipped string for the condition).

**Reset-to-automatic row** — only when `!best`: a full `MakeRow(Loc("best_available_provider"), "", kUrCoral, selected=false, false, false, false)` (36px species, coral dot). Click → `Sdk().ConnectBestAvailableFromRow()` → `Render()`. Hidden when it would change nothing.

**Group header** `Loc("available_providers")` **"Available providers"** — the bucket counts behind the list (honest counts for the CURRENT query, not a second copy of the list). Five MakePaneKeyValueRow(34) rows, integer values (0 when feed absent):
- `network_peers` "Network peers" → peers count
- `countries` "Countries" → Countries count
- `regions` "Regions" → Regions count
- `cities` "Cities" → Cities count
- `devices` "Devices" → Devices count

**Group header** `Loc("blocked_locations_2")` **"Blocked locations"**, then `kit::MakePaneTwoLineRowButton(Loc("blocked_locations_2"), Loc("select_country_to_block"))`:
Button on `UrPaneRowButtonStyle`, Height+MinHeight **44** (UrPaneRowTallHeight); Grid ColumnSpacing 10, cols Star/Auto/Auto: two-line text column (StackPanel Spacing **1**, VCenter; title `UrRowTitleStyle` 13px #F8F8F8 = "Blocked locations"; note `UrRowNoteStyle` **11px #989898, trimmed never wrapped** = "Select country to block"), value TextBlock (`UrValueTextStyle`, muted, MaxWidth 240 — unused here), chevron FontIcon **U+E76C, 11px, #989898** a11y Raw. Button a11y Name = title, FullDescription = note. Click → `MainWindow::ShowBlockedLocationsFromNetwork()` → `settings_->ShowBlockedLocationsSheet()` (§8). This is the SECOND door to the blocked list — it stays on Settings too; the page does not own the network API read.

---

## 7. Data flow / SDK plumbing (must be reproduced structurally)

### 7.1 Two independent subscribers to the same two feeds
- `ConnectPage` owns the primary handlers `SetLocationsHandler` / `SetPeersHandler` — they drive the **modal chooser sheet** while it is open (and the drawer's peer-count label).
- The **Network destination is a second slot**: `SetLocationsObserver` / `SetPeersObserver` (deliberately a second slot, not a subscriber list — exactly two consumers, both window-owned for the window's lifetime). Both slots fire on the SDK callback thread, **observer first**, payload COPIED to the observer and moved into the handler.
- MainWindow ctor binds the observers: marshal to the dispatcher queue with a weak window ref, then `network().OnLocations(locations, state)` / `network().OnPeers(peers)`.

### 7.2 Feed lifetime (`ReconcileFeeds`) — selection AND presentation
- `SetSelected(bool)` fires on navigation change; `SetPresentationActive(bool)` on the window gate (`shown && !minimized`). Either transition → `ReconcileFeeds()`.
- When `selected_ && presentationActive_`: call `Sdk().EnsureLocations()` (idempotent), then (unless `samplePinned_`) seed `locations_ = Sdk().CurrentFilteredLocations()`, `locationsState_ = Sdk().CurrentFilteredLocationState()`, `peers_ = Sdk().ConnectedProvidePeers()`. The seed is EMPTY by construction right after arming (view controller start() loads on a goroutine ~1.2s measured; api path is an HTTP round trip) — it settles the "loading" state, it does not fetch.
- When only `selected_`: `Render()` anyway.
- History (preserve as comment): SdkHost tears the presentation-scoped view controllers down when the presentation stops (minimize / hide-to-tray) and pushes nullopt; before both halves existed, returning to the destination you left on re-opened nothing and the pane stayed empty for the life of the window.

### 7.3 SdkHost::EnsureLocations — two sources, one gate
- With a device: open `device_->openLocationsViewController()`, `addFilteredLocationsListener` (fans out to observer+handler), `start()`; open `device_->openPeerViewController()` (SDK-filtered to **connected AND provide-enabled** peers), `addPeersListener`, `start()`; then push a seed read of `getFilteredLocations()` / `getFilteredLocationState()` / `getPeers()` through both slots. `deviceFeedOpen_` (atomic) is set BEFORE `start()` so no stray api push can precede the first listener push.
- Without a device (**the list does not need a session** — the provider list is public): `EnsureApiLocationsLocked()` runs the exact port of `LocationsViewController::FilterLocations` against the in-process `Api`: query empty → `api.getProviderLocations(cb)`; non-empty → `api.findProviderLocations({query}, cb)`; bucket the answer with `urnet::getFilteredLocationsFromResult(result, loadedQuery)` (**the query the RESULT answers, not the current box text**). Cache rules: skip if a fetch for this exact query is in flight, or the cache is `LOCATIONS_LOADED` for this exact query; **a `LOCATIONS_ERROR` cache is deliberately not "good"** — the next arming (re-entering the destination, window return, next search) IS the retry; a failed search keeps the last good buckets on screen and only records the failure state. A **generation counter** (`apiLocationsGeneration_`) drops superseded answers. On start it publishes `LOCATIONS_LOADING` immediately (previous result stays underneath — same as the view controller, which re-pushes its snapshot with LOADING rather than blanking). The api cache survives presentation close (alt-tab away/back must not empty the pane). Single-writer gate: while `deviceFeedOpen_`, `PublishApiLocations()` says nothing.
- `SetLocationFilter(query)`: device path → `locationsVc_->filterLocations(query)` (server-side narrowing; SDK re-buckets and pushes). Api path → trim whitespace exactly as Go's `strings.TrimSpace` (a box of spaces = the idle list, not a search for " "), no-op if unchanged, else store and `EnsureApiLocationsLocked()`.
- `CurrentFilteredLocations()` / `CurrentFilteredLocationState()` / `ConnectedProvidePeers()`: read from the view controller when open, else from the api cache (`ConnectedProvidePeers` returns **nullopt** with no peer VC — peers genuinely need a session).
- `SelectedLocation()`: `connectVc_->getSelectedLocation()` when the connect VC exists, else `device_->getConnectLocation()`, else nullopt.

### 7.4 Page push handlers
`OnLocations(locations, state)`: ignored while `samplePinned_`; else store BOTH (the state is the only separator of the three empty screens — dropping it, once `(void)state`, is the historical bug), `Render()`. `OnPeers(peers)`: same pattern.

### 7.5 SDK bucketing semantics (Go, for the port's understanding — do NOT re-implement client-side)
`GetFilteredLocationsFromResult` (locations_view_controller.go:206+): groups with `MatchDistance==0 && filter!=""` → BestMatches, else promoted groups → Promoted; locations with `MatchDistance==0 && filter!=""` → BestMatches, else by `location_type`: country → Countries always; city/region → Cities/Regions **only when filter non-empty**; `result.Devices` → Devices (ConnectLocationId.client_id + device name only). Sort (stable) for BestMatches/Promoted/Countries/Cities/Regions: matchDistance ≤ 1 first, then provider count descending, then name. **`getFilteredLocationsFromResult` does no text matching** — narrowing is server-side (`findProviderLocations`); the filter only changes bucketing.

---

## 8. The two modal sheets

Both are `ContentDialog`s: XamlRoot-bound, Title boxed hstring, `CloseButtonText = Loc("close")` "Close", **Background #151515 (SheetBrush)** — a sheet sits ABOVE the page. One dialog at a time via the window-level `sheetOpen` guard (open co-routines bail if a sheet is showing, set the flag, `ShowAsync`, reset the sheet ptr + flag on close).

### 8.1 LocationChooserSheet (modal picker — Home's location row & peers line open it; NOT retired by the destination)
Opened by `ConnectPage::ShowLocationChooserSheet()`: guard `sheetOpen`; `Sdk().EnsureLocations()`; create sheet; seed `Update(Sdk().CurrentFilteredLocations(), Sdk().ConnectedProvidePeers())`; `ShowAsync`. Live updates while open: `SetLocationsHandler` push → `Update(locations, Sdk().ConnectedProvidePeers())`; `SetPeersHandler` push → `Update(Sdk().CurrentFilteredLocations(), peers)` (each marshalled to the dispatcher; null-checked on `locationSheet_`). Row handlers capture the sheet weakly (the window's shared_ptr keeps it alive while showing; strong capture would cycle).

Structure (Build):
- Dialog title `Loc("browse_locations")` = **"Browse Locations"**.
- Content StackPanel: Spacing **12**, MinWidth **440**.
  1. Search `TextBox` (default WinUI style), placeholder `search_providers_input_placeholder`. TextChanged → `query_ = Narrow(text)`, `sdk_.SetLocationFilter(query_)` — no app-side debounce (SDK sequence numbers guard staleness) and no app-side filtering.
  2. `status_` TextBlock **12px** #989898, wraps, initially Collapsed — the no-results line.
  3. `ScrollViewer` MaxHeight **460** containing `sections_` StackPanel Spacing **12**.

Render (on every Update/search change): clear sections; `selected = sdk_.SelectedLocation()`; `searching = !query_.empty()`.
1. **Network peers** pinned first (self-hides when none): `SectionHeader(Loc("network_peers"))` — TextBlock **12px** #989898 with Margin top **8**; then StackPanel Spacing **4** of peer rows.
2. Searching → `AppendSection(Loc("top_matches"), BestMatches)`; idle → `SectionHeader(Loc("promoted_locations"))` + a StackPanel(4) holding the single best-available row.
3. `AppendSection` for Countries, Regions, Cities, Devices (each self-hides on empty; header + StackPanel Spacing 4 of rows).
4. No-results: only while **searching** with no location buckets AND zero peers → `status_.Text = Loc("no_providers_found")` "We could not find any providers.", Visible; else Collapsed. *(The sheet, unlike the page, has no Loading/Failed distinction — it discards the state string. Parity nuance, see flags.)*

Sheet row species (`MakeRowGrid`): Grid, three cols Auto/Star/Auto, ColumnSpacing **12**, Padding **0,6,0,6**, transparent background (hit-testable), whole row `Tapped`:
- *Location row*: dot Ellipse **10×10** = `LocationColor`; text column StackPanel Spacing **2** VCenter — name (MakeName: **15px** #F8F8F8, NoWrap+ellipsis) + optional caption **12px** #989898 = `Plural("provider_count", n)` when n>0; trailing StackPanel horizontal Spacing **6** VCenter with glyph order: **unstable Warning E7BA #F5C242 (when `!stable`), privacy Lock E72E #87FB67 (when strong_privacy), CheckMark E73E #638BFC (when selected)** — all FontIcon **14px**. Tap → `sdk_.ConnectFromRow(locationCopy)` (coalesced) + `dialog_.Hide()` (dismiss-on-connect, iOS/Android parity).
- *Peer row*: dot 10 = `getColorHex(ClientId)`; name = `PeerDisplayName`; secondary line = `DeviceSpec` **only when a distinct DeviceName is also shown**; trailing: **providing Globe E774 #87FB67 always**, CheckMark when selected (a deliberate FIX vs Android, which omits the peer check). Tap → build ConnectLocation from client_id + display name → `ConnectFromRow` + Hide.
- *Best-available row*: dot 10 **kUrCoral**; name = `Loc("best_available_provider")` (15px); CheckMark when `IsBestAvailableSelected` (FIX vs Android). Tap → `ConnectBestAvailableFromRow` + Hide.

### 8.2 BlockedLocationsSheet (list + add-a-country picker in one sheet)
Created via `BlockedLocationsSheet::Create(root, sdk)` → Build + `LoadBlocked()` + `LoadCountries()`. Openers: Settings' "Blocked locations" NavRow AND the Network detail pane's door (both funnel through `SettingsPage::ShowBlockedLocationsSheet()` with the `sheetOpen` guard).

Structure: dialog title `Loc("blocked_locations")` = **"Blocked Locations"** (note: title-case key, distinct from the row's `blocked_locations_2` "Blocked locations"). Content StackPanel MinWidth **420**, Spacing **12**:
1. `blockedPanel_` StackPanel Spacing **4** — the current blocked rows.
2. `blockedEmpty_` TextBlock **12px**, Faint #5A5A5A, initial text `Loc("no_blocked_locations")` "No blocked locations".
3. Divider (1px `kBorder` Border; the card-model `rows::Divider`).
4. Label `Loc("select_country_to_block")` "Select country to block" — `UrLabelStyle` (12px #989898).
5. Search `TextBox` `UrTextInputStyle` (Montreal **16px**, CornerRadius 0, Padding 0,0,0,8 underline-style), placeholder `Loc("search_placeholder")` = **"Search for all locations"**. TextChanged → `RenderCountries()` (client-side filter only).
6. `ScrollViewer` MaxHeight **240**, VScroll Auto, containing `countryPanel_` StackPanel Spacing **2**.
7. `errorText_` TextBlock 12px, wraps, **Danger #F8523B**, Collapsed until an error (iOS assigns a failure message and never renders it — this line is the fix).

State model: two `rows::FieldState` fields (`blockedState_`, `countriesState_`), enum `{NoSession, NoDevice, Loading, Loaded, Empty, Failed}`. `ApplyFieldState` renders: Loading → `Loc("loading")` "Loading..." faint; Empty → `Loc("none")` "None" faint; NoSession → `Loc("please_login_to_urnetwork")` "Please login to URnetwork" faint; NoDevice → `Loc("site_app_device_attaching")` "Attaching device controls…" faint; Failed → `Loc("something_went_wrong")` "Something went wrong." **danger**; Loaded → supplied text muted.

`LoadBlocked()`: gate on `sdk_.IsLoggedIn()` (NOT `apiReady()` — that is set at SDK init and once let this sheet fire an unauthenticated request whose 401 rendered as the reassuring "No blocked locations") → else NoSession. Set Loading, render, then `sdk_.api().getNetworkBlockedLocations(cb)`: failure = `!result || err` (the result type has NO error field); sort ascending by `location_name`; marshal; state = Failed / Empty / Loaded; `RenderBlocked()` + `RenderCountries()` (already-blocked countries drop out of the picker).

`RenderBlocked()`: non-Loaded or empty → show `blockedEmpty_` with `ApplyFieldState` (Empty state gets the specific shipped line "No blocked locations" instead of generic "None"). Loaded rows: for each blocked location, a `rows::Row(host, name, "", removeButton)` — in pane-mode this is `MakePaneTwoLineRow` 44px with the trailing control; the trailing `Button` label `Loc("remove")` "Remove", Foreground **#F8523B**, disabled when the entry has no `location_id`; Click → `Unblock(id)`.

`LoadCountries()`: same `IsLoggedIn` gate (getProviderLocations is actually unauthenticated, but a dev switch must not talk to production either way); re-entrancy latch `loadingCountries_`; `sdk_.api().getProviderLocations(cb)` → keep only `location_type == LocationTypeCountry` rows, id = `location_id` else `country_location_id`, drop empty id/name, **sort by display name ascending**; state Failed/Empty/Loaded.

`RenderCountries()`: filter = `LowerAscii(Trim(query))`, ASCII-lowercase substring match on the name; skip countries already in `blocked_` (dedupe, iOS parity); each hit a `Button` `UrCardRowButtonStyle` (card-scale row: radius 8, padding 12,8, no border, hover #242424 + strong edge), stretch, left content, text = name; Click → `Block(id)`. When zero shown: one 12px wrapping TextBlock — if Loaded and countries exist → `Loc("no_locations_found")` "No locations found" faint (search matched none); else `ApplyFieldState` for Loading/NoSession/Failed/Empty — four blank-panel causes, four different lines.

`Block(id)`: guard `IsLoggedIn && !id.empty()`; hide error; `api().networkBlockLocation({location_id}, cb)`; on server error (`ServerError(result, err)` non-empty) → `ShowError(Loc("blocked_location_could_not_be_added_please_try"))` = "Blocked location could not be added. Please try again later."; on success → **`LoadBlocked()` re-read** (the server owns names/types; a refetch cannot drift).
`Unblock(id)`: same shape; error line `blocked_location_could_not_be_removed_please_try` = "Blocked location could not be removed. Please try again later."; **`LoadBlocked()` re-sync either way**.

---

## 9. Location dot color (`LocationColor`, LocationSheets.cpp 142–159 + SDK)

Key selection: if `location_type == "country"` and `country_code` non-empty → key = country_code; else first non-empty of `connect_location_id.location_id` / `.client_id` / `.location_group_id`; if none → **#989898** and stop. Then `urnet::getColorHex(key)` → parse via `ColorFromHex` (accepts "RRGGBB", "#RRGGBB", "AARRGGBB"; parse failure → #989898).

`urnet::getColorHex` is the SDK's Go `GetColorHex` (sdk/locations_view_controller.go:381): a fixed `countryCodeColorHexes` map keyed by **lowercase ISO country codes** (e.g. `is`=639A88, `ee`=78C0E0, `ca`=449DD1, `de`=663F46, `au`=F29E4C, `us`=BAC5B3, `gb`=F1789B …), with a deterministic fallback for unknown keys: md5(code)[0] % len and md5(code+"salt")[0] % len pick two table entries (over the **sorted** key list) whose RGB channels are averaged. **The port must call the SDK function, not re-implement or port the elements TS table** (doc §8.1 points at `elements:src/utils/color-utils.ts`; the Windows code's actual source of truth is the SDK — same palette, but call the SDK).

Peer rows key on `ClientId` directly (`getColorHex(peer.ClientId.value_or(""))`). Best-available is always **kUrCoral #FF6C58** (hardcoded, mobile parity — no SDK call).

---

## 10. Preview sample (`--preview-ui` + `URNETWORK_PREVIEW_SAMPLE=1`, BOTH required)

`MainWindow` calls `network_->ApplyPreviewSample()` only when `PreviewSampleRequested()` (previewUi_ flag AND env var == "1"). The page builds synthetic buckets — 12 countries (Germany 412 providers stable+privacy+promoted; United States 1876; Japan 233; Netherlands 198 privacy; Brazil 76 **unstable**; Singapore 141; United Kingdom 604; Canada 287; France 351; Sweden 119 privacy; Australia 92 **unstable**; India 508), 5 regions (48 providers each), 10 cities (27 each), and 3 peers (`workshop-desktop`/windows, `kitchen-pi`/linux/arm64, `studio-mbp`/darwin/arm64, ClientId "peer-"+name) — logs a WARN that the content is synthetic, stores the buckets, sets `samplePinned_ = true` (so later real empty pushes cannot blank the pane; `OnLocations`/`OnPeers` early-return, ReconcileFeeds skips the snapshot seed, the §5.3 inline line is suppressed), then `Render()`. Every field of `SampleLocation` is set explicitly so the detail pane exercises all of them.

---

## 11. Localization inventory for this surface (key → English)

Store keys (`Loc`, from `Strings/en/Resources.resw`; ~1200 keys × 28 locales; a missing key renders AS the key id — visibly wrong, never blank):

| key | English |
|---|---|
| `network` | Network |
| `available_providers` | Available providers |
| `selected_provider` | Selected provider |
| `selected_location` | Selected location |
| `search_providers_input_placeholder` | Search countries, states, cities... |
| `search_providers_input_label` | Search providers |
| `network_peers` | Network peers |
| `top_matches` | Top Matches |
| `promoted_locations` | Promoted Locations |
| `countries` | Countries |
| `regions` | Regions |
| `cities` | Cities |
| `devices` | Devices |
| `best_available_provider` | Best available provider |
| `provider_count.one` / `.other` | {} provider / {} providers (via `Plural("provider_count", n)`) |
| `loading` | Loading... |
| `no_locations_found` | No locations found |
| `no_providers_found` | We could not find any providers. |
| `connecting_status_indicator` | Connecting to providers |
| `name_label` | Name |
| `country` | Country |
| `strong_anonymization` | Strong Anonymization |
| `promoted` | Promoted |
| `yes` / `no` | Yes / No |
| `unstable_providers_warning` | * (may be unstable) |
| `blocked_locations` | Blocked Locations (sheet title) |
| `blocked_locations_2` | Blocked locations (row/group) |
| `select_country_to_block` | Select country to block |
| `no_blocked_locations` | No blocked locations |
| `search_placeholder` | Search for all locations |
| `remove` | Remove |
| `blocked_location_could_not_be_added_please_try` | Blocked location could not be added. Please try again later. |
| `blocked_location_could_not_be_removed_please_try` | Blocked location could not be removed. Please try again later. |
| `something_went_wrong` | Something went wrong. |
| `none` | None |
| `please_login_to_urnetwork` | Please login to URnetwork |
| `site_app_device_attaching` | Attaching device controls… |
| `browse_locations` | Browse Locations (chooser sheet title) |
| `close` | Close (dialog close button) |

Adv() fallback (key NOT in store — the port must carry the same fallback or add the key):
- `adv_providers_load_failed` → **"Could not load the provider list."**

---

## 12. A11y summary (all mandatory)

- Pane landmarks named "Available providers" / "Selected provider"; PaneA LandmarkType Main.
- Search TextBox `Name` = "Search providers" (placeholder ≠ name).
- Every list row: the Button carries the composed Name (title, meta, unstable/privacy/providing state words); all inner elements Raw; selected rows add FullDescription "Selected provider". Rows are real Buttons: Tab-reachable, Enter/Space invoke.
- Group header titles are HeadingLevel 3.
- Key/value rows: key Raw, value Name = "key, value".
- Two-line row buttons: Name = title, FullDescription = note; chevron Raw.
- Dots and glyph panels are always Raw (color restates what text says).

---

## 13. Behavioral checklist (port acceptance)

1. Every keystroke in either search box → `SetLocationFilter` immediately; page also re-renders synchronously for the idle/searching branch flip. No timers; staleness via SDK sequence numbers / api generation.
2. Row click → coalesced connect with 1200ms settle; re-click of the active target = no-op that cancels pending intent; immediate connect/disconnect supersedes.
3. Loading vs LOCATIONS_ERROR vs genuinely-empty are three different sentences, state string checked before the snapshot.
4. Feeds reopen on BOTH selection and presentation edges; api-path cache survives presentation close; LOCATIONS_ERROR cache is retried on next arming.
5. Detail pane = ConnectLocation fields only; empty-valued rows skipped; unstable renders as the amber shipped sentence; reset row hidden when best-available already selected.
6. PaneB fully hidden (width 0 + rule + visibility) under 1000dip.
7. Chooser sheet and page never disagree: same feeds, same bucket order, shared PeerDisplayName + selection predicates.
8. Blocked sheet: server-owned truth (refetch after every mutation), four distinguishable empty causes, dedupe of already-blocked countries, danger-colored Remove, error lines rendered.


## SDK surface referenced
- Sdk().EnsureLocations()  [SdkHost; idempotent; device -> view controllers, no device -> in-process Api]
- Sdk().SetLocationFilter(query)  [device: LocationsViewController::filterLocations; api path: trimmed query + EnsureApiLocations]
- Sdk().CurrentFilteredLocations()  [locationsVc_->getFilteredLocations() else api cache via getFilteredLocationsFromResult]
- Sdk().CurrentFilteredLocationState()  [locationsVc_->getFilteredLocationState() else apiLocationsState_]
- Sdk().ConnectedProvidePeers()  [peerVc_->getPeers(); nullopt with no session]
- Sdk().SelectedLocation()  [connectVc_->getSelectedLocation() else device_->getConnectLocation()]
- Sdk().ConnectFromRow(ConnectLocation)  [coalesced 1200ms settle; idempotent vs current selection]
- Sdk().ConnectBestAvailableFromRow()  [same coalescer, kind BestAvailable]
- Sdk().SetLocationsObserver(handler)  [NetworkPage's second feed slot]
- Sdk().SetPeersObserver(handler)  [NetworkPage's second feed slot]
- Sdk().SetLocationsHandler(handler)  [ConnectPage; drives the open LocationChooserSheet]
- Sdk().SetPeersHandler(handler)  [ConnectPage; sheet + drawer peer count]
- Sdk().SetPresentationActive(active)  [SdkHost, non-blocking; owns view-controller lifetime]
- Sdk().IsLoggedIn()  [BlockedLocationsSheet session gate]
- Sdk().api().getProviderLocations(callback)  [idle provider list; also BlockedLocationsSheet country picker]
- Sdk().api().findProviderLocations(FindLocationsArgs{query}, callback)  [non-empty query path]
- Sdk().api().getNetworkBlockedLocations(callback)  [GetNetworkBlockedLocationsResult has NO error field; failure = !result || err]
- Sdk().api().networkBlockLocation(NetworkBlockLocationArgs{location_id}, callback)
- Sdk().api().networkUnblockLocation(NetworkUnblockLocationArgs{location_id}, callback)
- urnet::getColorHex(code)  [free function; Go countryCodeColorHexes + md5-mix fallback]
- urnet::getFilteredLocationsFromResult(FindLocationsResult, filter)  [api-path bucketing]
- device_->openLocationsViewController() / locationsVc_->addFilteredLocationsListener(cb) / locationsVc_->start()
- locationsVc_->filterLocations(query) / getFilteredLocations() / getFilteredLocationState()
- device_->openPeerViewController() / peerVc_->addPeersListener(cb) / peerVc_->start() / peerVc_->getPeers()
- connectVc_->getConnectionStatus()  [CONNECTED|CONNECTING|DESTINATION_SET gate in RowClickIsCurrent]
- connectVc_->getSelectedLocation() / device_->getConnectLocation()
- urnw::SameId / urnw::IsBestAvailableSelected / urnw::IsLocationSelected  [SdkHost.h shared selection predicates]
- urnet constants: LocationTypeCountry/Region/City ('country'/'region'/'city'), LocationsLoading/Loaded/Error ('LOCATIONS_LOADING'/'LOCATIONS_LOADED'/'LOCATIONS_ERROR')
- urnet types: ConnectLocation{connect_location_id{client_id,location_id,location_group_id,best_available}, name, provider_count, promoted, match_distance, location_type, city, region, country, country_code, city/region/country_location_id, stable, strong_privacy, network_peer}; FilteredLocations{BestMatches,Promoted,Countries,Cities,Regions,Devices}; NetworkPeer{ClientId,ProvideEnabled,Principal,Roles,DeviceSpec,DeviceName}; FindLocationsArgs/Result; BlockedLocation; Get/NetworkBlock/NetworkUnblockLocation args+results
- w_.ShowBlockedLocationsFromNetwork() -> SettingsPage::ShowBlockedLocationsSheet()  [window-level sheetOpen guard]

## Flags (doc-vs-code drift / risks)
- DOC vs CODE (doc §7.8 + XAML comment): both claim the detail pane shows 'location_type' and 'country(+code)'. The CODE's RenderDetail renders NO location_type row and NO country-code row — country_code only feeds the dot color and the color-key choice. Actual detail rows: Name, Available providers (plural count, only when >0), Country, region under label 'Regions', city under label 'Cities', Strong Anonymization Yes/No, Promoted Yes/No, plus the amber unstable sentence. Code wins.
- DOC vs CODE (doc §7.8 'group headers carry counts'): true for peers/top-matches/countries/regions/cities/devices, but the idle 'Promoted Locations' header is passed count 0 and shows NO meta; it labels the single synthetic Best-available row (the SDK's Promoted bucket is ignored by every client — intentional quirk to keep).
- DOC vs CODE (doc §8.1 'Per-country location colors: elements:src/utils/color-utils.ts'): the Windows code's actual source is the SDK's Go countryCodeColorHexes table via urnet::getColorHex (locations_view_controller.go:381). Same palette family, but the port must call the SDK function — porting the TS table risks drift on unknown-code fallback (md5-mix is Go-side).
- STALE COMMENT in SdkHost.cpp (~line 3465) says the presentation gate is 'shown && activated'; AppController.cpp:38 (with static_asserts) and LocationSheets.h both say the CURRENT gate is shown && !minimized. Port the latter — plain alt-tab must NOT close the feeds.
- Task prompt said 'FilterLocations debounce': there is NO timer debounce anywhere. Both search boxes call SetLocationFilter on every keystroke; staleness is guarded by SDK per-query sequence numbers (device path) and a generation counter + identical-query dedupe (api path). The only timer-like behavior on this surface is the 1200ms row-click connect settle (kRowClickSettle), which is a session-worker condition-variable wait, not a UI timer.
- Effectively-dead code that still must be decided for parity: the NetworkListEmptyHost full-height overlay ('Connecting to providers' / 'No locations found') can no longer fire — the idle list always holds the Best-available row, and when bucketRows<=0 the inline feed-state line fills the host first (and samplePinned_ implies non-empty buckets). Ported here as belt-and-braces per the markup guarantee; flag if the GTK port drops it.
- Parity nuance: the modal LocationChooserSheet DISCARDS the locations state string (ConnectPage's SetLocationsHandler ignores it) — the sheet has no Loading/Failed distinction, only the searching no-results line ('We could not find any providers.'). Only the NetworkPage destination distinguishes the three empty states. Do not 'fix' this asymmetry silently; it is the shipped behavior.
- Store gaps the port inherits: 'adv_providers_load_failed' ships as an Adv() English fallback ('Could not load the provider list.'); the detail pane deliberately reuses plural bucket keys 'regions'/'cities' as singular field labels because the store has no 'Region'/'City' keys (reported for the store upstream).
- Minor comment-vs-code nit in CurrentFeedState: the comment says an unrecognized state string 'falls through to loading' — that is only true while the snapshot is absent; with an engaged snapshot and an unrecognized/empty state it returns Loaded. Port the code, not the comment.
- Selection-echo nuance: NetworkPage::Render() runs immediately after a row click, but SdkHost persists the selection only when the settled (1200ms) intent fires — so the check glyph and detail pane may not reflect the click until the intent lands / the next status push. This is intentional (the click IS the connect); do not add an optimistic local highlight.
- UrPaneRowButtonStyle's MinHeight is 40 in App.xaml; NetworkPage rows explicitly override Height=MinHeight=36 (Home's list-row height), while the blocked-locations door row overrides to 44 (two-line). Row heights are per-call, not per-style — replicate the overrides, not the style default.
- GTK icon risk: the four state glyphs (E73E check, E7BA warning, E72E lock, E774 globe), the search glyph (E721) and the chevron (E76C) are Segoe Fluent Icons codepoints; the port needs equivalent symbolic icons — pixel parity of glyph shapes is not achievable with the Windows-licensed font.
