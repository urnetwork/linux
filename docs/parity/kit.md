# URnetwork Component Kit — Implementation-Ready Spec (GTK4/C++ port)

Source of truth (read in full, code wins over docs):
- `urnetwork-windows/app/src/App/UrComponents.h` (503 lines)
- `urnetwork-windows/app/src/App/UrComponents.cpp` (925 lines)
- `urnetwork-windows/app/src/App/UrColors.h`
- `urnetwork-windows/app/src/App/App.xaml` lines 830–1123 (the pane-shell styles; lines 830–899 are included because the kit resolves them by key)

This kit is the shared vocabulary all pages are assembled from. It is **pure UI**: it makes **zero** `urnet::`/Sdk calls. Its only platform side effects are (a) the system clipboard write in `MakeCopyField` and (b) a one-shot dispatcher timer in `Snackbar`. All user-facing strings are **passed in by callers** (from the localization store); the kit itself hard-codes exactly two literals — the mask string `••••••••` and the accessible-name prefix `"Copy "` (see Flags).

---

## 0. Tokens the kit spends

### 0.1 Colors (UrColors.h; App.xaml brushes mirror them 1:1)

| Constant / brush key | Hex (ARGB) | Role in the kit |
|---|---|---|
| `kBackground` / `UrBackgroundBrush` | `#FF101010` | page ground; `MakePaneSearchRow` root fill |
| `kSheet` / `UrSheetBrush` | `#FF151515` | pane header / group header / table header fill |
| `kCard` / `UrCardBrush` | `#FF1C1C1C` | card fill; row **hover** fill; selected-row fill |
| `kCardHover` / `UrCardHoverBrush` | `#FF242424` | card hover (card-button template; not used by pane rows) |
| `kCardPressed` / `UrCardPressedBrush` | `#FF2A2A2A` | row/card **pressed** fill |
| `kBorder` / `UrBorderBrush` | `#1FFFFFFF` (white @ 12%) | every hairline, dividers, separators |
| — / `UrBorderStrongBrush` | `#38FFFFFF` (white @ 22%) | outlined secondary action border |
| `kText` (= `kOffWhite`) / `UrTextBrush` | `#FFF8F8F8` | body text (never pure white) |
| `kTextMuted` / `UrTextMutedBrush` | `#FF989898` | secondary text, icons, notes, meta |
| `kTextFaint` / `UrTextFaintBrush` | `#FF5A5A5A` | tertiary: empty-state glyphs/lines, column headers, status-field captions |
| `kDanger` / `UrDangerBrush` | `#FFF8523B` | Invalid supporting text |
| `kInverseText` / `UrInverseTextBrush` | `#FF101010` | label ON the blue primary action |
| `kAccent` / `UrAccentBrush` | `#FFEFF7BB` | the 2px selection marker bar (and brand/earnings accents elsewhere) |
| `kToggleAccent` / `UrBlueMediumBrush` | `#FF638BFC` | the pane primary action fill (THE action color) |
| `kUrGreen` / `UrGreenBrush` | `#FF87FB67` | Valid supporting text |

(Full palette also defines: `kProGold #FFC400`, `kProGoldLight #FFE082` — Pro entitlement ONLY; `kStatusConnecting #E6EA23`, `kStatusIdle #2A60FF`; charts `kUrPink #ED8FFF`, `kUrCoral #FF6C58`, `kUrElectricBlue #0039DE`, `kUrMutedCoral #C8604F`, `kUrAmber #F5C242`; `UrCoralSubtleBrush #26FF6C58`. The kit itself only spends the rows in the table above.)

### 0.2 Fonts

| Key | Face | Kit use |
|---|---|---|
| `UrBodyFontFamily` | **PP Neue Montreal** (single Regular; SemiBold is synthesized) | every pane text style |
| `UrHeadingFontFamily` | **ABC Gravity Extended** | `UrTitleTextStyle` (page header title) |
| `UrHeadingCondensedFontFamily` | **ABC Gravity Extra Condensed** | `UrStatValueStyle` (metric values) |
| `UrIconFontFamily` | **Segoe Fluent Icons** (explicitly named — never the toolkit default icon face; a GTK port needs an equivalent glyph set) | every FontIcon the kit builds |

### 0.3 Pane metrics (App.xaml resource doubles)

