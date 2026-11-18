// SPDX-License-Identifier: MPL-2.0
#include "SessionsPage.hpp"

#include <glib.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <exception>
#include <string_view>
#include <utility>

#include "BittensorKeyHost.hpp"
#include "BrandIcons.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "KeyedReconcile.hpp"
#include "LeaderboardIndicator.hpp"
#include "Ui.hpp"
#include "UrTheme.hpp"

namespace urnw {
namespace {

constexpr int kDotPx = 40;           // §3: the country circle on Linux
constexpr int kRowGap = 12;          // circle | lines | Sign out
constexpr int kRowPadY = 10;         // a row's lines: padding 12,10
constexpr int kProsePadY = 10;       // the notes under the list
constexpr int kStateTopPx = 48;      // the state line, down from the header
constexpr int kCopyGlyphPx = 14;
constexpr int kRefreshGlyphPx = 16;
constexpr int kChevronPx = 11;       // a group heading's, as the pane kit's row buttons
constexpr int kConfirmWidth = 320;   // the remove-login-method confirm's width

// The sections SessionsPresentation.hpp names are the sdk's.
static_assert(std::string_view(sessions::kGroupSignIns) ==
                  std::string_view(urnet::ClientSessionGroupSignIns),
              "the device sign-ins' section is the sdk's");
static_assert(std::string_view(sessions::kGroupHostedProxies) ==
                  std::string_view(urnet::ClientSessionGroupHostedProxies),
              "the hosted proxies' group is the sdk's");
static_assert(std::string_view(sessions::kGroupApiClients) ==
                  std::string_view(urnet::ClientSessionGroupApiClients),
              "the API clients' group is the sdk's");
static_assert(std::string_view(sessions::kGroupPairedDevices) ==
                  std::string_view(urnet::ClientSessionGroupPairedDevices),
              "the paired devices' group is the sdk's");
static_assert(std::string_view(sessions::kGroupOtherClients) ==
                  std::string_view(urnet::ClientSessionGroupOtherClients),
              "the other clients' group is the sdk's");

// A line's text in a tone: a pango run beats the class colour (AccountPage's
// SetToned; the text is escaped, so metadata stays plain text).
void SetToned(Gtk::Label& line, const Rgba& color, const Glib::ustring& text) {
  line.set_markup("<span foreground='" + HexForMarkup(color) + "'>" +
                  Glib::Markup::escape_text(text) + "</span>");
}

std::string Lookup(const sessions::Text& text) { return T_(text.key, text.english); }

// A left-aligned line that wraps rather than cut a long place or version.
Gtk::Label* MakeLine(const char* cssClass) {
  auto* line = Gtk::make_managed<Gtk::Label>();
  line->add_css_class(cssClass);
  line->set_xalign(0);
  line->set_wrap(true);
  line->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  return line;
}

// A line's full date and time, for a screen reader (its description) and for
// the pointer (its tooltip). "" clears both.
void SetFullTime(Gtk::Label& line, const std::string& full) {
  gtk_accessible_update_property(GTK_ACCESSIBLE(line.gobj()), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                 full.c_str(), -1);
  if (full.empty()) {
    line.set_has_tooltip(false);
  } else {
    line.set_tooltip_text(full);
  }
}

// A prose row on the pane grid (12px inset and hairline from the kit), as tall
// as its text.
Gtk::Widget* MakeProseRow(Gtk::Label*& line, const Glib::ustring& text) {
  auto* host = kit::MakePaneRow(0);
  line = Gtk::make_managed<Gtk::Label>(text);
  line->add_css_class("ur-caption");
  line->set_xalign(0);
  line->set_wrap(true);
  line->set_hexpand(true);
  line->set_margin_top(kProsePadY);
  line->set_margin_bottom(kProsePadY);
  if (auto* inner = dynamic_cast<Gtk::Box*>(host->get_first_child())) inner->append(*line);
  return host;
}

// §5 C: a section's Show more, a flat button on a pane row of its own under
// the section's rows; hidden until the section has more to show.
Gtk::Widget* MakeShowMoreRow(std::function<void()> press) {
  auto* host = kit::MakePaneRow(0);
  auto* more = Gtk::make_managed<Gtk::Button>(Lookup(sessions::kShowMoreText));
  more->add_css_class("flat");
  more->set_halign(Gtk::Align::START);
  more->set_margin_top(kProsePadY);
  more->set_margin_bottom(kProsePadY);
  more->signal_clicked().connect(std::move(press));
  if (auto* inner = dynamic_cast<Gtk::Box*>(host->get_first_child())) inner->append(*more);
  host->set_visible(false);
  return host;
}

GdkRGBA RgbaOf(const std::string& hex) {
  const Rgba color = ParseHexColor(hex, ParseHexColor(sessions::kUnknownCountryColorHex, Rgba{}));
  return GdkRGBA{static_cast<float>(color.r), static_cast<float>(color.g),
                 static_cast<float>(color.b), 1.f};
}

// ---- the sdk's snapshot, as plain data -----------------------------------------
// Every typed getter, copied over; a field the sdk reports as nil (a handle of
// 0) is simply absent. The C ABI hands times over as Unix MILLISECONDS
// (CreateTime); SessionLastUsed.UnixTime is seconds already. The sections, the
// counts and the pages are copied as the controller laid them out.

sessions::Error ErrorOf(const urnet::ClientSessionError& error) {
  sessions::Error out;
  out.retryable = error.getRetryable();
  out.signInRequired = error.getSignInRequired();
  out.unsupported = error.getUnsupported();
  // the sdk's trusted cause: this session was signed out from another device
  out.sessionRevoked = error.getSessionRevoked();
  return out;
}

sessions::Action ActionOf(const urnet::ClientSessionAction& action) {
  sessions::Action out;
  out.sessionId = action.getSessionId();
  out.loading = action.getLoading();
  out.pending = action.getPending();
  if (urnet::ClientSessionError error = action.getError()) out.error = ErrorOf(error);
  return out;
}

sessions::Snapshot SnapshotOf(const urnet::ClientSessionSnapshot& source) {
  sessions::Snapshot out;
  if (!source) return out;
  // a section's rows: the device sign-ins, or an open group's
  const auto sessionsOf = [](const urnet::NetworkSessionInfoList& list) {
    std::vector<sessions::Session> rows;
    if (!list) return rows;
    const int64_t count = list.len();
    for (int64_t i = 0; i < count; ++i) {
      urnet::NetworkSessionInfo info = list.get(i);
      if (!info) continue;
      sessions::Session session;
      session.sessionId = info.getSessionId();
      // a session the server listed without an id can be neither keyed nor
      // signed out; it is not shown
      if (session.sessionId.empty()) {
        g_warning("sessions: a session without an id was left out");
        continue;
      }
      session.current = info.getCurrent();
      session.kind = info.getKind();
      session.createUnixSeconds = info.getCreateTime() / 1000;
      if (urnet::SessionLastUsed used = info.getLastUsed()) {
        sessions::LastUse use;
        use.unixSeconds = used.getUnixTime();
        use.city = used.getCity();
        use.region = used.getRegion();
        use.country = used.getCountry();
        use.countryCode = used.getCountryCode();
        use.deviceType = used.getDeviceType();
        use.appVersion = used.getAppVersion();
        session.lastUse = std::move(use);
      }
      rows.push_back(std::move(session));
    }
    return rows;
  };
  out.loaded = source.getLoaded();
  out.loading = source.getLoading();
  out.refreshing = source.getRefreshing();
  out.supported = source.getSupported();
  out.currentSessionId = source.getCurrentSessionId();
  out.legacyCoverage = source.getLegacyCoverage();
  out.sessions = sessionsOf(source.getSessions());
  out.sessionCount = source.getSessionCount();
  out.sessionsHasMore = source.getSessionsHasMore();
  out.otherSessionCount = source.getOtherSessionCount();
  out.truncated = source.getTruncated();
  if (urnet::ClientSessionGroupList groups = source.getGroups()) {
    const int64_t count = groups.len();
    for (int64_t i = 0; i < count; ++i) {
      urnet::ClientSessionGroup group = groups.get(i);
      if (!group) continue;
      sessions::Group copy;
      copy.kind = group.getKind();
      copy.count = group.getCount();
      copy.expanded = group.getExpanded();
      copy.hasMore = group.getHasMore();
      copy.sessions = sessionsOf(group.getSessions());
      out.groups.push_back(std::move(copy));
    }
  }
  if (urnet::ClientSessionError error = source.getError()) out.error = ErrorOf(error);
  if (urnet::ClientSessionAction bulk = source.getBulkAction()) out.bulkAction = ActionOf(bulk);
  if (urnet::ClientSessionActionList actions = source.getActions()) {
    const int64_t count = actions.len();
    for (int64_t i = 0; i < count; ++i) {
      if (urnet::ClientSessionAction action = actions.get(i)) {
        out.actions.push_back(ActionOf(action));
      }
    }
  }
  return out;
}

// The sdk controller behind sessions::Controller. The listener is subscribed
// here, before the binding starts the controller, so its first snapshot is not
// missed; Unsubscribe drops it (the Sub closes as it is destroyed).
class SdkController final : public sessions::Controller {
 public:
  SdkController(urnet::ClientSessionViewController controller,
                urnet::ClientSessionListener listener)
      : controller_(std::move(controller)),
        sub_(controller_.addClientSessionListener(std::move(listener))) {}

