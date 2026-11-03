// The one-tap Bittensor key's Linux port, read as source (apple
// test-bittensor-local-key_test.go, TestBittensorKeyDecisionsStayInTheSdk): the
// screens need GTK, libsecret and the SDK, so this reads them with the comments
// blanked and checks what the port must hold.
//
//  - The decisions stay in the SDK: no app source makes a key or signs.
//  - The store keeps the phrase in the Secret Service only: no file, no log;
//    a missing Secret Service is the SDK's BittensorLocalKeyStoreAbsent, so the
//    flow offers no key there; the GTK loop never waits on an unlock prompt.
//  - The funds disclaimer is the SDK's id wherever it shows, never the app's.
//  - The Key screen hides the words when its window loses the focus, takes
//    the two words in fields that learn nothing, and asks for the user's own
//    password (polkit) before it reveals or deletes a stored key.
//  - Copy holds the phrase on the clipboard for the SDK's 60 seconds only.
//  - The reminder's notification opens the Key screen; Sign out and Delete
//    account warn about an unsaved key and Back up first opens it; a deleted
//    account's key is forgotten; this session's sign-out says so too.
//  - Change wallet offers the key with its own disclaimer; the replace warning
//    comes before any key replaces a payout wallet; the claim relay's sheet
//    asks the SDK; the SDK's claim status decides the tile and draws
//    URnetwork's claim (4d); the points card takes the SDK's word that the
//    key here is paid.
//  - The polkit action and the Flatpak's permission exist, and the new
//    sources are built and listed for xgettext.
//
// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "TestHarness.hpp"
#include "WiringSource.hpp"

namespace {

namespace fs = std::filesystem;
using urnw::testing::wiring::Before;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::CountOf;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;
using urnw::testing::wiring::ReadRaw;

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// Every C++ source under src/, the daemon's included, relative to src/.
std::vector<std::string> AppSources() {
  std::vector<std::string> sources;
  std::error_code error;
  const fs::path root(UR_SRC_DIR);
  for (fs::recursive_directory_iterator it(root, error), end; !error && it != end;
       it.increment(error)) {
    const fs::path path = it->path();
    const std::string extension = path.extension().string();
    if (extension == ".cpp" || extension == ".hpp" || extension == ".h") {
      sources.push_back(fs::relative(path, root).string());
    }
  }
  return sources;
}

}  // namespace

// apple TestBittensorKeyDecisionsStayInTheSdk: a port reads the flow's card and
// screen and never makes a key or signs. Neither the C++ wrapper's calls nor the
// C ABI's appear in any app source.
UR_TEST(BittensorKeyWiring_TheDecisionsStayInTheSdk) {
  const std::vector<std::string> sources = AppSources();
  // CONTROL: the scan reads the app, or it would pass on nothing
  UR_EXPECT_TRUE_MSG("the scan finds the app's sources", sources.size() > 100);
  bool readsTheFlow = false;
  for (const std::string& source : sources) {
    const std::string code = Lower(ReadCode(source));
    for (const char* call : {"bittensorlocalkeygenerate", "bittensorlocalkeysign",
                             "bittensor_local_key_generate", "bittensor_local_key_sign"}) {
      UR_EXPECT_TRUE_MSG(source + " calls " + call, !Contains(code, call));
    }
    if (Contains(code, "getcard()") && Contains(code, "getscreen()")) readsTheFlow = true;
  }
  UR_EXPECT_TRUE_MSG("the host reads the flow's card and screen", readsTheFlow);
}

