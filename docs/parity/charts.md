# URnetwork Windows → GTK4 Port Spec — THE CHARTS
## TransferChart, UsageBar, and StatsFormat number formatting

Sources read in full:
- `/var/home/bazzite/Downloads/claude_sandbox_linux_app/urnetwork-windows/app/src/App/TransferChart.h` / `.cpp`
- `/var/home/bazzite/Downloads/claude_sandbox_linux_app/urnetwork-windows/app/src/App/UsageBar.h` / `.cpp`
- `/var/home/bazzite/Downloads/claude_sandbox_linux_app/urnetwork-windows/app/src/App/StatsFormat.h` / `.cpp`
- Hosting/wiring: `MainWindow.xaml` (lines 1146, 1263/1267, 1605), `ConnectPage.cpp` (chart clock, feeds, preview), `MainWindow.xaml.cpp` (UsageBar wiring, ApplyBalance), `SubscriptionBalance.h/.cpp` (balance feed), `SdkHost.cpp` (throughput feed), `UrColors.h`, `Strings/en/Resources.resw`
- Linux port compared: `/var/home/bazzite/Downloads/claude_sandbox_linux_app/urnetwork-linux/app/src/TransferChart.{hpp,cpp}`, `UsageBar.{hpp,cpp}`, `Formatters.{hpp,cpp}`, `Ui.hpp`, `ConnectDrawer.cpp`
- Doc cross-check: `urnetwork-linux/docs/linux_agent_help.md` §7.7/§7.9 (UI inventory), §8.1/§8.2 (tokens)

---

## 0. Shared tokens used by these components (from `UrColors.h`; doc §8.1 agrees exactly)

| Constant | Hex (ARGB) | Role here |
|---|---|---|
| `kUrGreen` | `#FF87FB67` | remote+local chart BYTE series |
| `kUrPink` | `#FFED8FFF` | remote+local chart PACKET series |
| `kUrCoral` | `#FFFF6C58` | blocked chart byte series; UsageBar PENDING |
| `kUrMutedCoral` | `#FFC8604F` | blocked chart packet series |
| `kUrElectricBlue` | `#FF0039DE` | UsageBar USED |
| `kTextFaint` | `#FF5A5A5A` | UsageBar AVAILABLE + empty track |
| `kText` | `#FFF8F8F8` | lit avg-arrow, active-direction indicator |
| `kTextMuted` | `#FF989898` | chart title, peak labels, dim arrow, legend labels |
| `kBorder` | `#1FFFFFFF` (white @12%) | chart center zero axis |

