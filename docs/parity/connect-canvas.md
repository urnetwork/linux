# THE CONNECT HERO CANVAS — GTK4/C++ port spec (near-perfect parity)

Source of truth read for this spec (the implementer does not need to re-open these):
- `urnetwork-windows/app/src/App/ConnectCanvas.h` (240 lines) / `ConnectCanvas.cpp` (898 lines)
- `urnetwork-windows/app/src/App/ConnectPage.cpp` (hero wiring, state mapping, tick clock, click)
- `urnetwork-windows/app/src/App/SdkHost.h/.cpp` (the grid feed), `UrColors.h` (tokens), `MainWindow.xaml` (host markup), `Strings/en/Resources.resw` (English)
- Cross-checked against `urnetwork-linux/docs/linux_agent_help.md` §7.7 + §8 (disagreements are listed in FLAGS; **code wins**).

The canvas is a decorative, non-interactive drawing surface. All interaction (click, focus, name) lives on the **hero button that wraps it**. It is a faithful port of iOS `Main/Connect/ConnectButton/` and every metric is written in iOS's 256pt canvas space.

---

## 1. Embedding — the hero button and its host

Element order in the CONNECT pane at this position (between the status group above and the selected-provider row below):

```
Border  (bottom hairline: BorderBrush = kBorder #FFFFFF @ 12% (#1FFFFFFF),
         BorderThickness 0,0,0,1; Padding 0,8  → 8dip vertical padding)
  └─ Button "ConnectHero"
       HorizontalAlignment=Stretch, content alignment Stretch
       Padding 0, BorderThickness 0
       ALL button chrome transparent (background + border in normal/hover/
       pressed/disabled states — the canvas is the whole visual)
       UseSystemFocusVisuals = OFF (the canvas draws its own focus ring)
       Click → OnConnectToggle (see §12)
       └─ Grid "ConnectCanvasHost"
            MaxWidth = 190 (Advanced Mode) / 320 (Simple Mode — set in code,
              MainWindow.xaml.cpp: canvasHost.MaxWidth(advancedMode_ ? 190.0 : 320.0))
            HorizontalAlignment = Center
            Accessibility: Raw (decorative — excluded from the a11y tree)
```

- The canvas constructor takes the host Grid and appends every visual into it.
- The host is stretched by its pane; **width is the driven dimension**. The canvas writes the host's **height** itself (see §3), so never react to height changes (recursion).
- Practical result on Windows: Advanced pane → host width 190 → globe side 182dip. Simple pane (480dip cap, host MaxWidth 320) → side 288dip (the max).
- If canvas construction throws, the page must still come up: log the error, drop the canvas, continue (a broken hero must not cost the page its live data wiring — this is called out in code as a real bug that happened).

## 2. Coordinate space and layout math

All constants below are **in 256-space** unless noted; multiply by `s`.

| Constant | Value | Meaning |
|---|---|---|
| `kIosCanvas` | 256.0 | iOS `canvasWidth`; every metric divides by this |
| `kMinSide` | 168 dip | globe side floor (below it the 12×12 grid stops resolving) |
| `kMaxSide` | 288 dip | globe side ceiling (above it it becomes a billboard) |
| `kSidePad` | 4 dip | horizontal breathing room inside the host (each side) |
| `kBandPad` | 12 dip | vertical padding — iOS `.padding()` |

Layout (run on host width change only; ignore changes < 0.5 dip):
```
side  = clamp(hostWidth − kSidePad·2, kMinSide, kMaxSide)     // 168..288
s     = side / 256
bandHeight = side + kBandPad·2                                 // host height, SET by the canvas
```
Windows guards the host-height write against NaN (unset height): write when unset OR |current−target| > 0.5. In GTK, simply request `bandHeight` as the widget's natural height for the measured width.

- The globe container is a `side × side` square, centered in the host both ways, **clipped to its own bounds** (rect clip 0,0,side,side). This clip is half of the mask mechanism: the connected circles start a full canvas-width off-centre and the clip eats them until they arrive.
- The globe container has a center-origin scale transform (used by hover, §10), initial 1.0.
- The whole canvas is hit-test invisible; the button takes the events.

## 3. The globe silhouette + the mask mechanism

**The shape.** `kGlobePath` — the ur-globe silhouette in a 32×32 box, byte-identical to the login-carousel globe (`Assets.xcassets/Icons/ur.symbols.globe.svg`). Path data (fill, non-zero):

