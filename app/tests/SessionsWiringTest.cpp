// Account -> Sessions as wired into the app (REVOKE-UI-FINAL.md §1, §4, §6,
// §7's Linux row): the Account row right after the profile, the window's
// navigation like the Refer and earn page's, the rail keeping Account lit,
// the controller opened on the GUI's Api with its listener marshaled to the
// main loop and released unsubscribe-first, visibility from the page's map
// and foreground from the window, and every sign-out confirmed with Cancel
// as the default before the controller is asked. The decisions themselves are
// tested in SessionsPresentationTest and SessionsBindingTest; these pages
// need GTK and the SDK, so this reads their sources.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "SessionsPresentation.hpp"
#include "ShellLayout.hpp"
#include "WiringSource.hpp"

using urnw::testing::wiring::Before;
using urnw::testing::wiring::Contains;
using urnw::testing::wiring::FunctionBody;
using urnw::testing::wiring::ReadCode;

UR_TEST(SessionsWiring_TheAccountRowFollowsTheProfile) {
  const std::string page = ReadCode("AccountPage.cpp");
  const std::string profile = FunctionBody(page, "void AccountPage::BuildProfileGroup(");
  const std::string row =
      "kit::MakePaneTwoLineRowButton(T_(\"sessions_title\", \"Sessions\"), {}, kRowTall,\n"
      "                                             RowIcon(MdiGlyph::SessionFace));";
  UR_EXPECT_TRUE_MSG("the Sessions row with the face profile glyph", Contains(profile, row));
  // right after Update password (the one row built between them is its own),
  // and the profile group comes first on the pane; the API keys row follows
  // it (ApiKeysWiringTest)
  const size_t update = profile.find("T_(\"update_password\", \"Update password\")");
  const size_t sessions = profile.find("T_(\"sessions_title\", \"Sessions\")");
  UR_EXPECT_TRUE(update != std::string::npos && sessions != std::string::npos && update < sessions);
  if (update != std::string::npos && sessions != std::string::npos && update < sessions) {
    UR_EXPECT_EQ(1u, urnw::testing::wiring::CountOf(profile.substr(update, sessions - update),
                                                    "kit::Make"));
  }
  const std::string pane = FunctionBody(page, "void AccountPage::BuildAccountPane()");
  UR_EXPECT_TRUE(Before(pane, "BuildProfileGroup(", "BuildSecurityGroup("));
  UR_EXPECT_TRUE(Contains(profile, "if (on_open_sessions) {"));
  UR_EXPECT_TRUE(Contains(ReadCode("AccountPage.hpp"), "std::function<void()> on_open_sessions;"));
}

UR_TEST(SessionsWiring_TheWindowNavigatesLikeReferrals) {
  const std::string window = ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(Contains(window, "sessionsPage_ = Gtk::make_managed<SessionsPage>(host_);"));
  UR_EXPECT_TRUE(Contains(window, "shell_->SetPage(\"sessions\", *sessionsPage_);"));
  UR_EXPECT_TRUE(Contains(window, "accountPage_->on_open_sessions = [this] {\n"
                                  "    if (shell_) shell_->Navigate(\"sessions\");"));
  UR_EXPECT_TRUE(Contains(window, "sessionsPage_->on_back = [this] {\n"
                                  "    if (shell_) shell_->Navigate(\"account\");"));
  // loads on open, through the shell's navigation (and again on a sign-in
  // while it is on screen)
  UR_EXPECT_TRUE(Contains(window, "if (tag == \"sessions\" && sessionsPage_) sessionsPage_->Load();"));
  // the window's presentation is the controller's foreground
  UR_EXPECT_TRUE(Contains(window, "if (sessionsPage_) sessionsPage_->SetPresentationActive(windowVisible_);"));
  // a sign-out releases the departed account's controller
  const std::string auth = FunctionBody(window, "void MainWindow::ApplyAuthState(bool loggedIn)");
  UR_EXPECT_TRUE(Contains(auth, "if (sessionsPage_) sessionsPage_->ResetForSignOut();"));
  // the window's one-modal gate
  UR_EXPECT_TRUE(Contains(window, "sessionsPage_->sheet_open = [this] { return sheetOpen_; };"));
  UR_EXPECT_TRUE(Contains(window, "if (sessionsPage_) sessionsPage_->SetPreviewMode(true);"));
}