  void Start() override { controller_.start(); }
  void SetVisible(bool visible) override { controller_.setVisible(visible); }
  void SetForeground(bool foreground) override { controller_.setForeground(foreground); }
  void Refresh() override { controller_.refresh(); }
  void RevokeSession(const std::string& sessionId) override {
    controller_.revokeSession(sessionId);
  }
  void RevokeOtherSessions() override { controller_.revokeOtherSessions(); }
  void SetGroupExpanded(const std::string& groupKind, bool expanded) override {
    controller_.setGroupExpanded(groupKind, expanded);
  }
  void ShowMoreSessions(const std::string& section) override {
    controller_.showMoreSessions(section);
  }
  sessions::Snapshot Read() override {
    try {
      return SnapshotOf(controller_.getSnapshot());
    } catch (const std::exception& e) {
      // a malformed snapshot must never take the page down
      g_warning("sessions: reading the controller failed: %s", e.what());
      return sessions::Snapshot{};
    }
  }
  void Unsubscribe() override { sub_.reset(); }
  void Close() override { controller_.close(); }

 private:
  urnet::ClientSessionViewController controller_;
  std::optional<urnet::Sub> sub_;
};

}  // namespace

// =============================================================================
// SessionsPage
// =============================================================================

SessionsPage::SessionsPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0), host_(host) {
  EnsureBrandCss();   // the pane-shell vocabulary
  EnsureDrawerCss();  // .ur-caption, the chips and the sheet styles
  set_hexpand(true);
  set_vexpand(true);
  BuildPane();
  append(*pane_.root);
  // The controller polls while the page is on screen: the mapped stack child
  // of a presented window (EarningsPage's rule for the provider status).
  // Hidden, it stops and keeps its last snapshot.
  signal_map().connect([this] { binding_.SetVisible(true); });
  signal_unmap().connect([this] { binding_.SetVisible(false); });
  Render();
}

