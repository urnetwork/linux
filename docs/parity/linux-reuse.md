# THE LINUX REUSE MAP — existing Linux surfaces → Windows-parity structure

Audience: the implementer building the GTK4/C++ Windows-parity shell. This spec says, for every
surface that already exists in `linux:app/src/`, exactly what it renders today (numbers, strings,
behavior, SDK calls), where it lands in the Windows-parity layout (per `docs/linux_agent_help.md`
§7), and whether to reuse, restyle, relocate, or rewrite it. Code wins over doc; disagreements are
flagged inline and collected in the flags list.

String notation: `key` → "English" means the gettext call `T_("key", "English")` (msgctxt = the
localization-store key id; the store's translation wins, the English is the compiled-in fallback).
`TN_` is the plural form. All colors are the shared brand palette (`Ui.hpp` constants; §8.1).

---

## 0. Quick landing table

| Linux surface (file) | Windows-parity landing (§7 ref) | Verdict |
|---|---|---|
| ConnectDrawer container + intro anim (`ConnectDrawer.*`) | dissolved — cards disperse into Home Panes A/B/C, Account, Settings | REWRITE (dispersal) |
| Drawer: insufficient-balance banner | Home Pane A `BalanceWarning` InfoBar (§7.7) | RESTYLE + RELOCATE |
| Drawer: controls card (location row, mode, fixed ip, anon, PQE) | Home Pane A selected-provider row + connect-options group (§7.7) | RELOCATE + RESTYLE |
| Drawer: kill switch row | Settings → VPN & privacy kill-switch toggle (§7.11) | RELOCATE (semantics caveat, flag 6) |
| Drawer: client-stats card (remote+blocked charts → ContractsSheet) | remote chart → Pane B header chart; blocked chart → Pane C; contracts entry → Pane C contracts group | RELOCATE (composite dies) |
| Drawer: local-stats card (local chart + split-rule count → SplitRulesSheet) | local chart → Pane C; count → Pane C split-rules group row | RELOCATE (composite dies) |
| Drawer: custom DNS card (4 rows + pill + unavailable) | Pane C custom DNS group (§7.7) — near 1:1 | RESTYLE (card → pane group) |
| Drawer: blocker card | Home Pane A connect-options toggle row "block_ads_and_trackers" | RELOCATE |
| Drawer: plan card + UsageBar | Account PLAN pane (§7.9) | RELOCATE + additions |
| Drawer: PQI panel | Settings → VPN & privacy PQI row + PostQuantumIdentitySheet (§7.11/§7.14) | RELOCATE (panel → sheet body) |
| ContractsSheet | ClientContractsSheet from Pane C contracts group (§7.14) | REUSE-AS-IS |
| LocationsSheet | Network destination Pane A feeds + modal LocationChooserSheet (§7.8) | REUSE (chooser) + REWRITE (pane A) — must add 3-state empty (flag 3) |
| DnsSheet | DnsEditorSheet from Pane C custom-DNS group (§7.14) | REUSE-AS-IS |
| SplitRulesSheet (+ editor) | SplitRulesSheet from Pane C split-rules group (§7.14) | REUSE-AS-IS |
| UpgradeSheet | UpgradeSheet from Account PLAN "upgrade" + BalanceWarning (§7.14) | REUSE-AS-IS |
| RedeemCodeSheet | entry → RedeemCodeSheet from Account PLAN Redeem row; embedded history table → Account Pane C CODES (§7.9) | SPLIT: entry REUSE, history RELOCATE |
| PostQuantumIdentity (sheets, identicon kit) | PostQuantumIdentitySheet + ProviderIdentities list + share; badge join reused by provider-locations rows | REUSE-AS-IS |
| ProviderLocationsSheet | from Pane C "Connected to N providers" (§7.7/§7.14); keeps the Linux-only GeoClue override section | REUSE-AS-IS |
| SubscriptionBalanceStore | backing store for Account PLAN pane, BalanceWarning, Upgrade/Redeem confirmation | REUSE-AS-IS |
| TransferChart | Pane B chart (Height 150) + Pane C blocked/local charts (Height 132) | RESTYLE (heights; flag 1) |
| UsageBar | Account PLAN pane UsageBar | REUSE-AS-IS |
| Formatters | app-wide | REUSE-AS-IS |
| Tray | tray (§7.15) | REWRITE toward 8-state matrix + 2 extra verbs (flag 4) |

---

## 1. Style baseline the Linux code carries today (`Ui.cpp` one-shot CSS) — and the deltas

What exists (exact values, reusable as the *card model* of §7.3's second vocabulary):
- `window.background`: `#101010`, text `#F8F8F8`. `.dim-label` `#989898`; `.ur-label-faint` `#5A5A5A`.
- `.ur-card`: fill `#1C1C1C`, border 1px `alpha(#fff,.12)`, radius 12, padding 16.
- `.ur-card-tappable`: 150ms ease transitions; hover `#242424` + border `alpha(#fff,.22)`; pressed
  (`.pressed` class toggled by `WireCardPressFeedback`, since GTK4 sets no `:active` on a box) `#2A2A2A`.
- `.ur-banner`: `#1C1C1C`, radius 12, padding 12.
- `.ur-dns-pill`: 11px/500, white text, padding 5×10, radius 999, bg `alpha(#FF6C58,.15)` (= UrCoralSubtle role).
- `.ur-chip`: 10px/600, padding 3×7, radius 999. Variants green `#87FB67` (tint .14 / hi solid on `#101010` text),
  coral `#FF6C58` (tint .14 / hi solid white text), muted `#989898` (tint .16 / hi solid `#101010` text).
- Dots/values: `.ur-dot-on`/`.ur-value-on` `#87FB67`; `.ur-dot-off` `alpha(#5A5A5A,.4)`.
- Mono ramp: `.ur-mono-15/13` (weight 500), `.ur-mono-12/11`; captions `.ur-caption-11/10`.
- `button.suggested-action`: **accent `#EFF7BB` on `#101010` text** (hover `#F5FBD1`, active `#D9E3A3`,
  disabled `alpha(#EFF7BB,.2)`) — this is the §8.1 *dialog-primary* role, correct as-is.
- `button.destructive-action`: `alpha(#F8523B,.15)` bg, `#F8523B` text (hover .25; flat variant transparent, hover .12).
- `switch:checked`: `#638BFC` (matches kToggleAccent).
- `button.ur-newchip`: `#EFF7BB` capsule, `#101010` text, radius 999, padding 6×12.
- `button.ur-option`: transparent, 2px border `#5A5A5A`, radius 8, padding 14×16; `:checked` border `#EFF7BB`; hover `alpha(#fff,.04)`.
- `entry.ur-otp`: mono 24px, letter-spacing 12px. `.ur-error-text`: `#FF6C58`.
- `Rgba` palette constants: kUrGreen `#87FB67`, kUrPink `#ED8FFF`, kUrCoral `#FF6C58`, kUrMutedCoral `#C8604F`,
  kUrAmber `#F5C242`, kUrElectricBlue `#0039DE`, kUrBackground `#101010`, kUrCardBackground `#1C1C1C`,
  kUrText white, kUrTextMuted `#989898`, kUrTextFaint `#5A5A5A`, kUrBorderBase white@12%, kUrDanger `#F8523B`, kUrAccent `#EFF7BB`.
- Helpers to keep: `PostToMain`, `ParseHexColor` ("rrggbb" or "#rrggbb"), `HexForMarkup`,
  `MarkdownLinksToPango` ("[text](url)" → pango `<a>`), `MakeCard(spacing)`, `MakeChip(text,colorClass,highlighted)`,
  `MakeCaption`, `MakeCardHeader(title)` (muted title + chevron), `RemoveAllChildren`, `SetPointerCursor`,
  `WireCardPressFeedback`, `ShowToast` (nearest AdwToastOverlay), `AddEscapeToClose`,
  `AnimateEntrance(widget, delayMs, slideOffsetPx, durationMs=300)` (~300ms cubic ease-out fade + up-slide).

Deltas the port must add (from §7.3/§8):
- The **pane shell vocabulary does not exist**: 40px pane header strip (`#151515`, letterspaced 12px SemiBold
  title + right 11px meta), 28px group headers (letterspaced 11px), 34px key-value rows, 36px list/table rows,
  40px standard rows, 44px two-line rows, 1px `#1FFFFFFF` hairlines, 12px inset, row hover to `#1C1C1C`,
  selection = fill + 2px leading accent bar + a11y suffix. Build the `MakePane*` kit fresh.
- **Sheets must sit at `#151515`**, not the page `#101010` — today every Gtk window (sheets included) paints
  `#101010` via `window.background` (flag 2).
- The blue URButton PRIMARY role (`#638BFC` container / white content, radius 12, min-height 48, PP NeueBit 24)
  exists nowhere in this CSS — the Connect button and login primaries need it; keep `suggested-action` accent
  for sheet/dialog primaries.
- Brand fonts are not in this CSS (system font + `monospace` only) — the pane build brings the 4 licensed faces (§8.2).

---

## 2. Surface-by-surface

### 2.1 ConnectDrawer (`ConnectDrawer.hpp/.cpp`) — the container dies, the organs live

**Today:** a `Gtk::Box` (vertical, spacing 16) appended under the home connect controls, in order:
(1) insufficient-balance banner, (2) controls card, (3) client-stats card, (4) local-stats card,
(5) custom-DNS card, (6) blocker card, (7) plan card, (8) PQI panel. Owns (kept alive, hide-on-close)
ContractsSheet, SplitRulesSheet, LocationsSheet, DnsSheet, RedeemCodeSheet, UpgradeSheet.
Intro (first map only): `AnimateEntrance(self, 0, slide 16px, 300ms)` then per-card fades starting at
40ms, +60ms per card, 300ms each. `on_create_account` callback routes guests into the
create-network page (guest-upgrade mode).

**Event routing (reuse verbatim as the page-level dispatcher):** `OnHostEvent(DrawerEvent)` —
`DeviceLifecycle`→RefreshAll; `Throughput`→PullThroughput (feeds all charts one shared
`ThroughputPointList` + `ThroughputWindowSeconds()`); `BlockActions`/`BlockStats`→refresh
SplitRulesSheet if visible; `Overrides`→RefreshSplitRuleCount + sheet-if-visible;
`DnsSettings`→RefreshDnsCard; `Blocker`→RefreshBlocker; `RouteLocal`→RefreshKillSwitch;
`Contracts`→ContractsSheet-if-visible (SDK VC already coalesced — one event per real change);
`Location`→RefreshControls + RefreshDnsCard (the pill follows the connected country);
`Profile`→RefreshControls; `Peers`→RefreshControls + LocationsSheet-if-visible;
`Locations`→LocationsSheet-if-visible; `ProviderIdentities`→PQI panel Refresh;
`ProviderLocations`→owned by MainWindow (override tracking must survive with the drawer unbuilt).
`RefreshAll()` = all of the above + sheets-if-visible; called on DeviceLifecycle and window re-show.

**Lands:** the container and its scroll column have no Windows analogue. The page dispatcher
pattern (marshalled `DrawerEvent` → per-group `Refresh*`, echo guards, read-through-accessors)
is exactly the §7.18 "one writer per surface" discipline — port it to each pane. The intro
animation dies; pane entrances come from motion Wave 2 (`DirectionalSwap`; drawer-entrance =
`RiseIn(incoming, Up, 8dip, 0)`) built **from the spec, not from this code** (flag 7).

**Verdict:** REWRITE (dispersal). **Shared accessors:** the entire drawer-accessor block of
SdkHost (see per-card sections below) plus `SetDrawerEventHandler`.

---

### 2.2 Insufficient-balance banner → Home Pane A `BalanceWarning`

**Today:** `MakeCard(4)`; header row (spacing 8): `dialog-warning-symbolic` icon with
`.ur-error-text`; title `insufficient_balance` → "Insufficient balance" (`.heading`, xalign 0,
hexpand); trailing `go-next-symbolic` chevron (dim). Body: `insufficient_balance_message` →
"Add balance or a plan to keep connecting." (dim, caption, wrap). Whole card tappable
(`MakeCardTappable`: hover/press feedback + pointer + click-released gesture):
guests (`balance_.IsGuest()`) → `on_create_account` (a Pro sub must never bind to a throwaway
guest network); everyone else → `OpenUpgrade()`. Visibility (one writer,
`RefreshPlanCard`): `insufficientBalance_ && !IsPro() && !IsPolling()` — `insufficientBalance_`
arrives from the live-stats feed (`LiveStats.insufficientBalance`, i.e. ContractStatus) via
`SetInsufficientBalance(bool)` with a change guard.

**Lands:** Home Pane A stacked InfoBar #1 `BalanceWarning` ("out of data → upgrade/create-account",
§7.7). Same gate, same two actions.
**Verdict:** RESTYLE (card → InfoBar/banner row in the pane vocabulary) + RELOCATE. Keep the
gate expression and the guest fork verbatim.
**Accessors:** `LiveStats.insufficientBalance` (stats push), `SubscriptionBalanceStore::IsPro/IsGuest/IsPolling`.

---

### 2.3 Controls card → Pane A selected-provider row + connect-options group

**Today** (`MakeCard(12)`), top to bottom:
1. **Selected-location row** (h-box spacing 12): color dot (pango `●`), two-line column
   (spacing 2): name (ellipsize END) + caption (dim, caption class), trailing `go-next-symbolic`
   chevron. Whole row tappable → `LocationsSheet::Open()`. Content rules
   (`RefreshControls`): best-available when `!SelectedLocation()` or
   `connect_location_id->best_available` → name `best_available_provider` → "Best available
   provider", dot = kUrCoral, caption hidden. Else: display name = `location->name`, but a
   selected **network peer** resolves its device name from `ConnectedProvidePeers()` by
   `client_id` via `PeerDisplayName` (DeviceName ▸ DeviceSpec ▸ client id); empty →
   `selected_location` → "Selected location". Dot = `ParseHexColor(urnet::getColorHex(country_code))`,
   fallback gray (.5,.5,.5). Caption `provider_count_int` → "{} providers" when count > 0.
2. Caption `connect_options` → "Connect options" (dim, caption).
3. **Connection mode row**: label `connection_mode` → "Connection Mode" (hexpand) + linked
   segmented ToggleButton group: `window_type_auto` → "Auto" / `window_type_quality` → "Web" /
   `window_type_speed` → "Streaming" (note key≠English is deliberate store reuse — flag 8).
   `toggled` fires for the deactivated button too — apply only on the activating one.
4. **Fixed IP row**: `fixed_ip` → "Fixed IP" + Switch. Insensitive while Auto; Auto forces it off.
5. **Strong Anonymization row**: `strong_anonymization` → "Strong Anonymization" + Switch,
   default ON (nil profile → allow_direct false; the switch is the *inverted* `allow_direct`).
6. **Post Quantum Encryption row**: `post_quantum_encryption` → "Post Quantum Encryption" + Switch, default off.
7. **Kill switch row**: `kill_switch` → "Kill switch" + Switch. NOT part of the profile:
   handler (echo-guarded by `updatingKillSwitch_`) calls `host_.SetRouteLocal(!active)`;
   `RefreshKillSwitch()` reads `!host_.GetRouteLocal()`. GUI persists to LocalState and applies
   live over device RPC; with the tunnel down it restores at next start.

**Write path (`ApplyControls`, guarded by `updatingControls_`):** always writes a **whole**
`urnet::PerformanceProfile` — `allow_direct = !anon`, `post_quantum_encryption = pqe`;
Auto → `window_type = urnet::WindowTypeAuto`, no window size; Web/Streaming →
`WindowTypeQuality`/`WindowTypeSpeed` + `WindowSizeSettings{min,max}` = `{1,1}` fixed else
`{2,4}` → `host_.SetPerformanceProfile(profile)`. Read path maps
`window_type==Quality`→Web, `==Speed`→Streaming, else Auto; fixed-ip = `window_size` present
and min==max==1.

**Lands:** Home Pane A — the selected-provider row (→ LocationChooserSheet) and the
connect-options group exactly as §7.7 lists them: mode segmented Auto/Web/Streaming + toggle
rows fixed_ip / strong_anonymization / post_quantum_encryption / block_ads_and_trackers (the
blocker card joins this group, see 2.7). The kill-switch row RELOCATES to Settings → VPN &
privacy (§7.11) with its explanation notes; semantics caveat in flag 6.
**Verdict:** RELOCATE + RESTYLE (card rows → 40px pane rows; segmented control keeps 4-signal
selection discipline). All logic (profile read/write, echo guards, peer-name resolution,
Auto-forces-fixed-off) reuses verbatim.
**Accessors:** `SelectedLocation`, `ConnectedProvidePeers`, `GetPerformanceProfile`,
`SetPerformanceProfile`, `GetRouteLocal`, `SetRouteLocal`; `urnet::getColorHex`.

---

### 2.4 Client-stats card → Pane B chart + Pane C blocked chart + contracts entry

**Today:** `MakeCard(12)`: `MakeCardHeader("client_statistics" → "Client statistics")`;
`TransferChart("remote" → "Remote", Route::Remote, kUrGreen, kUrPink)`;
`TransferChart("blocked" → "Blocked", Route::Block, kUrCoral, kUrMutedCoral)`.
Whole card tappable → `ContractsSheet::Open()`.

**Lands:** the composite dies. Remote chart → **Pane B ACTIVITY** header chart (Height **150**;
header carries live ↓/↑ throughput from `LiveStats.down/upBitsPerSecond`). Blocked chart →
**Pane C** (Height **132**). The contracts entry becomes Pane C's contracts group row →
ClientContractsSheet. "client_statistics"/"remote"/"blocked" captions survive as
group/series labels.
**Verdict:** RELOCATE; chart RESTYLE per flag 1. **Accessors:** `ThroughputPoints`,
`ThroughputWindowSeconds` (shared single list, each chart picks its route), `ContractRows` et al. via the sheet.

### 2.5 Local-stats card → Pane C local chart + split-rules group

**Today:** `MakeCard(12)`: header `local_statistics` → "Local statistics";
`TransferChart("local" → "Local", Route::Local, kUrGreen, kUrPink)`; label
`TN_("split_rule_count", "{} split rule", "{} split rules", n)` where n = count of
`BlockActionOverrides()` entries with `RouteOverride && RouteOverride->Local`. Card tappable →
`SplitRulesSheet::Open()`.

**Lands:** local chart → Pane C (Height 132); the count label → Pane C split-rules group row
(→ SplitRulesSheet). **Verdict:** RELOCATE. **Accessors:** `ThroughputPoints`,
`BlockActionOverrides`.

### 2.6 Custom-DNS card → Pane C custom DNS group (near 1:1)

**Today:** `MakeCard(0)`: header `custom_dns` → "Custom DNS"; then:
- **Recommendation pill** (`.ur-dns-pill`, halign START, margin-bottom 8, spacing 6): optional
  country-color dot (`●` pango markup, hidden for the safe-defaults nudge) + wrapped text.
  Logic (`RefreshDnsPill`): no applied settings → hidden. Connected country
  (lowercased `SelectedLocation()->country_code`) with a
  `urnet::getRecommendedDnsResolverSettings(code)` → if `!DnsSheet::SettingsEqual(current,
  recommended)` show `dns_unapplied_recommended` → "There are unapplied recommended settings
  for {}" ({} = country name else UPPERCASED code) with the country dot; equal → hidden; a
  regional recommendation **never falls through** to the defaults check. Otherwise if
  `urnet::getDefaultDnsResolverSettings()` differs → `dns_default_not_applied` → "The default
  safe settings are not applied", no dot.
- **4 status rows** (v-box spacing 8; each h-row spacing 8): dot `●` (`.ur-dot-on` green /
  `.ur-dot-off` faint) + title (hexpand) + state label (`on` → "On" with `.ur-value-on` /
  `off` → "Off" dim caption). Row derivations from `GetDnsResolverSettings()`:
  `dns_over_https` → "DNS over HTTPS" = `EnableRemoteDoh || EnableLocalDoh`;
  `unencrypted_dns` → "Unencrypted DNS" = `EnableRemoteDns || EnableLocalDns`;
  `local_dns` → "Local DNS" = `EnableLocalDoh || EnableLocalDns`;
  `local_dns_fallback` → "Local DNS fallback" = `EnableFallback`.
- **Unavailable state**: rows hidden, dim label `dns_settings_unavailable` → "DNS settings
  unavailable" (distinct from all-off — this is the no-settings/no-device state).
Card tappable → `DnsSheet::Open()` (which returns false and does not present when unavailable).

**Lands:** Pane C custom DNS group — §7.7 lists exactly these 4 rows + the coral-subtle
unapplied-recommendation pill + "dns_settings_unavailable". **Verdict:** RESTYLE only (card →
28px group header + 34px key-value rows; pill unchanged). Logic verbatim.
**Accessors:** `GetDnsResolverSettings`, `SelectedLocation`;
`urnet::getRecommendedDnsResolverSettings`, `urnet::getDefaultDnsResolverSettings`,
`urnet::getColorHex`; `DnsSheet::SettingsEqual` (the canonical comparison — keep it public/static).

### 2.7 Blocker card → Pane A connect-options toggle row

**Today:** `MakeCard(0)`, one row (spacing 8): label `block_ads_and_trackers` → "Block ads and
trackers" (hexpand) + Switch. Handler (echo-guarded `updatingBlocker_`) →
`host_.SetBlockerEnabled(active)`; refresh reads `GetBlockerEnabled()`. Device applies +
persists immediately; tunnel-down writes land in LocalState and restore at next device creation.
**Lands:** last toggle row of Pane A's connect-options group (§7.7). **Verdict:** RELOCATE.
**Accessors:** `GetBlockerEnabled`, `SetBlockerEnabled`.

### 2.8 Plan card + UsageBar → Account PLAN pane

**Today** (`MakeCard(8)`):
- Caption `plan` → "Plan" (dim caption).
- Plan row (spacing 8): value label (`.title-2`, hexpand) = `guest` → "Guest" / `supporter` →
  "Pro" / `free` → "Free" (note: key `supporter` renders "Pro", key `become_supporter` renders
  "Get UR Pro" — store English wins; flag 11). Buttons (both `.suggested-action`, valign
  center): `create_account` → "Create account" visible iff guest → `on_create_account`;
  `become_supporter` → "Get UR Pro" visible iff `!isPro && !isGuest` → `OpenUpgrade()`.
- **UsageBar** (margin-top 8; see 2.17): `SetData(UsedByteCount, PendingByteCount,
  AvailableByteCount, StartBalanceByteCount, TotalReferrals)`.
- Flat button `redeem_balance_code` → "Redeem Balance Code" (halign START, margin-top 4) →
  `RedeemCodeSheet::Open()`.
All content re-derived in the single writer `RefreshPlanCard()` on `OnBalanceChanged()`.

**Lands:** Account **PLAN pane** (360dip, §7.9): plan value at 22sp (Pro wears gold `#FFC400` —
new), UsageBar + legend, daily balance, "Total Referrals: N" + "+N*30 GiB/Month", upgrade /
create_an_account (hidden for Pro), Redeem row. §7.9 additions with **no Linux code yet**:
progress ring during the post-checkout confirmation poll (`IsPolling()` is already exposed —
just render it) and **Manage Subscription** (Stripe customer portal; no API call exists in the
Linux tree — flag 5).
**Verdict:** RELOCATE + RESTYLE (card → pane groups; plan value 22sp condensed face; Pro gold).
**Accessors:** the whole `SubscriptionBalanceStore` read surface + `on_create_account` routing.

### 2.9 PQI panel → Settings PQI row + PostQuantumIdentitySheet

**Today** (`PostQuantumIdentityPanel`, `.ur-card`, drawer bottom):
- Title `post_quantum_identity` → "Post Quantum Identity" (dim, margin-bottom 12).
- **Own identity block** (hidden until device exposes the key; visibility =
  `!PublicIdentityKeyHash().empty() && !PublicIdentityKey().empty()`): identicon 80px
  (kPanelIdenticonSize; click → share sheet), hash label `.ur-mono-13` (display rule:
  4-char groups, first 4 + "…" + last 2; ≤6 groups shown whole; **click copies the FULL
  un-grouped hash** + toast `identity_key_hash_copied` → "Identity key hash copied"),
  client-id label `.ur-mono-11 .ur-label-faint` (click-copy + toast `client_id_copied` →
  "Client ID copied").
- **Peer deck row** (height 28, always visible): up to 5 overlapping 28px identicons
  (overlap 10 → step 18px, later children on top via Gtk::Fixed, each with a 2px
  card-background ring), + count label — `peer_count_one` → "1 peer" /
  `peer_count_other` → "%d peers" (printf key shared cross-platform, NOT a gettext plural —
  flag 12; "0 peers" keeps the row height). Row tappable → ProviderIdentitiesSheet.
- Footer `post_quantum_identity_explanation` → "Your identity key is stored locally on this
  device. If any peer's key appears different than their locally stored key, it means the
  network operator cannot be trusted." (dim caption, wrap).
- `Refresh()` re-reads own identity + `ReadProviderIdentityRows` (skips rebuilds via
  `SameIdentityRows` on (clientId, hash)); clears the raster cache when device down and deck
  empty; cascades to the open identities sheet.

**Identicon kit (REUSE-AS-IS, app-wide):** `IdenticonCache` keyed `(hash, size)`, always the
canonical `urnet::renderIdenticonPng(key, size*2)` (2x for hidpi), decoded via PixbufLoader;
`IdenticonWidget(size, ring)` draws the raster clipped to rounded rect radius = size/6, quiet
0.15-alpha placeholder holds footprint; sizes: deck 28, list row 40, panel 80, share 320,
badge `kBadgeIdenticonSize = 16`. `ReadProviderIdentityRows(host)`: decode base64 `PublicKey`,
hash via `urnet::publicIdentityKeyHash(data,len)`, skip empty ClientId/key.

**ProviderIdentitiesSheet:** modal 480×560, hide-on-close, Esc closes, AdwToastOverlay; rows
(margin 12/12, spacing 16): identicon 40 + column (spacing 4) of hash (`.ur-mono-13`,
click-copy full) and client id (`.ur-mono-11 .ur-label-faint`, click-copy), separator after
each; full rebuild per change, skipped when `SameIdentityRows`.

**PostQuantumIdentityShareSheet:** modal 420×560; centered column margins 24: title (dim,
margin-top 32/bottom 24), identicon 320 (margin-bottom 24), FULL grouped hash `.ur-mono-15`
(max-width-chars 40, center, margin-bottom 8), client id `.ur-mono-12` dim (margin-bottom 32),
`suggested-action` button [send-to-symbolic + `share` → "Share"] (margin-bottom 32). Share =
clipboard "hash\nclientId" + toast `site_app_copied` → "Copied", then GtkFileDialog save of
the canonical png (`urnet::renderIdenticonPng(key, 640)`), initial name
"urnetwork-identity.png".

**Lands:** Settings → VPN & privacy "post-quantum identity row + sheet" (§7.11, §7.14
PostQuantumIdentitySheet). The panel body becomes the sheet body; the identities list and
share sheet hang off it unchanged. The badge join (`ReadProviderIdentityRows` +
`kBadgeIdenticonSize`) is already shared with ProviderLocationsSheet — keep sharing.
**Verdict:** RELOCATE (panel→sheet); everything else REUSE-AS-IS.
**Accessors:** `ProviderIdentities`, `PublicIdentityKeyHash`, `PublicIdentityKey`, `ClientId`;
`urnet::renderIdenticonPng`, `urnet::publicIdentityKeyHash`.

---

### 2.10 ContractsSheet → ClientContractsSheet (REUSE-AS-IS)

**Today:** modal 480×560, hide-on-close, Esc; AdwToastOverlay → Gtk::Overlay → scroller →
Gtk::Stack {empty, list}. Title by mode: `client_contracts` → "Client contracts" /
`provider_contracts` → "Provider contracts" (only Client is wired; a provider sheet would open
its own VC via `openProviderContractDetailsViewController`). Empty state (centered):
`no_open_contracts` → "No open contracts" + detail `contracts_appear_connected` → "Contracts
appear here while connected." (provider: `contracts_appear_providing` → "Contracts appear
here while providing."). List margin 16; one row per peer: full client id `.ur-mono-13`
wrap-CHAR click-copy (+ toast `client_id_copied`), then the two stacks h-box spacing 20 —
send (kUrGreen, mirrored, header `send` → "Send", ejects Leading/left) | receive (kUrPink,
header `receive` → "Receive", ejects Trailing/right); row margins 16/16 + separator; new row
fades in 250ms.
**Stack geometry/motion (exact):** circle slot 56×56; min ring diameter 16; outer-ring
diameter = 56·√(total/stackMax) clamped [16,56] (area-proportional); inner disc =
diameter·√(usedFraction), min 4px, fill alpha .3 + stroke .6 width .5; active (bitRate>0)
brightens ring alpha .55→1.0 and width 1→1.5; stream contract draws a second concentric ring
4px radially outside (`kStreamRingGap`); block gap 4 (pitch 60); stats gap 10; stats: used
`.ur-caption-11` + `of_total` → "of {}" `.ur-caption-10` dim. Eases (cubic-out on frame
clock): value/diameter/brightness 500ms; settle 500ms; leaver slide-off 400ms (distance
4×56 = 224px toward the eject edge, slot held open until the last leaver clears); arrival
fade 350ms; removed-row fade 250ms. Per-stack footer `contract_stack_max` → "max {}"
(`.ur-caption-10 .ur-label-faint`, opacity holds height); stack header carries the cumulative
run byte count (`FormatByteCountCompact`).
**Scroll contract:** at-top = vadjustment ≤ 4.0 → `SetContractsAtTop(bool)`; the SDK VC owns
membership/order (at top re-sorts active-above-idle; scrolled away freezes and collects);
floating chip `.ur-newchip` (go-up icon 10px + `TN_("new_items_count","{} new","{} new",n)`)
shown iff `!atTop && ContractsPendingCount()>0`; click → report at-top + scroll to 0.
`Open()` seeds at-top true. Rows reconciled by client-id key (identity-stable stacks),
reordered with `gtk_box_reorder_child_after`.
**Lands:** Pane C contracts group → ClientContractsSheet (§7.14 describes exactly this).
Advanced Mode shows full client ids (already does). **Verdict:** REUSE-AS-IS.
**Accessors:** `ContractRows`, `SetContractsAtTop`, `ContractsPendingCount`.

### 2.11 LocationsSheet → Network Pane A feeds + LocationChooserSheet

**Today:** modal 480×600, hide-on-close, Esc; root margin 16 spacing 12; title
`browse_locations` → "Browse Locations". Fixed `Gtk::SearchEntry` placeholder
`search_providers_input_placeholder` → "Search countries, states, cities..." →
`FilterLocations(query)` (SDK debounces; results re-emit as a Locations event → Refresh).
Status label (dim, wrap) under it. Sections (v-box spacing 12; captions via `MakeCaption`, row
groups spacing 4, rows margins 2/2 spacing 12):
1. `network_peers` → "Network peers" — pinned first, hidden when none; rows from
   `ConnectedProvidePeers()` (connected AND provide-enabled only): dot colored
   `getColorHex(clientId)`, name `PeerDisplayName`, caption = DeviceSpec only when a distinct
   DeviceName shows, trailing green `network-transmit-receive-symbolic` (providing), check
   `object-select-symbolic` when selected. Click → build `ConnectLocation{connect_location_id
   {client_id}, name}` → `host_.Connect(location)` → hide.
2. Searching → `top_matches` → "Top Matches" (filtered->BestMatches); idle →
   `promoted_locations` → "Promoted Locations" caption over the single best-available row
   (dot kUrCoral, `best_available_provider`) → `host_.ConnectBestAvailable()` → hide.
   (Both apps deliberately ignore the SDK Promoted list.)
3. `countries` → "Countries", `regions` → "Regions", `cities` → "Cities", `devices` →
   "Devices" from `GetFilteredLocations()`. Location rows: dot = country code for
   LocationTypeCountry else location/client/group id via `getColorHex`; name; caption
   `provider_count_int` → "{} providers" (count>0); `dialog-warning-symbolic` iff `!stable`;
   green `security-high-symbolic` iff `strong_privacy`; check when selected (selection
   matched on location_id OR client_id OR location_group_id). Click →
   `host_.Connect(locationCopy)` → hide.
No-results (only while searching, nothing anywhere incl. peers): `no_providers_found` → "We
could not find any providers."

**Lands:** two consumers of the same feeds (§7.8): the modal **LocationChooserSheet** from
Home Pane A's provider row (this sheet, as-is) and the **Network destination Pane A** (search
row + 36px rows + group headers with counts + Pane B detail 400dip). Pane A is a new build in
the pane vocabulary, reusing this sheet's row logic, colors, icons and connect actions.
**MANDATORY fix during the pane build (flag 3):** three distinguishable states Loading /
Failed / genuinely-empty via `GetFilteredLocationState()` — implemented in SdkHost
(wraps `LocationsViewController::getFilteredLocationState()`) but **never called by any UI
today**; the sheet's single no-results line does not meet §7.8. Also §7.8: detail pane uses
only real `ConnectLocation` fields — NO latency/load columns.
**Verdict:** chooser REUSE-AS-IS (plus the 3-state fix); Network Pane A REWRITE on shared logic.
**Accessors:** `GetFilteredLocations`, `FilterLocations`, `GetFilteredLocationState`,
`ConnectedProvidePeers`, `SelectedLocation`, `Connect`, `ConnectBestAvailable`;
`urnet::getColorHex`; `PeerDisplayName` (exported helper).