| Key | Value |
|---|---|
| `UrPaneHeaderHeight` | **40** |
| `UrGroupHeaderHeight` | **28** |
| `UrPaneRowHeight` | **40** (single-line row) |
| `UrPaneListRowHeight` | **36** (list/table row; NB: the kit's C++ default args carry 36 directly, only `MakePaneSearchRow` reads the dictionary) |
| `UrPaneRowTallHeight` | **44** (two-line row; a list of explained rows is 44 throughout, including rows with no note) |
| key-value row default | **34** (C++ default arg only, no dictionary key) |

### 0.4 Breakpoint constants (`UrComponents.h`)

- `urnw::kit::kWideBreakpointDip = 1000.0` — THE app-wide breakpoint. Below: 1:1000:1 centred single column (flyout shape). At/above: main pane + side pane beside it + full-width row under both. One window-level function (`MainWindow::ApplyBreakpoint`) applies it; there are deliberately no per-page size handlers. Rationale baked into the number: pane grid sits inside a 24px page margin + 48px nav rail, so 1000dip ≈ 900dip between panes, where a ~340dip rail beside a main column stops being cramped.
- `urnw::kit::kUltraWideDip = 1800.0` — the second breakpoint, **Home only**: above it Home gains a THIRD column (activity beside the hero). Doc §7.1 says this constant is currently *reserved* — Home's third pane in shipped code keys off `1000 + Advanced Mode`, and 1800 is only logged (see Flags). Port both constants; wire per `MainWindow::ApplyBreakpoint`, which is authoritative.

---

## 1. `SetTextOrCollapse(line, text)`

Sets a TextBlock's text AND visibility in one call: empty string ⇒ `Visibility::Collapsed`, non-empty ⇒ `Visible`. Null-safe (no-op on null).

WHY (normative, keep in the port): a StackPanel spends its `Spacing` on children that drew nothing; four empty conditional lines cost ~120px of blank card in the disconnected state — the state the app OPENS in. Any conditional line must use this, never bare `.Text()`. GTK equivalent: `gtk_widget_set_visible(label, *text != '\0')` alongside `set_text`.

## 2. `MakeEmptyState(glyph, text)` / `MakeEmptyStateCard(glyph, text)`

The Windows idiom for "there is nothing here yet". **A dash cannot distinguish "nothing" from "not loaded" from "failed"** — empty, loading and failed must each render distinguishably; this is the *empty* one.

`MakeEmptyState` construction (returns the column):
- StackPanel, vertical, `Spacing 8`, `HorizontalAlignment Center`, `Padding 16,24,16,24` (L,T,R,B).
- FontIcon: icon font, caller's glyph codepoint (e.g. U+E8C7 in the header's example; doc's catalogue lists U+E9D2 as the generic empty-state glyph), `FontSize 28`, foreground **faint `#5A5A5A`**, `HorizontalAlignment Center`, a11y **Raw** (decorative, not announced).
- TextBlock: caller's text, `FontSize 12`, `TextWrapping Wrap`, `TextAlignment Center`, foreground **muted `#989898`**, `HorizontalAlignment Center`. NOTE: no font family is set in code — it renders in the toolkit default face, not Montreal (see Flags).

`MakeEmptyStateCard`: the same column wrapped in a Border styled `UrCardStyle` (fill `#1C1C1C`, 1px `#1FFFFFFF` border, corner radius 12, padding 16). Use ONLY where no card already surrounds the content (leaderboard rows, payouts ledger) — a card in a card reads as two edges 16px apart; inside an existing card use `MakeEmptyState`. `text` must come from the localization store (kit takes it pre-localized).

## 3. `MakeDivider()`

Border: `Height 1`, `HorizontalAlignment Stretch`, background `#1FFFFFFF`. Identical to markup style `UrDividerStyle` (Height 1, Background UrBorderBrush, Stretch) — keep the two in step. Horizontal rule for code-built row groups (Settings sections, payouts ledger, points breakdown).

## 4. `MakeSectionHeader(glyph, text)`

Header above a card:
- StackPanel horizontal, `Spacing 8`.
- FontIcon: icon font, glyph, `FontSize 16`, muted `#989898`, `VerticalAlignment Center`, a11y **Raw** (decoration beside a label that already carries the word).
- TextBlock: text, `FontSize 16`, `FontWeight SemiBold`, foreground `#F8F8F8`, `VerticalAlignment Center`. **No FontFamily is set in code** ⇒ default system face — the header comment claims "the display face at 16 SemiBold" and doc §8.2 claims "body-face 15 SemiBold"; the code does neither (see Flags; code wins: 16 SemiBold, default face).

## 5. `MakePageHeader(title[, description])`

Every Wave-2 destination opens with one.
- StackPanel vertical, `Spacing 4`, `VerticalAlignment Top`.
- Title TextBlock: style `UrTitleTextStyle` = **ABC Gravity Extended, 28px, LineHeight 36, `#F8F8F8`**.
- Description (only appended if non-empty): style `UrBodyTextStyle` = Montreal 14/20, then foreground overridden to muted `#989898`, `TextWrapping Wrap`, **`MaxWidth 560`** (~60ch reading measure), `HorizontalAlignment Left`.

## 6. `MakeMetricCard(label[, value]) → {root, label, value}`

Boxed stat tile (KPI rows on Earnings/Account). Returns live TextBlocks so pages update without rebuilding.
- root Border: style `UrStatTileStyle` = UrCardStyle basis (fill `#1C1C1C`, 1px `#1FFFFFFF` border) with **Padding 16,12** and **CornerRadius 8** (not the card's 12).
- Inner StackPanel (spacing 0 — none set):
  1. label TextBlock: `UrStatLabelStyle` = Montreal 12, muted `#989898`, `TextTrimming CharacterEllipsis`.
  2. value TextBlock: `UrStatValueStyle` = **ABC Gravity Extra Condensed 26**, `#F8F8F8`, CharacterEllipsis.

## 7. `MakeSettingsCard(glyph, title[, description]) → {root, title, description, trailing}`

One settings row on a card surface.
- root Border: `UrCardStyle` (fill `#1C1C1C`, border 1px `#1FFFFFFF`, radius 12, padding 16).
- Grid, `ColumnSpacing 12`, columns **Auto | Star | Auto**:
  - Col 0 FontIcon: glyph, **FontSize 20**, muted `#989898`, `VerticalAlignment Center`, a11y **Raw**.
  - Col 1 StackPanel `Spacing 2`, `VerticalAlignment Center`:
    - title: `UrBodyTextStyle` (Montreal 14/20 `#F8F8F8`) + `TextWrapping Wrap`.
    - description: `UrCaptionTextStyle` (Montreal 12/16 muted `#989898`) + Wrap; `Visibility Collapsed` iff the description arg is empty (set once at construction — later updates must manage visibility themselves).
  - Col 2 `trailing` Grid: `HorizontalAlignment Right`, `VerticalAlignment Center` — the caller drops a ToggleSwitch / ComboBox / chevron here and MUST point that control's `AutomationProperties.LabeledBy` at `title`.

## 8. `MakeCopyField(label, value[, masked=false]) → {root, value, copy}`

Caption + (optionally masked) value + copy button. **The masking rule: masked affects DISPLAY ONLY; the copy button always writes the FULL real value to the clipboard, never the mask.**
- root StackPanel vertical, `Spacing 2`:
  1. caption TextBlock: `UrLabelStyle` = Montreal 12 muted `#989898`; text = `label`.
  2. Grid `ColumnSpacing 8`, columns **Star | Auto**:
     - Col 0 `value` TextBlock: text = `masked ? "••••••••" (exactly 8 × U+2022) : value`; `UrBodyTextStyle` (14/20 `#F8F8F8`); `TextTrimming CharacterEllipsis`; `VerticalAlignment Center`.
     - Col 1 `copy` Button: `Background null`, `BorderThickness 0`, `Padding 8,4,8,4`, `VerticalAlignment Center`; content = FontIcon **U+E8C8 (Copy)**, size 16, muted `#989898`. No custom style/template — it keeps the platform button's default hover/press chrome (rest state is chromeless). a11y Name = `"Copy " + label` (hard-coded English prefix — see Flags).
- Behavior on click: create a clipboard DataPackage, `SetText(value)` (the **constructor argument**, captured by value in the click lambda), set as clipboard content. Consequence for the port: the copied value is frozen at construction — if a page later edits `field.value.Text()`, the copy button still writes the ORIGINAL string; Windows pages rebuild the field when the value changes. Preserve this contract or bind copy to a stored full-value that is updated with the display.

## 9. `MakePlanUsageCard(planLabel[, planValue]) → {root, planValue, usageBarHost, legend}`

Subscription shape shared by the connect drawer and Account.
- root Border: `UrCardStyle` (as above).
- StackPanel vertical `Spacing 8`:
  1. caption TextBlock: `UrLabelStyle` (12 muted), text = planLabel.
  2. `planValue` TextBlock: `UrStatValueStyle` (Extra Condensed, `#F8F8F8`) with **FontSize overridden to 22**.
  3. `usageBarHost` Grid: **Height 32** — the caller constructs a `urnw::UsageBar` (custom-drawn, not a XAML control) into it.
  4. `legend` StackPanel: horizontal, `Spacing 16` — caller fills legend swatches/labels.

## 10. Status strip fields

The window-bottom status strip is a horizontal panel of these (surface itself = `UrStatusStripStyle`: bg `#151515`, top hairline 1px `#1FFFFFFF`, padding 16,7 — window-level chrome, not a card). The shape is deliberately uniform: Advanced Mode adds four more fields (egress interface, rpc port, session mode, raw status) at the cost of four more calls and zero layout change.

### `MakeStatusField(label, withDot=false, accessibleName={}) → {root, dot, caption, value, name}`
- root StackPanel horizontal, `Spacing 6`, `VerticalAlignment Center`.
- If `withDot`: Ellipse **8×8**, `VerticalAlignment Center`, a11y **Raw** (the colour restates the adjacent word); appended first; caller paints its Fill per state. `dot` is null otherwise.
- If `label` non-empty: caption TextBlock, text = label, style `UrStatusFieldLabelStyle` = **Montreal 11, FAINT `#5A5A5A`** (doc §7.3 calls it "muted" — code says faint; see Flags), `VerticalAlignment Center`; a11y **Raw** — the caption is the field's NAME, not separate content; this is also why hiding captions at narrow widths costs a screen reader nothing (the strip hides `caption` below the breakpoint; values stand alone).
- value TextBlock: style `UrStatusFieldValueStyle` = Montreal 12, muted `#989898`, `VerticalAlignment Center`, `CharacterEllipsis`, `NoWrap` (the strip is ONE row; long provider names shorten, never wrap).
- `field.name` = `accessibleName` if given else `label`. Seeded immediately as the value's automation Name so a field with no value yet announces at least what it is. `label` may be empty for self-describing fields (the state field: dot + the word "Connected"); then `accessibleName` MUST be passed or the value reaches a screen reader unnamed.

### `SetStatusFieldValue(field, value)`
Writes `value.Text` AND rebuilds the announced name as **`"<name>, <value>"`** (just `value` if name empty). Text alone is NOT enough: the caption is Raw, so the value is the field's only accessible node. Same "Label, value" shape as ConnectPage's location row. Every runtime update must go through this.

### `MakeStatusSeparator()`
Border: `Width 1`, `Height 14`, `Margin 14,0,14,0`, `VerticalAlignment Center`, background `#1FFFFFFF`. Vertical (the strip is a row; its rules run the other way from `MakeDivider`).

## 11. Pane dynamic rows (R3)

Purpose (normative): **one row height per list is a property of the code, not a hope.** Every row of a list comes from one call so no row can drift to a different height.

### `MakePaneRow(height)` — the base row
Border: **`Height` = height (FIXED, not MinHeight** — a minimum lets one row grow and the list stop being uniform); `HorizontalAlignment Stretch`; `Padding 12,0,12,0` (the pane's 12px inset); `BorderBrush #1FFFFFFF`; `BorderThickness 0,0,0,1` (bottom hairline). No background (transparent over the pane's `#101010`). Pass the height once per list.

### `MakePaneKeyValueRow(key, value={}, height=34) → {root, key, value}`
- root = `MakePaneRow(34)`.
- Grid `ColumnSpacing 8`, columns **Star | Auto**.
- key TextBlock: text=key, style `UrKeyTextStyle` = Montreal 13, muted `#989898`, `VerticalAlignment Center`, CharacterEllipsis, NoWrap. Col 0.
- value TextBlock: text=value, style `UrValueTextStyle` = Montreal 13, `#F8F8F8`, `VerticalAlignment Center`, `HorizontalAlignment Right`, `TextAlignment Right`, CharacterEllipsis, NoWrap. Col 1.
- a11y: key is **Raw**; value's Name = `"<key>, <value>"` — set ONCE at construction with the initial value (unlike StatusField there is no setter that rewrites it; pages that update `value.Text` later should also rewrite the name if the row is meant to re-announce).
- Use: session figures, inspector grids.

### Shared list-row internals (`BuildPaneListRowParts`, private)
One grid shared by the static and selectable list row so the two CANNOT drift:
- Grid `ColumnSpacing 10`, columns **Auto (marker) | Auto (dot) | Star (title) | Auto (meta)**.
- `marker` Border (col 0): `Width 2`, `VerticalAlignment Stretch`, **`Margin -10,0,0,0`** (cancels the ColumnSpacing so the bar sits flush at the row's left edge and the two row forms measure identically), `Opacity 0`, a11y Raw. Built for BOTH forms (zero-width visual impact until selected) so a list does not shift 2px when it becomes selectable.
- `dot` Ellipse (col 1): **7×7**, `VerticalAlignment Center`, a11y Raw (colour restates the row text). Caller paints Fill.
- `title` TextBlock (col 2): style `UrRowTitleStyle` = Montreal 13, `#F8F8F8`, `VerticalAlignment Center`, CharacterEllipsis, NoWrap.
- `meta` TextBlock (col 3): style `UrValueTextStyle` (13, right-aligned, trimmed) with foreground overridden to muted `#989898`.

### `MakePaneListRow(height=36) → {root, dot, title, meta}`
root = `MakePaneRow(36)`, child = the shared grid. Static (not focusable/clickable). Connections table, contracts list, split rules — all this one shape.

### `MakePaneListRowButton(height=36) → {root, dot, title, meta, marker}`
Identical grid/height/elements; root is a **Button** styled `UrPaneRowButtonStyle` (below) instead of a Border, so the row is clickable, Tab-reachable, and invokable with Enter/Space. `Height` AND `MinHeight` both pinned to `height` (the style only sets MinHeight 40; pinning keeps 36 exact). a11y: `title` and `meta` are set **Raw** — the row's own automation Name is the whole announcement, and **the caller MUST set `AutomationProperties::Name` on `root`**: a Button whose content is a panel gets NO automatic name and would announce as bare "button" (a defect this project shipped twice, on PeersLine and ConnectHero).

Deliberately a separate builder, not a flag: the root TYPES differ (Button vs Border) and one function returning either would force every caller to type-test.

### `SetPaneListRowSelected(row, selected)`
Called on EVERY row of the list on every selection change, `selected==true` for exactly one. Selection is carried on **three channels** (colour-alone is forbidden: a `#1C1C1C`-over-`#101010` fill step is invisible on a dim panel and to low-contrast vision):
1. **Fill**: root Background = `#1C1C1C` (CardBrush) when selected, transparent (`ARGB 0,0,0,0`) when not.
2. **Shape**: the 2px leading `marker` Border — Background set to accent `#EFF7BB`, `Opacity 1.0` selected / `0.0` not.
3. **A11y**: the row's automation Name gains its selected state. NOTE: the shipped function body implements only channels 1–2; the name suffix is applied by the CALLER when it sets the row Name (the header text and doc §7.3 both state three channels — port it so the announced name changes on selection; see Flags).

Note an interaction consequence to reproduce: the row style's hover fill is ALSO `#1C1C1C`, so a hovered unselected row matches the selected fill — the accent bar is what still disambiguates. 

## 12. Pane dynamic groups and rows (R4 — additive)

Each is the App.xaml style of the same name applied in code, so a pane built in code and a pane declared in markup are byte-for-byte the same pane.

### `MakePaneGroupHeader(title, meta={}) → {root, title, meta, trailing}`
- root Border: style `UrGroupHeaderStyle` = **Height 28** (`UrGroupHeaderHeight`), background `#151515` (sheet), BorderBrush `#1FFFFFFF`, **BorderThickness 0,1,0,1** (rules top AND bottom), Padding 12,0. (Code fallback if the key were missing: height 28 + sheet bg + padding only, no rules — implement the style values, the fallback is only key-miss robustness.)
- Grid `ColumnSpacing 8`, columns **Star | Auto | Auto**:
  - title: style `UrGroupHeaderTextStyle` = Montreal **11**, **CharacterSpacing 90** (letterspacing, XAML units = 90/1000 em ≈ +0.99px at 11px), muted `#989898`, `VerticalAlignment Center`, CharacterEllipsis. a11y: **HeadingLevel Level3** (a group header is a heading, not a list item).
  - meta: style `UrPaneMetaStyle` = Montreal 11, muted `#989898`, `VerticalAlignment Center`, trimmed, NoWrap; applied via `SetTextOrCollapse` (collapsed when empty).
  - `trailing` Grid, `VerticalAlignment Center`: host for a group's icon-only command (`UrPaneActionButtonStyle`, §16.2); costs no width when empty.

### `MakePaneTwoLineRow(title, note={}, height=44) → {root, title, note, trailing}`
- root = `MakePaneRow(44)` (default = `UrPaneRowTallHeight`; a settings list picks 44 once and rows without a note simply centre their title in the same 44).
- Grid `ColumnSpacing 10`, columns **Star | Auto**.
- Col 0 = the shared two-line text column: StackPanel `Spacing 1`, `VerticalAlignment Center`; `title` in `UrRowTitleStyle` (13 `#F8F8F8` trimmed); `note` in `UrRowNoteStyle` = Montreal **11**, muted `#989898`, **CharacterEllipsis + NoWrap — trimmed, never wrapped** (a wrapping note would break the fixed height); note applied via `SetTextOrCollapse` (an empty note must not spend the panel's 1px spacing and push the title off-centre).
- Col 1 `trailing` Grid: `VerticalAlignment Center`, `HorizontalAlignment Right` — switch/button/chevron host; point that control's AutomationProperties at `title`.

### `MakePaneTwoLineRowButton(title, note={}, height=44) → {root, title, note, value}`
Same metrics/hairline as the static form, root = Button on `UrPaneRowButtonStyle` with Height+MinHeight pinned to 44, so tappable and static rows share one left edge and baseline grid.
- Grid `ColumnSpacing 10`, columns **Star (text) | Auto (value) | Auto (chevron)**:
  - text column: same `MakeTwoLineText` as above.
  - `value` TextBlock: `UrValueTextStyle` (13, right) with muted `#989898` foreground and **MaxWidth 240** — the current-value readout left of the chevron.
  - chevron FontIcon: **U+E76C (ChevronRight)**, `FontSize 11`, muted `#989898`, `VerticalAlignment Center`, a11y Raw.
- a11y: `title`/`note`/`value` all **Raw**; `AutomationProperties::Name(root) = title`; if note non-empty, `FullDescription(root) = note`. (Name is set by the builder here, unlike `MakePaneListRowButton`.)

### `MakePaneTableRow(weights, height=36, textColumns=1) → {root, cells[]}`
- root = `MakePaneRow(height)`.
- Grid `ColumnSpacing 12`; one ColumnDefinition per weight: **Star-sized with that weight, `MinWidth 56`** (a star column otherwise collapses to zero and clips; the minimum makes the table NARROW instead of vanishing — required responsive behavior).
- One TextBlock per column, appended in order to `cells`:
  - index < `textColumns` (the row's subject): style `UrRowTitleStyle` — left, 13, `#F8F8F8`, trimmed.
  - index ≥ `textColumns` (figures): style `UrValueTextStyle` — **right-aligned** (HorizontalAlignment Right + TextAlignment Right), 13, trimmed — then foreground overridden to muted `#989898`. Right alignment is what makes a numbers column scannable.
- Column-count examples from the header: payouts ledger `textColumns=1` (date); leaderboard `textColumns=2` (rank + network name). Every table on the Wave-2 destinations (payouts, leaderboard, balance codes) is this call — one row species.

### `MakePaneTableHeader(weights, titles, textColumns=1)`
- Border on `UrGroupHeaderStyle` (28px sheet strip with top+bottom rules, padding 12,0 — same rhythm as group headers; fallback as in §12.1).
- Grid `ColumnSpacing 12`, same star weights, same `MinWidth 56` — so header and body stay aligned.
- Cells (iterates `min(weights.size, titles.size)`): style `UrPaneColumnTextStyle` = Montreal **11**, **CharacterSpacing 60**, **faint `#5A5A5A`**, `VerticalAlignment Center`, trimmed, NoWrap; figure columns (index ≥ textColumns) additionally `HorizontalAlignment Right` + `TextAlignment Right`.

### `MakePaneEmptyLine(text)`
The empty state of a pane that FILLS: ONE centred muted line mid-pane — deliberately **no glyph and no card** (a card would put a rounded island back inside a pane).
- TextBlock: style `UrSupportingTextStyle` (Montreal 12 + Wrap, based on UrLabelStyle) with foreground overridden to **faint `#5A5A5A`**; `TextAlignment Center`; `TextWrapping Wrap`; **MaxWidth 320**; `HorizontalAlignment Center`; `VerticalAlignment Center`; `Margin 16,16,16,16`. Place stretched in a Grid cell; it centres itself in whatever pane height remains.

### `MakePaneSearchRow(placeholder) → {root, box}`
- root = `MakePaneRow(UrPaneHeaderHeight → 40)` (read from the resource dictionary via `MetricByKey`, fallback 40), with Background overridden to **page `#101010`** (the one pane row with an explicit fill).
- Grid `ColumnSpacing 8`, columns **Auto | Star**:
  - FontIcon **U+E721 (Search)**, `FontSize 13`, muted `#989898`, `VerticalAlignment Center`, a11y Raw.
  - `box` TextBox: style `UrPaneSearchStyle` = Montreal 13, **CornerRadius 0, BorderThickness 0, Background Transparent, Padding 0, MinHeight 0**, `HorizontalAlignment Stretch`, `VerticalAlignment Center` — squares off the default rounded island so the field wears the pane row's exact metrics; the row's bottom hairline comes from the Border it sits in. `PlaceholderText = placeholder` AND `AutomationProperties::Name = placeholder` (a placeholder is NOT an accessible name; without this it is an unnamed edit box).

## 13. `ValidationState` + `ApplySupportingText`

```
enum class ValidationState { NotChecked, Validating, Valid, Invalid };
```
(Port of iOS `UrTextField/ValidationState.swift`.) `ApplySupportingText(line, text, state)` writes `text` on a field's supporting line (style `UrSupportingTextStyle`: 12, wraps) and sets its colour:

| State | Colour |
|---|---|
| `Invalid` | danger `#F8523B` |
| `Valid` | brand green `#87FB67` (the "this name is available" green) |
| `NotChecked` | muted `#989898` |
| `Validating` | muted `#989898` |

An **empty `text` still applies the colour**, so clearing the line cannot flash the previous verdict's colour on the next write. Null-safe.

## 14. `Snackbar`

An auto-dismissing InfoBar (the WinUI idiom for iOS `UrSnackBar`; on GTK the analogue is a revealer/toast surface — window-level `AccountSnackbar` is bottom-center, MaxWidth 480).

Constants: `kDefaultDurationMs = 4000` (~4s, the convention for a message with no action); `kPersistent = 0` ("do not dismiss yourself").

Construction: `Snackbar(bar, dispatcherQueue, durationMs = 4000)`. Creates a **one-shot** (non-repeating) timer whose Tick calls `Hide()`. The tick closure holds a `shared_ptr<Snackbar*>` self-token, nulled in the destructor, so a tick outliving the object no-ops instead of dereferencing a dangling `this` — the class is non-copyable/non-movable; hold by value or `unique_ptr` in the owning page. If constructed without a bar or queue it logs one error and every `Show` no-ops.

`Show(message, severity = Informational, durationMs = nullopt)`:
1. Set severity, message, open the bar.
2. Stop any running timer (**a second message restarts the window rather than inheriting the remainder**).
3. **Severity gate**: `safeToMiss = (severity == Informational || severity == Success)`. Effective duration = `durationMs.value_or(safeToMiss ? default(4000) : kPersistent)`. So with no explicit duration: **Informational/Success auto-dismiss at 4000ms; Warning/Error stay until the user dismisses them** — an error string (e.g. the raw wallet-connect server message) is often the only diagnostic the user ever gets. Callers may pass an explicit positive duration to override, or `kPersistent` to pin.
4. `duration <= 0` ⇒ no timer started. Else set interval, start.
5. The whole body is exception-guarded: a Show landing during dispatcher-queue teardown is logged (`"kit: snackbar show dropped (teardown in progress)"`) and dropped — a message nobody can see is safe to drop, a crash is not.

`Hide()`: stop timer, close bar, swallow teardown exceptions. Destructor: stop timer (swallow — a throwing destructor is terminate), null the self-token.

## 15. `App.xaml` pane-shell styles — exact values (lines 830–1123)

(830–899 are the block the kit consumes by key; 900–1123 are the tail of `UrPaneRowButtonStyle` plus everything after it, per the task scope. All fills are the existing `#101010/#151515/#1C1C1C` ramp; rules are `#1FFFFFFF`. Nothing in the pane vocabulary has a radius, margin, or shadow except the two action buttons.)

Context keys defined just above this block: `UrPaneHeaderHeight 40`, `UrGroupHeaderHeight 28`, `UrPaneRowHeight 40`, `UrPaneListRowHeight 36`; `UrPaneVRuleStyle` (Border: Width 1, Background `#1FFFFFFF`, VerticalAlignment Stretch — the rule BETWEEN panes); `UrPaneStyle` (Grid Background `#101010` — solid so the ConnectCanvas mask has a surface); `UrPaneHeaderStyle` (Border: Height 40, Background `#151515`, BorderBrush `#1FFFFFFF`, BorderThickness 0,0,0,1, Padding 12,0 — the strip at the top of every pane).

- **`UrPaneTitleStyle`** (TextBlock — pane header title; chrome that names a column, NOT the page-title voice): Montreal, 12, SemiBold, **CharacterSpacing 60**, `#F8F8F8`, VerticalAlignment Center, CharacterEllipsis.
- **`UrPaneMetaStyle`** (count/live figure at the right of a pane or group header): Montreal, 11, muted `#989898`, VerticalAlignment Center, CharacterEllipsis, NoWrap.
- **`UrGroupHeaderStyle`** (Border): Height 28, Background `#151515`, BorderBrush `#1FFFFFFF`, **BorderThickness 0,1,0,1**, Padding 12,0.
- **`UrGroupHeaderTextStyle`**: Montreal, 11, **CharacterSpacing 90**, muted `#989898`, VerticalAlignment Center, CharacterEllipsis.
- **`UrPaneRowStyle`** (Border — the markup static row): **MinHeight 40** (`UrPaneRowHeight`), BorderBrush `#1FFFFFFF`, BorderThickness 0,0,0,1, Padding 12,0, HorizontalAlignment Stretch. (Note: markup rows use MinHeight; the code builder `MakePaneRow` pins exact Height — both exist deliberately.)
- **`UrPaneRowButtonStyle`** (Button — the same row, tappable; "UrCardButton's template minus the radius and the box: hover is a fill step, not an outline"):
  - Setters: Background Transparent; Foreground `#F8F8F8`; BorderBrush `#1FFFFFFF`; BorderThickness 0,0,0,1; CornerRadius 0; MinHeight 40; Padding 12,0; HorizontalAlignment Stretch; HorizontalContentAlignment Stretch; VerticalContentAlignment Center; FontFamily Montreal; UseSystemFocusVisuals True.
  - Template: root Grid bound to Background/BorderBrush/BorderThickness; ContentPresenter bound to Content/Padding/Foreground/Font*/alignment. Visual states with a **generated transition of 0:0:0.15 (150 ms)** between states:
    - Normal: (rest).
    - PointerOver: root Background → `UrCardBrush` **`#1C1C1C`**.
    - Pressed: root Background → `UrCardPressedBrush` **`#2A2A2A`**.
    - Disabled: root **Opacity 0.38**.
- **`UrPaneActionButtonStyle`** (Button, BasedOn UrPaneRowButtonStyle — the icon-only command riding in a pane/group header: open editor, open full sheet): MinHeight 24, **Height 24**, MinWidth 28, **Width 28**, BorderThickness 0, Padding 0, HorizontalAlignment Right, HorizontalContentAlignment Center, VerticalAlignment Center. Inherits the hover fill/pressed/disabled states ("no chrome until hovered"). **Every instance MUST carry an explicit AutomationProperties.Name — a glyph is not a name.**
- **`UrKeyTextStyle`**: Montreal, 13, muted `#989898`, VerticalAlignment Center, CharacterEllipsis, NoWrap.
- **`UrValueTextStyle`**: Montreal, 13, `#F8F8F8`, VerticalAlignment Center, **HorizontalAlignment Right, TextAlignment Right**, CharacterEllipsis, NoWrap.
- **`UrRowTitleStyle`** (the thing a row IS): Montreal, 13, `#F8F8F8`, VerticalAlignment Center, CharacterEllipsis, NoWrap.
- **`UrPaneRowTallHeight`** = **44** (double). Rationale in-file: 40 is a single 13px label centred; a settings row is 13px + 11px + leading; a list of explained rows is 44 THROUGHOUT including rows with no explanation.
- **`UrRowNoteStyle`** (second line of a two-line row): Montreal, 11, muted `#989898`, CharacterEllipsis, **NoWrap** (a wrapped explanation would break the fixed height).
- **`UrPaneSearchStyle`** (TextBox): Montreal, 13, CornerRadius 0, BorderThickness 0, Background Transparent, Padding 0,0,0,0, MinHeight 0, HorizontalAlignment Stretch, VerticalAlignment Center.
- **`UrPaneColumnTextStyle`** (table column header cells, on the 28px group rhythm): Montreal, 11, **CharacterSpacing 60**, **faint `#5A5A5A`**, VerticalAlignment Center, CharacterEllipsis, NoWrap.
- **`UrPaneActionPrimaryStyle`** (Button, BasedOn `UrButtonBaseStyle` — the pane's primary action, e.g. Connect; REPLACES the deleted full-bleed lime `UrPaneAccentButtonStyle`): Background **`#638BFC`** (UrBlueMediumBrush — THE action colour; lime `#EFF7BB` is reserved for earnings/brand); Foreground **`#101010`** (UrInverseTextBrush — the ONE divergence from URButton PRIMARY's white: white on `#638BFC` is 3.0:1, fine for 24sp NeueBit large text, FAILS 4.5:1 for this 14sp SemiBold; `#101010` gives 5.9:1); BorderBrush Transparent; BorderThickness 0; **CornerRadius 4** (the spec's control radius); MinHeight 40; **Height 40** (not 44 — at 44 flush it was indistinguishable from the rows; 40 + 12 air each side reads as its own object); **Margin 12,12,12,12** (the pane's own inset, so the button's left edge lands on the row-label rule); Padding 12,0; FontFamily Montreal; FontSize **14**; FontWeight **SemiBold**. Template = UrButtonBaseStyle's Material state layer: hover/press are a **translucent wash of the CONTENT colour over the container**, not a container swap.
- **`UrPaneActionSecondaryStyle`** (BasedOn primary — the CONNECTED state's Disconnect: same geometry/position, no fill; the control's weight is itself a status reading — loud = something to do, quiet = you're fine; not colour-alone: label word, status line word and dot colour all change too, fill is a fourth channel): Background Transparent; Foreground `#F8F8F8`; BorderBrush **`#38FFFFFF`** (UrBorderStrongBrush); BorderThickness 1. Everything else inherited (radius 4, h40, margin 12, Montreal 14 SemiBold).

## 16. Localization contract

The kit accepts already-localized strings; it calls no `Loc()`/`Adv()` itself. Callers pass store keys through `Localized(key)` / `Adv(key, english)` / `Dev(key)` per page (missing key renders the key itself — visibly wrong, never blank). The kit's own literals:
- mask: `"••••••••"` (8 × U+2022) — not localizable, by design.
- accessible name `"Copy " + label` in `MakeCopyField` — hard-coded English prefix (flagged; the port should localize this, e.g. a `copy` store key, while keeping the "verb + label" shape).
- name join `", "` in `SetStatusFieldValue` / `MakePaneKeyValueRow` a11y names.

## 17. Cross-cutting invariants to preserve (verbatim from source comments)

- Icon font must be EXPLICITLY named on every icon; a wrong family name fails silently to a fallback face with different metrics.
- Styles are resolved BY KEY from the app dictionary at build time (`StyleByKey`), null-tolerant: a missing key degrades to hand-set fallbacks (documented above per builder) instead of throwing. Port equivalent: central style application, never per-site literals.
- Decorative glyphs/dots/captions are always a11y-Raw; every icon-only button and every panel-content Button gets an explicit accessible name; status values re-announce as "Label, value"; selection is never colour-alone.
- Fixed Height (not MinHeight) for every generated list row; one height per list.
- Empty vs loading vs failed must be three distinguishable renders; "nothing here" is a module (glyph + sentence) on card-model surfaces and ONE centred faint line on pane surfaces.


## SDK surface referenced
- (none) — the component kit references no urnet::/Sdk surface; it is pure UI. Platform-only side effects: Windows.ApplicationModel.DataTransfer.DataPackage/Clipboard.SetContent (MakeCopyField click) and Microsoft.UI.Dispatching.DispatcherQueueTimer (Snackbar auto-dismiss).

## Flags (doc-vs-code drift / risks)
- DOC vs CODE — kUltraWideDip: UrComponents.h's comment says Home gains a third column above 1800dip; docs §7.1 says the constant is 'reserved' and shipped code keys Home's third pane off 1000dip + Advanced Mode, only logging 1800. MainWindow::ApplyBreakpoint (out of this spec's scope) is the authority — verify there before wiring 1800.
- DOC vs CODE — MakeSectionHeader label typography: header comment says 'display face at 16 SemiBold', docs §8.2 says 'body-face 15 SemiBold', the code sets FontSize 16 + SemiBold and NO FontFamily (and App.xaml has no implicit TextBlock style), so it renders in the system default face. Code wins: 16 SemiBold, default face — but this is almost certainly an unintended fallback; decide deliberately for the port.
- DOC vs CODE — status-field caption colour: docs §7.3 calls it an '11sp muted caption'; UrStatusFieldLabelStyle actually uses UrTextFaintBrush #5A5A5A (faint), not #989898 (muted). Code wins: #5A5A5A.
- COMMENT vs CODE — SetPaneListRowSelected 'three channels': the header and docs both say selection = fill + 2px accent bar + a11y name suffix, but the function body implements only fill + bar; the announced-name suffix is left to callers (who must already set the row Name). Port must ensure the caller-side suffix actually happens or fold it into the setter.
- MakeEmptyState body text (FontSize 12) also sets no FontFamily — renders in the toolkit default face, not PP Neue Montreal. Same decide-deliberately note as MakeSectionHeader.
- MakeCopyField behavioral trap: the clipboard value is captured at construction; later edits to field.value.Text do NOT change what the copy button writes. Windows pages rebuild the field on value change — the port must preserve this or bind copy to a live full-value.
- MakeCopyField a11y name 'Copy ' + label is hard-coded English (unlocalized) in the Windows code; the port should draw the verb from the localization store while keeping the 'verb + label' shape.
- MakePaneKeyValueRow's accessible name 'key, value' is set once at construction and never rewritten (unlike SetStatusFieldValue); pages updating value.Text later get a stale announced name unless they rewrite it.
- Snackbar constructor error log has its branches inverted (logs 'without a dispatcher queue' when the bar exists but the queue is missing — message text happens to be correct, but the ternary reads backwards; harmless, don't copy blindly).
- Hover ambiguity to reproduce knowingly: UrPaneRowButtonStyle hover fill (#1C1C1C) equals the selected-row fill; only the 2px accent bar distinguishes a hovered unselected row from the selected one.
- UrPaneListRowHeight (36) exists as an App.xaml resource but the kit's C++ default args carry 36/34/44 directly; only MakePaneSearchRow reads a metric (UrPaneHeaderHeight) from the dictionary. Keep the numbers in one place in the port.
- Markup rows (UrPaneRowStyle / UrPaneRowButtonStyle) use MinHeight 40, while every code-generated row pins exact Height — intentional (markup rows are hand-checked; generated lists must be un-driftable), but MakePaneListRowButton/MakePaneTwoLineRowButton set BOTH Height and MinHeight to override the style's 40 down to 36/44.