SessionsPage::~SessionsPage() {
  *alive_ = false;  // every marshaled post does nothing from here
  // The controller only: by now the window's teardown may have disposed the
  // page's widgets (EarningsPage, issue #13), so nothing here touches them.
  binding_.Release();
  confirmDialog_.reset();
}

void SessionsPage::BuildPane() {
  pane_ = kit::MakePane(Lookup(sessions::kTitleText));
  pane_.root->set_hexpand(true);
  kit::SetAccessibleLabel(*pane_.root, Lookup(sessions::kTitleText));

  // The header: progress while a refresh runs, the refresh command (§1.9 and
  // §6: a header refresh button on Linux), and the way back to Account.
  refreshSpinner_ = Gtk::make_managed<Gtk::Spinner>();
  refreshSpinner_->set_valign(Gtk::Align::CENTER);
  refreshSpinner_->set_visible(false);
  kit::SetAccessibleLabel(*refreshSpinner_, Lookup(sessions::kLoadingText));
  pane_.header->append(*refreshSpinner_);

  refreshButton_ = Gtk::make_managed<Gtk::Button>();
  refreshButton_->add_css_class("ur-pane-action");
  refreshButton_->set_child(*Gtk::make_managed<GlyphIcon>(MdiGlyph::Refresh, kRefreshGlyphPx));
  refreshButton_->set_valign(Gtk::Align::CENTER);
  refreshButton_->set_tooltip_text(Lookup(sessions::kRefreshText));
  kit::SetAccessibleLabel(*refreshButton_, Lookup(sessions::kRefreshText));
  refreshButton_->signal_clicked().connect([this] { binding_.Refresh(); });
  pane_.header->append(*refreshButton_);

  auto* back = Gtk::make_managed<Gtk::Button>();
  back->add_css_class("flat");
  back->set_label(Glib::ustring("‹ ") + Lookup(sessions::kAccountText));
  kit::SetAccessibleLabel(*back, Lookup(sessions::kAccountText));
  back->signal_clicked().connect([this] {
    if (on_back) on_back();
  });
  pane_.header->append(*back);

  Gtk::Box* content = pane_.content;

  // a failed refresh keeps the list, under this non-blocking notice
  {
    Gtk::Label* notice = nullptr;
    noticeRow_ = MakeProseRow(notice, {});
    SetToned(*notice, kUrDanger, Lookup(sessions::kRefreshFailedText));
    noticeRow_->set_visible(false);
    content->append(*noticeRow_);
  }

  // the state line of a screen with no rows: progress, empty, failed (with Try
  // again), unsupported, sign-in required
  {
    stateBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    stateBox_->set_halign(Gtk::Align::CENTER);
    stateBox_->set_margin_top(kStateTopPx);
    stateBox_->set_margin_start(24);
    stateBox_->set_margin_end(24);
    stateSpinner_ = Gtk::make_managed<Gtk::Spinner>();
    stateSpinner_->set_visible(false);
    stateBox_->append(*stateSpinner_);
    stateText_ = Gtk::make_managed<Gtk::Label>();
    stateText_->add_css_class("ur-caption");
    stateText_->set_wrap(true);
    stateText_->set_justify(Gtk::Justification::CENTER);
    stateBox_->append(*stateText_);
    retryButton_ = Gtk::make_managed<Gtk::Button>(Lookup(sessions::kTryAgainText));
    retryButton_->set_halign(Gtk::Align::CENTER);
    retryButton_->set_visible(false);
    retryButton_->signal_clicked().connect([this] { Retry(); });
    stateBox_->append(*retryButton_);
    content->append(*stateBox_);
  }

  // the device sign-ins, a page at a time, then the groups of the clients'
  // sessions (§5 C)
  rowsBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  content->append(*rowsBox_);
  signInsMoreRow_ = MakeShowMoreRow([this] { binding_.ShowMore(sessions::kGroupSignIns); });
  content->append(*signInsMoreRow_);
  groupsBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  content->append(*groupsBox_);

  // §4: Sign out all other sessions, at the bottom of the list
  {
    auto* host = kit::MakePaneRow(0);
    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    column->set_hexpand(true);
    column->set_margin_top(kProsePadY);
    column->set_margin_bottom(kProsePadY);
    othersButton_ = Gtk::make_managed<Gtk::Button>(Lookup(sessions::kSignOutOthersText));
    othersButton_->add_css_class("flat");
    othersButton_->add_css_class("destructive-action");
    othersButton_->set_halign(Gtk::Align::START);
    othersButton_->signal_clicked().connect(
        [this] { PressSignOut(sessions::Target{true, std::string()}); });
    column->append(*othersButton_);
    othersStatus_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    othersSpinner_ = Gtk::make_managed<Gtk::Spinner>();
    othersStatus_->append(*othersSpinner_);
    othersStatusText_ = MakeLine("ur-row-note");
    othersStatus_->append(*othersStatusText_);
    othersStatus_->set_visible(false);
    column->append(*othersStatus_);
    if (auto* inner = dynamic_cast<Gtk::Box*>(host->get_first_child())) inner->append(*column);
    othersRow_ = host;
    othersRow_->set_visible(false);
    content->append(*othersRow_);
  }

  // §5's footer notes
  {
    Gtk::Label* line = nullptr;
    helpRow_ = MakeProseRow(line, Lookup(sessions::kLastUsedHelpText));
    helpRow_->set_visible(false);
    content->append(*helpRow_);
    legacyRow_ = MakeProseRow(line, Lookup(sessions::kLegacyNoteText));
    legacyRow_->set_visible(false);
    content->append(*legacyRow_);
  }
}