```
M30 8C28.8955 8 28 7.10453 28 6C28 4.89547 27.1045 4 26 4C24.8955 4 24 3.10453 24 2C24 0.895469 23.1045 0 22 0H10C8.89547 0 8 0.895469 8 2C8 3.10453 7.10453 4 6 4C4.89547 4 4 4.89547 4 6C4 7.10453 3.10453 8 2 8C0.895469 8 0 8.89547 0 10V22C0 23.1045 0.895469 24 2 24C3.10453 24 4 24.8955 4 26C4 27.1045 4.89547 28 6 28C7.10453 28 8 28.8955 8 30C8 31.1045 8.89547 32 10 32H22C23.1045 32 24 31.1045 24 30C24 28.8955 24.8955 28 26 28C27.1045 28 28 27.1045 28 26C28 24.8955 28.8955 24 30 24C31.1045 24 32 23.1045 32 22V10C32 8.89547 31.1045 8 30 8Z
```
(A rounded square with four scalloped/step-notched corners — the iOS 256-box GlobeMask/GlobeConnector assets are this same path ×8.) Stretch mode is **Fill** onto the `side × side` square — an exact 1:1 map of the 32-box, never letterboxed.

**The mask.** iOS masks the whole ZStack with the globe image; Android draws `connect_mask` over a clipped Box. Windows (and the GTK port, if not using a real cairo clip) uses the Android construction, two primitives:
1. the rectangular bounds clip on the globe container (above), and
2. `mask_` — ONE path, **fill rule EvenOdd**, data = outer square `M0,0 L32,0 L32,32 L0,32 Z` + the globe path (i.e. square MINUS globe), filled **opaque in the resolved ground color**, drawn **last** (above every content layer). The four scalloped corners are painted back out to the page colour, leaving the globe.

> GTK note: cairo can do this natively — either replicate the overlay exactly (`cairo_set_fill_rule(CAIRO_FILL_RULE_EVEN_ODD)`, square+globe subpaths, fill with ground) or clip to the globe path and skip the overlay. The overlay is what Windows ships; a true clip is visually identical **and removes the ground-color dependency** — prefer the true clip in cairo/gsk, keep the overlay only if snapshotting widgets.