// The store: one Secret Service item per network id through libsecret, never
// replaced, never a file, never a log; a Secret Service that isn't there is the
// SDK's absent answer; on the GTK loop a read asks the desktop nothing.
UR_TEST(BittensorKeyWiring_TheStoreIsTheSecretService) {
  const std::string store = ReadCode("BittensorKeyStore.cpp");
  UR_EXPECT_TRUE(!store.empty());
  UR_EXPECT_TRUE(Contains(store, "secret_password_store_sync("));
  UR_EXPECT_TRUE(Contains(store, "secret_password_search_sync("));
  UR_EXPECT_TRUE(Contains(store, "secret_password_clear_sync("));
  UR_EXPECT_TRUE(Contains(store, "schema.name = \"network.ur.bittensor-local-key\";"));
  // the add never replaces: an item there, or a store that can't say, refuses
  const std::string add = FunctionBody(store, "bool AddKey(");
  UR_EXPECT_TRUE(Before(add, "SearchNetwork(", "secret_password_store_sync("));
  UR_EXPECT_TRUE(Contains(add, "if (!search.failure.empty() || search.items != nullptr) return false;"));
  // no plain file, no log: the notes are the only file, and hold no phrase
  for (const char* never : {"ofstream", "fopen(", "ReplaceFile(", "g_message(", "g_warning(",
                            "g_print", "printf(", "std::cerr", "std::cout", "g_debug("}) {
    UR_EXPECT_TRUE_MSG(std::string("the store uses ") + never, !Contains(store, never));
  }
  UR_EXPECT_EQ(1u, CountOf(store, "bittensor_key::WriteNoteAt("));
  UR_EXPECT_TRUE(Contains(FunctionBody(store, "urnet::BittensorLocalKeyStore NewBittensorKeyStore("),
                          "store.write_note = [](std::string networkId, std::string name, "
                          "std::string value) {"));
  // no Secret Service: absent, so the flow offers no key and only the wallet
  UR_EXPECT_TRUE(Contains(FunctionBody(store, "std::string FailureAnswer("),
                          "? urnet::BittensorLocalKeyStoreAbsent"));
  UR_EXPECT_TRUE(Contains(store, "bittensor_key::StoreFailureFor(bittensor_key::DbusErrorName(error))"));
  // the GTK loop never waits on an unlock prompt
  const std::string flags = FunctionBody(store, "SecretSearchFlags SearchFlags(");
  UR_EXPECT_TRUE(Contains(flags, "if (mayAskToUnlock && !g_main_context_is_owner(g_main_context_default())) {"));
  UR_EXPECT_TRUE(Contains(flags, "flags |= SECRET_SEARCH_UNLOCK;"));
  // the sign-out's question asks the desktop nothing (read raw: the argument's
  // name is a comment)
  const std::string host = ReadRaw("BittensorKeyHost.cpp");
  UR_EXPECT_TRUE(Contains(FunctionBody(host, "void BittensorKeyNeedsBackup("),
                          "AskingFlow(networkId, /*mayAskToUnlock=*/false)"));
}

// The disclaimer is the SDK's id, looked up in the catalog, wherever it shows:
// the payout card, the Key screen's three steps, both claim tiles, the claim
// sheet and Change wallet's key offer. Only the lookup names its keys.
UR_TEST(BittensorKeyWiring_TheDisclaimerIsTheSdks) {
  for (const std::string& source : AppSources()) {
    if (source == "BittensorKeyText.cpp") continue;
    const std::string code = ReadCode(source);
    UR_EXPECT_TRUE_MSG(source + " names a disclaimer key",
                       !Contains(code, "\"bittensor_funds_disclaimer"));
  }
  const std::string text = ReadCode("BittensorKeyText.cpp");
  UR_EXPECT_TRUE(Contains(text, "if (disclaimer == urnet::BittensorFundsDisclaimerFull) {"));
  UR_EXPECT_TRUE(Contains(text, "if (disclaimer == urnet::BittensorFundsDisclaimerFundsOnly) {"));
  const std::string page = ReadCode("EarningsPage.cpp");
  UR_EXPECT_TRUE(Contains(page, "SetBittensorFundsDisclaimer(*consentDisclaimer_,\n"
                                "                              keys_->HasFlow() ? keys_->Card().funds_disclaimer : std::string());"));
  UR_EXPECT_TRUE(Contains(page, "SetBittensorFundsDisclaimer(*unclaimedDisclaimer_, keys_->Card().claim_funds_disclaimer);"));
  UR_EXPECT_TRUE(Contains(page, ": keys_->Card().claim_funds_disclaimer);"));
  UR_EXPECT_TRUE(Contains(page, "keys_->FundsDisclaimerAt(urnet::BittensorFundsDisclaimerPlaceChangeWallet)"));
  UR_EXPECT_TRUE(Contains(page, "keys_->FundsDisclaimerAt(urnet::BittensorFundsDisclaimerPlaceClaimSheet)"));
  const std::string screen = ReadCode("BittensorKeyScreen.cpp");
  UR_EXPECT_EQ(3u, CountOf(screen, ", screen.funds_disclaimer);"));
}