// ---- gates and words ---------------------------------------------------------

bool SessionsPage::CanCallApi() {
  // preview first (a preview build must never reach production authenticated),
  // then the SESSION, never apiReady() (true from sdk init, long before a login)
  return !previewMode_ && host_.IsLoggedIn();
}

sessions::Words SessionsPage::MakeWords() const {
  sessions::Words words;
  words.lookup = [](const sessions::Text& text) { return Lookup(text); };
  words.format = [](const std::string& pattern, const std::vector<std::string>& args) {
    return FormatArgs(pattern.c_str(), args);
  };
  words.relativeTime = [](int64_t secondsAgo) { return RelativeTime(secondsAgo); };
  words.date = [](int64_t unixSeconds) { return LocalDate(unixSeconds); };
  words.dateTime = [](int64_t unixSeconds) { return LocalDateTime(unixSeconds); };
  // the locale's thousands grouping, as the Earnings boards group their ranks
  words.count = [](int64_t count) { return leaderboard::GroupedRank(count); };
  return words;
}

void SessionsPage::Snack(const Glib::ustring& message, bool error) {
  if (on_snackbar) {
    on_snackbar(message, error);
    return;
  }
  g_warning("sessions: snackbar unbound; dropping message: %s", message.c_str());
}

// ---- lifecycle -----------------------------------------------------------------

void SessionsPage::SetPreviewMode(bool on) { previewMode_ = on; }

void SessionsPage::ShowPreviewState() {
  binding_.Release();
  snapshot_ = sessions::Snapshot{};
  Render();
}

void SessionsPage::Load() {
  if (!CanCallApi()) {
    // no session: no controller and no request (a 401 must never arrive to be
    // read as an empty list)
    binding_.Release();
    snapshot_ = sessions::Snapshot{};
    Render();
    return;
  }
  EnsureController();
  if (!binding_.Attached()) {
    // the controller did not open: a failed load, never a spinner that waits
    // forever; Try again opens it again (Retry)
    snapshot_ = sessions::Snapshot{};
    snapshot_.error = sessions::Error{/*retryable=*/true, false, false, false};
    Render();
    return;
  }
  ReadSnapshot();
}

void SessionsPage::SetPresentationActive(bool active) { binding_.SetForeground(active); }

void SessionsPage::ResetForSignOut() {
  binding_.Release();  // the departed account's controller: nothing it says lands now
  flow_.Cancel();
  if (confirmDialog_ && confirmDialog_->get_visible()) confirmDialog_->set_visible(false);
  snapshot_ = sessions::Snapshot{};
  Render();
}

// The controller is opened on the account's Api once, and again only for a
// different Api (a server or extender change replaces it): the binding's
// source is the Api's handle, which no other Api ever has.
void SessionsPage::EnsureController() {
  const uint64_t source = host_.api().handle();
  if (binding_.Attached() && binding_.Source() == source) return;
  binding_.Release();
  const uint64_t generation = binding_.NextGeneration();
  auto alive = alive_;
  try {
    urnet::ClientSessionViewController controller = host_.api().openClientSessionViewController();
    if (!controller) {
      g_warning("sessions: the api opened no controller");
      return;
    }
    binding_.Attach(
        std::make_unique<SdkController>(
            std::move(controller),
            // an sdk thread: post only, and read the controller's latest
            // snapshot on the main loop, so posts that arrive out of order
            // still render the newest state
            [this, alive, generation](urnet::ClientSessionSnapshot) {
              PostToMain([this, alive, generation] {
                if (!*alive || !binding_.Accepts(generation)) return;
                ReadSnapshot();
              });
            }),
        source);
  } catch (const std::exception& e) {
    g_warning("sessions: could not open the controller: %s", e.what());
  }
}

void SessionsPage::ReadSnapshot() {
  if (std::optional<sessions::Snapshot> snapshot = binding_.Read()) snapshot_ = std::move(*snapshot);
  Render();
}

void SessionsPage::Retry() {
  if (!binding_.Attached()) {
    Load();
    return;
  }
  binding_.Refresh();
}

// ---- rendering --------------------------------------------------------------------