### 2.12 DnsSheet → DnsEditorSheet (REUSE-AS-IS)

**Today:** modal 480×540, hide-on-close, Esc; title `custom_dns` → "Custom DNS". `Open()`:
`GetDnsResolverSettings()` — nullopt → return false (never presents); `Normalize` engages all
8 optional lists; draft_ + original_ copies; loads country (lowercased code + name from
`SelectedLocation()`), `recommendation_ = urnet::getRecommendedDnsResolverSettings(code)`,
`defaults_ = urnet::getDefaultDnsResolverSettings()`; rebuild suggestions; `SyncFromDraft`.
Layout (scroller; form margin 16 spacing 16):
- **Panel** (`.ur-banner`, spacing 12), rebuilt per state: with a recommendation —
  equal → status row [country dot + `dns_using_recommended` → "Using recommended regional
  settings"]; differing → message `dns_recommendation_message` → "The strongest security
  rules are known not to work in {}. There are less secure recommended DNS settings that work
  better." ({} = country name ▸ UPPER code ▸ `this_region` → "this region") + pill button
  `use_recommended_settings` → "Use recommended settings" (applies target to draft +
  SyncFromDraft). Without a recommendation but with defaults — equal → [green ✔ `#87fb67` +
  `dns_using_secure` → "Using most secure default settings"]; differing →
  `dns_restore_secure_message` → "Restore the most secure settings: encrypted DNS over HTTPS
  through the tunnel." + `restore_most_secure_settings` → "Restore to most secure settings".
- Caption `resolvers` → "Resolvers"; card (spacing 10) of 4 switch rows (title + dim caption
  detail + hexpand spacer + Switch): `dns_over_https`+`remote_lowercase` → "remote" ↔
  `EnableRemoteDoh`; `dns_over_https`+`local_lowercase` → "local" ↔ `EnableLocalDoh`;
  `unencrypted_dns`+remote ↔ `EnableRemoteDns`; `unencrypted_dns`+local ↔ `EnableLocalDns`.
  Every switch writes its draft flag (guard `updating_`), then RebuildPanel + RefreshDirty.
- Fallback card: `local_dns_fallback` ↔ `EnableFallback`; footer
  `local_dns_fallback_description` → "Races a local resolver while the tunnel starts. When
  off, DNS only resolves through the tunnel."
- **Suggestions**: caption `suggested_remote_dns_servers` → "Suggested remote DNS servers";
  card (spacing 10); rows from `urnet::getRegionalDnsServers()` sorted connected-country
  first, then code, then name; connected-country rows carry the country color dot; row =
  [dot?] + column(name + UPPER code caption; ipv4 `.ur-mono-12` dim) + Switch. ON adds the
  ipv4 to `draft_.RemoteDnsIpv4` AND sets `EnableRemoteDns = true` (mirrors the resolver
  switch, guarded); OFF removes it; state derived by membership (`SyncSuggestionRow`).
  Footer `suggested_remote_dns_servers_description` → "Suggestions for the connected country
  are marked with its color. Turning one on adds it to the remote DNS servers." Section
  hidden when the SDK returns none.
- **4 list sections × 2 sublists** (`ipv4` → "IPv4" / `ipv6` → "IPv6"): `remote_doh_urls` →
  "Remote DoH URLs" (placeholder literal "https://"), `local_doh_urls` → "Local DoH URLs",
  `remote_dns_servers` → "Remote DNS servers" (placeholder `ip_address` → "IP address"),
  `local_dns_servers` → "Local DNS servers". Value rows: `.ur-mono-12` ellipsize-MIDDLE +
  flat `window-close-symbolic` remove. Add row: entry + flat `list-add-symbolic`; add button
  sensitive only for a trimmed, valid (`IsValidDohUrl` https+host / `IsIpAddressValue`
  v4-or-v6), non-duplicate value; Enter also adds. Every mutation re-syncs suggestions,
  panel, dirty.
- Action bar (margin 12): pill `update` → "Update", `.suggested-action`, sensitive iff
  `!SettingsEqual(draft_, original_)` → `SetDnsResolverSettings(draft_)` + hide.
**Lands:** DnsEditorSheet from Pane C custom-DNS group (§7.14 lists this content verbatim:
recommendation panel, 5 switches, regional suggestions, 8 lists with validation).
**Verdict:** REUSE-AS-IS. **Accessors:** `GetDnsResolverSettings`, `SetDnsResolverSettings`,
`SelectedLocation`; `urnet::getRecommendedDnsResolverSettings`,
`urnet::getDefaultDnsResolverSettings`, `urnet::getRegionalDnsServers`, `urnet::getColorHex`.

### 2.13 SplitRulesSheet → SplitRulesSheet (REUSE-AS-IS)

**Today:** modal 480×540, hide-on-close, Esc; title `split_rules` → "Split rules"; scroller,
content margin 16 spacing 12. Info banner (`.ur-banner`): `split_rules_info_note` →
"Exclusions apply to the whole co-associated network cluster, so related traffic is caught
together." Caption `rules` → "Rules" over rulesBox (spacing 4); activity header row: caption
`activity` → "Activity" (hexpand) + counts label `allowed_blocked_counts` → "{0} allowed ·
{1} blocked" from `BlockStatsSnapshot()` (empty when both 0); activityBox (spacing 4).
`Open()` forces rebuild (`built_ = false` — relative times go stale); `Refresh()` re-reads and
rebuilds only changed sections (value-compared vectors).
- **Rules** = `BlockActionOverrides()` entries with `RouteOverride->Local`. Empty:
  `split_rules_hint` → "Tap traffic below to route it locally, bypassing the tunnel." Row:
  wrapping chip flow (FlowBox spacing 6/6) of green-hi chips — host base names via
  `urnet::collapseHostNames` then exact IPs (split by `IsIpAddressValue`) — + green-hi chip
  `local` → "Local" + flat trash `user-trash-symbolic` (→ `RemoveBlockActionOverride(id)` +
  Refresh — note: no override event fires on the local-state fallback path, hence the manual
  Refresh). Row click → editor pre-loaded with the rule.
- **Activity** = `BlockActions()` reversed (newest first). Empty:
  `split_rules_activity_hint` → "Routing activity appears here while connected." Row: chip
  flow — matched hosts + matched IPs as green-hi chips first (disjoint from the rest), then
  collapsed hostnames as muted chips, then one muted `TN_("ip_count","{} IP","{} IPs",n)`
  pill; caption `RelativeTime((now-timeMs)/1000)` + "  " + `FormatByteCountCompact(byteCount)`
  when bytes>0 (`.ur-caption-11` dim). Decision chips: `blocked` → "Blocked" (coral) /
  `allowed` → "Allowed" (muted), highlighted iff `BlockOverride` present; `local` → "Local"
  (green) / `remote` → "Remote" (muted), highlighted iff `RouteOverride` present. Click →
  editor.
- **Editor** (owned Gtk::Window 400×380, modal over the sheet, hide-on-close, Esc; margin 16
  spacing 12): title `edit_split_rule` → "Edit split rule" / `new_split_rule` → "New split
  rule"; subtitle `split_rule_description` → "Selected hosts are routed locally and bypass
  the tunnel."; scrolling checklist (spacing 4) of candidate values — for an action row:
  matchedHosts + hosts + matchedIps + ips in that order, all **unselected** for a new rule
  (common case = picking a few server names); an action decided by a still-existing rule
  edits that rule with candidates = OrderedUnion(rule.hosts, action values) and the rule's
  hosts pre-checked. Primary pill `update` → "Update" (editing) / `create` → "Create" (new),
  sensitive iff editing or ≥1 checked. Apply: editing + empty selection →
  `RemoveBlockActionOverride`; editing → `SetBlockActionOverrideHosts(ruleId, hosts)`; new →
  `AddBlockActionOverride({OverrideId: urnet::newId(), Hosts, RouteOverride{Local:true}})`.
  Editing also shows flat destructive `remove_rule` → "Remove rule".
**Lands:** SplitRulesSheet from Pane C split-rules group (§7.14). **Verdict:** REUSE-AS-IS.
**Accessors:** `BlockActionOverrides`, `BlockActions`, `BlockStatsSnapshot`,
`AddBlockActionOverride`, `SetBlockActionOverrideHosts`, `RemoveBlockActionOverride`;
`urnet::collapseHostNames`, `urnet::newId`.

### 2.14 UpgradeSheet → UpgradeSheet (REUSE-AS-IS)

**Today:** modal 440×natural, NOT resizable, hide-on-close, Esc; title literal "UR Pro"
(product name, never translated); root margin 24. States
Options/Launching/Checkout/Waiting/Success/TimedOut (`SetState` is the only visibility writer;
Launching spins `joinSpinner_` and de-sensitizes join + both option cards).
- **Options:** `become_a` → "Become a" (.title-3), `urnetwork_supporter` → "URnetwork
  Supporter" (.title-1); `support_us` → "Support us in building a new kind of network that
  gives instead of takes." (dim, margin-top 16); `unlock_speed` → "You'll unlock even faster
  speeds, and first dibs on new features like robust anti-censorship measures and data
  control." (dim, margin-top 8). Option cards (`.ur-option` ToggleButtons, grouped):
  `yearly` → "Yearly" + `includes_2_week_free_trial` → "Includes 2 week free trial",
  preselected, margin-top 20, with overlay chip green-hi `most_popular` → "Most Popular"
  (halign END, valign START, margin-end 12); `monthly` → "Monthly" (margin-top 12). Prices
  deliberately absent (Stripe checkout shows them). Join: pill `.suggested-action`
  [spinner + `join_the_movement` → "Join the movement"], margin-top 20. Inline error
  `.ur-error-text` caption. Terms (dim caption, margin-top 16, pango links via
  `MarkdownLinksToPango`): `by_subscribing_you_agree_to_urnetwork_s_terms` → "By subscribing,
  you agree to URnetwork's [Terms and Services](https://ur.io/terms) and [Privacy
  Policy](https://ur.io/privacy)".
- **Checkout flow:** `StartCheckout` → embedded first iff webkit usable at runtime
  (`EnsureWebView`), else hosted. `RequestSession(embedded)` calls
  `api().createStripeCheckoutSession({item_id: monthly? "pro_monthly" : "pro_yearly",
  ui_mode: "embedded"|"hosted"})` with an **epoch guard** (`shared_ptr<uint64_t>` bumped on
  every `Open()` — a stale callback is dropped). Embedded success needs `client_secret`;
  ANY embedded failure retries ONCE as hosted (nothing shown yet → nothing lost). Hosted:
  `g_app_info_launch_default_for_uri(checkout_url)`; failure → back to Options + inline error
  (server message ▸ `something_went_wrong_please_try_again_later` → "Something went wrong.
  Please try again later."); success → waiting headline `checkout_opened_in_browser` →
  "Complete your purchase in the browser. Your plan updates here automatically once payment
  is confirmed." + `balance_.StartConfirmationPolling()` → Waiting.
- **Embedded (UR_HAVE_WEBKIT only):** checkout box = header ["UR Pro" .title-3 + flat
  circular close → Options] + webview slot (vexpand, min height 560); webview bg painted
  kUrBackground pre-load. URL = `https://ur.io/checkout?client_secret=…&redirect_link=
  urnetwork://checkout`. decide-policy: `urnetwork://` → ignore + `HandleCheckoutCallback`
  (deferred via PostToMain; `status=complete` → waiting headline `processing_payment` →
  "Processing payment" + StartConfirmationPolling → Waiting; else error from `errorMessage`
  query param → Options + inline error); NEW_WINDOW actions → system browser. load-failed
  (never rendered, once per attempt) → hosted retry; web-process-terminated → Options +
  generic error. Sheet hidden mid-Checkout → Options (webview torn down so a stale payment
  page never lingers). All teardown deferred via PostToMain (never destroy the emitter inside
  its own signal).
- **Waiting:** spinner 32 + headline (.title-3, center) + `processing_subscription_balance` →
  "Processing subscription balance..." (dim caption) + `close` → "Close" (poll keeps running
  in the store). **Success** (mac PurchaseSuccessView): `you_re_premium` → "You're premium."
  (.title-1) + `thanks_for_building_the_new_internet_with_us` → "Thanks for building the new
  internet with us" + pill Close. **TimedOut:** `alarm-symbolic` 40px dim +
  `waiting_for_approval` → "Waiting for approval" (.title-3) + `purchase_confirmation_timeout`
  → "We couldn't confirm your purchase yet. If you completed checkout, your plan will update
  automatically in a few minutes — there's no need to buy again." + `got_it` → "Got it"
  (clears the store's timed-out flag). `OnBalanceChanged`: Waiting → Success on IsPro, →
  TimedOut on PurchaseConfirmationTimedOut; **TimedOut is NOT terminal** — a late Pro
  snapshot flips it to Success. `Open()` re-enters Waiting when `IsPolling()`.
**Lands:** UpgradeSheet (§7.14: "yearly/monthly cards → embedded Stripe checkout webview with
hosted-browser fallback → waiting/success/timeout pages" — matches this code exactly), opened
from Account PLAN upgrade + BalanceWarning + Earnings upgrade button. **Verdict:** REUSE-AS-IS.
**Accessors/API:** `api().createStripeCheckoutSession`; `SubscriptionBalanceStore`
(StartConfirmationPolling, IsPro, IsPolling, PurchaseConfirmationTimedOut,
ClearPurchaseConfirmationTimeout).

### 2.15 RedeemCodeSheet → RedeemCodeSheet + Account Pane C CODES

**Today:** modal 420×natural, NOT resizable, hide-on-close, Esc; title `redeem_code` → "Redeem
Code"; root margin 24; epoch guard bumped on Open (invalidates in-flight redeem/list callbacks).
- **Entry state:** caption `balance_code` → "Balance code"; entry `.ur-mono-13`, max length
  **26** (`kBalanceCodeLength`), placeholder `enter_balance_code` → "Enter balance code";
  changed → clear error + `redeemBtn` sensitive iff trimmed length == 26 && !redeeming;
  Enter submits. Error label `.ur-error-text` caption, default `invalid_balance_code` →
  "Invalid balance code". Hint (dim caption, margin-top 4): `balance_code_where_to_buy` →
  "Don't have a code? Data codes can be purchased at ur.io and emailed to you." (plain text,
  no link — store-policy). Action row (halign END, margin-top 16): spinner + `cancel` →
  "Cancel" + `redeem` → "Redeem" (`.suggested-action`). Redeeming de-sensitizes entry+button.
- **Redeem:** `api().redeemBalanceCode({secret})`. Transport failure/no result →
  `balance_code_transport_error` → "We couldn't reach the server — check your connection. If
  you were charged, the code may already be applied; check your balance before trying again."
  (NEVER "invalid", never error-styles the entry — the server may have committed). Server
  error → its message ▸ `invalid_balance_code`, entry gets `.error`. Success → success box
  [`emblem-ok-symbolic` 40 green + `balance_code_redeemed` → "Balance code redeemed."
  (.title-3) + `processing_subscription_balance` + Close] + `StartConfirmationPolling()` +
  `RefreshCodes()`.
- **History** (below both states; separator margin 16/12; caption `balance_codes_title` →
  "Balance Codes"): `api().getNetworkRedeemedBalanceCodes` → grid (col spacing 16, row 4)
  headers `code` → "Code" / `data` → "Data" / `redeemed` → "Redeemed" / `expires` →
  "Expires" (dim caption); rows: masked secret "abc...xyz" (first/last 3; ≤6 chars → all
  dots) `.ur-mono-12`, "+"+FormatByteCountCompact(balance_byte_count), ISO date prefixes
  (YYYY-MM-DD) of redeem_time / end_time. Scroller: natural height, max 240. **Three
  states:** list (non-empty) / `no_balance_codes_found` → "No balance codes found" (ONLY on a
  successful empty fetch) / `balance_codes_load_error` → "Couldn't load your balance codes.
  Check your connection and try again." (`.ur-error-text` — a failed fetch is never "empty").
**Lands:** entry sheet from Account PLAN Redeem row (§7.14: "26-char code, inline
validation"); the embedded history RELOCATES to **Account Pane C CODES** (380dip, ≥1500;
§7.9 "redeemed balance-code table") as a `MakePaneTableRow` table — keep the exact columns,
masking rule, and the three-state discipline. **Verdict:** SPLIT — entry REUSE-AS-IS,
history RELOCATE.
**API:** `api().redeemBalanceCode`, `api().getNetworkRedeemedBalanceCodes`;
`SubscriptionBalanceStore::StartConfirmationPolling`.

### 2.16 ProviderLocationsSheet → same sheet from Pane C (REUSE-AS-IS)

**Today:** modal 460×720, hide-on-close, Esc; AdwToastOverlay; title
`provider_locations_title` → "Provider Locations". HeaderBar (no title buttons): flat
`go-previous-symbolic` back (tooltip `close` → "Close") + small title (`.ur-caption-11` dim) —
explicit dismiss for WMs with no decorations. Then (Linux-only) the **GeoClue location-override
section** (margins 16/16/12): row = wrapped label `sync_device_location_with_provider` →
"Sync device location with provider" + `help-about-symbolic` MenuButton popover (320 wide,
3 wrapped paragraphs: `mock_location_guide_intro` → "When enabled, apps on this device see the
location of the provider you have been connected to the longest, instead of your real
location.", `location_override_coverage_geoclue` → "Apps that ask the system for your location
through GeoClue — including GNOME Settings and Maps, Firefox, and sandboxed Flatpak and Snap
apps — report the provider's location instead of yours.", `location_override_coverage_gaps` →
"It does not cover everything. Chrome and KDE Plasma never ask GeoClue, and any app that works
out your location from your IP address on its own is unaffected. Firefox falls back to its own
lookup if the system does not answer quickly." + flat button `mock_location_guide_title` →
"Sync device location" → LocationOverrideGuide) + echo-guarded Switch → `SetEnabled`;
enabling with incomplete setup opens the guide (checks `setupComplete()`, not `status`).
Note line (dim `.ur-caption-11`, always present): NeedsGeoClue/NeedsStaticSource/NeedsPrivilege
→ `mock_location_needs_setup` → "Setup required"; Eligible →
`mock_location_waiting_for_provider` → "Waiting for a provider location"; Active →
`mock_location_active` → "Syncing with {}" (target label) ▸ `use_most_stable_provider` → "Use
most stable provider"; Orphaned → `location_override_cleanup_required` → "URnetwork could not
remove the system location file, so this machine may still report a provider's location. Start
the URnetwork system service and turn this off, or remove /etc/geolocation as an
administrator."; Disabled/ErrorTransient → `use_most_stable_provider`. `Open()` re-probes the
on-disk setup signals (no change notification exists).
Fixed **ProviderGlobe** (margins 8/8; color resolver = country color; `on_select` → Select,
`on_step` → `StepProviderSelection(steps)` — SDK clamps at the extremes, no wraparound).
Status label (dim, wrap, center, margin 24) — **two distinct empties:** device unavailable →
`provider_locations_unavailable` → "Provider details are unavailable until connected";
genuinely none → `provider_locations_empty` → "No providers connected". Scrolling list:
rows in SDK **display order verbatim** (west→east about centroid, then no-coordinates;
NOT duration order — the override target is found by stamp elsewhere). Row (margins 16 h /
12 v, spacing 16): `ProviderDotArea` — box 31px, solid dot Ø20 country color (fallback web
`#0099FF`), selection ring gap 4 stroke 1.5 (box sized for the ring so columns never shift);
column (spacing 2): id-row (spacing 6) = client id `.ur-mono-11` ellipsize END hexpand
(tooltip `copy_to_clipboard` → "Copy to Clipboard", click-copy + toast `client_id_copied`) +
16px identicon badge ONLY on a verified-e2e join hit (tooltip `post_quantum_encryption`;
absence = not-e2e, no placeholder); place (ellipsize; `provider_location_unknown` → "Location
unknown" when empty); coordinates (dim `.ur-caption-11`); duration (dim `.ur-caption-11`) —
`provider_connected_duration_hours` → "{0}h {1}m" / `_minutes` → "{}m" / `_seconds` → "{}s",
ticking on ONE 1000ms timer for the whole list (stops when hidden), computed locally against
`connectedSinceMillis`. Trailing flat destructive `window-close-symbolic` remove (tooltip
`remove` → "Remove"; de-sensitized on press): **optimistic removal** — row trimmed locally +
`pendingRemovals_` suppresses it from every read until the SDK stops reporting it (no
flicker-back), then `RemoveConnectedProvider(clientId)`; the VC hands selection to the
nearest remaining. Row click → `SetSelectedProviderClientId` (VC = source of truth; echo
lands via ProviderSelection event → `RefreshSelection` → ring + faint/normal id + globe +
minimal `ScrollSelectedIntoView` deferred to idle). Refresh dedupes by value (rows AND the
identity set compared separately — a newly sealed provider must gain its badge without a
location change). `MapConnectedProviderLocations`: city centroid ▸ region centroid ▸ listed
unplotted.
**Lands:** the same sheet, opened from Pane C data_usage "Connected to N providers" (§7.7,
§7.14 — the doc's row spec matches this code). The override section is a **Linux platform
feature with no Windows analogue — KEEP it** (the doc treats it as real and supported).
**Verdict:** REUSE-AS-IS. **Accessors:** `ConnectedProviderLocations`,
`RemoveConnectedProvider`, `SelectedProviderClientId`, `SetSelectedProviderClientId`,
`StepProviderSelection`, `ProviderIdentities` (badge join via `ReadProviderIdentityRows`);
`urnet::getColorHex`; LocationOverrideController (Linux-only).

### 2.17 SubscriptionBalanceStore → Account/PLAN backing store (REUSE-AS-IS)

**Today (all state on the GTK loop; near-literal mac SubscriptionBalanceViewModel port):**
- `Start()` (login): seed Pro/Guest offline from `ParseByJwt()`, fetch once, then background
  poll **30s** while NOT Pro (a Pro network with balance does not poll). `Stop()` (logout):
  timers stopped, epoch bumped (stale results can never repopulate the next session), state
  cleared. `SetWindowVisible(bool)` gates ALL timers (hidden tray window never churns);
  re-show resyncs with an immediate fetch.
- Fetch = `api().subscriptionBalance` → used/pending/available/start bytes +
  `current_subscription` → isPro; `api().getNetworkReferralCode` rides the same cycle →
  referral code + total referrals.
- **Confirmation poll** (post checkout/redeem): **5s** interval with a **120s ACTIVE-time
  budget** (not wall clock — pauses with the timers while hidden/unfocused, banks the
  remainder, resumes with an immediate poll). Budget spent → `purchaseConfirmationTimedOut_`
  (the UpgradeSheet TimedOut state) + fall back to background poll; a late Pro confirmation
  still clears it.
- **Offline Pro + jwt refresh:** jwt claim seeds; server is truth; `RefreshJwt()`
  (`Device::refreshToken`) whenever the two disagree in EITHER direction; `OnJwtRefreshed()`
  re-derives Pro immediately (catches a Pro→free lapse a paused poll would miss).
- `DidDetectUpgradeToPro()` sticky flag → MainWindow applies `ResetProvideToNever()` exactly
  once per session (free→Pro resets provide mode; user can opt back in).
**Lands:** the data layer for Account PLAN pane, BalanceWarning, UpgradeSheet, RedeemCodeSheet,
and Earnings' upgrade gating. §7.4 wires Account loads on destination selection + auth
change — keep the store's poll as the richer superset (open question in flags).
**Verdict:** REUSE-AS-IS.
**API:** `api().subscriptionBalance`, `api().getNetworkReferralCode`,
`SdkHost::ParseByJwt`, `SdkHost::RefreshJwt`, (side effect) `SdkHost::ResetProvideToNever`.

### 2.18 TransferChart → Pane B/C charts (RESTYLE heights)

**Today:** `Gtk::DrawingArea`, content height **128**, hexpand. Mirrored around a center
axis: egress above / ingress below, newest at the right sliding left. Per route
(Remote/Local/Block) extracts its `ThroughputSample` from the shared `ThroughputPointList`
(SDK time unix ms → s; window default 60s from `ThroughputWindowSeconds`). Byte series
(line 1.5, fill-to-center alpha .07, stroke alpha .9) and packet series (line 1.0, no fill)
on independent auto-scales — floors **1024 bytes / 8 packets** — easing to new maxima with a
**0.5s cubic ease-out**; newest bucket lerps from the previous over 0.5s; left edge
zero-padded to window start, right edge holds latest to now. Catmull-Rom smoothing
(tension /6), per-direction clip so smoothing never crosses the axis; axis = 1px
kUrBorderBase full width. Bands: stats 30px top, peak 13px top+bottom; plot half min 8.
Title 11px sans muted top-left. Top-right rolling **5-bucket** averages, two rows (egress
y 7 ▲, ingress y 20 ▼): triangle 7px (lit `kUrText` when active else muted), mono 10 packet
rate then byte rate right-aligned, each dimmed to alpha .4 at zero. Sliding peak byte-rate
labels (mono 9 + triangle 6, gap 3): peak egress over the top band, peak ingress under the
bottom band, x tracking the bucket time clamped fully inside. Redraw timer **100ms (~10fps)**
runs ONLY while animated: recent activity in-window OR newest-bucket lerp OR a scale ease
settling; stops when unmapped; restarts on map/SetPoints. Formatters: `FormatByteRate`
("1.2 KiB/s"), `FormatPacketRate` ("340 pkt/s").
**Lands:** Pane B activity chart (**Height 150** — §7.7) and Pane C blocked + local charts
(**Height 132**). **Verdict:** RESTYLE — parameterize the height (128 → 150/132, flag 1);
everything else is already the documented behavior. **Accessors:** `ThroughputPoints`,
`ThroughputWindowSeconds` (one shared list, three subscribers).

### 2.19 UsageBar → Account PLAN pane (REUSE-AS-IS)

**Today:** v-box spacing 8. Bar: DrawingArea height **32**, rounded capsule radius **12**
(clip), three segments left→right — used **kUrElectricBlue `#0039DE`**, pending **kUrCoral
`#FF6C58`**, available **kUrTextFaint `#5A5A5A`**; non-zero segments floored at **1.5%**
(`kMinFraction`, mac minNonZeroValue) then renormalized; 0.5px right overshoot kills seams;
all-zero → faint full-width track (alpha .35). Legend (spacing 12; dot+label spacing 4):
`used_data_key` → "Used", `pending_data_key` → "Pending", `available_data_key` → "Available".
Daily row: `daily_data_balance_label` → "Daily Data Balance:" (dim, hexpand) +
`FormatByteCountCompact(dailyBalance)`. Separator. Referral row:
`total_referral_count` → "Total referrals: {}" (hexpand) + `referral_bonus` → "+{} GiB/Month"
with {} = totalReferrals × 30.
**Lands:** Account PLAN pane (§7.9 names exactly these colors/roles). **Verdict:** REUSE-AS-IS.
**Inputs:** SubscriptionBalanceStore bytes + referrals.

### 2.20 Formatters → app-wide (REUSE-AS-IS)

`FormatByteCountCompact`: IEC base-1024 — "<1024 → N B", then KiB/MiB/GiB/TiB with magnitude
precision ≥100→%.0f, ≥10→%.1f, else %.2f. `FormatByteRate` = compact + "/s".
`FormatCountCompact`: base-1000 — <1000 plain; <1e4 "%.1fk"; <1e6 "%.0fk"; else "%.1fM".
`FormatPacketRate` = count + " pkt/s". `FormatBitRate`: bps/Kbps/Mbps/Gbps same magnitudes
(the status strip's `↓ 24.6 Mbps` needs exactly this). `RelativeTime`: <5s `now` → "now";
<60 `seconds_ago_abbrev` → "{}s ago"; <3600 `minutes_ago_abbrev` → "{}m ago"; else
`hours_ago_abbrev` → "{}h ago". `IsIpAddressValue` = inet_pton v4/v6.
`IsValidDohUrl` = parses with scheme https + non-empty host. `TrimWhitespace`.
**Lands:** everywhere (§7 uses these formats verbatim). **Verdict:** REUSE-AS-IS.

### 2.21 Tray → §7.15 tray (REWRITE toward parity)

**Today:** hand-rolled SNI over GDBus (deliberately no libappindicator — GTK3 in-process; keep
this) + `com.canonical.dbusmenu` served in-process. Service
`org.kde.StatusNotifierItem-<pid>-1`; objects `/StatusNotifierItem` + `/MenuBar`. Props:
Category ApplicationStatus, Id "urnetwork", Title "URnetwork" (product name, untranslated),
Status Active, **IconName `urnetwork-tray-connected` / `urnetwork-tray-disconnected`** (2
states only), IconThemePath = runtime-resolved `<pkgdatadir>/icons` ($APPDIR-aware; empty →
"use the theme"), ItemIsMenu false. Menu (dbusmenu Version 3): id 1 `connect` → "Connect" /
`disconnect` → "Disconnect" (by state), separator, id 3 `show_urnetwork` → "Show URnetwork",
id 4 `quit` → "Quit"; Activate/SecondaryActivate → `on_activate`. `SetConnected` emits
NewIcon + LayoutUpdated (revision++). Degrades silently to no tray without a watcher.
**Windows parity gap (§7.15):** 8-state icon matrix (theme × providing × connected) — Linux
has 2 states and **ignores the shipped third asset `urnetwork-tray-provide.png`** (flag 4);
tooltip = app name + state; conditional **Force tunnel off** (`conn_tray_force_tunnel_off`,
shown iff routes installed) and **Lift kill switch** (`conn_tray_lift_kill_switch`, shown iff
firewall armed with no tunnel — both must work with NO window); Connect/Disconnect must share
the window predicate ("SDK driving OR machine captured"); "tray host restarted" resilience
(re-register on watcher restart — the SNI analogue of TaskbarCreated); the first-hide tray
balloon (`onb_tray_balloon_hide`, own pref `onb_tray_balloon_seen`). The `conn_tray_*` keys
are **Adv-style fallbacks not yet in the store** (§7.16) — carry key + English.
**Verdict:** REWRITE on the existing transport (the GDBus/SNI plumbing and menu scaffolding
are sound; the state model, icon matrix, and verbs grow). **Inputs:** shared connect/disconnect
predicate, provide state (`LiveStats.provideEnabled`), daemon route/failsafe status (needs the
§9.2 protocol extension — flag 6).

---

## 3. Drawer contents with NO place in the Windows layout (removal candidates)

1. **The drawer container itself** — the single scrolling column under the connect controls.
   Windows Simple mode is a 480-capped Connect pane + "More options" disclosure; Advanced is
   3 panes. Nothing keeps the column.
2. **The card-tap-opens-sheet idiom** (`MakeCardTappable` + `MakeCardHeader` chevron cards)
   for stats/DNS — replaced by pane group headers + `MakePaneRow` chevron rows. (The CSS
   hover/press machinery survives for the card model: Support/Developer/dialogs.)
3. **The composite "Client statistics" / "Local statistics" cards** — their charts and entry
   points split across Panes B/C; the two card titles die as card headers (survive only as
   labels if wanted; §7.7 groups are named "connections", "data_usage", "contracts", "split
   rules", "custom DNS").
4. **The drawer intro animation** (self slide-up 16px/300ms + per-card 40ms+60ms·i stagger)
   — replaced by motion Wave 2 `DirectionalSwap`/`RiseIn(Up, 8dip)` built from the motion
   spec (its 60ms stagger is off the 40ms token grid anyway; flag 7).
5. **RedeemCodeSheet's embedded history block** — relocates to Account Pane C; the sheet
   keeps only the entry + success states.
6. **The plan card's "balance home" role** — the drawer was the balance home only because
   Linux had no Account surface; that rationale (stated in the code comments) expires.
Nothing else is orphaned: every control, chart, row, pill, and sheet has a §7 slot.

## 4. What the Windows layout needs that NO Linux surface provides yet

- **Pane-shell kit** (§7.3): 40px header strip, 28px group headers, 34/36/40/44 rows,
  hairlines `#1FFFFFFF`, 12px insets, `MakePane*` builders, `MakeStatusField`, `Snackbar`
  (4000ms auto-dismiss, Warning/Error persistent), `ValidationState` + supporting text.
- **ConnectCanvas hero** (§7.7): 256pt space, side [168,288], 5 states, kPulseMs 1500,
  provider-grid dots, 500ms state fades. (ProviderGlobe and the LoginCarousel globe are
  different organs; the connect hero must be built.)
- **Status strip** (§7.5): 3 normal + 4 Advanced fields, `#151515`, padding 16,7.
- **Window-level bars**: ModeNoticeBar, AccountSnackbar (MaxWidth 480), ServiceSetupBar
  (probe + elevated install + reprobe on activation), UpdateBar. BalanceWarning exists only
  as the drawer banner (2.2).
- **Advanced Mode** (§7.2): `advanced_mode` pref in app_prefs.json, atomic standing state,
  persist-first/publish-second, bind-then-replay, one apply path, structural Simple/Advanced.
- **Navigation shell** (§7.4): NavigationView (220 pane, 44 items, accent pill), 7 sibling
  page grids + `CrossfadePageSwap` (incoming 250 Standard / outgoing 150 Exit, ONE timeline),
  per-destination API loads on selection + auth change, root Login⇄Home swap with
  `ResetForSignOut`.
- **Home Pane B**: routing-decision list (36px rows {verdict dot, host, right bytes}) —
  the `BlockActions()` feed already exists (SplitRulesSheet consumes it); the pane rendering,
  Advanced selection by block-action id, and the **connection inspector** (Pane C headline +
  key/values + exit-routing join from the reliability snapshot over RPC) do not.
- **Network Pane B detail** (400dip) + BlockedLocationsSheet + the 3-state Pane A empties
  (`GetFilteredLocationState` is implemented and unused — flag 3).
- **Account Pane B** wholesale: network-name edit mode (claim/24h-change rules), auth line,
  login methods + remove-confirm, AddAuthSheet, AuthCodeSheet (expires 5 min), client-id copy
  field (masked-copy rule: copy the FULL value), referral bonus code copy, ReferralNetworkSheet,
  referral summary + RoyaltyBadge (`Assets/ReferralFrog.png`), DeleteAccountSheet
  (typed confirmation). Also PLAN pane's confirmation-poll progress ring and **Manage
  Subscription** (Stripe customer portal — no Linux API call exists; flag 5).
- **Earnings destination** wholesale (§7.10): wallets pane (pending payout, unpaid bytes,
  wallet cards, per-chain debounced `walletValidateAddress` via AccountViewController,
  WalletDetailSheet with 2-press confirm + 20s watchdog + generation guard), ledger
  (payouts/leaderboard tabs, PayoutDetailSheet, block-explorer links), points pane
  (leaderboard-public echo-guarded toggle, account-points card, Verify Seeker 180s browser
  bridge — the Linux `WalletConnect` bridge covers sign-in only), `CanCallApi()` gating.
- **Settings destination** (§7.11): product-updates toggle, auto-update check, kill-switch
  row + notes (semantics: flag 6), blocked locations, per-app split tunnel AppRulesSheet
  (daemon `set_split_tunnel` absent — §9.2), device name (+ DeviceNameSheet), device spec,
  uninstall service, **the Advanced Mode toggle** (`adv_advanced_mode_note`), export/save
  logs, about group, sign out.
- **Support destination** (§7.12): rating, feedback box, include-logs (upload only after the
  server accepts, keyed by feedback id), the reach-a-human card.
- **Developer destination** (§7.13): entire surface (~50 controls; RPC-only; 5s gated poll;
  identity-keyed table rebuilds; whole-struct read-modify-write of ReliabilitySettings;
  nil-read = never write zeros; D6 fault injection).
- **Onboarding** (§7.15): kOnboardingVersion 2, `onboarding_version_seen`,
  `onb_tray_balloon_seen`, ServiceSetup focal banner, Connect TeachingTip.
- **Window chrome** (§7.5): 48epx title bar + wordmark PP NeueBit 24, account menu button
  (initials avatar 32, gold Pro ring 2px `#FFC400`), referral-share copy action.
- **Update pipeline UI** + zsync self-update integration (§9.6).
- **Preview harness + `--diagnose`** (§7.17): absent from `main.cpp` — port both
  (`--preview-ui[=…]`, `URNETWORK_PREVIEW_SAMPLE=1`, FailVisible rule).
- **Brand fonts + icon set**: the 4 licensed faces wired through fontconfig with exact
  internal family names; a Fluent-equivalent glyph set (~25 icons, §7.17) — today's code uses
  stock GNOME symbolic icons (acceptable interim, not parity).
- **Sheet-open guard**: §7.14's one-modal-at-a-time `sheetOpen` guard — Linux relies on GTK
  modality alone (flag 9).

## 5. Cross-cutting behaviors already implemented here that the port must keep (§7.18 conformance)

- **Echo guards** on every load-written toggle: `updatingControls_`, `updatingKillSwitch_`,
  `updatingBlocker_`, `updating_` (DnsSheet), `updatingOverrideSwitch_`, `syncingProvideMode_`.
- **Epoch/generation guards** on every async flow: UpgradeSheet, RedeemCodeSheet,
  SubscriptionBalanceStore (`shared_ptr<uint64_t>` bumped on reset/open/logout).
- **Marshal-only SDK callbacks** (`PostToMain`; drawer-event handler may run under the SdkHost
  lock — it must only marshal).
- **Value-compare before rebuild** (ProviderLocationsSheet rows + identity set,
  SplitRulesSheet sections, PQI decks, ContractsSheet keyed reconcile).
- **Distinguishable failure vs empty vs unavailable** already practiced in RedeemCodeSheet
  (3 states) and ProviderLocationsSheet (2 states) — extend to the locations pane (flag 3).
- **Optimistic removal with SDK-confirmation suppression** (`pendingRemovals_`).
- **Never-blame-the-user on transport failure** (redeem transport copy).
- **Payment-path never hard-fails** (embedded → hosted fallback ladder).
- **Visibility gating** of timers/polls (charts stop unmapped; balance store pauses hidden;
  duration tick stops with the sheet) — presentation gate, not focus gate.


## SDK surface referenced
- urnet::Api::createStripeCheckoutSession(StripeCreateCheckoutSessionArgs{item_id, ui_mode}) -> StripeCreateCheckoutSessionResult{client_secret, checkout_url, error} (UpgradeSheet)
- urnet::Api::redeemBalanceCode(RedeemBalanceCodeArgs{secret}) -> RedeemBalanceCodeResult{error} (RedeemCodeSheet)
- urnet::Api::getNetworkRedeemedBalanceCodes -> GetNetworkRedeemedBalanceCodesResult{balance_codes: RedeemedBalanceCodeList{secret, balance_byte_count, redeem_time, end_time}} (RedeemCodeSheet history)
- urnet::Api::subscriptionBalance -> used/pending/available/start bytes + current_subscription isPro (SubscriptionBalanceStore)
- urnet::Api::getNetworkReferralCode -> referral code + total referrals (SubscriptionBalanceStore)
- urnet::Device/DeviceRemote::refreshToken via SdkHost::RefreshJwt (SubscriptionBalanceStore jwt reconcile)
- urnet::LocalState::parseByJwt via SdkHost::ParseByJwt -> ByJwt{Pro, GuestMode, network name} (balance seed)
- urnet::ContractDetailsViewController (single-feed; rows/pendingCount/atTop) via SdkHost::ContractRows / SetContractsAtTop / ContractsPendingCount; types ContractPeerRowList{ClientId, SendContracts, ReceiveContracts, SendByteCount, ReceiveByteCount}, ContractEntry{ContractId, UsedByteCount, TotalByteCount, BitRate, HasStream} (ContractsSheet)
- urnet::ContractDetailsViewController provider feed via openProviderContractDetailsViewController (mentioned, not wired)
- urnet::LocationsViewController::getFilteredLocations / filterLocations / getFilteredLocationState via SdkHost::GetFilteredLocations / FilterLocations / GetFilteredLocationState; FilteredLocations{BestMatches, Countries, Regions, Cities, Devices} (LocationsSheet / Network Pane A)
- urnet::PeerViewController::getPeers via SdkHost::ConnectedProvidePeers; NetworkPeer{ClientId, DeviceName, DeviceSpec} (LocationsSheet, drawer location row)
- urnet::ConnectLocation{name, provider_count, location_type, country, country_code, stable, strong_privacy, connect_location_id{location_id, client_id, location_group_id, best_available}} + urnet::LocationTypeCountry (chooser rows, DNS pill)
- SdkHost::Connect(ConnectLocation) / ConnectBestAvailable / SelectedLocation (connect actions + selected row)
- SdkHost::GetPerformanceProfile / SetPerformanceProfile; urnet::PerformanceProfile{window_type, allow_direct, post_quantum_encryption, window_size}, urnet::WindowTypeAuto/WindowTypeQuality/WindowTypeSpeed, urnet::WindowSizeSettings{window_size_min, window_size_max} (controls card)
- SdkHost::GetRouteLocal / SetRouteLocal (kill switch = !routeLocal)
- SdkHost::GetBlockerEnabled / SetBlockerEnabled (block ads and trackers)
- SdkHost::GetDnsResolverSettings / SetDnsResolverSettings; urnet::DnsResolverSettings{EnableRemoteDoh, EnableLocalDoh, EnableRemoteDns, EnableLocalDns, EnableFallback, RemoteDohUrlsIpv4/6, LocalDohUrlsIpv4/6, RemoteDnsIpv4/6, LocalDnsIpv4/6} (DNS card + DnsSheet)
- urnet::getRecommendedDnsResolverSettings(countryCode) (DNS pill + DnsSheet panel; case-insensitive)
- urnet::getDefaultDnsResolverSettings() (DNS safe-defaults nudge)
- urnet::getRegionalDnsServers() -> RegionalDnsServer{CountryCode, Name, Ipv4} (DnsSheet suggestions)
- urnet::getColorHex(code) (country/id color dots everywhere; needs lowercased ISO-2 for countries)
- SdkHost::ThroughputPoints / ThroughputWindowSeconds; urnet::ThroughputPointList{Time ms, Remote, Local, Block: ThroughputSample{EgressByteCount, IngressByteCount, EgressPacketCount, IngressPacketCount}} (TransferChart x3)
- SdkHost::BlockActions; urnet::BlockActionList{BlockActionId, Time, Hosts, Ips, MatchedHosts, MatchedIps, Block, Local, OverrideId, BlockOverride, RouteOverride, ByteCount} (SplitRulesSheet activity; future Pane B list)
- SdkHost::BlockStatsSnapshot; urnet::BlockStats{AllowedCount, BlockedCount} (SplitRulesSheet counts)
- SdkHost::BlockActionOverrides / AddBlockActionOverride / SetBlockActionOverrideHosts / RemoveBlockActionOverride; urnet::BlockActionOverride{OverrideId, Hosts, RouteOverride{Local}} (split rules)
- urnet::collapseHostNames(hostNames) (chip base names)
- urnet::newId() (new override id)
- urnet::PostQuantumIdentityViewController via SdkHost::ProviderIdentities / PublicIdentityKeyHash / PublicIdentityKey; urnet::ProviderIdentityList{ClientId, PublicKey base64} (PQI panel/list/badge)
- urnet::publicIdentityKeyHash(data, len) (canonical 52-char hash)
- urnet::renderIdenticonPng(key, size) (canonical identicon raster, 2x display size)
- SdkHost::ClientId (PQI own row, contracts copy)
- urnet::ProviderLocationsViewController via SdkHost::ConnectedProviderLocations / RemoveConnectedProvider / SelectedProviderClientId / SetSelectedProviderClientId / StepProviderSelection; urnet::ConnectedProviderLocationList{ClientId, Country, CountryCode, Region, City, HasLocation, HasCityCoordinates, CityLat, CityLon, HasRegionCoordinates, RegionLat, RegionLon, ConnectedSinceMillis} (ProviderLocationsSheet)
- SdkHost::SetDrawerEventHandler + DrawerEvent enum {DeviceLifecycle, Throughput, BlockActions, BlockStats, Overrides, DnsSettings, Blocker, RouteLocal, Contracts, Location, Profile, Locations, Peers, ProviderIdentities, ProviderLocations, ProviderSelection} (event dispatch)
- SdkHost::CurrentStats / SetStatsHandler -> LiveStats{connectionStatus, connected, providerCount, down/upBitsPerSecond, insufficientBalance, provideEnabled, providePaused, provideClients, provideMode, provideHasNetworkKey} (banner gate, Pane B header, tray)
- SdkHost::ResetProvideToNever (free->Pro side effect, applied once by MainWindow)

## Flags (doc-vs-code drift / risks)
- FLAG 1 (doc vs code): TransferChart content height is 128 in Linux code; §7.7 specifies Height 150 for the Pane B activity chart and 132 for the Pane C blocked/local charts. The Windows spec wins for the new panes — parameterize the height on port.
- FLAG 2 (doc vs code): §8.1 requires sheets/dialogs at #151515 above the #101010 page ('or dialogs lose their edge'); the Linux CSS paints EVERY window (sheets included) #101010 via `window.background`. All reused sheets need the #151515 sheet surface on port.
- FLAG 3 (doc vs code, real gap): §7.8 mandates three distinguishable Network-pane states Loading / Failed (LOCATIONS_ERROR) / genuinely empty. SdkHost::GetFilteredLocationState() is implemented (wraps LocationsViewController::getFilteredLocationState) but NO Linux UI calls it — LocationsSheet shows only a searching-time 'no_providers_found' line. Must be fixed when building Network Pane A (and ideally in the chooser too).
- FLAG 4 (doc vs code): §7.15 tray = 8-state icon matrix (theme x provide x connect) + tooltip + Force-tunnel-off / Lift-kill-switch menu items sharing the window's connect predicate. Linux Tray.cpp has 2 icon states (urnetwork-tray-connected/disconnected), no provide state — the shipped urnetwork-tray-provide.png asset (§9.7) is unused — no theme variants, and a 4-item menu (Connect|sep|Show|Quit). Tray needs a rewrite on its (sound) GDBus/SNI transport; the two extra verbs also need the daemon status fields (routes_installed / failsafe_armed) that the Linux control protocol does not carry yet (§9.2).
- FLAG 5 (missing feature): Account PLAN pane per §7.9 needs a progress ring during the post-checkout confirmation poll (store exposes IsPolling(), UI never renders it) and a 'Manage Subscription' Stripe customer-portal entry — no customer-portal API call exists anywhere in the Linux tree; the SDK surface for it must be identified from the Windows sources.
- FLAG 6 (semantics divergence): the Linux drawer 'Kill switch' is the device routeLocal inversion (persisted in GUI LocalState, applied over device RPC — SDK-level packet dropping). The Windows Settings kill switch is additionally a daemon/WFP failsafe with a `set_kill_switch` control verb and routes/failsafe status fields — none of which exist in the Linux control protocol (§9.2 lists the gap). The toggle UI can relocate now on routeLocal semantics; daemon-level failsafe parity is protocol milestone work. Do not present the routeLocal toggle as a network-level kill switch in copy until the daemon side exists.
- FLAG 7 (motion drift): ConnectDrawer's intro (16px slide 300ms; per-card stagger 40ms start + 60ms increments) is off the §8.3 token grid (kStaggerMs 40, kBaseMs 250, kSlowMs 400) and predates the motion overhaul. §8.6 orders Wave 2 (DirectionalSwap incl. the drawer/pane entrance) built FROM THE SPEC, not from shipped code. Do not port AnimateEntrance timings into the new panes.
- FLAG 8 (loc-key trap): several keys deliberately render English different from the key name — window_type_quality -> 'Web', window_type_speed -> 'Streaming', supporter -> 'Pro', become_supporter -> 'Get UR Pro', promoted_locations caption over a best-available row. These are store-key reuses; keep the keys, do not invent new ones (§7.16).
- FLAG 9 (doc vs code): §7.14 requires all sheets modal one-at-a-time via a window-level sheetOpen guard; Linux relies on GTK modality alone with per-owner sheet instances and no guard. Add the guard in the new shell.
- FLAG 10 (missing tooling): §7.17's preview harness (--preview-ui, URNETWORK_PREVIEW_SAMPLE) and --diagnose are absent from linux main.cpp; most new destinations will be unreviewable without an account until it is ported.
- FLAG 11 (behavior note, keep): RedeemCodeSheet distinguishes transport-failure ('code may already be applied — check your balance') from server-rejection ('Invalid balance code'), and only a successful empty fetch shows 'No balance codes found'. This is the §7.18 empty/loading/failed discipline done right — replicate it in every new async pane, do not regress it when relocating the history table.
- FLAG 12 (plural-scheme exception): PQI peer count uses two printf-style keys peer_count_one / peer_count_other shared with other platforms instead of the TN_/gettext composite plural. Keep as-is; do not 'fix' into the .one/.other scheme.
- OPEN QUESTION: §7.4 loads Account data on destination selection + auth change; the Linux SubscriptionBalanceStore additionally background-polls 30s while non-Pro (mac parity) with visibility gating. Recommendation: keep the store's poll (superset, drives BalanceWarning live) and also trigger FetchNow() on destination selection; confirm against the Windows SdkHost if exact parity of cadence matters.
- RISK: ContractsSheet, ProviderLocationsSheet and the PQI deck rely on custom Gtk::Widget measure/allocate and Gtk::Fixed overlap tricks; restyling them into #151515 sheets must not touch their animation state machines (settle/slide/enter phase logic is timing-sensitive and value-keyed).
- NOTE: ProviderLocationsSheet's GeoClue location-override section is a Linux-only supported feature (§7 explicitly keeps it); it has no Windows analogue and must survive the parity port, including MainWindow's ownership of override target tracking while the sheet is closed.
