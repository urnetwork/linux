# URnetwork — ACCOUNT destination: implementation-ready spec (GTK4/C++ port)

Extracted from the Windows client (WinUI 3). Sources: App/AccountPage.{h,cpp}; App/BalanceSheets.{h,cpp} (RedeemCodeSheet + UpgradeSheet); App/SettingsSheets.{h,cpp} (row kit, FieldState, AuthCodeSheet, AddAuthSheet, ReferralNetworkSheet, DeleteAccountSheet); App/SettingsPage.cpp (builds the security/referral/subscription/danger sections INTO Account's host panels and owns their loads/sheets); App/MainWindow.xaml lines 1534–1809 (the pane shell markup); App/MainWindow.xaml.cpp (balance relay, breakpoints, sheet launchers, per-destination loads); App/SubscriptionBalance.{h,cpp}; App/UsageBar.cpp; App/UrColors.h; App/UrComponents.{h,cpp}; App/App.xaml (tokens); App/StatsFormat.cpp; App/Strings/en/Resources.resw. All dimensions are dip.

String notation: Loc(key) = localization-store lookup by snake_case id (a missing key renders the key itself, never blank). Format(key, args...) = std::vformat with reorderable {} / {0} placeholders. Adv(key, english) = store lookup falling back to the given English exactly when the key is absent. English shown after each key is the shipped en value.

---

## 0. Shared vocabulary

### 0.1 Colors (UrColors.h — one dark theme)
| Token | Hex | Use on this destination |
|---|---|---|
| kBackground | #101010 | pane body; inverse text on bright fills |
| kSheet | #151515 | pane/group header strips, ALL dialogs, name-edit panel bg |
| kCard | #1C1C1C | row hover fill; upgrade product-card fill |
| kCardPressed | #2A2A2A | row pressed fill |
| kBorder | #1FFFFFFF (white 12%) | every hairline (row bottoms, pane rules) |
| kText | #F8F8F8 | body text (never pure white) |
| kTextMuted | #989898 | values, notes, secondary text |
| kTextFaint | #5A5A5A | non-value FieldStates; 'available' usage segment; table column headers |
| kDanger | #F8523B | Failed state, destructive/inline errors |
| kInverseText | #101010 | text on accent/blue fills |
| kAccent | #EFF7BB | upgrade-sheet selection border/dot + success card fill |
| UrBlueMedium | #638BFC | the pane primary-action fill (Upgrade / Create an account) |
| kProGold | #FFC400 | Pro entitlement ONLY — the plan value wears it; nothing else may |
| kUrGreen | #87FB67 | Valid supporting text (accepted name, reset-mail sent); redeem success check; Most Popular chip |
| kUrElectricBlue | #0039DE | usage bar 'used' segment |
| kUrCoral | #FF6C58 | usage bar 'pending' segment |

### 0.2 Type
- Body face everywhere here: 'PP Neue Montreal' (single Regular file; SemiBold synthesized). Display faces (ABC Gravity) are NOT used on this destination.
- Pane title (UrPaneTitleStyle): body face 12 SemiBold, CharacterSpacing 60 (units of 1/1000 em), #F8F8F8, ellipsis.
- Pane meta (UrPaneMetaStyle): 11, #989898, right-aligned, ellipsis, no wrap.
- Group header text (UrGroupHeaderTextStyle): 11, CharacterSpacing 90, #989898.
- Key text (UrKeyTextStyle): 13, #989898. Value text (UrValueTextStyle): 13, #F8F8F8, right-aligned, ellipsis. Row title (UrRowTitleStyle): 13, #F8F8F8. Row note (UrRowNoteStyle): 11, #989898, single line trimmed. Table column header (UrPaneColumnTextStyle): 11, CharacterSpacing 60, #5A5A5A. Supporting text (UrSupportingTextStyle): 12, #989898, wraps.

### 0.3 Pane metrics (App.xaml tokens)
UrPaneHeaderHeight 40; UrGroupHeaderHeight 28; UrPaneRowHeight 40 (MinHeight for standard rows); UrPaneListRowHeight 36 (table rows); UrPaneRowTallHeight 44 (two-line rows — FIXED height, note trimmed to one line); key/value rows 34. Every row: 12px horizontal inset (Padding 12,0), 1px #1FFFFFFF bottom hairline. Pane header strip: 40px, #151515 fill, bottom hairline, Padding 12,0. Group header strip: 28px, #151515, hairline TOP and BOTTOM, Padding 12,0. Vertical pane rule: Width 1, #1FFFFFFF, stretched. Row button (UrPaneRowButtonStyle): transparent bg, bottom hairline, CornerRadius 0, MinHeight 40, Padding 12,0, stretch content, hover fill #1C1C1C, pressed #2A2A2A, disabled opacity 0.38, 150 ms state transition. Panes scroll independently (vertical only).

### 0.4 FieldState — the terminal states of EVERY async field (rows::FieldState)
enum {NoSession, NoDevice, Loading, Loaded, Empty, Failed}. ApplyFieldState(textblock, state, loadedText) writes:
- Loaded: loadedText, #989898.
- Loading: Loc('loading') 'Loading...', #5A5A5A.
- Empty: Loc('none') 'None', #5A5A5A.
- NoSession: Loc('please_login_to_urnetwork') 'Please login to URnetwork', #5A5A5A.
- NoDevice: Loc('site_app_device_attaching') 'Attaching device controls…', #5A5A5A (signed in but the device/daemon is not attached — telling a signed-in user to log in is a lie).
- Failed: Loc('something_went_wrong') 'Something went wrong.', #F8523B.
Rule: a 401/transport failure must NEVER render as an empty result. Results with no error field (redeemed codes, unlink, password reset, network delete): success = result present AND transport err empty; anything else = Failed.

### 0.5 ValidationState + supporting line (kit::ApplySupportingText)
enum {NotChecked, Validating, Valid, Invalid}. Colors: Invalid #F8523B, Valid #87FB67, NotChecked/Validating #989898. Empty text still applies the color (no stale-color flash on the next write).

### 0.6 Snackbar
InfoBar + timer: default auto-dismiss 4000 ms; severity-gated — Informational/Success auto-dismiss, Warning/Error stay until dismissed (an error string is often the only diagnostic).

### 0.7 Byte formatting (FormatByteCountCompact)
IEC base-1024: below 1 KiB → 'N B'; else KiB/MiB/GiB/TiB; precision: value >= 100 → 0 decimals, >= 10 → 1 decimal, else 2 decimals (e.g. '1.20 KiB', '34.5 MiB', '120 GiB').

### 0.8 Sheets
Every sheet is a modal ContentDialog on the #151515 surface with CloseButtonText Loc('close') 'Close' (or 'Cancel' where noted). ONE sheet at a time via a window-level sheetOpen guard; opening while one shows is a no-op. The window holds the sheet object while it shows.

---

## 1. Destination shell

Nav item: tag 'account', label Loc('account') 'Account'. Selecting it (and every auth-state change while it is selected) runs, when logged in: AccountPage::LoadAccount() + Balance().Refresh() + SettingsPage::LoadSettings(). LoadSettings is what fills login methods, client id, bonus referral code and referral network on this destination — without it those rows sit on 'Please login to URnetwork' while signed in. With no session the loads are skipped and every field shows NoSession. Preview mode (--preview-ui) has no session: loads skipped, panels settle on real NoSession/empty states.

Sign-out: SettingsPage::ResetForSignOut() then AccountPage::ResetForSignOut() wipe ALL per-account state. AccountPage clears userAuth_ (dangerous: password reset mails a link to it — a leftover value mails the PREVIOUS owner), referralCode_, totalReferrals_=0, needsNameClaim_=false, empties the name box, collapses the auth row, exits edit mode, applies NoSession to the name status + referral summary, hides RoyaltyBadge, renders the codes table NoSession. SettingsPage clears clientId_/referralCode_/networkName_/deviceName_/authTypes_, applies NoSession everywhere, disables both Copy buttons.

### 1.1 Layout (Grid 'AccountView', UrPaneStyle = #101010 background)
5 columns: [AccountPaneAColumn width 360] [Auto: 1px vertical rule] [AccountPaneBColumn width *] [Auto: 1px vertical rule] [AccountPaneCColumn width 380].

Breakpoints (MainWindow::ApplyBreakpoint — the only responsive switch; window default 1120x820 dip, min 400x480):
- width >= 1500: three panes — plan(360) | account(*) | codes(380).
- 900 <= width < 1500: two panes — plan(360) | account(*). Pane C column → 0, its rule + pane collapsed.
- width < 900: one pane — account(*) only. Pane A column → 0, rule + pane collapsed. (The codes table folds first: it is a record, nothing in it is actionable, Redeem lives in the plan pane. Below 900 the PLAN pane folds, not the account pane — 360 beside ~340 is two unreadable half-columns, and the usage figures also live on the status strip and tray.)
Note: 1500 for the third pane, not the app-wide 1000 — measured at 1240 with three panes the account list got 278dip and rendered '...' beside 'Please login to URnetwork'.

### 1.2 Accessibility (destination level)
Landmark names: pane A = Loc('plan') 'Plan'; pane B = Loc('account') 'Account' (the pane grid also carries LandmarkType Main); pane C = Loc('balance_codes_title') 'Balance Codes'. Every icon-only / panel-content button gets an explicit accessible name; decorative glyphs are marked non-announced (Raw).

---

## 2. PANE A — PLAN (fixed 360)

### 2.1 Header strip (40px, #151515, bottom hairline)
- Left: 'AccountPaneATitle', UrPaneTitleStyle, Loc('plan') 'Plan'.
- Right (horizontal stack, spacing 8, centered): 'AccountPlanRing' ProgressRing 14x14, inactive + Collapsed by default — active AND visible exactly while the post-checkout confirmation poll runs (poll.confirming); 'AccountPaneAMeta' UrPaneMetaStyle — present in markup, no writer in code, stays empty.

### 2.2 Body (vertical ScrollViewer), element order top to bottom
1. PLAN VALUE row: Border UrPaneRowStyle with MinHeight 0 and Padding overridden to 12,14. Child 'AccountPlanValueText': body face, FontSize 22, SemiBold. Content from the balance snapshot: guest → Loc('guest') 'Guest'; isPro → Loc('supporter') 'Pro'; else Loc('free') 'Free'. Foreground #FFC400 (ProGold) iff isPro, else #F8F8F8. (ApplyStrings seeds 'Free' before the first snapshot.) Static.
2. USAGE BAR row: Border, bottom hairline, Padding 12,10. StackPanel spacing 8: (a) 'AccountUsageBarHost' Grid Height 32 — the UsageBar renders into it; (b) 'AccountUsageLegend' horizontal StackPanel spacing 16. UsageBar: three star-weighted segments in order used → pending → available; colors #0039DE / #FF6C58 / #5A5A5A; outer corner radius 12 on first/last segment only; 1px left margin between segments; every non-zero segment floored at 1.5 percent of the bar; total 0 → one faint full-width track. Legend entries (built once): 8x8 colored dot + 12px #989898 label, spacing 6: Loc('used_data_key') 'Used', Loc('pending_data_key') 'Pending', Loc('available_data_key') 'Available'. Updated on every balance publish with (used, pending, available). Static.
3. 'AccountUpgradeButton': UrPaneActionPrimaryStyle — fill #638BFC, foreground #101010 (5.9:1 at 14sp; white fails AA), no border, CornerRadius 4, Height 40, Margin 12 all sides, Padding 12,0, body face 14 SemiBold, hover/press = translucent content-color layer (8%/12%, 150 ms), disabled opacity 0.38. Content + visibility per snapshot: guest → Loc('create_an_account') 'Create an account', ALWAYS visible for guests; signed-in free → Loc('upgrade') 'Upgrade', visible; Pro → Collapsed. Click (OnOpenUpgrade): guest → LoginPage::BeginGuestUpgrade() (routes into the guest-upgrade create-account step); else open UpgradeSheet (5.2).
4. GROUP HEADER 28px: 'AccountUsageGroupLabel' Loc('data_usage') 'Data usage'.
5. DAILY BALANCE row: Border UrPaneRowStyle MinHeight 34; Grid ColumnSpacing 8, columns * / Auto: 'AccountDailyLabel' UrKeyTextStyle Loc('daily_data_balance_label') 'Daily Data Balance:'; 'AccountDailyValue' UrValueTextStyle = FormatByteCountCompact(startBalanceByteCount). Static.
6. REFERRALS row (same 34 shape): 'AccountReferralTotals' UrKeyTextStyle = Format('total_referrals_lld', N) 'Total Referrals: {}'; 'AccountReferralBonus' UrValueTextStyle = Format('referral_bonus', N*30) '+{} GiB/Month' — i.e. '+N*30 GiB/Month' where N = AccountPage::totalReferrals() (0 until the referral load lands; the referral load calls ApplyBalance again to repaint these two). Static.
7. REDEEM row: 'RedeemRowButton', UrPaneRowButtonStyle, MinHeight 40, whole row clickable, a11y name = Loc('redeem_balance_code'). Grid columns * / Auto: 'RedeemRowText' UrRowTitleStyle (Raw) Loc('redeem_balance_code') 'Redeem Balance Code'; trailing chevron FontIcon U+E76C, UrChevronIconStyle (14, #5A5A5A, right). Click → RedeemCodeSheet (5.1).
8. 'AccountPlanExtraHost' StackPanel — SettingsPage::BuildSubscriptionSection builds here in pane mode: one two-line row BUTTON (44 fixed, chevron U+E76C 11px muted, a11y name = title): Loc('site_app_manage_subscription') 'Manage Subscription'. Click → OpenCustomerPortal(): if not logged in → Warning snackbar Loc('please_login_to_urnetwork') and stop; else disable the row, call Api::stripeCreateCustomerPortal({}); result.url non-empty → open in the default browser; else Error snackbar with the server error verbatim or Loc('something_went_wrong'); re-enable the row in all cases.

### 2.3 The balance store feeding this pane (SubscriptionBalanceStore)
Snapshot: usedByteCount, pendingByteCount, availableByteCount, startBalanceByteCount, isPro, guest, loaded. Poll state: {confirming, timedOut}.
- Source: Api::subscriptionBalance. Mapping: available = balance_byte_count; pending = open_transfer_byte_count; used = start_balance_byte_count - balance_byte_count - open_transfer_byte_count; start = start_balance_byte_count; isPro = current_subscription.has_value().
- Pro/guest seeded OFFLINE at login from the stored jwt claims (ParsedJwt().Pro / .GuestMode); server is truth afterwards; when server Pro disagrees with the jwt claim (either direction) → RefreshJwt() once per flip; the jwt-refresh listener advances the tracked claim only when the new token lands.
- Background poll: every 30 s while started + window visible + not confirming + not (Pro with available > 0). Pro-with-balance stops polling; every window re-show still fetches once.
- Confirmation poll (after a checkout handoff or a code redeem): every 5 s, give-up budget 120 s of ACTIVE polling time (monotonic clock). Window hide pauses the timer and BANKS the unspent budget; re-show re-arms from the remainder and fires an immediate fetch. Budget exhausted → timedOut=true, background polling resumes. One fetch in flight at a time; a generation counter drops fetches from a superseded session; a fetch failure keeps the last snapshot (the poll retries).
- Publishes {snapshot, poll} on the UI thread; MainWindow::ApplyBalance repaints: plan value + gold, upgrade/create visibility, the 14px ring, the usage bar, daily value, the two referral lines, the insufficient-balance warning, and forwards to an open UpgradeSheet.
- Window-level adjacent surface: 'BalanceWarning' InfoBar shows iff insufficientBalance AND !isPro AND !confirming, action button Loc('become_supporter') 'Get UR Pro' → guest → BeginGuestUpgrade, else UpgradeSheet.

---

## 3. PANE B — ACCOUNT (star width; the identity list)

Header strip: 'AccountPaneBTitle' = Loc('account') 'Account'. No meta. Body is one vertical ScrollViewer; exact element order:

### 3.1 Profile group
1. GROUP HEADER 28px: 'AccountProfileGroupLabel' = Loc('profile') 'Profile'.
2. NETWORK-NAME VIEW ROW 'NetworkNameRow': UrPaneRowButtonStyle Button, MinHeight 44, whole row clickable, a11y name Loc('network_name_label') 'Network name'. Grid ColumnSpacing 10, columns * / Auto / Auto: 'AccountNetworkNameLabel' UrRowTitleStyle (Raw) 'Network name'; 'NetworkNameValue' UrValueTextStyle with Foreground overridden to #989898 — the saved (server-acknowledged) name, collapsed while empty; pencil FontIcon U+E70F FontSize 12, UrRowIconStyle (Raw).
3. NETWORK-NAME EDIT PANEL 'NetworkNameEditPanel' (Collapsed by default): Border, bottom hairline, Padding 12,10, Background #151515. StackPanel spacing 10: 'NetworkNameBox' TextBox UrTextInputStyle (body face 16, CornerRadius 0, Padding 0,0,0,8 — underline field) with Header Loc('network_name_label'); horizontal StackPanel spacing 8: 'SaveNameButton' Loc('save') 'Save' + 'CancelNameButton' Loc('cancel') 'Cancel' (default button chrome).
   EDIT-MODE CONTRACT (exactly one of row/panel visible): pencil click → if not logged in, no-op (second guard for the keyboard path; the row is also disabled unless Loaded); else hide the view row, show the panel, seed the box from the last SERVER-ACKNOWLEDGED name, focus it programmatically. The TextBox is NEVER the source of truth. Cancel → close editor, clear the status line (the view row still shows the saved name). Save exists ONLY while editing.
4. AUTH ROW 'AccountAuthRow': Border UrPaneRowStyle MinHeight 38, COLLAPSED whenever its text is empty (an empty fixed-height row is a 38px hole). Child 'AccountAuthText' UrKeyTextStyle. After a successful account load: Format('account_auth_verified', userAuth) '{} (verified)' or Format('account_auth_unverified', userAuth) '{} (unverified)' per the server's verified flag. Hidden for accounts with no user_auth (e.g. seedphrase-only). Static.
5. 'AccountProfileExtra' host (built in code):
   a. STATUS LINE: Border Padding 12,8,12,8 + bottom hairline; child TextBlock 12px, wraps. Carries the account load state (FieldState), the save verdict, or the password-reset outcome; colored via ApplySupportingText/ApplyFieldState.
   b. UPDATE-PASSWORD ROW: two-line pane row (44 fixed): title Loc('update_password') 'Update password'; trailing Button Loc('send') 'Send', FullDescription 'Update password'. Enabled iff state Loaded AND userAuth non-empty. Click → 3.1.2.

#### 3.1.1 Name save (OnSaveNetworkName)
Guards: no-op while a save is in flight or with no session. Trim whitespace; empty → ApplySupportingText(status, Loc('network_name_length_error') 'Network names must be 6 characters or more', Invalid) and stop. Otherwise set saving flag, disable Save, status ← Loc('loading') 'Loading...' as Validating (muted).
CLAIM vs CHANGE — the account decides which call runs:
- needsNameClaim (computed from getNetworkUser): TRUE when auth_types contains NONE of {email, phone, google, apple, solana} (old single-auth_type shape: auth_type not in that list). Seedphrase deliberately does NOT count — a seedphrase-only account still claims its auto-generated name.
- needsNameClaim → Api::claimNetworkName{new_name} — claiming puts NO reclaim cooldown on the old auto-generated name.
- else → Api::changeNetworkName{new_name} — a change applies a 24 h cooldown protecting the name being given up. THE COOLDOWN HAS NO CLIENT-SIDE UI: it renders only as the server's refusal message ('already taken', 'too similar', cooldown text), not localizable, shown VERBATIM in the status line in Invalid red. (Never call networkUserUpdate — it is neither claim nor change.)
Shared continuation: re-enable Save, clear saving flag. Error (server error.message, transport err, or null result → Loc('something_went_wrong')) → status verbatim, Invalid, editor STAYS OPEN. Success → adopt result.network_name as the acknowledged name (repaint the view row AND the copy the editor seeds from), CLOSE the editor, status ← the accepted name itself in Valid green #87FB67. Deliberate: the store has no 'Network name changed to {}' string — the server's accepted name in brand green IS the acknowledgement.

#### 3.1.2 Password reset (SendPasswordReset)
Guards: in-flight, empty userAuth, signed out. Disable Send. Api::authPasswordReset{user_auth}. No error field: success = result present AND no transport err. Re-enable. Success → status ← Format('password_reset_link_sent_to', userAuth) 'Password reset link sent to {}.' in Valid green. Failure → Loc('error_sending_password_reset_link') 'Error sending password reset link' in Invalid red.

#### 3.1.3 Account load + gating (LoadAccount / ApplyAccountState)
Not logged in → ApplyAccountState(NoSession), and still run the referral + balance-code loads (they set their own NoSession states). Logged in → Loading, then Api::getNetworkUser. Failure (error.message, transport err, or missing network_user) → Failed. Success → store needsNameClaim + userAuth, repaint the name, set the auth line, state Loaded, clear the status line.
ApplyAccountState(state) writes the FieldState onto the status line and gates EVERYTHING: NetworkNameRow.enabled = NetworkNameBox.enabled = SaveNameButton.enabled = (state==Loaded); Send-password.enabled = Loaded AND userAuth non-empty; if the state leaves Loaded while the editor is open, the editor is force-closed. One-shot initial paint: before any load, ApplyStrings applies NoSession to the account card, the referral summary and the codes table so the screen is never blank.
Session test is IsLoggedIn(), never apiReady() — apiReady is set at SDK init, not login; using it fires unauthenticated requests whose 401 renders as an empty account.

### 3.2 Security group ('AccountSecurityHost', SettingsPage::BuildSecuritySection in pane mode)
1. GROUP HEADER 28px: Loc('secure_your_account') 'Secure Your Account' (the store has no plain 'Security' key — flagged upstream).
2. LOGIN-METHODS LIST (rebuilt by RenderAuthMethods):
   - Non-Loaded or empty → ONE state line instead of rows: TextBlock 12 wrap with ApplyFieldState (Loading / NoSession / Failed / Empty each say which), wrapped in a Border Padding 12,8,12,8 + bottom hairline so it sits on the pane grid.
   - Loaded → one 44px two-line pane row PER method: title = the server auth type rendered as DATA with its first letter uppercased ('Email', 'Google', 'Solana' — no per-provider localization keys exist, by design); trailing Button Loc('remove') 'Remove' (NOT red in pane mode — red belongs to the confirmation context), FullDescription = the method label.
   - Methods parsed from getNetworkUser: auth_types list (skip empties); fallback single auth_type; fallback derived from user_auth ('email' if it contains @, else the raw value), deduped.
   - Remove click → MODAL CONFIRM (never a two-click arm): sheet title Loc('site_app_login_methods') 'Login methods', primary Loc('remove') 'Remove', close Loc('cancel') 'Cancel', DefaultButton = Close (Enter must NOT remove), body = the method label, 14px, MinWidth 320. On Primary → Api::removeAuth{auth_type}; error (either channel) → Error snackbar with the message verbatim; then reload the network user EITHER WAY.
3. ADD ROW: 44px two-line row: title Loc('site_app_login_methods') 'Login methods', trailing Button Loc('add') 'Add' → AddAuthSheet:
   - Sheet: title 'Login methods', primary Loc('add') 'Add' (starts disabled), close 'Cancel', DefaultButton Primary. Content MinWidth 380 spacing 12: TextBox UrTextInputStyle Header Loc('your_email') 'Your email'; PasswordBox Header Loc('password_label') 'Password'; supporting line Loc('password_must_be_at_least_12_characters_long') 'Password must be at least 12 characters long' (12 muted); hidden 12px danger error line.
   - Validation on every keystroke: primary enabled iff trimmed auth non-empty AND password length >= 12 AND not submitting AND logged in (the server is the real validator). Signed out: both fields disabled, error line shows NoSession.
   - Submit (primary click canceled; the sheet decides): Api::addAuth{user_auth, password}. Success → onChanged (reloads the network user so the new method appears) and Hide. Failure → re-enable, error line = server/transport message verbatim or 'Something went wrong.'
4. AUTH-CODE ROW: 44px two-line row: title Loc('auth_code') 'Auth code', note (one line, trimmed) Loc('created_auth_codes_expire_after_5_minutes') 'Created auth codes expire after 5 minutes', trailing Button Loc('site_app_create_auth_code') 'Create auth code' → AuthCodeSheet:
   - Sheet: title Loc('auth_code_create') 'Auth Code Create', close 'Close'. Content MinWidth 380 spacing 12: supporting line 'Created auth codes expire after 5 minutes'; action row (horizontal spacing 8): Create button in UrPrimaryButtonStyle (fill #638BFC, white label, PP NeueBit Bold 24, radius 12, min height 48) + 16x16 ProgressRing (hidden/inactive until a create runs); hidden code panel; 12px status line.
   - Signed out: Create disabled, status = NoSession.
   - Create (guard re-entry + session): disable button, show + start ring, hide status. Api::authCodeCreate{duration_minutes: 5, uses: 1} (5 matches the caption). Re-enable + stop ring on completion.
   - Result: (a) error.auth_code_limit_exceeded set → status Loc('site_app_auth_code_limit') 'Auth code limit reached. Try again later.' danger. (b) other error / missing code → status = server message verbatim or Loc('auth_code_error') 'Error authenticating code. Please try again or generate a new code.' danger. (c) success → reveal code panel: code TextBlock 14 monospace (Consolas), wraps, text-selectable — ABBREVIATED when longer than 14 chars as first6 + '...' + last6 (shoulder-surfer defense); status Loc('auth_code_created') 'Auth code created' muted. Copy button Loc('copy_auth_code') 'Copy Auth Code' copies the WHOLE code (never the abbreviation) → status Loc('site_app_copied') 'Copied' muted.
5. CLIENT-ID ROW (ValueActionRow, 44): title Loc('client_id') 'Client ID'; trailing horizontal stack spacing 8: value TextBlock 14 muted MaxWidth 220 ellipsis + Copy Button Loc('copy') 'Copy' (FullDescription 'Client ID'). Value comes off the DEVICE, not the api: device().getClientId() when a device exists. States: value → Loaded; none + logged in → NoDevice ('Attaching device controls…'); none + logged out → NoSession. Copy enabled iff a value exists; click copies the full id → Success snackbar Loc('client_id_copied_to_clipboard') 'Client ID copied to clipboard'.

### 3.3 Referral group ('AccountReferralHost', BuildReferralSection in pane mode)
1. GROUP HEADER 28px: Loc('referrals') 'Referrals'.
2. BONUS-CODE ROW (ValueActionRow, 44): title Loc('bonus_referral_code_label') 'Bonus referral code'; value + Copy button (FullDescription = the label). Filled by Api::getNetworkReferralCode: Loaded with the code / Empty 'None' / Failed; Copy enabled iff a code exists; click copies it → Success snackbar Loc('bonus_referral_code_copied_to_clipboard') 'Bonus referral code copied to clipboard'. Starts NoSession.
3. REFERRAL-NETWORK ROW: 44px two-line row BUTTON (chevron; a11y name = title): title Loc('referral_network') 'Referral network'; value TextBlock right of the chevron slot (13 muted, MaxWidth 240) filled by Api::getReferralNetwork — CRITICAL semantics: the server answers 'No referral network found' on the ERROR channel of a lookup that SUCCEEDED; only a transport failure (err) or missing result is Failed; a structured response, error or not, renders name-or-'None'. Click → ReferralNetworkSheet:
   - Sheet: title Loc('update_referral_network') 'Update referral network', primary Loc('update') 'Update' (starts disabled), close 'Close'. Content MinWidth 400 spacing 12: current block (label Loc('current_referral_network') 'Current referral network', UrLabelStyle 12 muted; value 14, seeded 'Loading...'); code TextBox Header Loc('enter_network_referral_code') 'Enter network referral code'; Unlink button Loc('unlink_referral_network') 'Unlink referral network' in #F8523B, left-aligned, hidden until a current network is LOADED; hidden 12px danger error line.
   - Load on open: same getReferralNetwork semantics; ApplyCurrent(state, name) puts the FieldState on the current line; Unlink visible iff a Loaded non-empty name (never offer to unlink a guess); signed out → NoSession, fields disabled.
   - Update gate on every keystroke: primary enabled iff trimmed code length >= 6 AND not busy AND logged in. Submit → Api::setNetworkReferral{referral_code}; success → clear the box, reload the current value, fire onChanged (reloads the Account row); failure → error line Loc('invalid_referral_code_please_try_again') 'Invalid referral code. Please try again.'
   - Unlink is a TWO-STEP on DIFFERENT controls (a dialog cannot open a second dialog): Arm (inline button) → error line shows Format('when_unlinking_your_referral_network_you_will_no', currentName) 'When unlinking your referral network, you will no longer be able to earn points from {}.' in danger; code box + inline button disabled; the DIALOG PRIMARY relabels to 'Unlink referral network' and enables; DefaultButton → Close (Enter must not commit); the dialog close button is the explicit Cancel. Commit → Api::unlinkReferralNetwork (no error field). Either way disarm back to normal (primary 'Update', gate re-evaluated); success → reload + onChanged; failure → error line message-or-'Something went wrong.'
4. REFERRAL SUMMARY row (markup): Border UrPaneRowStyle MinHeight 0 Padding 12,10; 'ReferralText' UrKeyTextStyle, wraps (no trimming). Filled by AccountPage::LoadReferralInfo (Api::getNetworkReferralCode): Loading → 'Loading...'; Failed → danger line + badge hidden; success → Format('referral_summary', code, total) 'Code: {0} · Referrals: {1} · Friends enter the code when they sign up' (referrals no longer use deep links — friends type the code at sign-up). Success also stores totalReferrals and calls ApplyBalance (repaints pane A's referral rows).
5. ROYALTY BADGE 'RoyaltyBadge' (Collapsed by default): Border, bottom hairline, Padding 12,10; horizontal StackPanel spacing 10: Image Assets/ReferralFrog.png 40x40 (a11y Raw — the crowned frog mascot, same as ur.io) + 'RoyaltyText' UrKeyTextStyle centered, Loc('referral_royalty') "You're referral royalty!". Visible iff totalReferrals > 0; hidden on failure/sign-out. Static.

### 3.4 Danger group
1. GROUP HEADER 28px: 'SettingsAccountHeading' = Loc('account') 'Account' (painted by SettingsPage::ApplyStrings — that class still owns these rows).
2. 'AccountDangerHost' (BuildDangerSection): two whole-row 44px two-line row BUTTONS with chevrons (never 'Sign out [Sign out]'):
   - Loc('sign_out') 'Sign out' → SdkHost::Logout() immediately (no confirm).
   - Loc('delete_account_2') 'Delete account' — NOT red here (red belongs to the confirmation context) → DeleteAccountSheet:
     Sheet: title Loc('are_you_sure_delete_account') 'Are you sure you want to delete your account?', primary Loc('delete_account_2') 'Delete account' (starts DISABLED), close Loc('cancel') 'Cancel', DefaultButton Close (Enter can never destroy a network). Content MinWidth 400 spacing 12: warning TextBlock 13 wrap in danger — Loc('site_app_delete_warning') 'This permanently deletes your network, its data, and its earnings. This cannot be undone.'; name line (14, wrap) — WHICH network goes, shown as its own line (a placeholder disappears once typing starts); confirm TextBox UrTextInputStyle, Header Loc('site_app_delete_confirm') 'Type your network name to confirm', disabled until a fresh name lands, placeholder = the freshly-read name; hidden 12 danger error line.
     FRESHNESS RULE: the sheet takes NO cached name. On open it reads the CURRENT session's name itself via Api::getNetworkUser (Loading → Loaded/Failed on the name line via FieldState); the gate fails closed on empty (any failure, or before the read). Rationale — a real bug: a cached name survived a sign-out; Api::networkDelete takes no arguments and acts on the CURRENT JWT, so a stale gate could destroy the WRONG network.
     Gate (single place the primary enables): enabled iff not deleting AND name non-empty AND trimmed typed text == name exactly. Re-evaluated on every keystroke and on name arrival.
     Submit (primary click canceled; belt-and-braces re-check): Api::networkDelete. No error field: success = result + no transport err → Hide the sheet and SdkHost::Logout() (the session is meaningless). Failure → re-enable primary, error line = transport message verbatim or Loc('error_deleting_account') 'Sorry, there was an error deleting your account.'

---

## 4. PANE C — CODES (fixed 380; visible only at width >= 1500)

Header strip: 'AccountPaneCTitle' = Loc('balance_codes_title') 'Balance Codes'; right 'AccountPaneCMeta' UrPaneMetaStyle = the row count as a plain number when Loaded, cleared otherwise.
Body: a Grid holding (a) a ScrollViewer with 'BalanceCodesPanel' (the table) and (b) 'BalanceCodesEmptyText' — ONE centered line in the FULL-HEIGHT pane (Horizontal/Vertical Center, TextAlignment Center, wrap, MaxWidth 300, Margin 16, hit-test invisible, UrSupportingTextStyle), never a short card at the top of a tall column.

RenderBalanceCodes(codes, state) is the ONLY writer (called by the load, ApplyStrings and sign-out — one function, one shape; scattered writers used to disagree):
- loaded := state==Loaded AND non-empty. Empty text visible iff !loaded. Empty → the specific line Loc('no_balance_codes_found') 'No balance codes found' in faint #5A5A5A (beats the generic 'None'); Loading → 'Loading...' faint; NoSession → 'Please login to URnetwork' faint; Failed → 'Something went wrong.' danger #F8523B. All four MUST be textually distinct.
- Meta: count when loaded, else cleared.
- Table (only when loaded): THREE columns, not four — a 380 pane; expiry is dropped (a redeemed code's data is already on the plan). Star weights WITH minimums so the table narrows instead of clipping: weights {1.4, 1.0, 1.2}, per-column MinWidth 56, ColumnSpacing 12.
  - Header: 28px group-rhythm strip (#151515, hairlines top+bottom, Padding 12,0), cells in UrPaneColumnTextStyle (11, letterspacing 60, faint): Loc('code') 'Code' (left), Loc('data') 'Data' (right), Loc('redeemed') 'Redeemed' (right) — textColumns=1: first column left text, the rest right figures.
  - Rows: fixed 36px, 12px inset, bottom hairline; col0 UrRowTitleStyle (13 #F8F8F8), cols 1–2 UrValueTextStyle overridden muted #989898, right. Content per code: col0 = masked secret — first 3 + '...' + last 3 (length <= 6 renders as that many '.' chars); col1 = '+' + FormatByteCountCompact(balance_byte_count); col2 = the YYYY-MM-DD prefix (first 10 chars) of the redeem_time ISO timestamp, empty if absent. Rows static.
- Load (LoadBalanceCodes): signed out → NoSession render, no request. Else render Loading, call Api::getNetworkRedeemedBalanceCodes. The result has NO error field: failure = missing result OR transport err (without this a 401 arrived as an empty list and rendered the reassuring 'No balance codes found'). Then render Failed / Empty / Loaded.

---

## 5. Sheets owned by the plan pane

### 5.1 RedeemCodeSheet (Redeem row; macOS RedeemBalanceCodeSheet parity)
Dialog: title Loc('redeem_code') 'Redeem Code', primary Loc('redeem') 'Redeem' (starts disabled), DefaultButton Primary, close 'Close', #151515. Content Grid MinWidth 400 with two swap panels:
- FORM (spacing 8): code TextBox — Header Loc('balance_code') 'Balance code', placeholder Loc('enter_balance_code') 'Enter balance code', MaxLength 26 (a redeemable code is EXACTLY 26 characters); error line 12 danger wrap, hidden, default text Loc('invalid_balance_code') 'Invalid balance code'; note 12 muted wrap top-margin 4 — Loc('balance_code_where_to_buy') "Don't have a code? Data codes can be purchased at ur.io and emailed to you." (plain text, deliberately NO link).
- SUCCESS (spacing 8, centered, Margin 0,16): '✓' 32px in #87FB67; Loc('balance_code_redeemed') 'Balance code redeemed.' 15px; amount line 22 SemiBold = '+' + FormatByteCountCompact(amount).
Behavior: every TextChanged → primary enabled iff trimmed length == 26 AND not redeeming; typing hides the error. Primary click is CANCELED (the sheet stays open; Submit decides). Submit guards: re-entry, length == 26, IsLoggedIn. In flight: primary + code box disabled. Api::redeemBalanceCode{secret}.
Outcome taxonomy (load-bearing): ok = result with transfer_balance → swap to SUCCESS, amount = transfer_balance.balance_byte_count, primary text cleared (nothing left to submit), fire onRedeemed → Balance().StartConfirmationPolling() (the plan-pane ring spins) + AccountPage::LoadBalanceCodes() (refresh pane C). rejected = server reached, decided, refused (result.error present) → re-enable, error line = the server's own reason verbatim when non-empty else 'Invalid balance code'. NEITHER (transport failure — the redeem may have COMMITTED server-side with only the response lost) → re-enable, error line = Loc('balance_code_transport_error') "We couldn't reach the server — check your connection. If you were charged, the code may already be applied; check your balance before trying again." Never tell a user whose code just credited that it was invalid.

### 5.2 UpgradeSheet (macOS UpgradeSubscriptionSheet + PurchaseSuccessView; checkout leg = Stripe session)
Dialog: no title, close 'Close', #151515. Content: ScrollViewer MaxHeight 560 over a Grid MinWidth 440 holding five swap pages {Products, Checkout, Waiting, Success, TimedOut} — exactly one visible.
PRODUCTS page (StackPanel spacing 12), order:
1. Loc('become_a') 'Become a' — 20px.
2. Loc('urnetwork_supporter') 'URnetwork Supporter' — 28 Bold, wrap, Margin 0,-8,0,0.
3. Loc('support_us') 'Support us in building a new kind of network that gives instead of takes.' — 14 muted wrap.
4. Loc('unlock_speed') (see string table) — 14 muted wrap.
5. YEARLY card (Margin 0,8,0,0) then MONTHLY card. Card: Border CornerRadius 8, BorderThickness 2, Padding 16, fill #1C1C1C, whole card tappable (selects). Inner Grid ColumnSpacing 14, columns Auto/*/Auto: selection dot Ellipse 14x14 StrokeThickness 2; labels (spacing 2): title Loc('yearly') 'Yearly' / Loc('monthly') 'Monthly' 18 Bold; yearly only: Loc('includes_2_week_free_trial') 'Includes 2 week free trial' 13 muted; yearly only trailing chip: Border radius 10, Padding 10,4, fill #87FB67, text Loc('most_popular') 'Most Popular' 11 SemiBold #101010. Selection paint: selected card border + dot stroke/fill #EFF7BB; unselected border #5A5A5A, dot stroke muted, fill transparent. Default: YEARLY.
6. Loc('pricing_shown_at_checkout') 'Pricing is shown at checkout.' — 12 faint wrap (no price api; Stripe shows the authoritative price).
7. Checkout error line — 12 danger wrap, hidden, TEXT-SELECTABLE (a failed browser launch appends the raw url).
8. Subscribe row (Grid, Margin 0,4,0,0): Button Loc('join_the_movement') 'Join the movement', stretch, platform accent-button style; overlay ProgressRing 16x16 right, Margin 0,0,12,0.
9. Terms: 12 muted, the markdown store string by_subscribing_you_agree_to_urnetwork_s_terms rendered with the two bracketed ranges as real inline hyperlinks (https://ur.io/terms, https://ur.io/privacy), tab-reachable.
CHECKOUT page (embedded): header — 'UR Pro' 18 Bold (product name, NEVER translated) + close X Button (FontIcon U+E711 at 12; tooltip + a11y name Loc('close')) → back to Products (the abandoned session just expires; Stripe embedded has no cancel url — the X is the only way out); webview slot Grid MinWidth 440 Height 480 background #101010, 36x36 centered ProgressRing above the embedded browser widget.
WAITING page (Margin 0,24, spacing 12, centered): ProgressRing 36 active; Loc('waiting_for_approval') 'Waiting for approval' 18 SemiBold; body 14 muted centered — one of checkout_opened_in_browser (hosted), processing_payment (embedded completed), checkout_interrupted_confirming (browser process died AFTER the form rendered).
SUCCESS page: Border radius 12, Padding 24, fill #EFF7BB; spacing 8: Loc('you_re_premium') "You're premium." 22 Bold #101010; Loc('thanks_for_building_the_new_internet_with_us') 16 #101010 wrap.
TIMEDOUT page (Margin 0,24, spacing 12): clock glyph U+E823 32 muted (Segoe MDL2 Assets on Windows); Loc('purchase_confirmation_timeout') 14 muted centered.
FLOW: Join click (guards: in-flight, IsLoggedIn): disable button, spin ring, hide error, reset the one-shot hosted-fallback flag. Probe for an embeddable webview runtime → Api::createStripeCheckoutSession{item_id: 'pro_yearly' or 'pro_monthly', ui_mode: 'embedded' or 'hosted'}.
- EMBEDDED leg: needs result.client_secret with no errors; ANY embedded failure BEFORE the form rendered retries ONCE as a fresh HOSTED session (nothing shown yet → nothing can be lost; the embedded session expires server-side). Open: build a FRESH webview per attempt (a generation counter invalidates every async continuation of a torn-down attempt), default background #101010 (no white flash), navigate to https://ur.io/checkout?client_secret=ESC(secret)&redirect_link=ESC(urnetwork://checkout) (RFC 3986 percent-encoding, unreserved chars kept). The bridge page mounts Stripe Embedded Checkout (card data stays in Stripe's iframe) and hands control back by navigating to the urnetwork:// scheme — intercept ANY urnetwork:// navigation: cancel it and handle DEFERRED on the UI queue (teardown from inside the webview's own event is reentrancy). Navigation completed OK → mark page-loaded, stop the ring. Failed before ever loading (offline/dns/tls) → hosted fallback. Browser-process crash AFTER the form rendered → the card may already be charged; NEVER show a bare 'retry' (invites a double purchase) — waiting body = checkout_interrupted_confirming, StartConfirmationPolling, show Waiting. Crash BEFORE render → hosted fallback. target=_blank links open in the system browser. Webview user-data dir = the app's per-user storage dir + '/webview2'. Callback query: status=complete → waiting body 'Processing payment', StartConfirmationPolling, Waiting (the server only believes the Stripe webhook — a client saying 'I paid' is not evidence); else errorCode/errorMessage → back to Products with errorMessage verbatim or 'Something went wrong.' Hosted fallback: only from the Checkout page; ONCE per Join press — a second failure shows the inline error instead of looping.
- HOSTED leg: server error → inline error verbatim; empty url → transport message or 'Something went wrong.'; else launch the url in the system browser and OBSERVE the launch verdict — only on success: stop ring, re-enable Join, waiting body checkout_opened_in_browser, Balance().StartConfirmationPolling(), show Waiting. Launch failure → inline error Loc('checkout_browser_launch_failed') + newline + the url (selectable).
- Dialog dismissed mid-checkout (Close/Esc): mark the sheet closed (in-flight session results are dropped — no browser/webview may open for a dead sheet) and tear the webview down. Leaving the Checkout page always tears the webview down (a torn-down webview cannot be revived; next attempt builds fresh).
- Balance pushes (the window forwards every publish to the open sheet): a Pro-confirming snapshot flips Waiting OR TimedOut → Success (TimedOut is 'we have not heard yet', not terminal — the background poll or a later refresh can still confirm after the 120 s budget); poll.timedOut while Waiting → TimedOut.

---

## 6. English string inventory (key = shipped en value)
plan='Plan'; account='Account'; balance_codes_title='Balance Codes'; free='Free'; guest='Guest'; supporter='Pro'; upgrade='Upgrade'; create_an_account='Create an account'; data_usage='Data usage'; daily_data_balance_label='Daily Data Balance:'; total_referrals_lld='Total Referrals: {}'; referral_bonus='+{} GiB/Month'; redeem_balance_code='Redeem Balance Code'; site_app_manage_subscription='Manage Subscription'; used_data_key='Used'; pending_data_key='Pending'; available_data_key='Available'; become_supporter='Get UR Pro'; profile='Profile'; network_name_label='Network name'; save='Save'; cancel='Cancel'; update_password='Update password'; send='Send'; account_auth_verified='{} (verified)'; account_auth_unverified='{} (unverified)'; network_name_length_error='Network names must be 6 characters or more'; loading='Loading...'; none='None'; please_login_to_urnetwork='Please login to URnetwork'; site_app_device_attaching='Attaching device controls…'; something_went_wrong='Something went wrong.'; password_reset_link_sent_to='Password reset link sent to {}.'; error_sending_password_reset_link='Error sending password reset link'; secure_your_account='Secure Your Account'; site_app_login_methods='Login methods'; add='Add'; remove='Remove'; auth_code='Auth code'; created_auth_codes_expire_after_5_minutes='Created auth codes expire after 5 minutes'; site_app_create_auth_code='Create auth code'; auth_code_create='Auth Code Create'; auth_code_created='Auth code created'; auth_code_error='Error authenticating code. Please try again or generate a new code.'; site_app_auth_code_limit='Auth code limit reached. Try again later.'; copy_auth_code='Copy Auth Code'; site_app_copied='Copied'; client_id='Client ID'; copy='Copy'; client_id_copied_to_clipboard='Client ID copied to clipboard'; your_email='Your email'; password_label='Password'; password_must_be_at_least_12_characters_long='Password must be at least 12 characters long'; referrals='Referrals'; bonus_referral_code_label='Bonus referral code'; bonus_referral_code_copied_to_clipboard='Bonus referral code copied to clipboard'; referral_network='Referral network'; update_referral_network='Update referral network'; update='Update'; current_referral_network='Current referral network'; enter_network_referral_code='Enter network referral code'; unlink_referral_network='Unlink referral network'; when_unlinking_your_referral_network_you_will_no='When unlinking your referral network, you will no longer be able to earn points from {}.'; invalid_referral_code_please_try_again='Invalid referral code. Please try again.'; referral_summary='Code: {0} · Referrals: {1} · Friends enter the code when they sign up'; referral_royalty="You're referral royalty!"; sign_out='Sign out'; delete_account_2='Delete account'; are_you_sure_delete_account='Are you sure you want to delete your account?'; site_app_delete_warning='This permanently deletes your network, its data, and its earnings. This cannot be undone.'; site_app_delete_confirm='Type your network name to confirm'; error_deleting_account='Sorry, there was an error deleting your account.'; code='Code'; data='Data'; redeemed='Redeemed'; no_balance_codes_found='No balance codes found'; redeem_code='Redeem Code'; redeem='Redeem'; balance_code='Balance code'; enter_balance_code='Enter balance code'; invalid_balance_code='Invalid balance code'; balance_code_where_to_buy="Don't have a code? Data codes can be purchased at ur.io and emailed to you."; balance_code_redeemed='Balance code redeemed.'; balance_code_transport_error="We couldn't reach the server — check your connection. If you were charged, the code may already be applied; check your balance before trying again."; close='Close'; become_a='Become a'; urnetwork_supporter='URnetwork Supporter'; support_us='Support us in building a new kind of network that gives instead of takes.'; unlock_speed="You'll unlock even faster speeds, and first dibs on new features like robust anti-censorship measures and data control."; yearly='Yearly'; monthly='Monthly'; includes_2_week_free_trial='Includes 2 week free trial'; most_popular='Most Popular'; pricing_shown_at_checkout='Pricing is shown at checkout.'; join_the_movement='Join the movement'; by_subscribing_you_agree_to_urnetwork_s_terms="By subscribing, you agree to URnetwork's [Terms and Services](https://ur.io/terms) and [Privacy Policy](https://ur.io/privacy)"; waiting_for_approval='Waiting for approval'; checkout_opened_in_browser='Complete your purchase in the browser. Your plan updates here automatically once payment is confirmed.'; processing_payment='Processing payment'; checkout_interrupted_confirming="The checkout view closed unexpectedly. If your payment went through, your plan updates here automatically — there's no need to buy again."; purchase_confirmation_timeout="We couldn't confirm your purchase yet. If you completed checkout, your plan will update automatically in a few minutes — there's no need to buy again."; you_re_premium="You're premium."; thanks_for_building_the_new_internet_with_us='Thanks for building the new internet with us'; checkout_browser_launch_failed="We couldn't open your browser. Copy this link into a browser to finish checkout:".
No Adv()/Dev() fallback keys are used on this destination (they exist on Settings/Developer surfaces). DELIBERATELY MISSING strings (do not invent): no 'Network name changed to {}' (the accepted name in green IS the acknowledgement); no plain 'Security' heading key (secure_your_account is reused; flagged upstream).

---

## 7. Behavioral invariants checklist (port verbatim)
- One writer per surface: ApplyBalance is the only painter of pane A's plan/usage/referral figures; RenderBalanceCodes the only painter of pane C; ApplyAccountState the only gate of pane B's profile controls.
- Session gate is IsLoggedIn() everywhere; apiReady() is NOT a session test.
- Re-entry flags on every mutation: savingName, sendingReset, redeeming, checkingOut, deleting, busy (referral sheet), creating (auth code), submitting (add auth). Buttons disable during flight and re-enable on completion.
- Every SDK callback marshals to the UI thread with a weak ref + value-captured payload; balance fetches and webview attempts carry generation counters so a superseded session/attempt is dropped.
- All of Loading / Empty / NoSession / Failed are textually distinguishable on every async field; a 401 never renders as an empty result.
- Destructive confirmations: DefaultButton = Close (Enter never commits), the destructive word appears only on the committing control, red only in the confirmation context.
- Copy affordances always copy the FULL value; masked/abbreviated display never reaches the clipboard.

## SDK surface referenced
- Sdk().IsLoggedIn() — session gate for every load/mutation on this destination
- Sdk().apiReady() — explicitly NOT used as a session test (api_.has_value at SDK init)
- Sdk().Logout() — Sign out row; also after successful networkDelete
- Sdk().ParsedJwt() — offline Pro/GuestMode seed for the balance snapshot
- Sdk().RefreshJwt() — requested once per server/jwt Pro disagreement
- Sdk().hasDevice() / Sdk().device().getClientId() — DeviceRemote: the Client ID row value
- Sdk().api().getNetworkUser — account load (network_name, user_auth, verified, auth_types/needsNameClaim); login-methods list; DeleteAccountSheet fresh-name read
- Sdk().api().claimNetworkName{new_name} — save when the account still has its auto-generated name (no reclaim cooldown)
- Sdk().api().changeNetworkName{new_name} — save otherwise (24h cooldown, server-enforced; refusal shown verbatim)
- Sdk().api().authPasswordReset{user_auth} — Update password row (no error field; result + no transport err = success)
- Sdk().api().getNetworkReferralCode — referral summary (referral_code + total_referrals) AND the bonus-referral-code row
- Sdk().api().getReferralNetwork — referral-network row + sheet current value ('No referral network found' arrives on the error channel of a SUCCESS; only transport err is Failed)
- Sdk().api().setNetworkReferral{referral_code} — ReferralNetworkSheet Update (gate: >= 6 chars)
- Sdk().api().unlinkReferralNetwork — ReferralNetworkSheet armed Unlink (no error field)
- Sdk().api().getNetworkRedeemedBalanceCodes — pane C table (no error field; missing result / transport err = Failed)
- Sdk().api().redeemBalanceCode{secret} — RedeemCodeSheet (ok/rejected/transport taxonomy; 26-char secret)
- Sdk().api().removeAuth{auth_type} — login-method Remove after modal confirm; list reloaded either way
- Sdk().api().addAuth{user_auth, password} — AddAuthSheet (password >= 12)
- Sdk().api().authCodeCreate{duration_minutes:5, uses:1} — AuthCodeSheet (error.auth_code_limit_exceeded is its own case)
- Sdk().api().subscriptionBalance — SubscriptionBalanceStore fetch (30s background / 5s confirmation, 120s active budget)
- Sdk().api().createStripeCheckoutSession{item_id: pro_yearly|pro_monthly, ui_mode: embedded|hosted} — UpgradeSheet
- Sdk().api().stripeCreateCustomerPortal{} — Manage Subscription row (opens result.url in the default browser)
- Balance().Refresh() — on navigating to Account
- Balance().StartConfirmationPolling() — after redeem success and after checkout handoff/completion
- Balance().Current() / Balance().CurrentPoll() — seed the pane before the first publish
- LoginPage::BeginGuestUpgrade() — the guest 'Create an account' route (plan button + balance-warning action)

## Flags
- DOC vs CODE — linux_agent_help.md section 7.9 lists 'name-availability' on the Account pane: NO availability pre-check exists in AccountPage. There is no debounced availability API call and no setNetworkName/networkCheck call on this surface; the only client validation is empty-after-trim (network_name_length_error), plus the server's refusal at save time via claimNetworkName/changeNetworkName shown verbatim. The task prompt's 'setNetworkName availability check' likewise matches nothing here (the debounced name check lives in the login create-network flow). Code wins: port save-time validation only.
- DOC vs CODE (stale in-file comment; the doc is right): MainWindow.xaml's Account comment block says 'one pane < 640'; the code folds pane A below 900 and pane C below 1500. Doc 7.1 (>=1500 / >=900 / <900) matches the code.
- Deliberately missing store strings (do not invent): no 'Network name changed to {}' — success renders the accepted name itself in kUrGreen; no plain 'Security' heading key — secure_your_account is reused and reported upstream as a needed addition.
- AccountPaneAMeta exists in markup but has NO writer anywhere — it stays empty; only the 14px confirmation ring occupies the pane-A header's right slot. Implement the slot but expect no content.
- FormatByteCountCompact's header comment shows '1.2 KiB' style but the code prints 2 decimals below 10 (e.g. '1.20 KiB'), 1 decimal for 10–99.99, 0 for >=100. Code wins.
- The 24h change-cooldown and no-cooldown-claim rules are SERVER-side; the client's only rendering of a cooldown is the server refusal message verbatim in the status line (danger red). There is no client cooldown timer or countdown UI.
- needsNameClaim counts identity methods {email, phone, google, apple, solana} only; seedphrase deliberately does not count. The legacy single-auth_type result shape must be handled identically.
- The security/referral/subscription/danger sections are BUILT BY the Settings page object into Account's host panels (pane mode), and their loads run from LoadSettings — the Account destination must trigger LoadSettings or those rows sit on 'Please login to URnetwork' while signed in. LoadSettings also fires getNetworkClients + accountPreferencesGet for Settings-pane rows (device name/spec, product updates) that are NOT on Account; decide whether to split the load in the port.
- 401-vs-empty distinctions depend on several results having NO error field (redeemed codes, unlink, password reset, network delete): success is strictly 'result present AND no transport error'. getReferralNetwork inverts the usual rule: a structured error response is a valid 'none' answer; only transport failure is Failed.
- UpgradeSheet's embedded leg is WebView2-specific (runtime probe, per-attempt rebuild, urnetwork:// interception, per-user data dir). A GTK port needs a WebKitGTK equivalent honoring the same state machine: embedded-first with exactly one hosted rescue before the form renders, crash-after-render → confirmation poll (never 'retry'), generation-guarded deferred teardown, deferred handling of the scheme callback. The 'Join the movement' button uses the platform accent-button style when present — pick the port's accent-primary equivalent.
- Timeout is not terminal: a Pro-confirming balance snapshot flips the TimedOut page to Success at any later time; keep the sheet subscribed to balance pushes while open.
- Glyph dependencies to map to a Linux icon set: U+E70F pencil (name edit), U+E76C chevron, U+E711 close X (checkout header), U+E823 clock (timeout page, Segoe MDL2), plus a literal '✓' in the redeem success. Fonts: PP Neue Montreal everywhere on the panes; PP NeueBit Bold 24 only in the AuthCode sheet's UrPrimaryButtonStyle button.
- Windows-only mechanics needing equivalents: clipboard via DataPackage (GDK clipboard), default-browser launch with an OBSERVED success/failure verdict (the hosted-checkout error path depends on knowing the launch failed), and the one-ContentDialog-at-a-time semantics via the window-level sheetOpen guard.
- connected_provider_count in the store is a Plural composite key (base entry empty; real entries are key.one/key.other). Not used on Account, but the port's plural scheme must handle composite keys.