void SessionsPage::Render() {
  const bool signedIn = CanCallApi();
  const sessions::Screen screen = sessions::ScreenFor(snapshot_, signedIn);

  refreshButton_->set_sensitive(signedIn);
  refreshSpinner_->set_visible(screen.refreshing);
  if (screen.refreshing) {
    refreshSpinner_->start();
  } else {
    refreshSpinner_->stop();
  }
  kit::SetBusy(*pane_.content, screen.refreshing || screen.body == sessions::Body::Progress);

  const std::optional<sessions::Text> line = sessions::BodyText(screen.body);
  stateBox_->set_visible(line.has_value());
  const bool progress = screen.body == sessions::Body::Progress;
  stateSpinner_->set_visible(progress);
  if (progress) {
    stateSpinner_->start();
  } else {
    stateSpinner_->stop();
  }
  const bool failed = screen.body == sessions::Body::LoadFailed;
  retryButton_->set_visible(failed);
  if (line) SetToned(*stateText_, failed ? kUrDanger : kUrTextMuted, Lookup(*line));

  if (screen.body == sessions::Body::Rows) {
    const sessions::Words words = MakeWords();
    const int64_t now = g_get_real_time() / G_USEC_PER_SEC;
    RenderRows(*rowsBox_, rows_, snapshot_.sessions, sessions::kGroupSignIns, words, now);
    RenderGroups(words, now);
  } else {
    ClearRows(*rowsBox_, rows_);
    ClearGroups();
  }
  signInsMoreRow_->set_visible(screen.signInsMore);
  noticeRow_->set_visible(screen.refreshFailed);
  RenderOthers(screen);
  helpRow_->set_visible(screen.lastUsedHelp);
  legacyRow_->set_visible(screen.legacyNote);
}

// One section's rows, reconciled by session ID (KeyedReconcile.hpp): a row
// still listed is rewritten in place, so its Sign out keeps the keyboard focus
// and a screen reader its place across the polls; a new session is inserted,
// a gone one removed.
void SessionsPage::RenderRows(Gtk::Box& box, std::vector<RowWidgets>& rows,
                              const std::vector<sessions::Session>& listed,
                              const std::string& section, const sessions::Words& words,
                              int64_t now) {
  std::vector<std::string> wanted;
  wanted.reserve(listed.size());
  for (const sessions::Session& session : listed) wanted.push_back(session.sessionId);
  std::vector<std::string> onScreen;
  onScreen.reserve(rows.size());
  for (const RowWidgets& row : rows) onScreen.push_back(row.sessionId);

  const auto colorHexFor = [](const std::string& code) { return urnet::getColorHex(code); };
  const auto render = [&](RowWidgets& row, size_t wantedIndex) {
    const sessions::Session& session = listed[wantedIndex];
    UpdateRow(row, sessions::RowFor(snapshot_, session, words, now, colorHexFor, section),
              sessions::ActionViewOf(sessions::ActionFor(snapshot_, session.sessionId)));
  };
  for (const reconcile::Step& step : reconcile::Plan(onScreen, wanted)) {
    switch (step.kind) {
      case reconcile::StepKind::Remove:
        box.remove(*rows[step.index].root);
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(step.index));
        break;
      case reconcile::StepKind::Update:
        render(rows[step.index], step.wantedIndex);
        break;
      case reconcile::StepKind::Move: {
        RowWidgets row = std::move(rows[step.from]);
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(step.from));
        render(row, step.wantedIndex);
        if (step.index == 0) {
          box.reorder_child_at_start(*row.root);
        } else {
          box.reorder_child_after(*row.root, *rows[step.index - 1].root);
        }
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(step.index), std::move(row));
        break;
      }
      case reconcile::StepKind::Insert: {
        RowWidgets row = BuildRow(wanted[step.wantedIndex]);
        render(row, step.wantedIndex);
        if (step.index == 0) {
          box.insert_child_at_start(*row.root);
        } else {
          box.insert_child_after(*row.root, *rows[step.index - 1].root);
        }
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(step.index), std::move(row));
        break;
      }
    }
  }
}

void SessionsPage::ClearRows(Gtk::Box& box, std::vector<RowWidgets>& rows) {
  for (const RowWidgets& row : rows) box.remove(*row.root);
  rows.clear();
}

// §5 C: the client groups, reconciled by kind as the rows are by session ID:
// a group still listed keeps its widgets, so its heading keeps the keyboard
// focus as the group opens and closes.
void SessionsPage::RenderGroups(const sessions::Words& words, int64_t now) {
  const std::vector<sessions::GroupView> views = sessions::GroupsFor(snapshot_, words);
  std::vector<std::string> wanted;
  wanted.reserve(views.size());
  for (const sessions::GroupView& view : views) wanted.push_back(view.kind);
  std::vector<std::string> onScreen;
  onScreen.reserve(groups_.size());
  for (const GroupWidgets& group : groups_) onScreen.push_back(group.kind);

  // GroupsFor keeps the snapshot's order: a view and its group share an index
  const auto render = [&](GroupWidgets& group, size_t wantedIndex) {
    UpdateGroup(group, views[wantedIndex], snapshot_.groups[wantedIndex].sessions, words, now);
  };
  for (const reconcile::Step& step : reconcile::Plan(onScreen, wanted)) {
    switch (step.kind) {
      case reconcile::StepKind::Remove:
        groupsBox_->remove(*groups_[step.index].root);
        groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(step.index));
        break;
      case reconcile::StepKind::Update:
        render(groups_[step.index], step.wantedIndex);
        break;
      case reconcile::StepKind::Move: {
        GroupWidgets group = std::move(groups_[step.from]);
        groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(step.from));
        render(group, step.wantedIndex);
        if (step.index == 0) {
          groupsBox_->reorder_child_at_start(*group.root);
        } else {
          groupsBox_->reorder_child_after(*group.root, *groups_[step.index - 1].root);
        }
        groups_.insert(groups_.begin() + static_cast<std::ptrdiff_t>(step.index),
                       std::move(group));
        break;
      }
      case reconcile::StepKind::Insert: {
        GroupWidgets group = BuildGroup(wanted[step.wantedIndex]);
        render(group, step.wantedIndex);
        if (step.index == 0) {
          groupsBox_->insert_child_at_start(*group.root);
        } else {
          groupsBox_->insert_child_after(*group.root, *groups_[step.index - 1].root);
        }
        groups_.insert(groups_.begin() + static_cast<std::ptrdiff_t>(step.index),
                       std::move(group));
        break;
      }
    }
  }
}

