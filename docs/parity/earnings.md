# URnetwork — EARNINGS destination (Wallet + Leaderboard merged) — GTK4/C++ port spec

> **2026-09-02 — points-first rewrite.** The destination described below (payout wallets,
> USDC payouts, Solana/Polygon wallet validation, the payouts ledger, the Solana-based
> Seeker verification) is gone. `app/src/EarningsPage.{hpp,cpp}` now renders the
> points-first Earnings page from the earnings design review: the points headline with its
> Providing / Referral / Reliability breakdown, the protocol note with the ur.xyz link, the
> Unclaimed SN25α tile (only once a Bittensor coldkey is attached), the Bittensor wallet block
> (connect through the ur.io wallet bridge with purpose `connect`, or enter an address manually —
> validated locally, then through the unauthenticated validate endpoint, and still signed), the
> Top 200 head-spot tile / bound status, the per-epoch History (points; the SN25α column and
> status only with a wallet) beside the Leaderboard tab, and the claim dialog (claimable /
> needs gas with the gas key's mirror address / sending / sent / claimed / expired / failed).
> Claims are sent by the SDK on the device straight to the settlement vault; no URnetwork API
> is in that path. Pane C keeps the ranking, the public-leaderboard switch and reliability.
> The pane-fold table, the flow/gate discipline and the preview harness below still apply
> (`URNETWORK_PREVIEW_WALLET`, `URNETWORK_PREVIEW_CLAIM`, `URNETWORK_PREVIEW_GAS=low`,
> `URNETWORK_PREVIEW_MANUAL`, `URNETWORK_PREVIEW_TOP200=bound` extend the sample).

Source of truth read in full: `urnetwork-windows/app/src/App/WalletPage.{h,cpp}` (1681+262 lines), `WalletSheets.{h,cpp}` (687+185), plus the markup (`MainWindow.xaml` 1838–2156), the kit (`UrComponents.{h,cpp}`, `App.xaml`), tokens (`UrColors.h`), breakpoints (`MainWindow.xaml.cpp` ApplyBreakpoint ~561–580), balance gating (`MainWindow.xaml.cpp` ~1407–1418), navigation loads (~1351–1375), and the en string table (`Strings/en/Resources.resw`). Doc cross-check: `linux_agent_help.md` §7.10 / §7.3 / §8 — disagreements listed in FLAGS; **the code wins**.

---

## 0. Destination identity & shell

- Nav item tag `wallet`, label `Loc("earnings")` = **"Earnings"** (wallet + leaderboard are ONE destination; the old separate Leaderboard destination is gone — its tables live behind a tab in the ledger pane).
- The destination is a full-bleed pane grid (`UrPaneStyle`: background `#101010`), 5 grid columns:
  `WalletPaneAColumn` **360** | vertical rule (Auto) | `WalletPaneBColumn` **\*** | vertical rule (Auto) | `WalletPaneCColumn` **380**.
- Vertical rules: `UrPaneVRuleStyle` = Width 1, background `#1FFFFFFF`, stretch.
- **Breakpoints** (window width, dip; the app's single `ApplyBreakpoint` at window level):
  - `>= 1500`: three panes — wallets(360) | ledger(\*) | points(380).
  - `>= 900`: two panes — wallets(360) | ledger(\*). Pane C column width → 0, its rule and pane Collapsed.
  - `< 900`: one pane — ledger(\*) only. Pane A column → 0, its rule and pane Collapsed. The LEDGER survives to the smallest width (a payouts table is what the user opens this destination to read).
- **Load triggers**: selecting the destination AND every auth change run `LoadWallet()` (via `LoadCurrentDestination`, guarded `IsLoggedIn()`). `LoadLeaderboard()` is *not* run on selection — it fires the first time the Leaderboard tab is looked at (one-shot `leaderboardRequested_` flag) **[LINUX DEVIATES — see "Linux deviation" at the end of this file]**. Legacy: a `leaderboard` tag branch still exists in `LoadCurrentDestination` but no nav item carries that tag anymore.
- **Modal discipline**: window-level `sheetOpen` flag — one ContentDialog at a time; every sheet open path checks it and bails.
- Accessibility landmark names (`ApplyStrings`): Pane A = `Loc("payout_wallets")` "Payout Wallets"; Pane B = `Loc("payouts")` "Payouts" (static, even while the Leaderboard tab shows) with `LandmarkType="Main"`; Pane C = `Loc("network_earnings")` "Network earnings".

### Shared visual vocabulary (numbers the pane is built from)
- Pane header strip: `UrPaneHeaderStyle` — Height **40**, background `#151515`, bottom hairline 1px `#1FFFFFFF`, Padding 12,0. Title `UrPaneTitleStyle`: PP Neue Montreal 12 SemiBold, CharacterSpacing **60**, `#F8F8F8`, trimmed. Right meta `UrPaneMetaStyle`: Montreal 11, `#989898`.
- Group header: `UrGroupHeaderStyle` — Height **28**, `#151515`, hairlines top+bottom, Padding 12,0. Text `UrGroupHeaderTextStyle`: Montreal 11, CharacterSpacing **90**, `#989898`. Heading level 3 for a11y when built by `MakePaneGroupHeader`.
- Rows: `UrPaneRowStyle` Border — MinHeight **40** (overridden per row below), bottom hairline, Padding 12,0. `UrPaneRowButtonStyle` Button — same metrics, transparent bg, hover fill `#1C1C1C`, pressed `#2A2A2A`, disabled opacity 0.38, visual-state transition **150 ms**, `UseSystemFocusVisuals`.
- Key/value text: `UrKeyTextStyle` Montreal 13 `#989898`; `UrValueTextStyle` Montreal 13 `#F8F8F8` right-aligned; `UrRowTitleStyle` Montreal 13 `#F8F8F8`; `UrRowNoteStyle` Montreal 11 `#989898` (single-line trimmed in the kit; the XAML note blocks here explicitly set `TextWrapping=Wrap` + `TextTrimming=None`).
- Table header cells: `UrPaneColumnTextStyle` Montreal 11, CharacterSpacing 60, `#5A5A5A`.
- Condensed values (`MakeValue` in both files): **ABC Gravity Extra Condensed**, default 22, `#F8F8F8`.
- Fonts by internal family name: `ABC Gravity Extra Condensed` (condensed stat values), `PP NeueBit` Bold (disc ticker, DEFAULT chip, sheet masked address), `PP Neue Montreal` (everything else on this surface).
- Palette used here: bg `#101010`, sheet `#151515`, card `#1C1C1C`, hover `#242424`(kit)/pressed `#2A2A2A`, border `#1FFFFFFF`, text `#F8F8F8`, muted `#989898`, faint `#5A5A5A`, danger `#F8523B`, lime/earnings-green `#87FB67` (kUrGreen), pink `#ED8FFF` (kUrPink), action blue `#638BFC`.

### Shared presentation helpers (WalletSheets.cpp — port exactly)
- `ChainDisplayName(blockchain)`: `SOL`→`Loc("solana")` "Solana", `TAO`→`Loc("bittensor")` "Bittensor", `MATIC`→`Loc("polygon")` "Polygon", anything else → the raw chain id.
- `MaskAddress(address)`: empty → ""; else `"***"` + last `min(6, len)` characters. (Sample data deliberately gives each chain distinct last-six.)
- `FormatUsdcAmount(double)`: `%.2f` (two decimals always).
- `FormatPointsValue(double)`: if `|x − round(x)| < 0.005` render as integer, else `%.2f`; then hand-insert `,` thousands separators every 3 digits before the decimal point (locale-independent; handles leading `-`).
- `ShortDate(ts)`: if `len>=10 && ts[4]=='-' && ts[7]=='-'` → first 10 chars (`"2026-08-06"`); else the raw string. Deliberately ISO, not localized month names.
- `ExplorerTxUrl(chain, hash)`: empty hash → `""`; `SOL` → `https://solscan.io/tx/<hash>`; **everything else** → `https://polygonscan.com/tx/<hash>`.
- `WalletIconElement(blockchain, size=44)`: Grid `size`×`size`; Ellipse filled with a vertical LinearGradient (start (0.5,0) → end (0.5,1)):
  - SOL: `#9945FF` → `#14F195`
  - TAO: `#1C1C1C` → `#3A3A3A`
  - MATIC / default: `#8A46FF` → `#6E38CC`
  Centered TextBlock = the raw chain id string ("SOL"/"MATIC"/"TAO"), PP NeueBit **Bold**, FontSize `size*0.42`, **pure white**.
- `PayoutWalletTag(isPayout)`: Border CornerRadius **6**, Padding 6,2. Not payout → returned Collapsed (caller places unconditionally). Payout → background off-white at alpha 0x0A (`#0AF8F8F8`), child TextBlock `Loc("default_txt")` = "DEFAULT", PP NeueBit Bold **16**, `#989898`.
- `BuildPointsBreakdown(points, seekerHolder)` (shared by pane C card and PayoutDetailSheet):
  1. `Loc("points_breakdown")` "Points breakdown", Montreal 15, `#F8F8F8`.
  2. Grid, Margin top 12, ColumnSpacing 16, three equal star columns of `StatCell` (12sp muted label over condensed-22 value): `Loc("payout")` "Payout" / `Loc("referral")` "Referral" / `Loc("reliability")` "Reliability", values via `FormatPointsValue`.
  3. HairLine (Border h=1, `#1FFFFFFF`, Margin 0,8,0,8).
  4. **Only if seekerHolder**: Grid ColumnSpacing 12 {star, auto}: left column stack — `Loc("seeker_token_verified")` "Seeker Token Verified!" 14sp in `#87FB67`, then `Loc("you_re_earning_2x_points")` "You're earning 2x points" 12sp muted wrapped; right — condensed-22 `Format("plus_amount", multiplier)` = "+{n}", vertically centered. Then another HairLine. (Green, NOT gold — gold is reserved product-wide for Pro.)
  5. Right-aligned stack: net total condensed **38**, then caption `Loc("net_points_earned")` "net points earned" 12sp muted, both right-aligned.
- `PointsBreakdown` struct: net/payout/referral/multiplier/reliability doubles; event ids from the server: `payout`, `payout_linked_account` (referral), `payout_multiplier`, `payout_reliability`. Values via `urnet::nanoPointsToPoints(point_value)` — **NOTE: divides by 1e6, not 1e9** despite the name; use the SDK helper, never your own constant.

### Gating & refusal (normative for every action on this destination)
- `CanCallApi()` = `!previewUi && Sdk().apiReady() && Sdk().IsLoggedIn()`. Guards **every** server write AND every server question (not only loads — the history: preview-mode actions reached production authenticated).
- `RefuseNoSession()` = log warn + `Notify(Loc("please_login_to_urnetwork"), Error)` — "Please login to URnetwork". **Never silently ignore an affordance** — the one exception is address validation while typing (the user didn't ask for anything; it declines silently).
- `Fetch { Loading, Ready, Failed }` per panel — every list renders exactly one of loading / empty / failed; the three must be visually distinguishable.

### Flow watchdogs & generations (normative)
- `kApiTimeoutMs = 20000` (plain API calls; also the sheet's watchdog). `kBridgeTimeoutMs = 180000` (browser-bridge flows — the user legitimately spends minutes in a browser).
- `Flow` = one non-repeating dispatcher timer + a `uint32 generation`. `BeginFlow` bumps the generation, arms the timer; on timeout the handler checks `flow.generation == its generation`, bumps again (making the give-up final), logs "a request never answered - giving up on it", runs the timeout action. `SettleFlow(gen)` returns false (caller must do NOTHING) if superseded/timed out, else stops the timer. Three flows: `seekerFlow_` (180 s), `connectFlow_` (20 s), `rankingFlow_` (20 s). Address validation uses a bare generation counter (`walletValidateGeneration_`) with no timer.

### Snackbars
- TWO bars, one per pane: `WalletInfo` (pane A, `UrSnackbarStyle` InfoBar, Margin 12,8, after the connect group) and `LeaderboardInfo` (pane C, Margin 12,8, after the leaderboard description row). `Notify(msg, severity)` routes to `LeaderboardInfo` iff `LeaderboardHost` (pane B leaderboard table) is Visible, else `WalletInfo` — the message must land beside content the user is looking at.
- Snackbar behavior (kit): severity-gated auto-dismiss — Informational/Success close after **4000 ms**; **Warning/Error persist** until user-dismissed (the error string is often the only diagnostic). IsClosable, CornerRadius 8, Montreal.

---

## 1. PANE A — WALLETS (360 dip)

Structure: 40px pane header (`WalletPaneATitle` = `Loc("payout_wallets")` "Payout Wallets"; right meta `WalletPaneAMeta` = wallet count, collapsed when empty list) over a vertical ScrollViewer containing, **top to bottom**:

1. **Pending payout row** — `UrPaneRowStyle` with `MinHeight=0, Padding=12,12`. Grid {star, auto}, ColumnSpacing 8. Left: `WalletPendingLabel` `Loc("pending_payout")` "Pending payout", `UrKeyTextStyle`, VerticalAlignment Bottom. Right: `WalletPendingValue`, Montreal **22 SemiBold**, right-aligned — `Format("amount_usdc", …)` = "{n} USDC" of `nanoCentsToUsd(balance_usdc_nano_cents)` 2-dp; dash "-" while unloaded.
2. **Unpaid row** — `UrPaneRowStyle MinHeight=34`. Left `WalletUnpaidLabel` `Loc("unpaid_data_provided")` "Unpaid data provided" (key style 13 muted); right `WalletUnpaidValue` `UrValueTextStyle` 13 — `FormatByteCountCompact(unpaid_bytes_provided)` (units B/KiB/MiB/GiB/TiB; <10 → 2 dp, <100 → 1 dp, else 0 dp, e.g. "38.4 GiB").
3. **Referrals row** — same 34 metrics. `WalletReferralsLabel` `Loc("total_referrals")` "Total referrals"; `WalletReferralsValue` = plain integer.
   - **Stat placeholder rule** (`SetStatValue`): while not loaded OR fetch failed, the value shows `"-"` in **faint `#5A5A5A`**; a real figure shows in `#F8F8F8`. (A dash must read "no answer yet", not "the answer is nothing".) Failure of these three fetches renders as the faint dash — no error line of their own.
4. **Payout threshold note** — `UrPaneRowStyle MinHeight=0 Padding=12,8`; `PayoutsThresholdText` `UrRowNoteStyle` 11 muted, Wrap, TextTrimming None: `Loc("payouts_amount_threshold")` "Payouts occur every Sunday at 00:00 UTC, and require meeting a minimum USDC threshold." (The threshold amount is never named — the server doesn't report it.)
5. **Upgrade button** — `UpgradeButton`, style `UrPaneActionPrimaryStyle`: fill `#638BFC`, foreground `#101010` (inverse text — contrast 5.9:1 at 14sp), radius **4**, Height **40**, Margin 12,12,12,12, Padding 12,0, Montreal 14 SemiBold. Content `LocBox("upgrade_with_stripe")` "Upgrade with Stripe". Click → `OnOpenUpgrade` (window-level): guest → guest-upgrade create-account flow; else → UpgradeSheet (Stripe checkout).
   **Visibility rule** (window balance relay, not WalletPage): Visible iff `!balance.isPro && !balance.guest`; hidden for Pro AND for guests (an account comes first).
6. **"Payout Wallets" group header** — `UrGroupHeaderStyle` 28px; `PayoutWalletsHeading` `Loc("payout_wallets")`; right-aligned `WalletsStatusText` (`UrPaneMetaStyle` 11 muted) = the wallets fetch status: `Loc("loading")` "Loading..." → `Loc("something_went_wrong")` "Something went wrong." on failure → Collapsed on success.
7. **`WalletCardsPanel`** — one **row per wallet**, rebuilt by `RebuildWalletCards()`:
   - `MakePaneTwoLineRowButton(MaskAddress(addr), ChainDisplayName(chain))` — a 44px `UrPaneRowButtonStyle` Button: title = masked address (Montreal 13 `#F8F8F8`), note under it = chain product name (11 muted, trimmed), right-aligned value TextBlock (13, MaxWidth 240) then a chevron FontIcon (Segoe glyph U+E76C, 11, muted, a11y Raw). Bottom hairline; hover `#1C1C1C` / pressed `#2A2A2A`.
   - Value = `Format("amount_usdc", FormatUsdcAmount(TotalPaidToWallet(wallet_id)))` — lifetime **completed** USDC paid into this wallet (sum of `token_amount` over payments with `completed==true` and matching `wallet_id`; iOS totalPaymentsByWalletId parity). Default muted; **if this is the payout wallet the value renders in `#87FB67`** — that is the row's only visual default-marker.
   - **No gradient disc and no DEFAULT chip on the row** (see FLAGS — doc drift; the disc/chip live on the detail sheet).
   - A11y name: `Format("wallet_provider", ChainDisplayName)` = "{Chain} Wallet" + ", " + masked address, and if payout wallet + ", " + `Loc("default_wallet")` "Default". Title/note/value marked Raw.
   - Click → `ShowWalletDetail(wallet)` (guarded by window `sheetOpen`).
8. **Empty state** `WalletsEmptyPanel` (Ready + zero wallets only): Border bottom hairline, Padding 12,12, StackPanel Spacing 6: `WalletsEmptyText` = `Loc("to_start_earning_connect_your_solana_wallet_to")` "To start earning, connect your Solana wallet to URnetwork." (key style 13 muted, wrap) + `WalletsEmptyNote` = `Loc("these_wallets_are_not_affiliated_or_controlled")` "These wallets are not affiliated or controlled by URnetwork. We will send earnings into the connected wallet." (11 muted, wrap). Collapsed on failure (failure clears cards AND hides the empty panel — status text carries the failure).
9. **"Connect a wallet" group header** — `ConnectWalletHeading` `Loc("connect_a_wallet")`.
10. **Connect form** — Border bottom hairline Padding 12,12, StackPanel Spacing 10:
    - `ConnectWalletChainsText` (11 muted wrap): TWO store sentences joined with one space — `Loc("connect_external_wallet_supported_chains")` "USDC addresses on Solana and Polygon are currently supported." + `Loc("bittensor_wallet_future_use")` "Bittensor wallets are stored for future use and can't receive payouts yet."
    - `WalletAddressBox` — TextBox, `UrTextInputStyle` (Montreal 16, underline field: CornerRadius 0, Padding 0,0,0,8), Placeholder AND a11y name = `Loc("enter_wallet_address")` "Enter wallet address". `TextChanged` → `OnWalletAddressChanged`.
    - `WalletChainText` — the verdict line (11 muted, wrap): "" / detected chain / TAO caveat (below).
    - `ConnectWalletButton` — platform-default Button (no kit style), Stretch, **IsEnabled=False initially**, Content `LocBox("connect")` "Connect". Click → `OnConnectWallet`.
11. `WalletInfo` InfoBar (snackbar surface), Margin 12,8, closed.

### Wallets fetch semantics
`LoadWallet()` fires **8 independent requests** — never chained; each settles its own panel so one 500 doesn't blank the rest: `getAccountWallets`, `getPayoutWallet`, `getTransferStats`, `walletBalance`, `getNetworkReferralCode`, `getAccountPoints`, `getNetworkReliability`, `getAccountPayments`. All callbacks marshal to the UI thread via captured queue + weak window ref (never raw this). Also snapshots `ownNetworkId_` from `Sdk().ParsedJwt()->NetworkId`.
- `ApplyWallets(wallets, state)`: recompute `seekerHolder_` = any wallet with `has_seeker_token`. Failed → status "Something went wrong.", cards cleared, empty panel collapsed, `ApplySeekerState()`. Ready → status collapsed, empty panel iff none, pane meta = count (collapsed when empty), rebuild cards, `ApplySeekerState()`.
- `ApplyPayoutWalletId(id)`: **empty id is ignored** (server may answer nil transiently; dropping the marker would make the default wallet look unset). Non-empty → store + rebuild cards. `getPayoutWallet` transport errors are logged only — a miss is not an error.
- `ApplyPayments` (see pane B) calls `RebuildWalletCards()` on **every** path — failed/empty included — because per-wallet totals derive from payments (stale totals under "Something went wrong" shipped as a bug).
- `ApplyPoints` also calls `RebuildWalletCards()` (the seeker state can change).

### Connect-a-wallet behavior (exact)
- `OnWalletAddressChanged` (every keystroke): clear `walletValidation_{sol,matic,tao}` + `walletChain_`; `++walletValidateGeneration_` (drops in-flight answers); disable Connect; clear the verdict line; restart the **300 ms** non-repeating debounce timer (created in `Initialize()`).
- Debounce fires → `ValidateWalletAddress()`: if `address.size() < 32` (shortest supported: Solana base58) OR `!CanCallApi()` → **silent** return. Else take a fresh generation and issue **three** `walletValidateAddress({address, chain})` calls, one per chain in order `SOL, MATIC, TAO`. Transport errors are logged (`walletValidateAddress({chain}) failed: …`) and treated as invalid.
- `ApplyWalletValidation(chain, gen, valid)`: drop if `gen != current`. Record the flag; resolve `walletChain_` by precedence **SOL > MATIC > TAO** (first chain that accepted wins). Connect enabled = chain found && `!connectingWallet_`. Verdict line:
  - TAO → `Loc("bittensor_wallet_future_use")` (the stored-for-future copy — TAO can be connected but never pays out),
  - other chain → `Format("wallet_provider_lower", ChainDisplayName)` = "{Chain} wallet" (e.g. "Solana wallet"),
  - none → "".
- `OnConnectWallet`: no-op if address empty / no chain / already connecting. `CanCallApi()` else `RefuseNoSession()`. Set `connectingWallet_`, disable Connect. `BeginFlow(connectFlow_, 20000)`; timeout → clear flag, re-enable Connect iff a chain is still detected, `Notify(Loc("wallet_connect_failed"), Error)` "Failed to connect the wallet." Then `createAccountWallet({blockchain: walletChain_, wallet_address, default_token_type: "USDC"})` (WalletViewController.addExternalWallet parity). Success = `result && wallet_id nonempty`.
- `ApplyWalletConnectResult(gen, ok, serverError)`: `SettleFlow` or drop (log). Clear flag. Notify: ok → `Loc("wallet_connected")` "Wallet connected." Success (auto-dismiss); fail → the raw server error verbatim if present (unlocalizable) else `wallet_connect_failed`, Error (persists). On ok: clear the address box (which resets the verdict via TextChanged) and `LoadWallet()`. On fail: re-enable Connect iff chain still detected.

---

## 2. WalletDetailSheet (modal ContentDialog)

Opened by a wallet row. Inputs: the wallet, `isPayoutWallet` (id == payoutWalletId), the payments filtered to `payment.wallet_id == wallet.wallet_id`, `allowActions = CanCallApi()` (the sheet **opens and READS with no session** — preview needs that — but its two buttons are the two API writes and are disabled), `onChanged` (page reloads via `RefreshAfterWalletChange()` = `LoadWallet()`), `onSuccess(message)` (page snackbar; only fired on paths that also CLOSE the sheet). The page holds the sheet `shared_ptr` for the life of `ShowAsync`; all handlers capture weak.

Dialog: Title `Loc("wallet")` "Wallet"; CloseButtonText `Loc("close")` "Close"; Background `#151515`. Content = ScrollViewer MaxHeight **520** over StackPanel Spacing 8, MinWidth **460**:

1. **Identity header** — Grid ColumnSpacing 12 {Auto, Star, Auto}:
   - `WalletIconElement(chain)` — the **44px** chain gradient disc + ticker (see helper; header comment says 48 but the code default is 44 — 44 wins).
   - Center stack (v-centered): chain display name 15sp `#F8F8F8`; masked address **18sp in PP NeueBit Bold**.
   - `PayoutWalletTag(isPayout)` — the "DEFAULT" chip, top-aligned (collapsed when not payout).
2. Label `Loc("site_app_wallet_address")` "Wallet address" (12 muted), then the **full address** 13sp `#F8F8F8`, wrapped, `IsTextSelectionEnabled` (this is what the user came to copy).
3. HairLine.
4. **Actions** (the Bittensor rule):
   - `chain == TAO` → **no Make default at all**; instead the reason: `Loc("bittensor_wallet_future_use")` 12 muted wrapped. (The server refuses TAO as payout wallet; the affordance is replaced by the reason.)
   - else if `!isPayoutWallet` → **Make default** Button, Content `LocBox("make_default")` "Make default", Stretch, `IsEnabled = allowActions`.
   - **Remove wallet** Button (always present), Content `LocBox("remove_wallet")` "Remove wallet", Stretch, `IsEnabled = allowActions`.
   - `confirmText_` = `Loc("are_you_sure_you_want_to_remove_this_wallet")` "Are you sure you want to remove this wallet?" — 12sp **danger `#F8523B`**, wrapped, Collapsed until armed.
   - `errorText_` — 12sp danger, wrapped, Collapsed; **every failure renders HERE on the sheet** (a snackbar behind a modal is unreadable — shipped bug).
5. HairLine; `Loc("earnings")` "Earnings" 15sp; then this wallet's payouts:
   - none → `Loc("no_payouts_found")` "No payouts found for this wallet" 12 muted wrapped.
   - else per payment: Grid ColumnSpacing 12, Margin top 6 {Star, Auto}: left `ShortDate(complete_time ?: create_time)` 13 muted; right — completed → `Format("plus_amount_usdc", amount)` "+{n} USDC" 13 `#F8F8F8`, pending → `Loc("pending_payout")` 13 muted.

### Sheet behavior
- `CanAct()`: `allowActions_` or → log error + `ShowError(Loc("please_login_to_urnetwork"))` (checked at press time too, though the buttons are already disabled — defense in depth, out loud).
- `SetBusy(true)`: disables both buttons (`enabled = !busy && allowActions` — never re-enable what has no session), bumps `requestGeneration_`, arms the **20 s** watchdog (single registered Tick). Watchdog fire: if still busy → bump generation (give-up is FINAL — a late success must not hide the sheet and reload after the user was told it failed), log "request timed out with no callback after 20000 ms", `SetBusy(false)`, `ShowError(Loc("something_went_wrong"))`. `SettleRequest(gen)`: mismatched generation → log + return false (do nothing); else `SetBusy(false)`.
- **Make default** (single press): busy → no-op (a second press during flight must NOT paint an error over a live request); `CanAct()`; missing/empty `wallet_id` → `ShowError(Loc("error_setting_default_wallet"))` "Error setting default wallet"; else busy+generation, hide error, `setPayoutWallet({wallet_id})`. ok = result present && no err. Success → `onSuccess(Loc("payout_wallet_updated"))` "Payout wallet updated" (page Success snackbar), `onChanged()`, `dialog.Hide()`. Failure → `ShowError` — `error_setting_default_wallet` or `Format("error_setting_default_wallet_with_reason", serverError)` "Error setting default wallet: {}". (Known SDK trap the watchdog covers: `setPayoutWallet` DROPS the call silently for a non-UUID wallet_id.)
- **Remove** (two-press): first press arms — `removeArmed_=true`, confirm text Visible, button relabels `remove_wallet` → `LocBox("remove")` "Remove". Second press commits: `CanAct()`; missing id → `something_went_wrong`; busy+generation; `removeWallet({wallet_id})`; ok = `result && result->success && !err`, error falls back to `result->error->message`. Success → **deliberately NO snackbar** (the store has no "wallet removed" sentence; inventing English is banned; removal reports itself by the sheet closing and the card vanishing — flagged upstream as a missing string), `onChanged()`, `Hide()`. Failure → `ShowError(server message verbatim | something_went_wrong)`.

---

## 3. PANE B — THE LEDGER (star column; survives to the smallest width)

40px header strip (`UrPaneHeaderStyle`, Padding 12,0) — Grid ColumnSpacing 12 {Auto, Star}:
- `EarningsTableBar` — a **SelectorBar** (2-item segmented tab switch), v-centered: `PayoutsTabItem` `Loc("payouts")` "Payouts" | `LeaderboardTabItem` `Loc("leaderboard")` "Leaderboard". Default selection = Payouts (set in ApplyStrings when none).
- `WalletPaneBMeta` right — the row count **of whichever table is showing** (`ApplyLedgerMeta`: payouts → `payments_.size()`, leaderboard → `leaderboardCount_`; `<= 0` collapses the meta). Recomputed on tab switch, after payouts render, after leaderboard render.

Body: two stacked full-height hosts, exactly one Visible:
- `PayoutsHost` (default) = ScrollViewer over `PayoutsPanel` + centered `PayoutsStatusText` overlay (`UrSupportingTextStyle` 12 muted, wrap, MaxWidth 320, Margin 16, hit-test invisible).
- `LeaderboardHost` (Collapsed initially) = same shape: `LeaderboardRows` + `LeaderboardStatusText`.

`OnEarningsTableChanged`: payouts = (`SelectedItem != LeaderboardTabItem`); toggle the two hosts; `ApplyLedgerMeta()`; **first time the Leaderboard tab is looked at** (`!leaderboardRequested_`) set the flag and `LoadLeaderboard()` — never refetched on later switches.

### 3a. Payouts table
`ApplyPayments(payments, state)`: store; **sort newest-first** by `PaymentTime` = `complete_time` if non-empty else `create_time` (SDK promises no order).
- Failed → `PayoutsStatusText` "Something went wrong." Visible; panel cleared; `RebuildWalletCards()`.
- Empty → status Collapsed; panel = `MakeEmptyStateCard(L"", Loc("site_app_no_payouts"))` "No payouts yet" — an `UrCardStyle` card (`#1C1C1C`, 1px border, radius 12, padding 16) containing a centered column (spacing 8, padding 16,24): FontIcon with an **empty glyph** at 28 faint + the sentence 12 muted centered; `RebuildWalletCards()`.
- Ready → status Collapsed; `RebuildPayouts()`; `RebuildWalletCards()`.

`RebuildPayouts()` — a real table, header + uniform rows via the shared kit (one row species across payouts/leaderboard/balance-codes):
- Column star weights `{2, 2, 3, 3}`, every column `MinWidth 56` (tables narrow, never clip to zero), ColumnSpacing 12, `textColumns = 1` (col 0 left/text voice; cols 1–3 right-aligned muted figure voice).
- Header: `MakePaneTableHeader` — 28px group-header strip; titles `Loc("payout")` "Payout" | `Loc("amount")` "Amount" | `Loc("site_app_wallet")` "Wallet" | `Loc("transaction")` "Transaction" (`UrPaneColumnTextStyle` 11, spacing 60, faint).
- Each row: `MakePaneTableRow(weights)` (height 36) filled:
  - cell0 = `ShortDate(PaymentTime)` (13 `#F8F8F8`, left).
  - cell1 = completed → `Format("plus_amount_usdc", amount)` "+{n} USDC" **in `#87FB67`** (lime = money that ARRIVED; the only lime on the row) | pending → `Loc("pending_payout")` in the muted default.
  - cell2 = `MaskAddress(wallet_address)`.
  - cell3 = tx hash ? `MaskAddress(hash)` : `Loc("none")` "None".
  - The bordered row is wrapped in a Button (`UrPaneRowButtonStyle`, Height=MinHeight=**36**, Padding 0; the inner row's own bottom border zeroed so the hairline isn't drawn twice) → keyboard reachable, hover/press, automation peer. A11y name = `Format("date_payout", ShortDate)` "{date} Payout" for EVERY row, pending included (the date is the only thing distinguishing pending rows); the date cell is marked Raw. Click → `ShowPayoutDetail(payment)`.
- After building: `ApplyLedgerMeta()`.

### 3b. PayoutDetailSheet (read-only — no requests, no failure states of its own)
Dialog: Title = completed ? `Format("date_payout", ShortDate(when))` : `Loc("pending_payout")`; Close button `Loc("close")`; bg `#151515`. ScrollViewer MaxHeight **560** over StackPanel Spacing 12 MinWidth **460**:
1. **Points card** — Border `#1C1C1C`, CornerRadius 12, Padding 16, child `BuildPointsBreakdown(breakdownForThisPayment, seekerHolder)`. The per-payment breakdown = the account points filtered to `account_payment_id == payment_id` (computed by the page from already-loaded points; empty payment_id → zeros).
2. **If completed**:
   - `Loc("amount")` "Amount" label (12 muted) + value 14sp `#F8F8F8`: `FormatUsdcAmount(token_amount)` + " " + (`token_type` if non-empty else `Loc("usdc")` "USDC").
   - `Loc("wallet_address")` "Wallet Address" label + the **full** address 13sp wrapped selectable.
   - `Loc("transaction")` "Transaction" label + :
     - no hash → `Loc("none")` 13 muted.
     - else the **hash itself is the link**: HyperlinkButton (Padding 0) with `ExplorerTxUrl(blockchain, hash)` (solscan for SOL, polygonscan for everything else), content = the hash 13sp wrapped. **URI-parse failure is caught** (a server hash is unvalidated; an unparseable URI must not make the whole sheet unopenable): log and fall back to the hash as plain 13sp text. No separate "view on explorer" row (no store string for it).
3. **If pending**: one line 13 muted wrapped — `Format("pending_mb_provided", FormatUsdcAmount(payout_byte_count / 1'000'000.0))` "Pending: {n} MB provided" (decimal MB, two decimals — iOS parity).

### 3c. Leaderboard table
`LoadLeaderboard()`: guard `IsLoggedIn()`. Status → "Loading..." Visible. Snapshot `ownNetworkId_` from the parsed JWT. Two calls: `getNetworkLeaderboardRanking` (→ `ApplyRanking`, feeds pane C) and `getLeaderboard(GetLeaderboardArgs{})` (→ `ApplyLeaderboard`). Both errors fall back to `result->error->message` where present; both marshal [queue, weak].

`ApplyLeaderboard(earners, state)` — all three states explicit (they were once indistinguishable):
- Failed → `LeaderboardStatusText` "Something went wrong."
- Empty → `Loc("site_app_leaderboard_empty")` "No networks on the leaderboard yet." — ONE centered line in the full-height pane (the overlay status text; deliberately NOT a card).
- Ready → status Collapsed; header + rows:
  - weights `{1, 5, 2}`, `textColumns = 2` (rank and network name read left as text; net-provided reads right as a figure). Header titles: `Loc("current_ranking")` "Current Ranking" | `Loc("network")` "Network" | `Loc("net_provided")` "Net Provided".
  - Rows `MakePaneTableRow(weights, 36, 2)`; rank = 1-based list position: cell0 `"#N"`, cell1 = network name, cell2 = `FormatMiB(net_mib_count)` (MiB float × 1024² → `FormatByteCountCompact`, e.g. "4.0 TiB").
  - **Masking**: `masked = !isOwn && (!is_public || contains_profanity)` → name replaced by `Loc("private_network")` "Private Network" and all cells muted. The name is never rendered for a non-public network; profanity is flagged by the server, hidden by the client. The OWN row is never masked.
  - **Own row** (`network_id == ownNetworkId_`): all three cells `#87FB67` AND row background `#1C1C1C` — color plus fill step, never color alone.
- `leaderboardCount_` = size; `ApplyLedgerMeta()`.

---

## 4. PANE C — POINTS (380 dip, visible only ≥ 1500)

40px header: `WalletPaneCTitle` = `Loc("network_earnings")` "Network earnings" (deliberately NOT "Account points" — that's the first group's own header). ScrollViewer, top to bottom:

1. **Group header** `LeaderboardRankLabel` = `Loc("current_ranking")` "Current Ranking".
2. **Own ranking row** — `UrPaneRowStyle MinHeight=0 Padding=12,12`, Grid {star,auto} ColumnSpacing 8: left `LeaderboardNetProvidedLabel` `Loc("net_provided")` "Net Provided" (key style, bottom-aligned); right horizontal stack Spacing 12: `LeaderboardNetProvidedValue` Montreal **18 SemiBold** (= `FormatMiB(ranking.net_mib_count)`), then `LeaderboardRankValue` Montreal **22 SemiBold** (= `"#" + leaderboard_rank`, only when rank > 0; unranked shows the faint dash).
   `ApplyRanking(ranking, ok)`: `!ok` → both values faint "-" (the list's own status line carries the failure; two error messages for one screen is noise). ok → rank/net as above, store `rankingPublic_ = leaderboard_public`, and write the switch **through `SetRankingToggle`** (echo guard below).
3. **Public toggle row** — `UrPaneRowStyle MinHeight=44`, Grid ColumnSpacing 10: `LeaderboardPublicLabel` `Loc("display_network_on_leaderboard")` "Display network on leaderboard" (`UrRowTitleStyle` 13); right `LeaderboardPublicToggle` ToggleSwitch Width 44, `UrSwitchToggleStyle` (no On/Off words, MinWidth 0; on-state fill = brand blue `#638BFC`), `AutomationProperties.LabeledBy` = the label.
   **Behavior** (`OnLeaderboardPublicToggled`):
   - `applyingRankingToggle_` set → return (**echo guard**: the handler can't tell a user flip from the programmatic render of the server's answer; `SetRankingToggle` sets the flag, writes `IsOn`, clears it).
   - `requested == rankingPublic_` → return (no-op flip).
   - a set already in flight (`settingRankingPublic_`) → snap the switch back, return.
   - `!CanCallApi()` → snap back + `RefuseNoSession()` (this switch used to fire a real API write from preview builds).
   - else: flag set, **toggle disabled**; `BeginFlow(rankingFlow_, 20000)` — timeout: clear flag, re-enable, snap back, `Notify("Something went wrong.", Error)`; `setNetworkLeaderboardPublic({is_public: requested})`.
   - `ApplyRankingPublicResult(gen, ok, requested, serverError)`: `SettleFlow` or drop+log. Clear flag, re-enable. `!ok` → snap back to `rankingPublic_` + Notify(server error verbatim | "Something went wrong.", Error — routed to the bar the user can see). ok → `rankingPublic_ = requested`; **`LoadLeaderboard()`** (the board itself changes: our row masks or unmasks).
4. **Description row** — MinHeight 0 Padding 12,8: `LeaderboardDescription` `Loc("leaderboard_description")` "The leaderboard is the sum of the last 4 payments. It is updated each payment cycle." (11 muted wrap).
5. `LeaderboardInfo` InfoBar Margin 12,8 (the leaderboard-pane snackbar surface).
6. **Group header** `AccountPointsHeading` `Loc("account_points")` "Account points" + right `AccountPointsStatusText` meta (Loading... / Something went wrong. / collapsed).
7. **`AccountPointsCard`** — Border bottom hairline Padding 12,10, Collapsed until Ready; child = `BuildPointsBreakdown(accountTotals, seekerHolder_)` (§0). `ApplyPoints`: aggregate all points by event into the five buckets via `nanoPointsToPoints`; Failed → status + card Collapsed + panel cleared; Ready → card Visible + rebuild, then `RebuildWalletCards()`.
8. **Group header** `EarningMultipliersHeading` `Loc("earning_multipliers")` "Earning multipliers".
9. **Seeker block** — Border bottom hairline Padding 12,10, StackPanel Spacing 10:
   - `SeekerStatusText` (key style 13 muted, wrap). `ApplySeekerState()`:
     - holder → text = `Loc("seeker_token_verified")` + " " + `Loc("you_re_earning_2x_points")` ("Seeker Token Verified! You're earning 2x points"); **button Collapsed**.
     - not holder → text = verifying ? `Loc("opening_wallet_in_browser")` "Opening your wallet in the browser…" : `Loc("connect_seeker_wallet")` "Connect a wallet with the Saga Genesis or Seeker Pre-Order Token"; button Visible, `IsEnabled = !verifying`. ("Waiting" must be VISIBLE — a silently greyed button is indistinguishable from broken.)
   - `VerifySeekerButton` — platform-default Button, Stretch, Content `LocBox("verify_seeker_token_btn")` "Verify Seeker Pre-Order Token". Click → the browser-bridge flow:
     1. Guards: `sheetOpen` or already verifying → return. **`CanCallApi()` BEFORE the picker** (the flow ends in an API write and opens a browser on the way) else `RefuseNoSession()`.
     2. Wallet picker ContentDialog: Title `Loc("confirm_seeker_token")` "Confirm Seeker Token"; Content `Loc("connect_seeker_wallet")`; Primary `Loc("phantom")` "Phantom" (default button); Secondary `Loc("solflare")` "Solflare"; Close `Loc("cancel")` "Cancel"; bg `#151515`. None → abort.
     3. `verifyingSeeker_ = true`; `ApplySeekerState()`. `BeginFlow(seekerFlow_, kBridgeTimeoutMs = 180000)` — timeout: clear flag, `ApplySeekerState()`, `Notify(Loc("error_claiming_multiplier"), Error)` "Sorry, there was an error claiming multiplier." (WalletConnect reports errors only when a deep link comes BACK; a closed browser tab produces nothing, ever — this watchdog is what un-bricks the button.)
     4. Challenge (android parity, replay-proof): `"Verify Seeker Token Holder - " + <epoch millis>`.
     5. `Sdk().SignWithSolanaWallet(provider, message, cb)` — opens the ur.io/wallet-connect browser bridge; cb delivers `(ok, address, signature, error)` on an arbitrary thread → marshal. `!ok` → `ApplySeekerResult(gen, false, error)`.
     6. ok → `verifySeekerHolder({wallet_address, wallet_signature, wallet_message})`; verified = `result && result->success && no error` (error falls back to `result->error->message`).
     7. `ApplySeekerResult(gen, ok, serverError)`: `SettleFlow` else drop+log ("dropping a result for an abandoned verification"). Clear flag. Notify: ok → `Loc("successfully_claimed_multiplier")` "Successfully claimed multiplier!" Success; fail → `error_claiming_multiplier` or `Format("error_claiming_multiplier_with_reason", serverError)` "Sorry, there was an error claiming multiplier: {}", Error. ok → **`LoadWallet()`** (has_seeker_token now reads true → holder state, 2x row appears); fail → `ApplySeekerState()`.
10. **Group header** `NetworkReliabilityHeading` `Loc("site_app_network_reliability")` "Network reliability" + right `ReliabilityStatusText` meta.
11. **`ReliabilityCard`** — Border bottom hairline Padding 12,10, Collapsed by default; `ReliabilityPanel` StackPanel Spacing 8. `ApplyReliability(window, state)`:
    - Failed → status "Something went wrong.", card hidden. Ready + no window → status `Loc("site_app_no_reliability")` "No reliability data yet." Ready + window → status collapsed, card visible, rebuilt:
    1. Stats grid (ColumnSpacing 24, two star cells of label-over-condensed-22): `Loc("average_reliability")` "Average reliability" + `%.2f` of `mean_reliability_weight`; `Loc("total_clients")` "Total Clients" + `max_total_client_count`.
    2. **Chart** (only if ≥2 buckets in either series): Grid Height **110** hosting a Canvas with three Polylines, plotted on SizeChanged (canvas has no size until measured; weak refs to the shapes — value capture leaked one chart per load):
       - mean: flat dashed line (dash 5,3), thickness 1.5, `#989898`; weight series: thickness 2.0, **pink `#ED8FFF`**; client series: thickness 2.0, **green `#87FB67`** (z-order: mean, clients, weight on top).
       - Scales are independent: weights and the mean normalize against `max(mean, max(weights))`; clients against `max(clients)`. Points evenly spaced; y clamped [0,1]·height inverted.
       - Legend: horizontal StackPanel Spacing 16 of dot(8px)+label(12 muted): `Loc("reliability_weight")` "Reliability Weight" (pink) | `Loc("total_clients")` "Total Clients" (green) | `Loc("average_reliability_2")` "Average Reliability" (muted).
    3. **Country multipliers** (only entries with `reliability_multiplier > 1.0`; exactly 1.0 is "no multiplier" and is dropped; skip section if none): 1px rule (Margin 0,4); `Loc("country_multipliers")` "Country multipliers" 15sp; header grid {star,auto}: `Loc("country")` "Country" / `Loc("multiplier")` "Multiplier" 12 muted; rows sorted multiplier-desc: country name 13 + `x%.2f` 13, both **lime `#87FB67` when ≥ 2.0** (kMultiplierHighlight) else `#F8F8F8`.

---

## 5. Initial / loading / preview states

- `ApplyStrings()` seeds every panel on its Loading state at startup: `WalletsStatusText`, `AccountPointsStatusText`, `ReliabilityStatusText`, `PayoutsStatusText`, `LeaderboardStatusText` all `Loc("loading")` "Loading..." Visible; the five stat values (`WalletUnpaid/Pending/Referrals`, `LeaderboardRank/NetProvided`) all faint "-"; `ApplySeekerState()` runs. (An unloaded blank destination would read "there is nothing", not "nothing asked yet".)
- **Preview harness** (`--preview-ui`, port it): navigation skips the loads (no session — apiReady() is NOT a session check) and instead calls `ShowPreviewWalletState()` + `ShowPreviewLeaderboardState()` on every visit to the destination, settling every panel on its real empty state (Ready+empty, stats not-ok) — otherwise permanent "Loading..." is indistinguishable from a hang. `--preview-ui=wallet` also raises the wallet snackbar with `wallet_connect_failed` at Error severity (demonstrates the persist-on-error rule). With `URNETWORK_PREVIEW_SAMPLE=1` (second gate + a log warning every launch) obviously-synthetic rows flow through the SAME Apply* functions: 3 wallets (SOL default/MATIC/TAO, addresses that spell "SAMPLE" with distinct last-six per chain), 3 payments (1 pending SOL, 2 completed SOL/MATIC with per-payment correct addresses), 6 points rows, a 24-bucket reliability window + 4 country multipliers (the 1.00 one proves the filter), a 5-row leaderboard (one private, one profane — both must render "Private Network"), own ranking #42 / 768 GiB / public. Sample rows are INTERACTIVE, which is exactly why `CanCallApi()` gates the actions, not the loads.
- Every panel failure keeps its own panel; other panels stand. Header-stat failures = faint dash only.

## 6. Localization keys used (key → English; all exist in the store — NO Adv()/Dev() fallbacks on this surface)

earnings→"Earnings" · payout_wallets→"Payout Wallets" · network_earnings→"Network earnings" · payouts→"Payouts" · leaderboard→"Leaderboard" · pending_payout→"Pending payout" · unpaid_data_provided→"Unpaid data provided" · total_referrals→"Total referrals" · payouts_amount_threshold→"Payouts occur every Sunday at 00:00 UTC, and require meeting a minimum USDC threshold." · upgrade_with_stripe→"Upgrade with Stripe" · to_start_earning_connect_your_solana_wallet_to→"To start earning, connect your Solana wallet to URnetwork." · these_wallets_are_not_affiliated_or_controlled→"These wallets are not affiliated or controlled by URnetwork. We will send earnings into the connected wallet." · connect_a_wallet→"Connect a wallet" · connect_external_wallet_supported_chains→"USDC addresses on Solana and Polygon are currently supported." · bittensor_wallet_future_use→"Bittensor wallets are stored for future use and can't receive payouts yet." · enter_wallet_address→"Enter wallet address" · connect→"Connect" · wallet_connected→"Wallet connected." · wallet_connect_failed→"Failed to connect the wallet." · wallet_provider→"{} Wallet" · wallet_provider_lower→"{} wallet" · default_wallet→"Default" · default_txt→"DEFAULT" · amount_usdc→"{} USDC" · plus_amount_usdc→"+{} USDC" · plus_amount→"+{}" · date_payout→"{} Payout" · payout→"Payout" · amount→"Amount" · site_app_wallet→"Wallet" · transaction→"Transaction" · none→"None" · site_app_no_payouts→"No payouts yet" · loading→"Loading..." · something_went_wrong→"Something went wrong." · wallet→"Wallet" · close→"Close" · site_app_wallet_address→"Wallet address" · wallet_address→"Wallet Address" · make_default→"Make default" · remove_wallet→"Remove wallet" · remove→"Remove" · are_you_sure_you_want_to_remove_this_wallet→"Are you sure you want to remove this wallet?" · error_setting_default_wallet→"Error setting default wallet" · error_setting_default_wallet_with_reason→"Error setting default wallet: {}" · payout_wallet_updated→"Payout wallet updated" · earnings(sheet heading)→"Earnings" · no_payouts_found→"No payouts found for this wallet" · usdc→"USDC" · pending_mb_provided→"Pending: {} MB provided" · points_breakdown→"Points breakdown" · referral→"Referral" · reliability→"Reliability" · net_points_earned→"net points earned" · seeker_token_verified→"Seeker Token Verified!" · you_re_earning_2x_points→"You're earning 2x points" · connect_seeker_wallet→"Connect a wallet with the Saga Genesis or Seeker Pre-Order Token" · opening_wallet_in_browser→"Opening your wallet in the browser…" · confirm_seeker_token→"Confirm Seeker Token" · phantom→"Phantom" · solflare→"Solflare" · cancel→"Cancel" · verify_seeker_token_btn→"Verify Seeker Pre-Order Token" · successfully_claimed_multiplier→"Successfully claimed multiplier!" · error_claiming_multiplier→"Sorry, there was an error claiming multiplier." · error_claiming_multiplier_with_reason→"Sorry, there was an error claiming multiplier: {}" · account_points→"Account points" · earning_multipliers→"Earning multipliers" · site_app_network_reliability→"Network reliability" · average_reliability→"Average reliability" · average_reliability_2→"Average Reliability" · total_clients→"Total Clients" · reliability_weight→"Reliability Weight" · site_app_no_reliability→"No reliability data yet." · country_multipliers→"Country multipliers" · country→"Country" · multiplier→"Multiplier" · current_ranking→"Current Ranking" · net_provided→"Net Provided" · network→"Network" · private_network→"Private Network" · site_app_leaderboard_empty→"No networks on the leaderboard yet." · display_network_on_leaderboard→"Display network on leaderboard" · leaderboard_description→"The leaderboard is the sum of the last 4 payments. It is updated each payment cycle." · please_login_to_urnetwork→"Please login to URnetwork" · solana→"Solana" · bittensor→"Bittensor" · polygon→"Polygon". Missing-key rule: `Localized()` returns the key id itself (visible, not blank). Known missing string: a "wallet removed" confirmation (deliberately not invented).

## 7. Threading & lifetime invariants (port verbatim)
- All SDK callbacks capture `[queue, weak]` (dispatcher queue + weak window/sheet ref) and marshal the raw outcome to the UI thread; the localization store is only read on the UI thread. Never a raw `this` across a callback.
- Sheet handlers capture the sheet weakly; the page's `shared_ptr` keeps it alive for the life of `ShowAsync`, so late callbacks after dismissal find nothing.
- Chart SizeChanged handlers hold their Polylines weakly (strong capture = ancestor cycle = one leaked chart per load).
- Destructor stops all four timers (validate debounce + 3 flow timers).
- Sheet-open failures are logged, never silent ("a click that opens nothing stays a mystery").


## SDK surface referenced
- urnet::Api::getAccountWallets(cb) -> GetAccountWalletsResult{wallets}
- urnet::Api::getPayoutWallet(cb) -> GetPayoutWalletIdResult{wallet_id}
- urnet::Api::getTransferStats(cb) -> TransferStatsResult{unpaid_bytes_provided}
- urnet::Api::walletBalance(cb) -> WalletBalanceResult{wallet_info.balance_usdc_nano_cents}
- urnet::Api::getNetworkReferralCode(cb) -> GetNetworkReferralCodeResult{total_referrals}
- urnet::Api::getAccountPoints(cb) -> AccountPointsResult{network_points: AccountPoint{event, point_value, account_payment_id}}
- urnet::Api::getNetworkReliability(cb) -> GetNetworkReliabilityResult{reliability_window, error}
- urnet::Api::getAccountPayments(cb) -> GetNetworkAccountPaymentsResult{account_payments, error}
- urnet::Api::getNetworkLeaderboardRanking(cb) -> GetNetworkRankingResult{network_ranking{leaderboard_rank, net_mib_count, leaderboard_public}, error}
- urnet::Api::getLeaderboard(GetLeaderboardArgs{}, cb) -> LeaderboardResult{earners: LeaderboardEarner{network_id, network_name, net_mib_count, is_public, contains_profanity}}
- urnet::Api::setNetworkLeaderboardPublic(SetNetworkRankingPublicArgs{is_public}, cb) -> SetNetworkRankingPublicResult{error}
- urnet::Api::setPayoutWallet(SetPayoutWalletArgs{wallet_id}, cb) -> SetPayoutWalletResult
- urnet::Api::removeWallet(RemoveWalletArgs{wallet_id}, cb) -> RemoveWalletResult{success, error}
- urnet::Api::createAccountWallet(CreateAccountWalletArgs{blockchain, wallet_address, default_token_type="USDC"}, cb) -> CreateAccountWalletResult{wallet_id}
- urnet::Api::walletValidateAddress(WalletValidateAddressArgs{address, chain}, cb) -> WalletValidateAddressResult{valid}  [called 3x per validation: SOL, MATIC, TAO]
- urnet::Api::verifySeekerHolder(VerifySeekerNftHolderArgs{wallet_address, wallet_signature, wallet_message}, cb) -> VerifySeekerNftHolderResult{success, error}
- urnet::nanoCentsToUsd(int64) [pending payout figure]
- urnet::nanoPointsToPoints(int64) [divides by 1e6 — every points figure]
- urnet::SOL / urnet::MATIC / urnet::TAO chain-id constants ("SOL"/"MATIC"/"TAO")
- urnet::AccountWallet{wallet_id, blockchain, wallet_address, default_token_type, active, has_seeker_token}
- urnet::AccountPayment{payment_id, wallet_id, blockchain, token_type, token_amount, complete_time, create_time, completed, wallet_address, tx_hash, payout_byte_count}
- urnet::ReliabilityWindow{mean_reliability_weight, max_total_client_count, bucket_duration_seconds, reliability_weights, total_client_counts, client_counts, country_multipliers: CountryMultiplier{country, country_code, reliability_multiplier}}
- SdkHost: Sdk().api() (in-process Api on the owned NetworkSpace)
- SdkHost: Sdk().apiReady()  [api_.has_value() — NOT a session check]
- SdkHost: Sdk().IsLoggedIn()
- SdkHost: Sdk().ParsedJwt() -> urnet::ByJwt{NetworkId}  [own leaderboard row highlight]
- SdkHost: Sdk().SignWithSolanaWallet(WalletConnect::Provider{Phantom|Solflare}, message, cb(ok, address, signature, error))  [ur.io/wallet-connect browser bridge + urnetwork:// deep link; cb on arbitrary thread]
- Balance() (SubscriptionBalanceStore via window relay): isPro/guest gate the UpgradeButton visibility; OnOpenUpgrade -> UpgradeSheet / BeginGuestUpgrade

## Flags (doc-vs-code drift / risks)
- DOC-VS-CODE (§7.10): the doc says payout wallets render as "cards (chain gradient disc with ticker Solana/Bittensor/Polygon, masked address, DEFAULT chip)" — the CODE renders one 44px pane ROW per wallet (MakePaneTwoLineRowButton: masked address title, chain-name note, lifetime-USDC value, chevron) with NO disc and NO chip; the gradient disc and the DEFAULT chip appear only inside WalletDetailSheet. On the row the payout wallet is marked by the lime #87FB67 value + an a11y-name suffix. The 248x132 horizontally-scrolling card strip was deliberately deleted. Code wins; the task prompt repeats the doc's stale claim.
- DOC-VS-CODE (§7.10): "own ranking (net provided + rank 22sp)" — code: rank value is 22sp SemiBold but net-provided is 18sp SemiBold (XAML lines 2071-2076).
- DOC AMBIGUITY (§7.10): "Make default / Remove with two-press confirm" reads as if both are two-press — only REMOVE is two-press (arm → relabel to "Remove" + danger confirm line → commit); Make default is a single press.
- CODE-INTERNAL: WalletSheets.h comments call the chain disc "the 48px gradient disc" but the implementation default (and the only call site) is size=44. Implement 44.
- RISK for the port: Notify() routes leaderboard-pane messages to the LeaderboardInfo bar, which lives in PANE C — hidden below 1500dip. Since the public toggle (the only snackbar-raising leaderboard action) also lives in pane C this is mostly unreachable, but a late ApplyRankingPublicResult arriving after the window narrows below 1500 would render its error on a hidden bar. Consider routing to whichever bar is actually visible.
- SDK NAMING TRAP: urnet::nanoPointsToPoints divides by 1e6, NOT 1e9, despite the "nano" name (verified against the shipped sample-data fix comment); always call the SDK helper urnet_nano_points_to_points, never hand-roll the divisor.
- SDK BEHAVIOR TRAP: Api::setPayoutWallet silently drops the call (callback never fires) when wallet_id is not a UUID — the sheet's 20s watchdog exists for this; the port must keep it.
- ODDITY (deliberate, keep): pending payout detail renders "Pending: {} MB provided" using payout_byte_count/1e6 (decimal MB) formatted through the two-decimal USDC formatter — iOS parity, not a bug.
- INCONSISTENCY (in code, keep for parity): the payouts empty state is a MakeEmptyStateCard (rounded card) inside the pane while the leaderboard empty state is a bare centered line — the two tables in the same pane use different empty-state vocabulary.
- MISSING STRING (raised upstream): the store has no "wallet removed" confirmation; Remove success deliberately shows no snackbar — the sheet closes and the card disappears. Do not invent English.
- LEGACY: LoadCurrentDestination still handles a "leaderboard" nav tag that no nav item carries anymore (the destination merged into Earnings); harmless dead branch.
- Doc §7.16 lists adv_*/dev_* English fallbacks as a port concern — this surface uses none; every key on the Earnings destination exists in the store (verified against Strings/en/Resources.resw).
- A11y nuance: WalletPaneB's landmark name is statically Loc("payouts") even while the Leaderboard tab is showing.

---

## Linux deviation: the leaderboard one-shot is per LOOK, not per PROCESS

Windows latches `leaderboardRequested_` for the life of the page and never
refetches. The Linux port copied that, and it produced a real bug against a
real account: `SettleAllEmpty()` — the no-session path of the WALLET loader —
stamped the board as a *successful empty answer*, and because nothing ever
reset the latch and `Load()` never re-ran the fetch, that pre-session verdict
outlived the sign-in that repopulated pane A. "No networks on the leaderboard
yet." stayed frozen beside a live wallet pane, on an account whose board has
99 rows.

The port therefore diverges in three ways, all toward states this spec already
demands elsewhere:

1. `Load()` re-issues the fetch when the Leaderboard tab is showing and re-arms
   the one-shot otherwise, so the board self-heals on navigation and on auth
   change exactly as every pane-A read already did.
2. A live no-session lands on its own `Fetch::NoSession` state ("Please login to
   URnetwork"), never on Ready+empty. Only the preview harness still gets
   Ready+empty — the state it exists to review.
3. An unset `earners` list is a FAILURE, not "no networks" (matching the Windows
   completion predicate).

Consequence: navigating to Earnings with the Leaderboard tab showing costs two
requests (`getNetworkLeaderboardRanking` + `getLeaderboard`), and a failed or
signed-out board is retryable without restarting the app.

### Related: the Api must be re-authorized wherever it is re-derived

`SdkHost::Initialize()` and `SdkHost::SwitchNetworkSpace()` both build a fresh
`urnet::Api` from the network space. A fresh Api carries NO token, and the SDK
otherwise only re-authorizes it as a side effect of creating a `DeviceRemote`
(which needs `urnetworkd` running). Both call sites now restore it from the
persisted session (`localState_->getByJwt()`), mirroring the Windows host.
Without that, a relaunched — or network-space-switched — client looks signed in
and 401s every authenticated read.