**Ground resolution (`ResolveGround`).** The overlay must vanish into whatever is behind the hero, and the globe base is *derived* from it:
- Walk up the widget ancestry (≤ 16 levels) looking for the first **fully opaque solid** background color (alpha == 255). On Windows this checks Panel/Border/Control backgrounds; the hero button itself is transparent, so opacity is the test, not "first background found".
- Fallback when nothing opaque is found: `kBackground` **#101010** (the window clear color).
- On every resolve (re-run at layout time; skip if unchanged):
  - `mask.fill = ground`
  - `globeBase = Lift(ground, hovered ? 0x14 : 0x0C)` — per-channel add, clamped 0..255. On #101010 that lands on **#1C1C1C** (= `kCard`, iOS `tintedBackgroundBase`) resting, **#242424** (= `kCardHover`) hovered.
  - `globeFill.fill = globeBase` and `coreGap.stroke = globeBase` (the 50pt gap ring is stroked in the globe's own base so it reads as a slot cut between disc and ring).
- Initial values before the first resolve: globeFill and coreGap = `kCard` #1C1C1C, mask = `kBackground` #101010.

## 4. Layer stack (bottom → top, inside the clipped globe square)

1. **globeFill** — globe path, filled `globeBase` (the lit-globe ground; iOS's `.background(tintedBackgroundBase)`).
2. **gridLayer** (one fadeable group — iOS mounts GlobeConnector + grid as one view and cross-fades that view). Initial: opacity 0, hidden. Children:
   a. **connectorBg** — globe path filled **white @ 4%** (#FFFFFF alpha 0x0A = 10/255 ≈ 0.039; GlobeConnector.svg `fill-opacity 0.04`), sized side×side.
   b. **equator** — full-width rectangle, height `2·s`, top edge at `126·s` (band covers 256-space y ∈ [126,128], **center y=127** = SVG "Line 1"), filled **#101010** (`kBackground`). The connector lines are PAGE colour, not light: in GlobeConnector.svg they are `stroke="#101010"` over the 4% wash — grooves cut out of the globe, not lines laid on it.
   c. **meridian** — centered ellipse **outline**, width `103·s` (rx 51.5), height `254·s` (ry 127) — SVG "Ellipse 54" — stroke #101010, stroke width `2·s`, no fill.
   d. **pointCanvas** — a side×side absolute-position canvas holding the live provider dots (§7).
3. **blobLayer** — the five connected-state circles (§8). Initially all hidden.
4. **idleLayer** — the disconnected core (§6). Children in order: pulse, coreRing, coreGap, core.
5. **glyph** — error/processing icon (§9). Initial: opacity 0, hidden.
6. **mask** — the square-minus-globe overlay (§3), sized side×side. (Skip if using a real clip.)

Outside the clip, appended to the host:
7. **focusRing** — the globe path again, **no fill**, stroke `kOffWhite` #F8F8F8, thickness **2 (unscaled)**, sized `side + 8` square, centered. Hidden by default; shown only for keyboard focus (§10).

Everything is hit-test invisible.

## 5. The five states and the crossfade rule

```
enum State { Disconnected, Connecting, Connected, Error, Processing }
```
iOS branch order: Processing and Error are **balance** states that REPLACE the connection canvas; the other three are connection status (DESTINATION_SET folds into Connecting, as Android's ConnectStatus does).

`SetState(state)` — no-op if unchanged. If leaving a live state ({Connecting, Connected}) for a non-live one, **ClearPoints()** (drop every dot + element; the next connect grows the grid in from nothing — iOS unmounts the connecting view and its animated-point map). Then apply visuals:

| Layer | Disconnected | Connecting | Connected | Error / Processing |
|---|---|---|---|---|
| idleLayer | fade → 1 | fade → 0 | fade → 0 | fade → 0 |
| gridLayer | fade → 0 | fade → 1 | fade → **1** (stays lit under the circles, but **frozen** — SetGrid ignores pushes) | fade → 0 |
| blobs | out | out | slide **in** | out |
| glyph | hidden | hidden | hidden | shown, delayed (§9) |
| idle pulse | burst starts | — | — | — |

**Crossfade:** `kStateFadeMs = 500`, cubic ease-in-out (iOS `.animation(.easeInOut(duration: 0.5), value: connectionStatus)`).

**Fade helper semantics** (used for idleLayer/gridLayer):
- Showing: make visible first, then animate opacity → target.
- If animations are disabled OR the canvas is not presenting: snap opacity, and when hiding also set fully hidden (not merely opacity 0 — a zero-opacity element still costs a composite; measured worth several points of a core).
- If already within 0.01 of the target: no animation (just hide if target 0).
- Hiding with animation: on completion, if opacity ≤ 0.01, set hidden.

**State sources** (ConnectPage::ApplyConnectStatus — one writer; the canvas may never lag the status line above it, same inputs, same instant, one function):

| Health / condition | Canvas state |
|---|---|
| Connected | Connected |
| Evaluating (providers, none proven) | **Connecting** (on purpose — the state in which SetGrid keeps taking updates, so the dots keep telling the evaluation story instead of freezing mid-way under a green headline) |
| Degraded | Connecting |
| Connecting (incl. DESTINATION_SET) | Connecting |
| Failed (settled failure) | Error |
| Disconnected/NoService but machine still captured (routes installed) | Connecting |
| Disconnected, kill switch armed | Disconnected |
| Disconnected (plain), NoService | Disconnected |
| **overridden by balance:** `balanceConfirming()` (post-checkout poll running) | Processing (wins over out-of-balance) |
| **overridden by balance:** `balanceBlocked()` and not confirming | Error |

## 6. Disconnected — the electric-blue core + bounded pulse burst

All diameters in 256-space (×`s`), all centered:

| Element | Diameter | Paint |
|---|---|---|
| `pulse` | **56** (`kPulseD`) | filled `kUrElectricBlue` **#0039DE**; center-origin scale transform; resting opacity **0** |
| `coreRing` | **52** (`kCoreRingD`) | ring, stroke #0039DE, stroke width `4·s` |
| `coreGap` | **50** (`kCoreGapD`) | ring, stroke = **globeBase** (lifted ground, #1C1C1C on the page), stroke width `2·s` — the hairline of background between disc and ring |
| `core` | **48** (`kCoreD`) | filled #0039DE |

Three separate ring/disc elements because iOS draws three.

**The pulse burst** (`StartIdle`) — iOS `ConnectCanvasDisconnectedStateView`: a 56pt disc scales to **1.5** while its opacity runs **1.5 − scale**, ease-out, 1.5 s, `.repeatForever`. The desktop version is **bounded** (tray app; measured cost of the always-on version: settled 0.42–0.62% of a core, animating +0.8 to +3.3 points):
- Preconditions: presenting AND OS animations enabled AND state == Disconnected; otherwise pulse opacity stays 0.
- Three simultaneous tracks, each duration `kPulseMs = 1500`, **cubic ease-out**, repeated `kIdlePulseBursts = 3` times, then stop (pulse still, opacity 0):
  - scaleX: 1.0 → **1.5** (`kPulseScaleTo`)
  - scaleY: 1.0 → 1.5
  - opacity: **0.5** (`kPulseOpacityFrom` = 1.5 − 1.0) → 0.0
  (same easing on all three keeps the iOS invariant `opacity = 1.5 − scale` throughout.)
- The burst runs: (a) when the disconnected state lands while presenting, (b) when the window is re-shown, (c) on pointer **arrival** over the hero (§10) — exactly when a desktop user is deciding whether this is clickable. Pointer leaving does NOT start another round. In between the hero is still — "still is fine: it is a lit globe, not an empty screen."
- `StopAll()` = stop the pulse animation and force pulse opacity 0. Called before every state application and on hover change.

## 7. Connecting — GlobeConnector lines + the live provider grid

Static geometry: §4 items 2a–2c (white-4% globe wash, equator top 126·s height 2·s, meridian ellipse 103×254·s stroke 2·s, both #101010).

**Empty grid is a NORMAL state** (no session, rpc-only session, connection carrying no traffic yet): render the bare lattice (wash + equator + meridian, zero dots). Not an error, never a spinner.

### 7.1 The feed
`SetGrid(points, gridWidth, gridHeight)` — called from the stats push (§13). **Ignored unless state == Connecting** (iOS freezes the grid the instant the connection lands; the connecting layer stays mounted under the circles but stops taking updates, which also lets Connected settle to zero work).

`ProviderGridPoint` (SDK JSON shape): `X:number, Y:number, ClientId: uuid|null, State: string, EndTime: rfc3339|null, Active: boolean`. The canvas consumes X, Y, ClientId, State.

### 7.2 Cell math
```
cols = max(gridWidth, gridHeight)         // iOS scales by gridWidth alone; taking the larger
                                          // keeps a non-square grid inside the globe  [FLAG vs doc]
cell = cols > 0 ? side / cols : 0         // iOS maxPointSize
dot center = (x·cell + cell/2, y·cell + cell/2)
dot diameter = cell                       // neighbouring dots touch
```
Nothing is culled at the rim — the mask/clip does that. Dots have a center-origin scale transform (scale, not size: per-frame size changes would invalidate layout; a render transform does not).

### 7.3 Point states and colors
Parse `State` string → `{InEvaluation, EvaluationFailed, NotAdded, Added, Removed}`; **unknown strings → InEvaluation** (a provider the SDK has not accepted must not render as one it has). `Removed` is iOS's own extra case for points that leave the grid.

Colors (iOS `ConnectCanvasConnectingStateViewModel.colorForState`, exactly):

| PointState | Color |
|---|---|
| InEvaluation | `kAccent` (urLightYellow) **#EFF7BB** |
| EvaluationFailed | `kUrCoral` **#FF6C58** |
| NotAdded | `kUrCoral` **#FF6C58** (really the same coral on iOS: the grid says "not carrying traffic", not why) |
| Added | `kUrGreen` **#87FB67** |
| Removed | **ARGB(0, 0x10, 0x10, 0x10)** — urBlack at opacity 0; the RGB matters because the color blend runs through it |

### 7.4 The diff (per SetGrid push)
- Key = `ClientId` if present and non-empty, else `"x,y"` (a cell with no client id is still a cell; keying by position stops it churning in/out every push).
- Hard cap `kMaxPoints = 1024` live dots (malformed-push guard; the SDK never produces a grid this big).
- **New key** → create dot: fill = color for its state, scale **0** (grow-in: sizeProgress = 0), colorProgress = 1, previous = state.
- **Existing key** → update x/y; if state changed: `previous = old state; state = new; colorProgress = 0` (color re-blends from old to new).
- **Missing key** (SDK stopped reporting it) and not already Removed → `previous = state; state = Removed; colorProgress = 0` (fades out through the Removed color over one transition rather than vanishing between frames).
- If gridWidth/gridHeight changed → full relayout; else just reposition/resize dots.
- Recompute `animating` = any dot has colorProgress < 1 or sizeProgress < 1.

### 7.5 Per-tick animation (~10 fps, §14)
`Tick()` returns immediately unless `animating` (TransferChart's rule: the per-frame path costs nothing unless something is genuinely moving; everything that repeats is a compositor animation, not the tick).
Per animating dot, per tick:
- `colorProgress = min(1, colorProgress + kPointStep)`; `sizeProgress` likewise. **`kPointStep = 0.15`** → ~0.67 s to settle at 10 fps (iOS runs 60 fps × 0.05 = 0.33 s, but per-point-arrival; this step keeps the grow-in legible instead of snapping in five frames).
- Fill color = channel-wise **linear ARGB lerp** from `colorFor(previous)` to `colorFor(state)` at `colorProgress` (round-to-nearest per channel).
- Scale = cubic ease-in-out of `sizeProgress`: `t < 0.5 ? 4t³ : 1 − (−2t+2)³/2` (the view model's own easeInOut applied to the grow-in), applied to both axes.
- A dot in `Removed` whose both progresses hit 1 is **deleted** (element removed from the canvas and the map).

`ClearPoints()` (on leaving live states): remove all dot elements, clear map, `animating = false`.

## 8. Connected — five 180pt brand circles that slide in

`ConnectCanvasConnectedStateView`: five **opaque** circles, diameter **180** in 256-space (`kBlobD`, ×`s`), that slide in from off-canvas and occlude one another. They never fade — off-canvas is how they hide (parked outside the clip). Layered ABOVE the grid (globe → dots → circles). The Windows extra: also hide them entirely when parked (five sprites outside the clip still cost a tree walk every presented frame).

**Offsets are in canvas-widths** (multiply by `side`; that is literally how the Swift writes them, `canvasWidth / 3.5` etc.), applied as translate transforms from center:

Entry (initial) offsets — 5 pairs:
```
(−1, −1)  (+1, −1)  (−1, +1)  (+1, +1)  (+1, 0)
```
Settled (final) offsets — 5 pairs, index-paired with the initials:
```
(−1/3.5, −1/4)  (+1/4, −1/3)  (−1/3, +1/4)  (+1/5, +1/2.5)  (+1/4, 0)
```
Palette — 5 colors:
```
kUrCoral  #FF6C58
kUrGreen  #87FB67
urLightBlue ≈ blend(kOffWhite #F8F8F8 → kToggleAccent #638BFC, t=0.2) = #DAE2F9
          (iOS's own urLightBlue is #D6E6F4; Windows derives it because the shared
           token file is byte-matched across clients and not extendable — replicate
           the blend for Windows parity)  [FLAG: code comment says "#D9E1F8"; the
           arithmetic with round-half-up lands on #DAE2F9]
kAccent   #EFF7BB   (urLightYellow)
kUrPink   #ED8FFF   (iOS .accent)
```

**Reshuffle rule** (`ShuffleBlobs`): shuffle the palette array and, independently, the (initial,final) pair order — so the overlaps differ on every connect (iOS reshuffles both arrays in `onChange(of: isActive)`). Run at construction and again on every out→in transition. RNG: `std::mt19937` seeded **0x5EED0BE** (constant seed — the sequence of arrangements is deterministic across runs; keep for parity).

**RunBlobs(in)**:
- No-op transitions (same direction), animations disabled, or not presenting → settle instantly: positions at (in ? final : initial)·side, visible iff in.
- Out→in with animations: reshuffle, make all five visible, animate X and Y translate from `initial·side` to `final·side`, duration `kBlobMs = 1000` ms, **cubic ease-in-out** (iOS `.easeInOut(duration: 1)`). Nothing else animates — the connected state settles the instant its circles land (iOS does not animate it either).
- In→out with animations: animate back to the entry offsets (same 1000 ms curve), hide all five on completion (guarded: skip if state flipped back mid-flight).
- Window re-shown while connected: settle circles at final offsets, visible — never replay the entrance on every return from the tray.

## 9. Error / Processing — the faint glyph, 500 ms delayed

Both states REPLACE the canvas (first two arms of ConnectButtonView's if/else): idle and grid layers fade out, blobs out — the glyph sits alone on the bare lit globe.

- One centered icon: Windows uses Segoe Fluent Icons — **Error = U+E7BA "Warning"**, **Processing = U+E823 "Recent"** (a clock/waiting dial). [FLAG: font not on Linux — substitute visually-equivalent icons: a triangle-bang warning outline and a clock outline.]
- Color `kTextFaint` **#5A5A5A**; size **32** in 256-space (`kGlyphSize`, ×`s`; iOS `.font(.system(size: 32))`).
- Entrance: opacity 0, then **delay `kGlyphDelayMs = 500`**, fade in **`kGlyphFadeMs = 300`** cubic ease-in-out — iOS delays both glyphs 500 ms so a transient balance blip never flashes a warning. If animations disabled or not presenting: opacity 1 immediately.
- On leaving a blocked state: opacity 0 + hidden immediately (no fade-out).

## 10. Desktop affordances (neither phone client has these)

**Hover** (`SetHovered`, driven by pointer enter/leave on the hero button):
- The globe **warms one card step**: globeFill = `Lift(ground, 0x14)` (hover) vs `Lift(ground, 0x0C)` (rest) → #242424 vs #1C1C1C on the page. (Note: only globeFill warms on hover; coreGap keeps the resting base until the next ApplyGround.)
- The globe **lifts**: whole-globe center scale to **1.03** (kHoverScale), duration **180 ms**, cubic **ease-out**; back to 1.0 on exit, same curve. Snap if animations disabled. [FLAG: motion doc phase-4 wants 150 ms Standard — spec-only, not shipped; code = 180 ms ease-out.]
- Pointer **arrival** restarts the bounded idle pulse burst (StopAll → StartIdle, i.e. 3 more cycles, Disconnected only). Leaving stops the pulse and does not start another round.

**Keyboard focus ring** (`SetFocusRingVisible`):
- Shown on focus-in **only when the focus is keyboard-sourced** (Windows: `FocusState == Keyboard`; GTK: `gtk_widget_has_visible_focus`/focus-visible). Hidden on focus-out. A ring on mouse press is noise.
- The ring: the globe silhouette path itself, stroke #F8F8F8 width 2 (unscaled), sized `side + 8`, centered, drawn OUTSIDE the mask/clip so it is not eaten by it.

## 11. Presentation gating + motion budget (normative)

- `SetPresentationActive(bool)` — idempotent. False (window hidden / another page): stop every animation (pulse and blob slide). True: if connected, settle circles at final offsets visible (no entrance replay); then StartIdle (pulse burst if Disconnected).
- `AnimationsEnabled()` — the OS "show animations" switch (Windows `UISettings::AnimationsEnabled`; GTK: `gtk-enable-animations`). Off means motion GONE, not reduced: no repeating animation is ever started, every Fade/slide snaps to its settled value.
- Repeating/one-shot motion (pulse, blob slide, fades, hover scale) runs on the compositor/frame clock, off the tick.
- `Tick()` is the ONLY per-frame code path and returns immediately unless a grid-point transition is in flight (~10 fps, shared clock, §14).
- Hidden elements are fully unmapped/collapsed, not opacity-0 (a zero-opacity element still gets walked and composited).

## 12. Click behavior, gating, a11y (on the hero button)

**Click = connect toggle** — identical handler to the Connect button (`OnConnectToggle`):
1. If rendered health == Failed → **Retry contract**: `Sdk().Disconnect()` (tears down multi-client/routes/window) then fall through to connect — a retry press IS a connect gesture, and it must win over the disconnect predicate (in Failed the SDK still holds a destination, so the predicate reads Disconnect, but the button SAYS Retry).
2. Else if `ConnectActionIsDisconnect()` (SDK driving OR machine captured — routes/firewall; predicate shared with the tray) → `Sdk().Disconnect()`, return.
3. Else: optimistically set local status = Connecting and health = Connecting and re-render NOW (a press with no visible change is indistinguishable from a hang; the next SDK push corrects it), then `Sdk().ConnectBestAvailable()` if the selection is best-available, else `Sdk().Connect(*Sdk().SelectedLocation())` — connect to what the user PICKED (the immediate entry points, not the coalesced row variants).

**Enable gating** (applied to ConnectButton AND ConnectHero together):
- `blocked = processing || outOfBalance` → disabled (nothing a press can do; iOS blocks the tap in exactly these two cases).
- `transitional = connectStatus == CONNECTING` → disabled, **as a watchdog, not a latch**: record when the transition began; after `kConnectWatchdog = 8 s` re-enable even though still CONNECTING (a hung connect must not leave a dead control). The watchdog is re-checked on the shared tick (~10 fps) by re-rendering while connecting-and-not-yet-fired. DESTINATION_SET and CONNECTED stay enabled (Disconnect is legitimate in both).
- `enabled = !blocked && (!transitional || watchdogFired)`.

**Accessibility:**
- The canvas host and everything in it: decorative, out of the a11y tree.
- The hero button gets NO automatic name (its content is a panel) — set an explicit accessible name **= the current status text**, updated on every status render. The names it carries (store key → English):
  - `connected` → "Connected"
  - `connecting_status_indicator` → "Connecting to providers"
  - `disconnected` → "Disconnected"
  - Adv `conn_finding_providers` → "Finding providers…"
  - Adv `conn_degraded` → "Connection degraded — reconnecting"
  - Adv `conn_failed` → "Couldn't connect"
  - Adv `conn_disconnecting` → "Disconnecting — your traffic is still going through the tunnel"
  - Adv `conn_blocked_kill_switch` → "Blocked — kill switch on"
- The canvas itself renders no text. (Related, same handler, for context: the Connect button's own labels are `connect` "Connect" / `disconnect` "Disconnect" / `retry` "Retry".)

## 13. The grid feed — SDK wiring (what actually calls SetGrid)

Subscription (SdkHost, once per session):
```
connectVc = device->openConnectViewController();  connectVc->start();
connectVc->addConnectionStatusListener(→ PublishStats)
connectVc->addGridListener(→ PublishStats)            // THE feed the canvas rides
connectVc->addSelectedLocationListener(→ PublishStats)
```
Every push → `ReadStats()` snapshot:
```
grid = connectVc->getGrid()          // returns a HANDLE; 0 while nothing is connected —
                                     // treat handle 0 as the empty grid, make NO getter
                                     // calls on it (each was a recovered Go nil-receiver
                                     // panic, ~570 log lines per idle session)
providerCount = grid.getWindowCurrentSize()
gridWidth  = grid.getWidth()         // scalars, cannot throw
gridHeight = grid.getHeight()
gridPoints = grid.getProviderGridPointList()   // through the guarded list reader:
                                     // a nil Go slice marshals as the JSON document
                                     // `null` → treat as empty, log once, never throw
```
Clamps that zero the grid before it reaches the UI: **rpc-only session** and **service down** (the hero must not draw a live populated grid for a session that isn't tunneling).
Then `ConnectPage::ApplyStats(stats)` → `canvas->SetGrid(stats.gridPoints, stats.gridWidth, stats.gridHeight)` — fed through **unconditionally** (empty list included; the canvas renders it as the bare lattice), gated only by preview mode.
C ABI surface underneath (linux SDK header, for the GTK port): `urnet_connect_grid_get_width/get_height/get_window_current_size/get_window_target_size/get_provider_grid_point_list/get_provider_grid_point_by_client_id`.

## 14. The tick clock

- One shared page timer: **100 ms interval (~10 fps)**, drives all charts AND `canvas->Tick()` AND the connect watchdog re-render AND (dev) the preview walk.
- Gates, in order: window visible (tray-hidden ticks do nothing) → connect view visible → tick charts + canvas.
- `SetPresentationActive(active)` is called from page/window visibility changes, independently of the timer gate.

## 15. Dev/preview harness (port it — it is how the motion gets verified)

`--preview-ui` + env `URNETWORK_PREVIEW_HERO` set → the real status/grid writes to the canvas are suppressed and a preview tick (on the same 100 ms clock) drives it: walks the five states in order {Disconnected, Connecting, Connected, Error, Processing} at a **4 s cadence** (40 ticks/state), and pushes a synthetic **14×14** grid once per second (every 10th tick): deterministic hash `h(v)=v·2654435761`, cell occupied unless `(h>>8)%100 < 42`, ClientId `"preview-x-y"`, state by `(h>>3)%8`: 0→InEvaluation, 1→EvaluationFailed, 2→NotAdded, else Added.

## 16. GTK4/cairo implementation notes

- Draw the whole canvas in one `GtkDrawingArea`/snapshot widget inside a `GtkButton` (or a click-gesture-carrying focusable widget) that supplies name/click/enable, with `can-focus`, focus-visible-driven ring, and enter/leave controller for hover.
- Prefer a real cairo even-odd clip to the globe path over the opaque overlay (see §3) — then the ground walk is only needed for the globe base lift. If you keep the overlay, resolve ground from the widget's ancestor CSS background; fallback #101010.
- Scale the 32-box path by `side/32`; all 256-space metrics by `side/256`.
- Animations: `gtk_widget_add_tick_callback`/`AdwAnimation` for pulse (1500 ms ease-out ×3), blob slide (1000 ms ease-in-out), state fades (500 ms ease-in-out), hover scale (180 ms ease-out), glyph (500 ms delay + 300 ms fade); honor `gtk-enable-animations` exactly as §11; keep dot transitions on the shared 100 ms tick with `kPointStep 0.15`.
- Fonts/icons: no Segoe Fluent — draw the warning/clock glyphs as paths or use symbolic icons at 32·s, color #5A5A5A.

---

## FLAGS (doc vs code, risks, open questions) — code wins

1. **Equator position**: doc §7.7 says "equator y=127"; code places the 2pt band's TOP at 126·s (center 127, matching the SVG line). Implement top=126·s, height=2·s.
2. **Dot size divisor**: doc says "dot diameter = side/gridWidth"; code divides by **max(gridWidth, gridHeight)** so a non-square grid stays inside the globe. Implement the max.
3. **Blob light-blue**: code comment claims the blend lands on #D9E1F8; the actual arithmetic (`lround` per channel of #F8F8F8→#638BFC at t=0.2) gives **#DAE2F9**. iOS's true token is #D6E6F4. Recommend replicating the blend (#DAE2F9) for Windows parity; note the three-way drift.
4. **Pulse burst count**: doc says "bounded pulse burst" without a count; code is exactly **3 cycles** (kIdlePulseBursts), re-armed on page-show and on hover arrival only.
5. **Hover lift duration**: motion doc phase 4 ("hero hover lift → 150 ms Standard") is marked spec-only; shipped code is **180 ms cubic ease-out, scale 1.03**. Port the shipped values; the 150 ms unification is a future work item.
6. **Connect success bloom** (doc §8 phase 3: hero pulses 1→1.02→1 on Connecting→Connected, ζ=0.75/40 ms spring): **spec-only, NOT in ConnectCanvas** — do not build it as part of parity; it is a separate motion-phase item.
7. **#0039DE role**: doc §8.1 palette table lists it only as "chart: used balance / usage bar"; it is also the hero's disconnected core/ring/pulse color (kUrElectricBlue). Same token, extra role.
8. **Segoe Fluent glyphs** U+E7BA/U+E823 do not exist on Linux — the port must substitute equivalent warning/clock icons (risk: pick outline weights that read "faint" at #5A5A5A).
9. **Deterministic RNG** seed 0x5EED0BE means the blob arrangement sequence is identical on every app launch (first connect always shows the same composition). This is shipped behavior; keep it unless the owner asks for true randomness.
10. **Hover warm scope**: on hover only globeFill warms to Lift(ground,0x14); coreGap's stroke stays at the resting base until the next ApplyGround pass. Faithful ports should replicate this (the 2·s hairline difference is invisible in practice).
11. **Ground-walk portability**: the ancestor background walk is WinUI-specific; a cairo clip removes the need for the mask fill entirely. If the overlay is kept, the GTK CSS background resolution is an approximation — the doc's warning stands: the overlay assumes the hero sits on an opaque solid.
12. **Doc §7.7 omission**: the freeze rule (SetGrid ignored outside Connecting) and the Removed fade-out lifecycle are not in the doc at all — they are load-bearing for both fidelity and the idle budget.

## SDK surface referenced
- urnet::Device::openConnectViewController()
- urnet::ConnectViewController::start()
- urnet::ConnectViewController::addConnectionStatusListener(cb)
- urnet::ConnectViewController::addGridListener(cb)  — the provider-grid feed the hero rides
- urnet::ConnectViewController::addSelectedLocationListener(cb)
- urnet::ConnectViewController::getConnectionStatus()
- urnet::ConnectViewController::getConnected()
- urnet::ConnectViewController::getGrid()  — returns a handle; 0 == no grid, make no calls on it
- urnet::ConnectGrid::getWindowCurrentSize()
- urnet::ConnectGrid::getWidth()
- urnet::ConnectGrid::getHeight()
- urnet::ConnectGrid::getProviderGridPointList()  — nil Go slice marshals as JSON `null`; treat as empty
- urnet::ProviderGridPoint {X, Y, ClientId?, State, EndTime?, Active}
- C ABI: urnet_connect_grid_get_width / get_height / get_window_current_size / get_window_target_size / get_provider_grid_point_list / get_provider_grid_point_by_client_id
- Sdk().Disconnect()  — hero click, disconnect/retry arm
- Sdk().Connect(selectedLocation)  — hero click, connect arm
- Sdk().ConnectBestAvailable()  — hero click when selection is best-available
- Sdk().SelectedLocation()
- Sdk().RepublishStats()  — health re-evaluation nudge on the shared tick
- SdkHost::LiveStats {gridPoints, gridWidth, gridHeight, connectionStatus, health, ...}  — the snapshot ApplyStats feeds into SetGrid/SetState
- Balance surfaces read for the two override states: balanceConfirming() → Processing, balanceBlocked() → Error

## Flags (doc-vs-code drift / risks)
- Doc §7.7 says equator 'y=127'; code puts the 2pt band's top at 126·s (center 127). Use top=126·s, height=2·s — code wins.
- Doc §7.7 says dot diameter = side/gridWidth; code divides by max(gridWidth, gridHeight) to keep non-square grids inside the globe — code wins.
- Windows code comment claims the blended blob light-blue is #D9E1F8; the actual blend(kOffWhite→kToggleAccent, 0.2) computes to #DAE2F9, and iOS's real urLightBlue token is #D6E6F4. Replicate the blend (#DAE2F9) for Windows parity.
- Idle pulse burst count (3, kIdlePulseBursts) and the hover-arrival re-arm are in code but absent from the doc.
- Motion doc phase-4 'hover lift → 150 ms Standard' is spec-only; shipped hover is 180 ms cubic ease-out to scale 1.03 — port the shipped values.
- Doc §8 phase-3 connect success bloom (hero 1→1.02→1 spring on Connecting→Connected) is spec-only and NOT in ConnectCanvas — exclude from parity.
- Doc §8.1 lists #0039DE only as a chart semantic; it is also the disconnected hero core/ring/pulse color (kUrElectricBlue).
- Segoe Fluent glyphs U+E7BA (Warning) and U+E823 (Recent) do not exist on Linux — the port needs substitute warning/clock icons at 32·s in #5A5A5A.
- The blob shuffle RNG is seeded with the constant 0x5EED0BE — arrangement sequence is identical every app run; shipped behavior, keep for parity.
- On hover only globeFill warms to Lift(ground,0x14); the coreGap stroke keeps the resting base until the next ApplyGround — replicate as-is.
- The mask-overlay approach assumes an opaque solid ground resolved from the ancestor tree; in cairo/gsk a true even-odd clip to the globe path is visually identical and removes that dependency — recommended for the GTK port.
- The grid freeze rule (SetGrid ignored unless state==Connecting) and the Removed-state fade-out lifecycle are load-bearing but undocumented in linux_agent_help.md §7.7.
- connectVc->getGrid() returns handle 0 when no grid exists; calling getters on it was ~570 recovered Go panics per idle session on Windows — the GTK port must guard identically.
- Hero enable gating: disabled while processing/out-of-balance, and during CONNECTING only as an 8 s watchdog (re-enabled after), re-checked on the 100 ms tick — a latch here is a known past bug pattern.