SessionsPage::GroupWidgets SessionsPage::BuildGroup(const std::string& kind) {
  GroupWidgets group;
  group.kind = kind;
  group.root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  // The heading opens and closes the group: a button, so the keyboard reaches
  // it and Space or Enter activates it, with the heading for its name and its
  // expanded state for a screen reader (UpdateGroup). The chevron says the
  // same to the eye and is hidden from the accessibility tree.
  group.header = Gtk::make_managed<Gtk::Button>();
  group.header->add_css_class("ur-pane-row");
  auto* grid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, kRowGap);
  grid->set_hexpand(true);
  group.heading = MakeLine("ur-row-title");
  group.heading->set_hexpand(true);
  group.heading->set_valign(Gtk::Align::CENTER);
  grid->append(*group.heading);
  group.chevron = Gtk::make_managed<Gtk::Image>();
  group.chevron->set_from_icon_name("go-next-symbolic");
  group.chevron->set_pixel_size(kChevronPx);
  group.chevron->add_css_class("ur-label-faint");
  group.chevron->set_valign(Gtk::Align::CENTER);
  kit::MarkDecorative(*group.chevron);
  grid->append(*group.chevron);
  group.header->set_child(*grid);
  group.header->signal_clicked().connect([this, kind] { ToggleGroup(kind); });
  group.root->append(*group.header);

  // its rows while open, the device sign-ins' rows; then their Show more
  group.rowsBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  group.rowsBox->set_visible(false);
  group.root->append(*group.rowsBox);
  group.moreRow = MakeShowMoreRow([this, kind] { binding_.ShowMore(kind); });
  group.root->append(*group.moreRow);
  return group;
}

void SessionsPage::UpdateGroup(GroupWidgets& group, const sessions::GroupView& view,
                               const std::vector<sessions::Session>& listed,
                               const sessions::Words& words, int64_t now) {
  // the count is the controller's, in the locale's grouping; plain text
  group.heading->set_text(view.heading);
  kit::SetAccessibleLabel(*group.header, view.heading);
  group.chevron->set_from_icon_name(view.expanded ? "go-down-symbolic" : "go-next-symbolic");
  gtk_accessible_update_state(GTK_ACCESSIBLE(group.header->gobj()), GTK_ACCESSIBLE_STATE_EXPANDED,
                              view.expanded ? TRUE : FALSE, -1);
  // a closed group carries no rows, and its rows go with its closing
  RenderRows(*group.rowsBox, group.rows, listed, view.kind, words, now);
  group.rowsBox->set_visible(view.expanded);
  group.moreRow->set_visible(view.showMore);
}

void SessionsPage::ClearGroups() {
  for (const GroupWidgets& group : groups_) groupsBox_->remove(*group.root);
  groups_.clear();
}

// A heading's activation: the open group closed, the closed one opened, as the
// latest snapshot has it. The controller lays the list out again (no request)
// and its listener's post renders it.
void SessionsPage::ToggleGroup(const std::string& kind) {
  if (const std::optional<bool> expanded = sessions::ExpandedAfterToggle(snapshot_, kind)) {
    binding_.SetGroupExpanded(kind, *expanded);
  }
}

