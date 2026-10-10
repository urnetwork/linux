// SPDX-License-Identifier: MPL-2.0
#include "SessionsPage.hpp"

#include <glib.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <exception>
#include <utility>

#include "BrandIcons.hpp"
#include "Formatters.hpp"
#include "I18n.hpp"
#include "KeyedReconcile.hpp"
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
constexpr int kConfirmWidth = 320;   // the remove-login-method confirm's width

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

GdkRGBA RgbaOf(const std::string& hex) {
  const Rgba color = ParseHexColor(hex, ParseHexColor(sessions::kUnknownCountryColorHex, Rgba{}));
  return GdkRGBA{static_cast<float>(color.r), static_cast<float>(color.g),
                 static_cast<float>(color.b), 1.f};
}

// ---- the sdk's snapshot, as plain data -----------------------------------------
// Every typed getter, copied over; a field the sdk reports as nil (a handle of
// 0) is simply absent. The C ABI hands times over as Unix MILLISECONDS
// (CreateTime); SessionLastUsed.UnixTime is seconds already.

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
  out.loaded = source.getLoaded();
  out.loading = source.getLoading();
  out.refreshing = source.getRefreshing();
  out.supported = source.getSupported();
  out.currentSessionId = source.getCurrentSessionId();
  out.legacyCoverage = source.getLegacyCoverage();
  if (urnet::NetworkSessionInfoList list = source.getSessions()) {
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
      out.sessions.push_back(std::move(session));
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

  rowsBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  content->append(*rowsBox_);

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
    RenderRows(MakeWords(), g_get_real_time() / G_USEC_PER_SEC);
  } else {
    for (const RowWidgets& row : rows_) rowsBox_->remove(*row.root);
    rows_.clear();
  }
  noticeRow_->set_visible(screen.refreshFailed);
  RenderOthers(screen);
  helpRow_->set_visible(screen.lastUsedHelp);
  legacyRow_->set_visible(screen.legacyNote);
}

// The rows, reconciled by session ID (KeyedReconcile.hpp): a row still listed
// is rewritten in place, so its Sign out keeps the keyboard focus and a screen
// reader its place across the polls; a new session is inserted, a gone one
// removed.
void SessionsPage::RenderRows(const sessions::Words& words, int64_t now) {
  std::vector<std::string> wanted;
  wanted.reserve(snapshot_.sessions.size());
  for (const sessions::Session& session : snapshot_.sessions) wanted.push_back(session.sessionId);
  std::vector<std::string> onScreen;
  onScreen.reserve(rows_.size());
  for (const RowWidgets& row : rows_) onScreen.push_back(row.sessionId);

  const auto colorHexFor = [](const std::string& code) { return urnet::getColorHex(code); };
  const auto render = [&](RowWidgets& row, size_t wantedIndex) {
    const sessions::Session& session = snapshot_.sessions[wantedIndex];
    UpdateRow(row, sessions::RowFor(snapshot_, session, words, now, colorHexFor),
              sessions::ActionViewOf(sessions::ActionFor(snapshot_, session.sessionId)));
  };
  for (const reconcile::Step& step : reconcile::Plan(onScreen, wanted)) {
    switch (step.kind) {
      case reconcile::StepKind::Remove:
        rowsBox_->remove(*rows_[step.index].root);
        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(step.index));
        break;
      case reconcile::StepKind::Update:
        render(rows_[step.index], step.wantedIndex);
        break;
      case reconcile::StepKind::Move: {
        RowWidgets row = std::move(rows_[step.from]);
        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(step.from));
        render(row, step.wantedIndex);
        if (step.index == 0) {
          rowsBox_->reorder_child_at_start(*row.root);
        } else {
          rowsBox_->reorder_child_after(*row.root, *rows_[step.index - 1].root);
        }
        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(step.index), std::move(row));
        break;
      }
      case reconcile::StepKind::Insert: {
        RowWidgets row = BuildRow(wanted[step.wantedIndex]);
        render(row, step.wantedIndex);
        if (step.index == 0) {
          rowsBox_->insert_child_at_start(*row.root);
        } else {
          rowsBox_->insert_child_after(*row.root, *rows_[step.index - 1].root);
        }
        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(step.index), std::move(row));
        break;
      }
    }
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
  ShowConfirmation(*confirmation);
}

void SessionsPage::ShowConfirmation(const sessions::Confirmation& confirmation) {
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
  auto* body = Gtk::make_managed<Gtk::Label>(sessions::ConfirmationBody(confirmation, MakeWords()));
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
  signOut->signal_clicked().connect([this] {
    // the target, once: a second press cannot send a second revoke
    const std::optional<sessions::Target> target = flow_.Confirm();
    confirmDialog_->set_visible(false);
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
// is still there and still takes a press.
void SessionsPage::RestoreFocus(const sessions::Target& target) {
  if (target.others) {
    if (othersRow_->get_visible() && othersButton_->get_sensitive()) othersButton_->grab_focus();
    return;
  }
  for (RowWidgets& row : rows_) {
    if (row.sessionId != target.sessionId) continue;
    if (row.signOut->get_sensitive()) row.signOut->grab_focus();
    return;
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