UR_TEST(SessionsWiring_TheRailKeepsAccountLit) {
  UR_EXPECT_TRUE(urnw::shell::RailTagFor("sessions") == "account");
  UR_EXPECT_TRUE(urnw::shell::RailTagFor("referrals") == "account");
  UR_EXPECT_TRUE(urnw::shell::RailTagFor("account") == "account");
  UR_EXPECT_TRUE(urnw::shell::RailTagFor("network") == "network");
  UR_EXPECT_TRUE(Contains(FunctionBody(ReadCode("HomeShell.cpp"), "void HomeShell::PaintSelection()"),
                          "shell::RailTagFor(currentTag_)"));
}

// §7: opened from host_.api() in the GUI process, once per Api; the listener
// only posts, and the post renders only for the live page and the controller
// attached now.
UR_TEST(SessionsWiring_TheControllerIsTheGuiApisAndItsListenerOnlyPosts) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string ensure = FunctionBody(page, "void SessionsPage::EnsureController()");
  UR_EXPECT_TRUE(Contains(ensure, "const uint64_t source = host_.api().handle();"));
  UR_EXPECT_TRUE(Contains(ensure, "if (binding_.Attached() && binding_.Source() == source) return;"));
  UR_EXPECT_TRUE(Contains(ensure, "host_.api().openClientSessionViewController()"));
  UR_EXPECT_TRUE(Before(ensure, "const uint64_t generation = binding_.NextGeneration();",
                        "binding_.Attach("));
  UR_EXPECT_TRUE(Contains(ensure, "PostToMain([this, alive, generation] {\n"
                                  "                if (!*alive || !binding_.Accepts(generation)) return;\n"
                                  "                ReadSnapshot();"));
  // the listener is subscribed before the binding starts the controller, and
  // dropped before it is closed
  UR_EXPECT_TRUE(Contains(page, "sub_(controller_.addClientSessionListener(std::move(listener)))"));
  UR_EXPECT_TRUE(Contains(page, "void Start() override { controller_.start(); }"));
  UR_EXPECT_TRUE(Contains(page, "void Unsubscribe() override { sub_.reset(); }"));
  UR_EXPECT_TRUE(Contains(page, "void Close() override { controller_.close(); }"));
  const std::string binding = ReadCode("SessionsBinding.hpp");
  UR_EXPECT_TRUE(Before(FunctionBody(binding, "void Release()"), "controller->Unsubscribe();",
                        "controller->Close();"));
}

UR_TEST(SessionsWiring_VisibilityIsTheMapAndForegroundTheWindow) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string constructor = FunctionBody(page, "SessionsPage::SessionsPage(SdkHost& host)");
  UR_EXPECT_TRUE(Contains(constructor, "signal_map().connect([this] { binding_.SetVisible(true); });"));
  UR_EXPECT_TRUE(Contains(constructor, "signal_unmap().connect([this] { binding_.SetVisible(false); });"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::SetPresentationActive(bool active)"),
                          "binding_.SetForeground(active);"));
}