SessionsPage::RowWidgets SessionsPage::BuildRow(const std::string& sessionId) {
  RowWidgets row;
  row.sessionId = sessionId;
  auto* host = kit::MakePaneRow(0);  // as tall as its lines
  auto* inner = dynamic_cast<Gtk::Box*>(host->get_first_child());
  inner->set_spacing(kRowGap);

  // the country circle with the device logo, decorative: line 1 names the device
  row.dot = Gtk::make_managed<GlyphDot>(MdiGlyph::DeviceUnknown, kDotPx);
  row.dot->set_valign(Gtk::Align::START);
  row.dot->set_margin_top(kRowPadY);
  inner->append(*row.dot);

  auto* lines = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  lines->set_hexpand(true);
  lines->set_valign(Gtk::Align::CENTER);
  lines->set_margin_top(kRowPadY);
  lines->set_margin_bottom(kRowPadY);
  {
    auto* first = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row.title = MakeLine("ur-row-title");
    first->append(*row.title);
    // a word, not a colour: the tag says which session is this one
    row.tag = MakeChip(Lookup(sessions::kThisSessionText), "muted", false);
    row.tag->set_valign(Gtk::Align::CENTER);
    row.tag->set_visible(false);
    first->append(*row.tag);
    lines->append(*first);
  }
  row.lastUse = MakeLine("ur-row-note");
  lines->append(*row.lastUse);
  {
    auto* third = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    row.signedIn = MakeLine("ur-row-note");
    third->append(*row.signedIn);
    // §1.3: the short ID shows, the copy button copies the full one
    auto* copy = Gtk::make_managed<Gtk::Button>();
    copy->add_css_class("ur-pane-action");
    copy->set_child(*Gtk::make_managed<GlyphIcon>(MdiGlyph::Copy, kCopyGlyphPx));
    copy->set_valign(Gtk::Align::CENTER);
    copy->set_tooltip_text(Lookup(sessions::kCopyIdText));
    kit::SetAccessibleLabel(*copy, Lookup(sessions::kCopyIdText));
    copy->signal_clicked().connect([this, sessionId] { CopySessionId(sessionId); });
    third->append(*copy);
    lines->append(*third);
  }
  row.status = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  row.spinner = Gtk::make_managed<Gtk::Spinner>();
  row.status->append(*row.spinner);
  row.statusText = MakeLine("ur-row-note");
  row.status->append(*row.statusText);
  row.status->set_visible(false);
  lines->append(*row.status);
  inner->append(*lines);

  // §4: the trailing Sign out of a pointer platform, flat and destructive; it
  // only opens the confirmation
  row.signOut = Gtk::make_managed<Gtk::Button>(Lookup(sessions::kSignOutText));
  row.signOut->add_css_class("flat");
  row.signOut->add_css_class("destructive-action");
  row.signOut->set_valign(Gtk::Align::CENTER);
  row.signOut->signal_clicked().connect(
      [this, sessionId] { PressSignOut(sessions::Target{false, sessionId}); });
  inner->append(*row.signOut);

  row.root = host;
  return row;
}

void SessionsPage::UpdateRow(RowWidgets& row, const sessions::Row& text,
                             sessions::ActionView view) {
  row.dot->SetGlyph(text.glyph);
  row.dot->SetColor(RgbaOf(text.colorHex));
  // metadata is plain text, never markup
  row.title->set_text(text.title);
  row.tag->set_visible(text.thisSession);
  row.lastUse->set_text(text.lastUse);
  SetFullTime(*row.lastUse, text.lastUseFull);
  row.signedIn->set_text(text.signedIn);
  SetFullTime(*row.signedIn, text.signedInFull);
  // "Sign out Android": the row's action by name, for a screen reader
  kit::SetAccessibleLabel(*row.signOut, text.signOutName);

  const bool busy = view == sessions::ActionView::Busy;
  row.signOut->set_sensitive(!busy);
  const std::optional<sessions::Text> status = sessions::ActionStatusText(view, /*others=*/false);
  row.status->set_visible(status.has_value());
  row.spinner->set_visible(busy);
  if (busy) {
    row.spinner->start();
  } else {
    row.spinner->stop();
  }
  if (status) SetToned(*row.statusText, busy ? kUrTextMuted : kUrDanger, Lookup(*status));
  kit::SetBusy(*row.root, busy);
}

void SessionsPage::RenderOthers(const sessions::Screen& screen) {
  const bool shown = screen.body == sessions::Body::Rows && screen.signOutOthers;
  othersRow_->set_visible(shown);
  if (!shown) return;
  const bool busy = screen.signOutOthersView == sessions::ActionView::Busy;
  // a failure leaves the button on: another press asks the controller again
  othersButton_->set_sensitive(!busy);
  // under the button: "Signing out…", or why the other sessions are still in
  const std::optional<sessions::Text> status =
      sessions::ActionStatusText(screen.signOutOthersView, /*others=*/true);
  othersStatus_->set_visible(status.has_value());
  othersSpinner_->set_visible(busy);
  if (busy) {
    othersSpinner_->start();
  } else {
    othersSpinner_->stop();
  }
  if (status) SetToned(*othersStatusText_, busy ? kUrTextMuted : kUrDanger, Lookup(*status));
  kit::SetBusy(*othersButton_, busy);
}

// ---- actions ------------------------------------------------------------------------

void SessionsPage::CopySessionId(const std::string& sessionId) {
  if (sessionId.empty()) return;
  get_clipboard()->set_text(sessionId);  // the FULL id, never the short one
  Snack(Lookup(sessions::kCopiedText), false);
}

void SessionsPage::PressSignOut(const sessions::Target& target) {
  if (!CanCallApi() || !binding_.Attached()) return;
  // nothing while that sign-out runs, while a confirmation is open, or for a
  // session that is no longer listed (SignOutFlow)
  const std::optional<sessions::Confirmation> confirmation =
      flow_.Press(snapshot_, target, MakeWords());
  if (!confirmation) return;
  if (!confirmation->self) {
    ShowConfirmation(*confirmation, false);
    return;
  }
  // this app's own session: a sign-out leaves the network's one-tap key
  // behind, and the confirmation says so while its phrase isn't saved
  const std::optional<urnet::ByJwt> jwt = host_.ParseByJwt();
  const std::string networkId = jwt ? jwt->NetworkId.value_or(std::string()) : std::string();
  auto alive = alive_;
  const sessions::Confirmation asked = *confirmation;
  BittensorKeyNeedsBackup(networkId, [this, alive, asked](bool keyNeedsBackup) {
    if (!*alive) return;
    ShowConfirmation(asked, keyNeedsBackup);
  });
}