// The Key screen: the words hide when the window loses the focus, are plain
// labels only while revealed, and the two words are typed into fields that
// learn nothing. A stored key asks for the user's password before it shows
// (Reveal) and at Delete's button, never at Keep.
UR_TEST(BittensorKeyWiring_TheKeyScreensPrivacy) {
  const std::string screen = ReadCode("BittensorKeyScreen.cpp");
  UR_EXPECT_TRUE(!screen.empty());
  UR_EXPECT_TRUE(Contains(screen, "property_is_active().signal_changed().connect([this] {\n"
                                  "    if (!is_active()) {\n      host_.Hide();"));
  UR_EXPECT_TRUE(Contains(screen, "Gtk::InputHints::NO_SPELLCHECK | Gtk::InputHints::NO_EMOJI |\n"
                                  "                           Gtk::InputHints::PRIVATE"));
  UR_EXPECT_TRUE(Contains(screen, "entry->property_enable_emoji_completion() = false;"));
  UR_EXPECT_TRUE(Contains(screen, "word->set_selectable(false);"));
  UR_EXPECT_FALSE(Contains(screen, "set_selectable(true)"));
  // the words are in the tree only while they show
  const std::string main = FunctionBody(screen, "void BittensorKeyScreen::ApplyMain(");
  UR_EXPECT_TRUE(Before(main, "while (Gtk::Widget* child = wordsGrid_->get_first_child())",
                        "if (screen.revealed) {"));
  // Reveal: the password first for a stored key, and the words only in a focused window
  const std::string reveal = FunctionBody(screen, "void BittensorKeyScreen::OnReveal()");
  UR_EXPECT_TRUE(Contains(reveal, "if (!screen.needs_authentication) {\n    host_.Reveal(false);"));
  UR_EXPECT_TRUE(Before(reveal, "AuthenticateForBittensorKey(", "host_.Reveal(true);"));
  UR_EXPECT_TRUE(Contains(reveal, "if (is_active()) {"));
  // Delete's button: the password after the words, and nothing without it
  const std::string submit = FunctionBody(screen, "void BittensorKeyScreen::OnSubmit()");
  UR_EXPECT_TRUE(Contains(submit, "if (!host_.CheckWords(first, second).ready) return;"));
  UR_EXPECT_TRUE(Contains(submit, "if (!screen.confirm_needs_authentication) {\n"
                                  "    host_.SubmitWords(first, second, false);"));
  UR_EXPECT_TRUE(Contains(submit, "if (authenticated) host_.SubmitWords(first, second, true);"));
  // what deleting means hides the words first, and replaces the screen
  UR_EXPECT_TRUE(Contains(screen, "host_.Hide();\n    deleteExplained_ = true;"));
  // a signing on its way is not abandoned
  UR_EXPECT_TRUE(Contains(FunctionBody(screen, "bool BittensorKeyScreen::OnCloseRequest()"),
                          "if (host_.Screen().busy) return true;"));
}