UR_TEST(SessionsWiring_RefreshAndTryAgainAskTheController) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string build = FunctionBody(page, "void SessionsPage::BuildPane()");
  UR_EXPECT_TRUE(Contains(build, "refreshButton_->add_css_class(\"ur-pane-action\");"));
  UR_EXPECT_TRUE(Contains(build, "refreshButton_->signal_clicked().connect([this] { binding_.Refresh(); });"));
  UR_EXPECT_TRUE(Contains(build, "kit::SetAccessibleLabel(*refreshButton_, Lookup(sessions::kRefreshText));"));
  UR_EXPECT_TRUE(Contains(build, "retryButton_->signal_clicked().connect([this] { Retry(); });"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::Retry()"), "binding_.Refresh();"));
}

// §4: the row's trailing Sign out is flat and destructive and only opens the
// confirmation; the confirmation's default is Cancel; only its Sign out sends.
UR_TEST(SessionsWiring_EverySignOutIsConfirmedFirst) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string row = FunctionBody(page, "SessionsPage::RowWidgets SessionsPage::BuildRow(");
  UR_EXPECT_TRUE(Contains(row, "row.signOut->add_css_class(\"flat\");"));
  UR_EXPECT_TRUE(Contains(row, "row.signOut->add_css_class(\"destructive-action\");"));
  UR_EXPECT_TRUE(Contains(row, "[this, sessionId] { PressSignOut(sessions::Target{false, sessionId}); }"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::BuildPane()"),
                          "[this] { PressSignOut(sessions::Target{true, std::string()}); }"));
  const std::string press = FunctionBody(page, "void SessionsPage::PressSignOut(");
  UR_EXPECT_TRUE(Before(press, "flow_.Press(snapshot_, target, MakeWords())", "ShowConfirmation("));
  UR_EXPECT_FALSE(Contains(press, "binding_.Revoke("));
  const std::string confirm = FunctionBody(page, "void SessionsPage::ShowConfirmation(");
  UR_EXPECT_TRUE(Contains(confirm, "cancel->set_receives_default(true);"));
  UR_EXPECT_TRUE(Contains(confirm, "confirmDialog_->set_default_widget(*cancel);"));
  UR_EXPECT_TRUE(Contains(confirm, "confirmDialog_->set_modal(true);"));
  UR_EXPECT_TRUE(Contains(confirm, "AddEscapeToClose(*confirmDialog_);"));
  UR_EXPECT_TRUE(Before(confirm, "const std::optional<sessions::Target> target = flow_.Confirm();",
                        "if (target) binding_.Revoke(*target);"));
  // closed any other way: nothing sent, the gate closed, the focus restored
  UR_EXPECT_TRUE(Contains(confirm, "flow_.Cancel();\n        EndSheet();\n        RestoreFocus(target);"));
  // the row names its action for a screen reader
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::UpdateRow("),
                          "kit::SetAccessibleLabel(*row.signOut, text.signOutName);"));
}

UR_TEST(SessionsWiring_SignOutAndDestructionReleaseTheController) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string reset = FunctionBody(page, "void SessionsPage::ResetForSignOut()");
  UR_EXPECT_TRUE(Before(reset, "binding_.Release();", "Render();"));
  UR_EXPECT_TRUE(Contains(reset, "flow_.Cancel();"));
  const std::string destructor = FunctionBody(page, "SessionsPage::~SessionsPage()");
  UR_EXPECT_TRUE(Before(destructor, "*alive_ = false;", "binding_.Release();"));
  // the destructor touches no widget (EarningsPage, issue #13)
  for (const char* widgetCall : {"Render(", "rowsBox_", "->set_", "remove("}) {
    UR_EXPECT_TRUE_MSG(widgetCall, !Contains(destructor, widgetCall));
  }
  // signed out, no controller and no request
  const std::string load = FunctionBody(page, "void SessionsPage::Load()");
  UR_EXPECT_TRUE(Before(load, "if (!CanCallApi()) {", "EnsureController();"));
  // a controller that did not open is a failed load (Try again), not a
  // spinner forever
  UR_EXPECT_TRUE(Before(load, "EnsureController();", "if (!binding_.Attached()) {"));
  // (the reader blanks the /*retryable=*/ comment)
  UR_EXPECT_TRUE(Contains(load, "snapshot_.error = sessions::Error{"));
  UR_EXPECT_TRUE(Contains(load, "true, false, false, false};"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::Retry()"),
                          "if (!binding_.Attached()) {\n    Load();"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "bool SessionsPage::CanCallApi()"),
                          "return !previewMode_ && host_.IsLoggedIn();"));
}