void SessionsPage::ShowConfirmation(const sessions::Confirmation& confirmation,
                                    bool keyNeedsBackup) {
  Gtk::Window* root = RootWindow();
  if (root == nullptr) {
    g_warning("sessions: no window root; the sign-out confirmation did not open");
    flow_.Cancel();
    return;
  }
  if (!BeginSheet("sign out a session")) {
    flow_.Cancel();
    return;
  }

  // A MODAL confirmation for every sign-out (§4), the remove-login-method
  // confirm's shape: the destructive word only on the committing control.
  const Glib::ustring title = Lookup(confirmation.title);
  confirmDialog_ = std::make_unique<Gtk::Window>();
  confirmDialog_->set_transient_for(*root);
  confirmDialog_->set_modal(true);
  confirmDialog_->set_title(title);
  confirmDialog_->set_resizable(false);
  // hide-on-close, not destroy: a unique_ptr owns this window
  confirmDialog_->set_hide_on_close(true);
  confirmDialog_->add_css_class("ur-sheet");
  AddEscapeToClose(*confirmDialog_);
  // Closed without Sign out (Cancel, Escape, the window manager): nothing is
  // sent. The focus goes back to the control that asked.
  {
    auto alive = alive_;
    const sessions::Target target = confirmation.target;
    confirmDialog_->signal_hide().connect([this, alive, target] {
      // never tear anything down inside the widget's own signal
      PostToMain([this, alive, target] {
        if (!*alive) return;
        flow_.Cancel();
        EndSheet();
        RestoreFocus(target);
      });
    });
  }

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_size_request(kConfirmWidth, -1);
  auto* heading = Gtk::make_managed<Gtk::Label>(title);
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  heading->set_wrap(true);
  box->append(*heading);
  std::string bodyText = sessions::ConfirmationBody(confirmation, MakeWords());
  if (confirmation.self && keyNeedsBackup) {
    bodyText += "\n\n";
    bodyText += T_("bittensor_key_sign_out_message",
                   "Your Bittensor recovery phrase isn't saved yet. The key stays on this device and "
                   "comes back when this network signs in again. If you lose this device before "
                   "saving it, your SN25α can't be reached.");
  }
  auto* body = Gtk::make_managed<Gtk::Label>(bodyText);
  body->add_css_class("ur-body");
  body->set_xalign(0);
  body->set_wrap(true);
  body->set_max_width_chars(40);
  box->append(*body);

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actions->set_halign(Gtk::Align::END);
  auto* cancel = Gtk::make_managed<Gtk::Button>(Lookup(sessions::kCancelText));
  cancel->signal_clicked().connect([this] { confirmDialog_->set_visible(false); });
  actions->append(*cancel);
  auto* signOut = Gtk::make_managed<Gtk::Button>(Lookup(sessions::kSignOutText));
  signOut->add_css_class("destructive-action");  // red IS the confirmation context
  signOut->signal_clicked().connect([this, self = confirmation.self] {
    // the target, once: a second press cannot send a second revoke
    const std::optional<sessions::Target> target = flow_.Confirm();
    confirmDialog_->set_visible(false);
    // this app's own session: the window follows it with the app's sign-out
    if (target && self && on_own_session_sign_out) on_own_session_sign_out();
    if (target) binding_.Revoke(*target);
  });
  actions->append(*signOut);
  box->append(*actions);

  confirmDialog_->set_child(*box);
  // DEFAULT IS CANCEL: Enter must not sign anything out
  cancel->set_receives_default(true);
  confirmDialog_->set_default_widget(*cancel);
  confirmDialog_->present();
  cancel->grab_focus();
}

// After the confirmation: the focus back on the control that opened it, when it
// is still there and still takes a press, among the device sign-ins or in an
// open group.
void SessionsPage::RestoreFocus(const sessions::Target& target) {
  if (target.others) {
    if (othersRow_->get_visible() && othersButton_->get_sensitive()) othersButton_->grab_focus();
    return;
  }
  const auto restore = [&target](std::vector<RowWidgets>& rows) {
    for (RowWidgets& row : rows) {
      if (row.sessionId != target.sessionId) continue;
      if (row.signOut->get_sensitive()) row.signOut->grab_focus();
      return true;
    }
    return false;
  };
  if (restore(rows_)) return;
  for (GroupWidgets& group : groups_) {
    if (restore(group.rows)) return;
  }
}

// ---- the one-modal gate --------------------------------------------------------------

Gtk::Window* SessionsPage::RootWindow() { return dynamic_cast<Gtk::Window*>(get_root()); }

bool SessionsPage::BeginSheet(const char* what) {
  if (sheetShowing_ || (sheet_open && sheet_open())) {
    // a click that opens nothing stays a mystery: logged, never silent
    g_message("sessions: %s suppressed — a modal is already open", what);
    return false;
  }
  sheetShowing_ = true;
  if (on_sheet_open_changed) on_sheet_open_changed(true);
  return true;
}

void SessionsPage::EndSheet() {
  if (!sheetShowing_) return;
  sheetShowing_ = false;
  if (on_sheet_open_changed) on_sheet_open_changed(false);
}

}  // namespace urnw