// polkit's check: the GUI's own bus name as the subject, the details empty,
// interaction allowed, the policy's own action.
UR_TEST(BittensorKeyWiring_TheDeviceAuthenticationIsPolkit) {
  const std::string auth = ReadCode("BittensorKeyAuth.cpp");
  UR_EXPECT_TRUE(Contains(auth, "\"CheckAuthorization\""));
  UR_EXPECT_TRUE(Contains(auth, "\"system-bus-name\""));
  UR_EXPECT_TRUE(Contains(auth, "g_dbus_connection_get_unique_name(call->bus)"));
  UR_EXPECT_TRUE(Contains(auth, "bittensor_key::kAuthAction"));
  UR_EXPECT_TRUE(Contains(auth, "constexpr guint32 kAllowUserInteraction = 1;"));
  // nothing is ever added to the details
  UR_EXPECT_TRUE(Contains(auth, "g_variant_builder_init(&details, G_VARIANT_TYPE(\"a{ss}\"));"));
  UR_EXPECT_FALSE(Contains(auth, "g_variant_builder_add(&details"));
  UR_EXPECT_TRUE(Contains(auth, "bittensor_key::CountsAsAuthenticated(outcome)"));
  const std::string policy = ReadRaw("../../packaging/polkit/com.bringyour.network.policy");
  const size_t action = policy.find("<action id=\"com.bringyour.network.bittensor-key\">");
  UR_EXPECT_TRUE(action != std::string::npos);
  if (action == std::string::npos) return;
  const std::string defaults = policy.substr(action, policy.find("</action>", action) - action);
  UR_EXPECT_TRUE(Contains(defaults, "<allow_active>auth_self</allow_active>"));
  UR_EXPECT_TRUE(Contains(defaults, "<allow_inactive>no</allow_inactive>"));
  UR_EXPECT_TRUE(Contains(defaults, "<allow_any>no</allow_any>"));
  const std::string flatpak = ReadRaw("../../packaging/flatpak/com.bringyour.network.yml");
  UR_EXPECT_TRUE(Contains(flatpak, "  - --system-talk-name=org.freedesktop.PolicyKit1\n"));
}

// Copy: only while the words show (the flow's PhraseToCopy), marked secret for
// the clipboard managers that honor it, and taken back after the SDK's 60
// seconds if the clipboard still holds that copy.
UR_TEST(BittensorKeyWiring_TheClipboardIsClearedAfterTheSdksSeconds) {
  const std::string host = ReadCode("BittensorKeyHost.cpp");
  const std::string copy = FunctionBody(host, "bool BittensorKeyHost::CopyPhrase()");
  UR_EXPECT_TRUE(Contains(copy, "const std::string phrase = box_->flow.phraseToCopy();"));
  UR_EXPECT_TRUE(Contains(copy, "if (phrase.empty() || !display) return false;"));
  UR_EXPECT_TRUE(Contains(copy, "kSecretHintMime"));
  UR_EXPECT_TRUE(Contains(copy, "static_cast<unsigned>(urnet::BittensorLocalKeyCopySeconds)"));
  UR_EXPECT_TRUE(Contains(copy, "if (held && held->gobj() == copied_->gobj()) clipboard->unset_content();"));
  UR_EXPECT_TRUE(Contains(host, "constexpr const char* kSecretHintMime = \"x-kde-passwordManagerHint\";"));
}