UR_TEST(SessionsWiring_CopyCopiesTheFullId) {
  const std::string page = ReadCode("SessionsPage.cpp");
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::CopySessionId("),
                          "get_clipboard()->set_text(sessionId);"));
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "SessionsPage::RowWidgets SessionsPage::BuildRow("),
                          "copy->signal_clicked().connect([this, sessionId] { CopySessionId(sessionId); });"));
}

// The C ABI's times are Unix milliseconds; SessionLastUsed.UnixTime is seconds.
UR_TEST(SessionsWiring_TheSdkSnapshotIsCopiedInItsUnits) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string copy = FunctionBody(page, "sessions::Snapshot SnapshotOf(");
  UR_EXPECT_TRUE(Contains(copy, "session.createUnixSeconds = info.getCreateTime() / 1000;"));
  UR_EXPECT_TRUE(Contains(copy, "use.unixSeconds = used.getUnixTime();"));
  UR_EXPECT_TRUE(Contains(copy, "use.countryCode = used.getCountryCode();"));
  // the colour comes from the sdk's palette
  UR_EXPECT_TRUE(Contains(page, "return urnet::getColorHex(code);"));
}

// The circle's fallback is the colour the app already gives an unknown
// country (the provider locations sheet and globe, #0099FF).
UR_TEST(SessionsWiring_TheUnknownCountryColourIsTheApps) {
  UR_EXPECT_TRUE(std::string(urnw::sessions::kUnknownCountryColorHex) == "#0099FF");
  UR_EXPECT_TRUE(Contains(ReadCode("ProviderLocationsSheet.cpp"),
                          "constexpr Rgba kUnknownCountry{0x00 / 255.0, 0x99 / 255.0, 0xFF / 255.0, 1.0};"));
}

// The line under each sign-out's control is the presentation's: "Signing
// out…", then the row's failure, or under Sign out all other sessions its own
// ("Couldn't sign out the other sessions. Try again."); a failed control stays
// on for another press, which asks the controller again.
UR_TEST(SessionsWiring_TheStatusLinesAreThePresentations) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string others = FunctionBody(page, "void SessionsPage::RenderOthers(");
  // (the reader blanks the /*others=*/ comments)
  UR_EXPECT_TRUE(Contains(others, "sessions::ActionStatusText(screen.signOutOthersView, "));
  UR_EXPECT_TRUE(Contains(others, "true);"));
  UR_EXPECT_TRUE(Contains(others, "othersStatus_->set_visible(status.has_value());"));
  UR_EXPECT_TRUE(Contains(others, "if (status) SetToned(*othersStatusText_, busy ? kUrTextMuted : kUrDanger, Lookup(*status));"));
  UR_EXPECT_TRUE(Contains(others, "othersButton_->set_sensitive(!busy);"));
  for (const char* text : {"kOthersFailedText", "kSigningOutText", "kActionFailedText",
                           "something_went_wrong"}) {
    UR_EXPECT_TRUE_MSG(text, !Contains(others, text));
  }
  const std::string row = FunctionBody(page, "void SessionsPage::UpdateRow(");
  UR_EXPECT_TRUE(Contains(row, "sessions::ActionStatusText(view, "));
  UR_EXPECT_TRUE(Contains(row, "row.status->set_visible(status.has_value());"));
  UR_EXPECT_TRUE(Contains(row, "row.signOut->set_sensitive(!busy);"));
  // the bulk press is refused only while it runs (SignOutFlow), never after a
  // failure
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::PressSignOut("),
                          "if (!CanCallApi() || !binding_.Attached()) return;"));
}

// The sdk's trusted session-revoked cause is copied with the other flags.
UR_TEST(SessionsWiring_TheRevokedCauseIsTheSdks) {
  const std::string copy = FunctionBody(ReadCode("SessionsPage.cpp"), "sessions::Error ErrorOf(");
  UR_EXPECT_TRUE(Contains(copy, "out.signInRequired = error.getSignInRequired();"));
  UR_EXPECT_TRUE(Contains(copy, "out.sessionRevoked = error.getSessionRevoked();"));
}