`WithAlpha(c, a)` replaces the alpha byte. All chart micro-labels are FontWeight **500 (Medium)**; numeric labels use **Consolas** (monospace); title and arrows use the default app body face. (Doc §8.2's type ramp does not cover these micro-labels — the code is the source.)

---

## 1. TransferChart

One class renders one throughput **route** (`ThroughputRoute { Remote, Local, Block }`) as a live mirrored 60-second chart: **egress above / ingress below** a center zero axis, newest data at the right edge sliding left.

### 1.1 The three instances (`ConnectPage::BuildCharts`)

| Instance | Host (XAML) | Height (dip) | Title key → EN | byteColor | packetColor |
|---|---|---|---|---|---|
| `remoteChart_` | `RemoteChartHost`, Pane B (Activity) row 1 | **150** | `remote` → "Remote" | `kUrGreen` | `kUrPink` |
| `blockedChart_` | `BlockedChartHost`, Pane C (Statistics) stack | **132** | `blocked` → "Blocked" | `kUrCoral` | `kUrMutedCoral` |
| `localChart_` | `LocalChartHost`, Pane C, directly under blocked | **132** | `local` → "Local" | `kUrGreen` | `kUrPink` |

Each host is `<Border BorderBrush=UrBorderBrush BorderThickness="0,0,0,1" Padding="12,0"><Grid Height=… /></Border>` — full pane width, bottom hairline rule, **12dip horizontal padding, 0 vertical**. The two Pane C charts are stated in XAML as "two charts, the SAME height".

**Clipping (mandatory port rule):** WinUI Canvas/Grid do not clip children, so `ConnectPage` installs a `ClipToBounds` helper on each host: on every `SizeChanged` a fresh `RectangleGeometry{0,0,w,h}` is set as the host's `Clip` (a Canvas overdraw once bled a green sliver + a stray peak marker into the next pane). The chart's own Canvas also re-clips itself to its bounds on SizeChanged. GTK4 `Gtk::DrawingArea` clips inherently — nothing to do, but never remove that property.

### 1.2 Element order (z back → front)

Host `Grid` children, in append order:
1. **`canvas_`** (Canvas, Stretch/Stretch, `IsHitTestVisible(false)` — the whole chart is non-interactive/static). Canvas children in z-order:
   1. `egressBytes_.fill` (Path)
   2. `egressBytes_.stroke` (Path)
   3. `ingressBytes_.fill`
   4. `ingressBytes_.stroke`
   5. `egressPackets_.stroke` (no fill)
   6. `ingressPackets_.stroke` (no fill)
   7. `axis_` (Line — drawn OVER the series "so it always reads consistently")
   8. `peakEgressLabel_` (TextBlock, initially Collapsed)
   9. `peakIngressLabel_` (TextBlock, initially Collapsed)
2. **Title** TextBlock (only if title non-empty): FontSize **11**, weight 500, `kTextMuted`, HorizontalAlignment Left, VerticalAlignment Top. Static text; not localized here — caller passes `Localized(key)`.
3. **Stats** StackPanel: Vertical, `Spacing(3)`, HorizontalAlignment **Right**, VerticalAlignment Top. Two rows (egress first, then ingress), each a Horizontal StackPanel `Spacing(5)` containing, in order:
   - `byteAvg` TextBlock — FontSize **10**, Consolas, weight 500, Foreground = series byteColor
   - `packetAvg` TextBlock — FontSize **10**, Consolas, weight 500, Foreground = series packetColor
   - `arrow` TextBlock — FontSize **8**, body face, weight 500, `kTextMuted`; text `"▲"` (egress row) / `"▼"` (ingress row)

Peak labels: FontSize **9**, Consolas, weight 500, `kTextMuted`.

### 1.3 Geometry constants

```
kTransitionSeconds = 0.5   // newest-bucket lerp AND axis-scale ease duration
kStatsBand         = 30    // reserved band at top for the average rows
kPeakBand          = 13    // sliding peak-label bands (top and bottom)
kAverageBucketCount = 5    // rolling average window (~5 s)
window_ default    = 60    // seconds
byte scale floor   = 1024  // bytes
packet scale floor = 8     // packets
plotTop    = kStatsBand + kPeakBand = 43
plotBottom = height − kPeakBand
centerY    = (plotTop + plotBottom) / 2      // 150 → 90;  132 → 81
plotHalf   = max((plotBottom − plotTop)/2, 8) // 150 → 47;  132 → 38
```

Stroke widths: **1.5** for byte series, **1.0** for packet series; both with round line joins and round start/end caps. Byte-series strokes at series color **alpha 230/255 (~0.9)**; byte-series area FILL at **alpha 18/255 (~0.07)**, closed down to the center axis. Packet series: stroke only, alpha 230/255, no fill. Axis: `kBorder` (#FFFFFF@12%), thickness **1**, spans full width at `centerY`.

### 1.4 Data model & `SetPoints`

```
struct Sample { int64 egressBytes, ingressBytes, egressPackets, ingressPackets; }
struct Entry  { double time /*unix SECONDS*/; Sample sample; }
```

`SetPoints(const std::vector<urnet::ThroughputPoint>& points, int64_t windowSeconds)`:
- Rebuilds `entries_` (ascending by time). Per point: `entry.time = point.Time / 1000.0` (SDK times are **unix milliseconds**); the route's `std::optional<urnet::ThroughputSample>` (`point.Remote` / `.Local` / `.Block`) is copied field-for-field; an absent optional leaves an all-zero sample.
- `window_ = windowSeconds` if > 0 (SDK supplies it; app substitutes 60 when the SDK returns ≤0).
- Sets `dirty_ = true` (forces one redraw on next tick).

### 1.5 Tick, activity gating, redraw budget

`ConnectPage` owns ONE shared `DispatcherQueueTimer` at **100 ms (~10 fps)**; every tick calls `Tick()` on all three charts **only while `ConnectView` is Visible**; the timer itself is started/stopped by `SetPresentationActive(shown && !minimized)` — a hidden/minimized window ticks nothing (doc §7.18 idle-budget rule). The same clock also drives the hero canvas, a 1 s sub-cadence (`++chartTickCount_ % 10 == 0`) for relative-time refreshes, and a 2 s sub-cadence (`% 20 == 0`) for the preview sample push.

`TransferChart::Tick()`:
1. Return immediately if canvas width or height ≤ 0.
2. `now = ` wall clock in seconds (`system_clock`, double).
3. Compute window peaks over ALL entries: `peakBytes = max(egressBytes, ingressBytes)` across entries; likewise `peakPackets`. Update the two `ValueTransition` scales toward `max(peakBytes, 1024)` and `max(peakPackets, 8)`.
4. **Redraw-only-while-active rule:** find the newest entry with any nonzero field (`IsActive`); `hasRecentActivity = now − itsTime < window_`. `animating = hasRecentActivity || byteScale_.InFlight(now) || packetScale_.InFlight(now) || dirty_`. If `!animating && !wasAnimating_` → return (no draw). Otherwise `wasAnimating_ = animating` — i.e., when activity just drained, **exactly one settling frame** is drawn before going quiet. Clear `dirty_`, then `Redraw(now, w, h)` and `UpdateAverageLabels()`.

### 1.6 `ValueTransition` — the eased auto-scale

Cubic ease-out over 0.5 s: `eased = 1 − (1 − progress)³`, `progress = clamp((now − start)/0.5, 0, 1)`; `ValueAt = from + (to − from)·eased`; `InFlight = start > 0 && now − start < 0.5`.

`UpdateTransition(t, target, now)`:
- **First observation** (`t.start <= 0`): land on target with NO easing (`{target, target, now − 0.5}`).
- Target changed: ease **from wherever the previous transition currently is** (`{t.ValueAt(now), target, now}`) — never jump.

### 1.7 `Redraw` — full pipeline

1. Position axis line at `centerY`, x 0→width.
2. **Empty state** (`entries_` empty): clear all four series geometries (`Data(nullptr)`), collapse both peak labels, keep the axis. Return. (The chart has no separate loading/failed rendering — before the first SDK push it shows the axis, the title, and zeroed dimmed average rows; that is the intended idle look.)
3. **Newest-bucket lerp:** copy `entries_`; replace the LAST entry's sample with `Lerp(prevSample, lastSample, eased)` where `progress = clamp((now − last.time)/0.5, 0, 1)`, cubic ease-out, `prev` = second-to-last sample or all-zeros. Per-field integer lerp with `llround`. (A changed rightmost value transitions in instead of hopping.)
4. **Padding** so the baseline spans the full width:
   - Left, if `windowStart = now − window_` precedes the first entry: `step = max(0.2, entries[1].time − entries[0].time)` (or 1.0 with a single entry); `rampTime = first.time − step`; append `{windowStart, zeros}` and, if `windowStart < rampTime`, `{rampTime, zeros}` — a zero baseline plus a one-bucket ramp up into the first real sample.
   - Right: if the last padded time < now, append `{now, lastLerpedSample}` (hold latest value to the right edge).
5. **Mapping:** `x(t) = width · (1 − (now − t)/window_)`; `offset(v, scale) = plotHalf · min(1, v / max(scale, 1))`; egress y = `centerY − offset`, ingress y = `centerY + offset`. Scales are the eased `ValueAt(now)` of each transition.
6. **Half-plane clips:** egress series clipped to `{0, 0, width, centerY}`, ingress to `{0, centerY, width, height − centerY}` — Catmull-Rom overshoot must never cross the center axis (explicit macOS-parity comment).
7. Draw the 4 series (byte series with closed area fill down to `axisY = centerY`; packet series stroke-only).
8. **Sliding peak labels:** scan RAW `entries_` (not the lerped copy) for the entry with max `egressBytes` and max `ingressBytes` (strict `<` — earliest max wins ties). For each with value > 0: text = `FormatByteRate(value) + " ▲"` (egress) / `" ▼"` (ingress); make visible; Measure; label center x = `clamp(rawX, halfW, max(halfW, width − halfW))` where `rawX = width · (1 − (now − peakTime)/window_)` and `halfW = labelWidth/2 + 2` — the label **slides every frame** tracking its bucket, clamped fully inside the canvas. Vertical: top = `y + (kPeakBand − labelHeight)/2`, with `y = kStatsBand` (=30) for egress (band 30…43) and `y = height − kPeakBand` for ingress (bottom band). Value ≤ 0 → Collapsed.

### 1.8 Catmull-Rom smoothing (`SmoothGeometry`)

< 2 points → empty geometry. Exactly 2 → straight line. Otherwise, for i in 1…n−1 with `p0 = pts[max(i−2,0)]`, `p1 = pts[i−1]`, `p2 = pts[i]`, `p3 = pts[min(i+1, n−1)]`, a cubic Bézier to `p2` with control points:
```
c1 = p1 + (p2 − p0)/6      c2 = p2 − (p3 − p1)/6
```
When closing for the area fill: line down to `(lastX, axisY)`, line back to `(firstX, axisY)`, close figure.

### 1.9 Rolling average labels (`UpdateAverageLabels`)

Mean over the last `min(entries, 5)` RAW buckets (~per-second average), integer division; all zeros when empty. Per row:
- `byteAvg.Text = FormatByteRate(bytes)`; **Opacity 1.0 if bytes > 0 else 0.4**.
- `packetAvg.Text = FormatPacketRate(packets)`; Opacity 1.0 / 0.4 likewise.
- `arrow.Foreground = kText` if `bytes > 0 || packets > 0` else `kTextMuted` — "lights up like a link light" when the direction is active.

### 1.10 SDK feed (`ConnectPage::WireDrawerFeeds` + `SdkHost`)

- `SdkHost::SetThroughputHandler(handler)` — fired on SDK listener threads; ConnectPage hops via `DispatcherQueue.TryEnqueue` and calls `SetPoints(points, windowSeconds)` on **all three charts with the same point list** (each extracts its own route).
- SdkHost wiring: `device_->openContractViewController()` → `contractVc_->addThroughputListener(...)` → `PublishThroughput()` reads `contractVc_->getThroughputPoints()` and `contractVc_->getWindowDurationSeconds()` (≤0 → 60), caches `lastThroughputPoints_` under a mutex (`CurrentThroughputPoints` for late subscribers), then invokes the handler.
- `urnet::ThroughputPoint { int64 Time /*ms*/; optional<ThroughputSample> Remote, Local, Block; }`
- `urnet::ThroughputSample { int64 EgressByteCount, IngressByteCount, EgressPacketCount, IngressPacketCount, EgressBitRate, IngressBitRate; }`

**Pane B header figure** (owned by the same pane as the remote chart): `ThroughputText` = `"↓ " + FormatBitRate(down) + "   ↑ " + FormatBitRate(up)` when stats.connected, else collapsed; down/up come from the newest throughput point with a `Remote` sample (`Remote->IngressBitRate` / `Remote->EgressBitRate`).

**Preview harness:** with `--preview-ui` AND env `URNETWORK_PREVIEW_SAMPLE`, a deterministic 61-bucket synthetic minute (hash-driven, scales 40000/6000/3000 per route, `Bitrate = bytes·8`) is pushed to all three charts every 2 s so the window keeps scrolling.

### 1.11 A11y / interactivity

The entire chart is decorative: Canvas is hit-test-invisible; no control, no focus stop, no automation names set. Title/avg/peak TextBlocks are ordinary text elements. Nothing is clickable.

---

## 2. UsageBar

The stacked **used / pending / available** balance bar (macOS UsageBar parity) + color legend.

### 2.1 Host & instance

ONE live instance on Windows: the Account pane's plan card. XAML:
```
<Border BorderBrush=UrBorderBrush BorderThickness="0,0,0,1" Padding="12,10">
  <StackPanel Spacing="8">
    <Grid x:Name="AccountUsageBarHost" Height="32" />
    <StackPanel x:Name="AccountUsageLegend" Orientation="Horizontal" Spacing="16" />
  </StackPanel>
</Border>
```
Bar height **32dip** (host height; the widget itself sets none), constructed once in `MainWindow` right after `connect_->Initialize()`: `UsageBar(AccountUsageBarHost(), AccountUsageLegend())`. (`UrComponents::MakePlanUsageCard` builds the same host shape — Grid Height 32 + horizontal legend Spacing 16 — for reuse, but currently has no callers.)

### 2.2 Bar construction (`Update(used, pending, available)`)

Constants: `kBarRadius = 12.0` (macOS 32pt bar / 12pt outer corner radius), `kMinFraction = 0.015`.

1. Clear host children + column definitions. Clamp each input at ≥ 0; `total` = sum.
2. Segment order fixed: **used = `kUrElectricBlue`, pending = `kUrCoral`, available = `kTextFaint`**.
3. `total <= 0` → **empty state**: one full-width `kTextFaint` track (available segment weight 1.0) — visually a faint capsule; distinct from a loaded bar.
4. Otherwise every segment's weight = `max(value/total, 0.015)` — **note: on Windows the 1.5% floor applies to ALL segments including zero-valued ones**, so all three colors are always visible once total > 0 (see FLAGS — this diverges from the mac name `minNonZeroValue` and from the Linux port).
5. Layout: one star-sized `ColumnDefinition` per visible segment (`GridLength{weight, Star}` — star sizing normalizes, no manual renormalize). Each segment is a `Border` with `Background = segment color` and:
   - **Rounded OUTER corners only**: first visible segment gets radius 12 on top-left/bottom-left; last visible gets 12 on top-right/bottom-right; inner corners 0. A single segment gets all four.
   - **1px hairline gap**: every non-first segment gets `Margin{1, 0, 0, 0}` so colors don't bleed together.

### 2.3 Legend (built once in constructor)

`legendHost.Children().Clear()`, then three entries in order — each a Horizontal StackPanel `Spacing(6)`:
- `Ellipse` **8×8**, Fill = segment color, VerticalAlignment Center
- `TextBlock` FontSize **12**, Foreground `kTextMuted`, VerticalAlignment Center

| Entry | Key | English |
|---|---|---|
| used | `used_data_key` | "Used" |
| pending | `pending_data_key` | "Pending" |
| available | `available_data_key` | "Available" |

Constructor ends with `Update(0,0,0)` → the faint empty track until the first balance lands. Everything is static/non-clickable; no automation names.

### 2.4 Data feed — `SubscriptionBalanceStore` (Api + cadences)

`MainWindow::ApplyBalance` (the store's change handler) calls `accountUsageBar_->Update(balance_.usedByteCount, balance_.pendingByteCount, balance_.availableByteCount)`.

- SDK call: `sdk.api().subscriptionBalance(callback)` → `urnet::SubscriptionBalanceResult`. Derivation (`Apply`):
  - `available = balance_byte_count`
  - `pending  = open_transfer_byte_count`
  - `used     = start_balance_byte_count − balance_byte_count − open_transfer_byte_count`
  - `startBalanceByteCount = start_balance_byte_count` → the "Daily Data Balance" row (`FormatByteCountCompact`)
  - Pro = `current_subscription.has_value()`; a disagreement with the jwt claim triggers `sdk.RefreshJwt()` once per flip.
- Cadences: **30 s** background poll while started + window visible + not confirming + not (Pro with available > 0); **5 s** post-checkout confirmation poll with a **120 s ACTIVE-polling budget** (monotonic clock; pauses and banks remaining budget on focus loss, immediate fetch on refocus); one fetch on every window show; one in-flight fetch at a time (`loading_`); **generation counter** drops results from a superseded login session; fetch guarded by `IsLoggedIn()` (not apiReady). Errors keep the last snapshot (the poll retries) — the bar never blanks on a failed refresh.
- Neighboring rows fed by the same handler (Account pane, NOT part of the UsageBar widget on Windows): plan value (`guest`/`supporter`/`free`, Pro wears `kProGold`), daily value, `total_referrals_lld` → "Total Referrals: {}", `referral_bonus` → "+{} GiB/Month" (arg = referrals × 30).

---

## 3. StatsFormat — every formatter, exact precision rules

All return `std::string` (UTF-8). Shared magnitude rule `FormatMagnitude(value, unit)`:
**value ≥ 100 → `%.0f`; ≥ 10 → `%.1f`; else `%.2f`**, then `" " + unit`. (The header's examples like "1.2 KiB" are loose; the code prints "1.20 KiB" below 10 — code wins.)

| Function | Rule | Examples |
|---|---|---|
| `FormatByteCountCompact(int64)` | IEC base-1024. `< 1024` → integer + `" B"`. Then KiB / MiB / GiB / TiB thresholds at successive ×1024, each via FormatMagnitude | `996 B`, `1.20 KiB`, `34.5 MiB`, `112 GiB` |
| `FormatByteRate(int64)` | `FormatByteCountCompact + "/s"` | `1.20 KiB/s` |
| `FormatCountCompact(int64)` | decimal base-1000. `< 1000` → integer. `< 1e6` → `v/1000` with `%.1fk` if v < 10000 else `%.0fk`. Else `%.1fM` of `v/1e6` | `996`, `1.2k`, `34k`, `3.4M` |
| `FormatPacketRate(int64)` | `FormatCountCompact + " pkt/s"` | `34k pkt/s` |
| `FormatBitRate(int64)` | decimal. `< 1000` → integer + `" bps"`; `< 1e6` → FormatMagnitude(v/1e3, "Kbps"); `< 1e9` → Mbps; else Gbps | `996 bps`, `1.20 Kbps`, `24.6 Mbps` |
| `RelativeTime(thenMillis, nowMillis)` | `seconds = max(0, (now−then)/1000)`. `< 5` → `Localized("now")`; `< 60` → `Format("seconds_ago_abbrev", s)`; `< 3600` → `minutes_ago_abbrev` with `s/60`; else `hours_ago_abbrev` with `s/3600` (integer truncation) | `now`, `12s ago`, `3m ago`, `2h ago` |
| `FormatHostClusterText(hosts, ips)` | If `hosts.size() > 10`: collapse each host to `"*." + urnet::hostBaseName(host)` (public-suffix aware), dedup preserving first-seen order. Concatenate displayHosts + ips; empty → `Localized("unknown")`; else `CompactValueList` | |
| `CompactValueList(values)` (internal) | ≤ 20 → join all with `", "` in ORIGINAL order. > 20 → sort a copy alphanumerically; `first7 + ", …, " + middle7 (start (n−7)/2) + ", …, " + last7`; if `omitted = n − 21 > 0` append `" " + Plural("plus_n_more", omitted)` | `a, b, …, m, + 3 more` |
| `IsIpAddressValue(v)` | `inet_pton` AF_INET then AF_INET6 (Windows lazily WSAStartup's once) | |
| `IsValidDohUrl(v)` | must start case-insensitively with `https://` and be longer; authority = up to first `/?#`; strip userinfo after last `@`; `[…]` bracketed IPv6 host; else strip `:port`; host must be non-empty | |

Localization keys used (Adv-style key + English):

| Key | English |
|---|---|
| `now` | "now" |
| `seconds_ago_abbrev` | "{}s ago" |
| `minutes_ago_abbrev` | "{}m ago" |
| `hours_ago_abbrev` | "{}h ago" |
| `plus_n_more.one` / `.other` | "+ {} more" (arg = omitted count) |
| `unknown` | "unknown" |
| `remote` / `blocked` / `local` | "Remote" / "Blocked" / "Local" |
| `used_data_key` / `pending_data_key` / `available_data_key` | "Used" / "Pending" / "Available" |
| `daily_data_balance_label` | "Daily Data Balance:" |
| `total_referrals_lld` | "Total Referrals: {}" |
| `referral_bonus` | "+{} GiB/Month" |

---

## 4. Linux port comparison (existing `urnetwork-linux/app/src`)

### 4.1 `TransferChart.cpp` (Cairo `Gtk::DrawingArea`) — **algorithm faithful, mostly reusable as-is**

Identical to Windows: constants (0.5 s ease, bands 30/13, 5-bucket average, floors 1024/8, 100 ms tick), Catmull-Rom control-point math, half-plane clipping, left zero+ramp / right hold padding, newest-bucket cubic lerp, x/offset mapping, fill alpha 0.07 / stroke alpha 0.9, stroke widths 1.5/1.0 round caps+joins, peak-label clamp (`halfWidth = total/2 + 2`), avg dimming (alpha 0.4 when zero) and arrow lighting, axis `kUrBorderBase` white@12% width 1, draw order (fills→strokes→axis→peak labels), empty-entries → axis only.

Differences to reconcile (Windows code wins for parity):
1. **Heights**: Linux hardcodes `set_content_height(128)` for every chart. Windows hosts are **150 (remote)** and **132 (blocked/local)** — make the height a constructor/host parameter.
2. **First-scale observation**: Windows lands the very first peak WITHOUT easing (`start ≤ 0` special case); Linux seeds `ScaleEase{1024,1024,0}` and eases the first real peak in from the floor. Minor visible difference on first data.
3. **Scale update site**: Windows recomputes targets every `Tick()`; Linux recomputes in `SetPoints()`. Same result (entries only change there) — acceptable.
4. **Settling frame**: Windows `wasAnimating_` draws one extra frame after activity drains; Linux's self-stopping timer queues a final draw when it exits — equivalent outcome. Linux adds an explicit `lerpInFlight` term to `Animated()`, covering what Windows covers via `dirty_`. Equivalent.
5. **Glyphs**: Windows arrows are TEXT glyphs (`▲▼`, font 8 in avg rows; `" ▲"/" ▼"` appended to peak labels, font 9); Linux draws filled Cairo triangles (avg 7 px at right edge with 5 px gaps, peak 6 px + 3 px gap after the value). Visual near-parity; keep Linux triangles or switch to glyphs — pick one and note it.
6. **Fonts**: Windows Consolas / weight 500; Linux toy-font "monospace"/"sans-serif" NORMAL. Acceptable substitution; consider a 500-weight Pango font description for closer parity.
7. **Avg-row geometry**: Linux draws rows at fixed centers y=7 and y=20 (≈ Windows StackPanel with Spacing 3 at top-right); title baseline centered at y=7. Matches the 30 px stats band.
8. Linux stops its tick when unmapped and restarts on `signal_map` — the correct GTK analogue of Windows' `SetPresentationActive` gate. Windows additionally gates per-tick on ConnectView visibility; Linux drawer should gate equivalently if the chart can be in a hidden stack page.

### 4.2 `UsageBar.cpp` — **reusable core, several exactness gaps**

Same: 32 px bar, radius 12 (rounded capsule), 1.5 % floor concept, used/pending/available colors, legend order and keys, `SetData` clamps at ≥ 0.

Differences (Windows code wins for near-perfect parity):
1. **Zero-segment floor**: Windows floors ALL segments at 1.5 % when total > 0 (zero-valued segments still render); Linux floors only non-zero fractions (`f > 0 && f < kMinFraction`) — zero segments vanish. Match Windows (three colors always visible once loaded).
2. **Segment gap**: Windows inserts a **1 px gap** before every non-first segment; Linux deliberately overlaps +0.5 px to hide seams. Visibly different — add the 1 px gap.
3. **Empty track alpha**: Windows paints the empty track in full-alpha `kTextFaint` (#5A5A5A); Linux paints it at 0.35 alpha. Match Windows.
4. **Corner shape**: Windows rounds only the outer corners of the first/last segment (radius 12, inner corners square, gaps visible as square-cut hairlines); Linux clips the whole bar to a capsule (all segments square inside the clip). With the 1 px gaps added, the Windows look needs per-segment outer rounding, not just an outer clip.
5. **Renormalization**: Linux renormalizes fractions by their sum after flooring; Windows relies on Grid star sizing (mathematically the same normalization). No change needed once floors match.
6. **Composition**: the Linux widget bundles legend + daily-balance row + separator + referral rows into one `Gtk::Box` (drawer-card design); Windows keeps the bar+legend widget minimal and renders daily/referral as separate pane rows via `ApplyBalance`. For the pane-shell port, split them like Windows.
7. **Legend metrics**: Windows dot = 8×8 Ellipse, dot↔label gap 6, label FontSize 12 muted, entry↔entry gap 16. Linux: text "●" glyph, gap 4, caption class, entry gap 12. Align to Windows numbers.
8. **String-key mismatch**: Linux uses `total_referral_count` → "Total referrals: {}"; Windows ships `total_referrals_lld` → "Total Referrals: {}". Use the shipped Windows key/casing.

### 4.3 `Formatters.cpp` vs `StatsFormat.cpp` — **byte/rate/count/bit formatters are exact matches; reusable as-is**

Identical: `FormatByteCountCompact`, `FormatByteRate`, `FormatCountCompact` (same 1e4 pivot), `FormatPacketRate`, `FormatBitRate`, magnitude precision rule, `IsIpAddressValue`. Near-identical: `IsValidDohUrl` (Linux uses GUri — same accept/reject envelope for practical inputs). Differences:
- `RelativeTime` signature: Linux takes `secondsAgo`; Windows takes `(thenMillis, nowMillis)`. Thresholds/keys identical.
- **Missing on Linux entirely**: `FormatHostClusterText`, `CompactValueList`, `urnet::hostBaseName` usage, and the `plus_n_more` plural — required by the inspector's addresses row. Must be ported.
- Linux adds `TrimWhitespace` (harmless extra).

---

## 5. urnet:: / SDK surface summary for these components

- `urnet::ContractViewController` — obtained via `device->openContractViewController()`; `addThroughputListener(cb)`, `getThroughputPoints()`, `getWindowDurationSeconds()`.
- `urnet::ThroughputPoint { Time(ms), optional Remote/Local/Block }`; `urnet::ThroughputSample { EgressByteCount, IngressByteCount, EgressPacketCount, IngressPacketCount, EgressBitRate, IngressBitRate }` (`ThroughputPointList = std::vector<ThroughputPoint>`).
- `urnet::Api::subscriptionBalance(cb)` → `urnet::SubscriptionBalanceResult { balance_byte_count, open_transfer_byte_count, start_balance_byte_count, current_subscription, … }`.
- `urnet::hostBaseName(host)` — inline SDK helper (public-suffix aware) for host clustering.
- App-side relays (port equivalents exist in Linux `SdkHost`): `SdkHost::SetThroughputHandler`, `SdkHost::CurrentThroughputPoints(int64& windowSeconds)`, `SdkHost::IsLoggedIn`, `SdkHost::ParsedJwt` (Pro/GuestMode claims), `SdkHost::RefreshJwt`.


## SDK surface referenced
- urnet::DeviceLocal::openContractViewController (SdkHost.cpp:2116)
- urnet::ContractViewController::addThroughputListener (SdkHost.cpp:2126, 2413)
- urnet::ContractViewController::getThroughputPoints (SdkHost.cpp:2213, 2490)
- urnet::ContractViewController::getWindowDurationSeconds (SdkHost.cpp:2493)
- urnet::ThroughputPoint {Time ms; optional Remote/Local/Block} (urnetwork_sdk.hpp:2036)
- urnet::ThroughputSample {EgressByteCount, IngressByteCount, EgressPacketCount, IngressPacketCount, EgressBitRate, IngressBitRate} (urnetwork_sdk.hpp:2027)
- urnet::Api::subscriptionBalance -> urnet::SubscriptionBalanceResult {balance_byte_count, open_transfer_byte_count, start_balance_byte_count, current_subscription} (SubscriptionBalance.cpp:183)
- urnet::hostBaseName (StatsFormat.cpp:111; urnetwork_sdk.hpp:19800)
- SdkHost::SetThroughputHandler / SdkHost::CurrentThroughputPoints (app relay for the three charts)
- SdkHost::IsLoggedIn / SdkHost::ParsedJwt / SdkHost::RefreshJwt (balance store guards)

## Flags (doc-vs-code drift / risks)
- CODE vs CODE-COMMENT: UsageBar.h claims two hosts ('the account plan card and the connect drawer plan card') but only one UsageBar is instantiated on Windows (MainWindow.xaml.cpp:224, Account pane); UrComponents::MakePlanUsageCard builds the second host shape but has no callers. Port one instance; keep the card builder optional.
- WINDOWS vs MAC-NAMING vs LINUX: Windows UsageBar floors ALL segments at 1.5% including zero-valued ones (std::max(value/total, kMinFraction) unconditionally), so all three colors always render once total>0; the mac name minNonZeroValue and the existing Linux port floor only non-zero segments. Code wins: replicate the Windows behavior.
- LINUX PORT DRIFT (UsageBar): no 1px inter-segment gap (Windows has Margin{1,0,0,0} on non-first segments), empty track drawn at 0.35 alpha (Windows full-alpha kTextFaint), capsule-clip instead of per-segment outer-corner radius 12, legend metrics 4/12 vs Windows 6/16 with an 8x8 ellipse dot, and the widget bundles daily-balance+referral rows that Windows renders as separate pane rows.
- LINUX PORT DRIFT (strings): Linux UsageBar uses key total_referral_count ('Total referrals: {}'); the shipped Windows key is total_referrals_lld ('Total Referrals: {}').
- LINUX PORT DRIFT (TransferChart): content height hardcoded 128 for all charts; Windows hosts are 150 (remote) / 132 (blocked, local) — doc §7.7 agrees with the Windows code.
- LINUX PORT DRIFT (TransferChart, minor): first scale observation eases from the floor; Windows lands the first target with no easing (ValueTransition start<=0 special case).
- LINUX MISSING: FormatHostClusterText / CompactValueList / urnet::hostBaseName clustering / plus_n_more plural are absent from Formatters.cpp — required by the inspector addresses row; must be ported.
- HEADER-COMMENT vs CODE (StatsFormat.h): examples like '1.2 KiB' suggest 1 decimal, but the code prints 2 decimals below 10 ('1.20 KiB'); precision rule is >=100 %.0f / >=10 %.1f / else %.2f. Code wins.
- DOC §8.2 gap: the chart micro-typography (Consolas monospace 9-11px, FontWeight 500, arrow glyphs at 8px) is not in the doc's type ramp; the Windows code is the only source.
- DOC §7.7/§7.9 vs code: no disagreements found for these components (heights 150/132, floors 1024/8, 0.5s cubic ease-out, 5-bucket averages, sliding peak labels, ~10fps redraw-while-active, 60s window, usage bar colors/rounding/legend all match the code).
- PORT RISK: WinUI Canvas/Grid do not clip children so ConnectPage installs explicit RectangleGeometry clips on each chart host (a chart once bled into the neighboring pane); Gtk::DrawingArea clips inherently, but any GTK reimplementation that draws outside a snapshot/allocation must preserve clipping.
- RelativeTime signatures differ (Linux: secondsAgo; Windows: thenMillis+nowMillis) — same thresholds and keys; unify when porting the inspector 'last decision' row.