// The one notification: its default action opens the Key screen, the app
// registers that action, and the window opens Earnings on it.
UR_TEST(BittensorKeyWiring_TheReminderOpensTheKeyScreen) {
  const std::string host = ReadCode("BittensorKeyHost.cpp");
  const std::string post = FunctionBody(host, "bool BittensorKeyHost::PostReminder()");
  UR_EXPECT_TRUE(Contains(post, "T_(\"bittensor_key_reminder_title_2\""));
  UR_EXPECT_TRUE(Contains(post, "T_(\"bittensor_key_reminder_body_2\""));
  UR_EXPECT_TRUE(Contains(post, "notification->set_default_action(std::string(\"app.\") + kReminderAction);"));
  // only a posted reminder is reported
  UR_EXPECT_TRUE(Contains(FunctionBody(host, "void BittensorKeyHost::ApplyReminder("),
                          "if (PostReminder()) box_->flow.reminderDelivered();"));
  UR_EXPECT_FALSE(Contains(host, "reminderScheduled("));
  const std::string main = ReadCode("main.cpp");
  UR_EXPECT_TRUE(Contains(main, "app->add_action(urnw::BittensorKeyHost::kReminderAction, [&] {\n"
                                "      if (window) window->OpenEarningsKeyScreen();"));
  const std::string window = ReadCode("MainWindow.cpp");
  const std::string open = FunctionBody(window, "void MainWindow::OpenEarningsKeyScreen()");
  UR_EXPECT_TRUE(Before(open, "shell_->Navigate(\"earnings\");",
                        "earningsPage_->OpenBittensorKeyScreen();"));
  UR_EXPECT_TRUE(Contains(window, "accountPage_->on_back_up_bittensor_key = [this] { OpenEarningsKeyScreen(); };"));
}

// Sign out and Delete account warn while the key isn't backed up (3e to 3g),
// and the deleted network's key is forgotten before the sign-out; this
// session's sign-out on the Sessions page says the same.
UR_TEST(BittensorKeyWiring_SignOutAndDeleteAccountWarn) {
  const std::string account = ReadCode("AccountPage.cpp");
  UR_EXPECT_TRUE(Contains(account, "row.root->signal_clicked().connect([this] { RequestSignOut(); });"));
  const std::string signOut = FunctionBody(account, "void AccountPage::RequestSignOut()");
  UR_EXPECT_TRUE(Contains(signOut, "BittensorKeyNeedsBackup(networkId,"));
  UR_EXPECT_TRUE(Contains(signOut, "if (!needsBackup) {\n      host_.Logout();"));
  UR_EXPECT_TRUE(Contains(signOut, "ShowKeySignOutWarning();"));
  const std::string warning = FunctionBody(account, "void AccountPage::ShowKeySignOutWarning()");
  for (const char* drawn : {"T_(\"bittensor_key_sign_out_title_2\"", "T_(\"bittensor_key_sign_out_message_2\"",
                            "T_(\"bittensor_key_back_up_first\"", "T_(\"sign_out\", \"Sign out\")",
                            "ADW_RESPONSE_DESTRUCTIVE", "ctx->self->host_.Logout();",
                            "ctx->self->on_back_up_bittensor_key();"}) {
    UR_EXPECT_TRUE_MSG(drawn, Contains(warning, drawn));
  }
  const std::string deletion = FunctionBody(account, "void AccountPage::ShowDeleteAccountSheet()");
  UR_EXPECT_TRUE(Before(deletion, "BittensorKeyForgetNetwork(deleteSheet_->DeletedNetworkId(), nullptr);",
                        "host_.LogoutIfStill(signIn);"));
  UR_EXPECT_TRUE(Contains(deletion, "deleteSheet_->Open(needsBackup);"));
  UR_EXPECT_TRUE(Contains(account, "T_(\"bittensor_key_delete_account_message_2\""));
  const std::string sessions = ReadCode("SessionsPage.cpp");
  UR_EXPECT_TRUE(Contains(FunctionBody(sessions, "void SessionsPage::PressSignOut("),
                          "BittensorKeyNeedsBackup(networkId,"));
  UR_EXPECT_TRUE(Contains(FunctionBody(sessions, "void SessionsPage::ShowConfirmation("),
                          "if (confirmation.self && keyNeedsBackup) {"));
}