// ---- the list at scale (session1118 C.md §5 C) ----------------------------------

// The controller's layout is copied as it is: the counts, the pages, the
// truncation and each group with its rows, by the same per-session copy as
// the device sign-ins; the section names are the sdk's, or the GUI does not
// build.
UR_TEST(SessionsWiring_TheControllersLayoutIsCopied) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string copy = FunctionBody(page, "sessions::Snapshot SnapshotOf(");
  for (const char* field :
       {"out.sessions = sessionsOf(source.getSessions());",
        "out.sessionCount = source.getSessionCount();",
        "out.sessionsHasMore = source.getSessionsHasMore();",
        "out.otherSessionCount = source.getOtherSessionCount();",
        "out.truncated = source.getTruncated();", "source.getGroups()",
        "copy.kind = group.getKind();", "copy.count = group.getCount();",
        "copy.expanded = group.getExpanded();", "copy.hasMore = group.getHasMore();",
        "copy.sessions = sessionsOf(group.getSessions());"}) {
    UR_EXPECT_TRUE_MSG(field, Contains(copy, field));
  }
  for (const char* kind : {"SignIns", "HostedProxies", "ApiClients", "PairedDevices", "OtherClients"}) {
    const std::string assertion = std::string("static_assert(std::string_view(sessions::kGroup") +
                                  kind + ") ==\n                  std::string_view(urnet::ClientSessionGroup" +
                                  kind + "),";
    UR_EXPECT_TRUE_MSG(assertion, Contains(page, assertion));
  }
  // the controller's calls, and nothing of the page's own
  UR_EXPECT_TRUE(Contains(page, "controller_.setGroupExpanded(groupKind, expanded);"));
  UR_EXPECT_TRUE(Contains(page, "controller_.showMoreSessions(section);"));
  const std::string binding = ReadCode("SessionsBinding.hpp");
  UR_EXPECT_TRUE(Contains(
      FunctionBody(binding, "void SetGroupExpanded(const std::string& groupKind, bool expanded) {"),
      "if (controller_) controller_->SetGroupExpanded(groupKind, expanded);"));
  UR_EXPECT_TRUE(Contains(FunctionBody(binding, "void ShowMore(const std::string& section) {"),
                          "if (controller_) controller_->ShowMoreSessions(section);"));
}

// A group's heading is a keyboard-focusable button named by its heading, whose
// accessible state says whether it is open; activating it asks the controller
// to open or close it by its latest snapshot. Its rows are the sign-ins' rows,
// with the group's label; Show more asks the controller for the next page.
UR_TEST(SessionsWiring_AGroupOpensFromItsHeading) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string build = FunctionBody(page, "SessionsPage::GroupWidgets SessionsPage::BuildGroup(");
  UR_EXPECT_TRUE(Contains(build, "group.header = Gtk::make_managed<Gtk::Button>();"));
  UR_EXPECT_TRUE(Contains(build, "group.header->add_css_class(\"ur-pane-row\");"));
  UR_EXPECT_TRUE(Contains(build, "kit::MarkDecorative(*group.chevron);"));
  UR_EXPECT_TRUE(Contains(build, "group.header->signal_clicked().connect([this, kind] { ToggleGroup(kind); });"));
  UR_EXPECT_TRUE(Contains(build, "MakeShowMoreRow([this, kind] { binding_.ShowMore(kind); });"));
  const std::string update = FunctionBody(page, "void SessionsPage::UpdateGroup(");
  UR_EXPECT_TRUE(Contains(update, "group.heading->set_text(view.heading);"));
  UR_EXPECT_TRUE(Contains(update, "kit::SetAccessibleLabel(*group.header, view.heading);"));
  UR_EXPECT_TRUE(Contains(update, "GTK_ACCESSIBLE_STATE_EXPANDED"));
  UR_EXPECT_TRUE(Contains(update, "view.expanded ? TRUE : FALSE"));
  UR_EXPECT_TRUE(Contains(update, "RenderRows(*group.rowsBox, group.rows, listed, view.kind, words, now);"));
  UR_EXPECT_TRUE(Contains(update, "group.rowsBox->set_visible(view.expanded);"));
  UR_EXPECT_TRUE(Contains(update, "group.moreRow->set_visible(view.showMore);"));
  const std::string toggle = FunctionBody(page, "void SessionsPage::ToggleGroup(");
  UR_EXPECT_TRUE(Before(toggle, "sessions::ExpandedAfterToggle(snapshot_, kind)",
                        "binding_.SetGroupExpanded(kind, *expanded);"));
  // the rows of every section are built and named by the presentation
  const std::string rows = FunctionBody(page, "void SessionsPage::RenderRows(");
  UR_EXPECT_TRUE(Contains(rows, "sessions::RowFor(snapshot_, session, words, now, colorHexFor, section)"));
  UR_EXPECT_TRUE(Contains(rows, "sessions::ActionViewOf(sessions::ActionFor(snapshot_, session.sessionId))"));
  // the groups follow the presentation's order and headings
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::RenderGroups("),
                          "sessions::GroupsFor(snapshot_, words)"));
  // the counts in the locale's grouping
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "sessions::Words SessionsPage::MakeWords() const"),
                          "words.count = [](int64_t count) { return leaderboard::GroupedRank(count); };"));
}

// Show more under the device sign-ins asks the controller for their next page,
// and shows while the screen says so; the rows and groups go with a body that
// has none.
UR_TEST(SessionsWiring_TheSignInsShowMore) {
  const std::string page = ReadCode("SessionsPage.cpp");
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "void SessionsPage::BuildPane()"),
                          "MakeShowMoreRow([this] { binding_.ShowMore(sessions::kGroupSignIns); });"));
  const std::string render = FunctionBody(page, "void SessionsPage::Render()");
  UR_EXPECT_TRUE(Contains(render, "signInsMoreRow_->set_visible(screen.signInsMore);"));
  UR_EXPECT_TRUE(Contains(render, "RenderRows(*rowsBox_, rows_, snapshot_.sessions, sessions::kGroupSignIns, words, now);"));
  UR_EXPECT_TRUE(Contains(render, "RenderGroups(words, now);"));
  UR_EXPECT_TRUE(Before(render, "ClearRows(*rowsBox_, rows_);", "ClearGroups();"));
  // Show more is the store's
  UR_EXPECT_TRUE(Contains(FunctionBody(page, "Gtk::Widget* MakeShowMoreRow("),
                          "Lookup(sessions::kShowMoreText)"));
}

// After a confirmation the focus goes back to the row's Sign out wherever the
// row is listed: among the device sign-ins or in an open group.
UR_TEST(SessionsWiring_AGroupRowKeepsItsFocus) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string focus = FunctionBody(page, "void SessionsPage::RestoreFocus(");
  UR_EXPECT_TRUE(Contains(focus, "if (restore(rows_)) return;"));
  UR_EXPECT_TRUE(Contains(focus, "if (restore(group.rows)) return;"));
}

// This app's own sign-out is announced to the window before it is sent, so the
// app signs out however the sdk reports it (ApiSignOutWiringTest).
UR_TEST(SessionsWiring_ThisAppsOwnSignOutIsAnnounced) {
  const std::string page = ReadCode("SessionsPage.cpp");
  const std::string confirm = FunctionBody(page, "void SessionsPage::ShowConfirmation(");
  UR_EXPECT_TRUE(Contains(confirm, "[this, self = confirmation.self] {"));
  UR_EXPECT_TRUE(Before(confirm, "if (target && self && on_own_session_sign_out) on_own_session_sign_out();",
                        "if (target) binding_.Revoke(*target);"));
  const std::string window = ReadCode("MainWindow.cpp");
  UR_EXPECT_TRUE(Contains(window, "sessionsPage_->on_own_session_sign_out = [this] {\n"
                                  "    ownSessionSignOut_ = host_.SignInNow("));
}