// The payout card: the one tap, and every key action that replaces a payout
// wallet warns first (3c); Change wallet offers the key first (3b); the card
// leads Earnings while the flow says it needs action; the Key screen follows
// the flow's screen; the claim relay asks the SDK, whose claim status decides
// the tile and URnetwork's claim (4d).
UR_TEST(BittensorKeyWiring_TheEarningsPageDrawsTheFlow) {
  const std::string page = ReadCode("EarningsPage.cpp");
  const std::string request = FunctionBody(page, "void EarningsPage::RequestKeyAction(");
  UR_EXPECT_TRUE(Before(request, "if (keys_->Card().replaces_payout_wallet) {", "RunKeyAction(action, false);"));
  const std::string warning = FunctionBody(page, "void EarningsPage::ShowReplaceWarning(");
  UR_EXPECT_TRUE(Contains(warning, "T_(\"bittensor_key_replace_title\""));
  UR_EXPECT_TRUE(Contains(warning, "T_(\"bittensor_key_replace_message_2\""));
  UR_EXPECT_TRUE(Contains(warning, "ADW_RESPONSE_DESTRUCTIVE"));
  UR_EXPECT_TRUE(Contains(warning, "adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), \"cancel\");"));
  UR_EXPECT_TRUE(Contains(FunctionBody(ReadRaw("EarningsPage.cpp"), "void EarningsPage::ShowReplaceWarning("),
                          "ctx->self->RunKeyAction(ctx->action, /*replaceConfirmed=*/true);"));
  // Change wallet with a key offer only where the flow offers one
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::OnConsentAction()"),
                          "if (consent_.phase() == consent::Phase::Active && keys_->Card().offers_set_up) {"));
  // the card leads while the flow says it needs action
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::PlaceConsentBlock()"),
                          "const bool lead = keys_->HasFlow() && keys_->Card().shows_key_path;"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::RebuildKeyBanner()"),
                          "keys_->Card().backup_reminder"));
  // the points card never asks for a coldkey the key here is paid to: the
  // network has one when a wallet is read or the SDK says the consent pays the
  // key on this device
  const std::string payoutLine = FunctionBody(page, "void EarningsPage::RebuildPayoutLine()");
  UR_EXPECT_TRUE(Contains(payoutLine, "const bool paysKeyHere = keys_->Card().pays_key_on_this_device;"));
  UR_EXPECT_TRUE(Contains(payoutLine, "wallet_.has_value() || paysKeyHere,"));
  UR_EXPECT_TRUE(!Contains(payoutLine, "BittensorLocalKeyStateSaveLaterLinked"));
  // the Key screen follows the flow's screen, one modal at a time
  const std::string screen = FunctionBody(page, "void EarningsPage::ApplyKeyScreen()");
  UR_EXPECT_TRUE(Contains(screen, "if (!screen.open) {"));
  UR_EXPECT_TRUE(Contains(screen, "keys_->CloseScreen();"));
  // the flow is told the consent only when it changed, and every visit reads the store again
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::FeedKeyConsent()"),
                          "if (keyConsentFed_ && *keyConsentFed_ == fed) return;"));
  UR_EXPECT_TRUE(Before(FunctionBody(page, "void EarningsPage::LoadEarnings()"), "consent_.Load();",
                        "VisitKeys();"));
  // claims through the relay: the SDK's status and Claim now, on the device
  UR_EXPECT_TRUE(Contains(page, "host.device().snClaimStatus("));
  UR_EXPECT_TRUE(Contains(page, "host.device().snClaimNow(coldkeySs58,"));
  const std::string tile = FunctionBody(page, "void EarningsPage::RebuildLedgerTile()");
  UR_EXPECT_TRUE(Contains(tile, "AppendClaimRelaySection("));
  UR_EXPECT_TRUE(Contains(tile, "ledgerClaimButton_->set_visible(!relay && view.showClaim);"));
  // with no wallet here, the SDK's ShowsTile decides the tile, never a sum of
  // balances; the relay's part stays while URnetwork's claim is the latest
  const std::string relayOnly = FunctionBody(page, "bool EarningsPage::LedgerTileShowsRelayOnly() const");
  UR_EXPECT_TRUE(Contains(relayOnly, "claimStatus_->shows_tile;"));
  UR_EXPECT_TRUE(!Contains(relayOnly, "claimable_rao") && !Contains(relayOnly, "pending_rao"));
  UR_EXPECT_TRUE(Contains(tile, "|| claimStatus_->shows_claimed);"));
  // URnetwork's claim (4d), as the SDK names it: the claim and its amount, the
  // coldkey, the transaction's explorer page, and the date Claim now is back
  const std::string relay = ReadCode("ClaimRelay.cpp");
  const std::string banner = FunctionBody(relay, "void AppendClaimedBanner(");
  UR_EXPECT_TRUE(Contains(banner, "status.claim_amount_rao.value_or(0)"));
  UR_EXPECT_TRUE(Contains(banner, "status.coldkey_ss58.value_or(std::string())"));
  UR_EXPECT_TRUE(Contains(banner, "ClaimTransactionLink(status)"));
  // the store's one claimed-state sentence on every app, the amount without its
  // symbol, which the sentence names; Claimed alone without an amount or a coldkey
  UR_EXPECT_TRUE(Contains(banner, "T_(\"sn_claim_relay_claimed\","));
  UR_EXPECT_TRUE(Contains(banner, "urnet::formatAlphaAmount(amountRao), urnet::shortSs58(coldkey)"));
  UR_EXPECT_TRUE(!Contains(banner, "urnet::formatAlpha(amountRao)"));
  UR_EXPECT_TRUE(Contains(banner, "T_(\"claim_confirmed\", \"Claimed\")"));
  UR_EXPECT_TRUE(Contains(FunctionBody(relay, "std::string ClaimTransactionLink("),
                          "snledger::TransactionLink(status.claim_tx_url.value_or(std::string()))"));
  UR_EXPECT_TRUE(Contains(FunctionBody(relay, "Glib::ustring ClaimedAgainNote("),
                          "status.claim_now_next_at_millis.value_or(0)"));
  const std::string section = FunctionBody(relay, "void AppendClaimRelaySection(");
  UR_EXPECT_TRUE(Contains(section, "if (status.shows_claimed) AppendClaimedBanner(into, status, openLink);"));
  const std::string sheet = FunctionBody(relay, "void ClaimRelaySheet::Apply(");
  UR_EXPECT_TRUE(Contains(sheet, "const bool claimed = status && !status->error && status->shows_claimed;"));
  UR_EXPECT_TRUE(Before(sheet, "if (claimed) {", "switch (outcome.kind) {"));
  UR_EXPECT_TRUE(Contains(sheet, "T_(\"done\", \"Done\")"));
  // the claim spends a Claim now tap's answer
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void EarningsPage::ApplyClaimStatus("),
                          "claimNowOutcome_ = ClaimNowOutcome{};"));
}

// The new sources are built into the GUI and listed for xgettext.
UR_TEST(BittensorKeyWiring_TheSourcesAreBuiltAndListed) {
  const std::string meson = ReadRaw("../meson.build");
  const std::string potfiles = ReadRaw("../po/POTFILES.in");
  for (const char* source : {"BittensorKeyStore.cpp", "BittensorKeyAuth.cpp", "BittensorKeyText.cpp",
                             "BittensorKeyHost.cpp", "BittensorKeyScreen.cpp", "ClaimRelay.cpp"}) {
    UR_EXPECT_TRUE_MSG(source, Contains(meson, std::string("'src/") + source + "',"));
  }
  for (const char* source :
       {"BittensorKeyText.cpp", "BittensorKeyHost.cpp", "BittensorKeyScreen.cpp", "ClaimRelay.cpp"}) {
    UR_EXPECT_TRUE_MSG(source, Contains(potfiles, std::string("src/") + source + "\n"));
  }
}
