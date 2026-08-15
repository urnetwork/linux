// SPDX-License-Identifier: MPL-2.0
#include "SupportPage.hpp"

#include <glib.h>

#include <exception>
#include <utility>

#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr int kStarCount = 5;  // WinUI RatingControl defaults: 5 stars, clearable

}  // namespace

SupportPage::SupportPage(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 0), host_(host) {
  EnsureDrawerCss();  // .ur-card / .ur-caption / input styles
  set_hexpand(true);
  set_vexpand(true);

  // The whole page lives in one scroll column with the card-page 24px gutter.
  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_hexpand(true);
  scroller->set_vexpand(true);

  // The windows cap grid is * : 1000* : * with MaxWidth on the middle column
  // (centred at wide widths, fills the viewport at narrow ones, no extra
  // branch for the centring). AdwClamp is the GTK equivalent — do NOT put a
  // width request on the panel itself.
  GtkWidget* clampWidget = adw_clamp_new();
  clamp_ = ADW_CLAMP(clampWidget);
  adw_clamp_set_maximum_size(clamp_, 560);  // narrow default; ApplyBreakpoint re-caps
  adw_clamp_set_tightening_threshold(clamp_, 560);
  gtk_widget_set_hexpand(clampWidget, TRUE);
  adw_clamp_set_child(clamp_, GTK_WIDGET(column_.gobj()));
  column_.set_margin(24);

  BuildMainStack();
  BuildSideStack();

  // wide: form and contact card split the cap evenly (star + 1* columns)
  topRow_.set_homogeneous(true);
  topRow_.append(mainStack_);
  column_.append(topRow_);
  // default narrow placement (the code-applied margins, not the markup
  // default — windows ApplyBreakpoint overwrites the markup immediately)
  sideStack_.set_margin_top(16);
  sideStack_.set_margin_bottom(24);
  column_.append(sideStack_);

  scroller->set_child(*Glib::wrap(clampWidget));
  append(*scroller);
}

SupportPage::~SupportPage() {
  // orphan any in-flight send: its marshaled completion checks the epoch
  // before touching the page
  ++*epoch_;
}

void SupportPage::Load() {
  // The page loads nothing (empty/loading/failed do not apply — every state
  // is a snackbar or the form itself). Invalidate any in-flight send from a
  // previous session and reset the in-flight gate so a hung callback cannot
  // wedge Send across a re-navigation.
  ++*epoch_;
  sending_ = false;
  if (sendButton_) sendButton_->set_sensitive(true);
}

void SupportPage::ApplyBreakpoint(int widthDip) {
  const bool wide = widthDip >= static_cast<int>(kit::kWideBreakpointDip);
  if (wide_ && *wide_ == wide) return;
  wide_ = wide;

  // ONE gate moves three things at once: the cap (560 <-> 1080), the side
  // column, and the contact card's position. Rationale fixed in the windows
  // code: one form, no data; a feedback box 540 dip across is already
  // generous.
  adw_clamp_set_maximum_size(clamp_, wide ? 1080 : 560);
  adw_clamp_set_tightening_threshold(clamp_, wide ? 1080 : 560);

  // reparent the side stack; it is a member widget, so the C++ wrapper keeps
  // it alive across remove/append
  if (sideStack_.get_parent() == &topRow_) {
    topRow_.remove(sideStack_);
  } else if (sideStack_.get_parent() == &column_) {
    column_.remove(sideStack_);
  }
  if (wide) {
    // beside the form: 20 dip gutter, both top-aligned {20, 0, 0, 24}
    sideStack_.set_margin_start(20);
    sideStack_.set_margin_top(0);
    sideStack_.set_margin_end(0);
    sideStack_.set_margin_bottom(24);
    topRow_.append(sideStack_);
  } else {
    // stacked under the form with a 16 dip gap {0, 16, 0, 24}
    sideStack_.set_margin_start(0);
    sideStack_.set_margin_top(16);
    sideStack_.set_margin_end(0);
    sideStack_.set_margin_bottom(24);
    column_.append(sideStack_);
  }
}

void SupportPage::ShowPreviewSnackbar() {
  // deliberately the timing-out severity (wallet previews the persistent one)
  Snack(T_("thanks_for_the_feedback", "Thanks for the feedback!"), false);
}

// ---- construction -----------------------------------------------------------

void SupportPage::BuildMainStack() {
  mainStack_.set_valign(Gtk::Align::START);
  mainStack_.set_hexpand(true);

  // section header: comment glyph (windows E9CE, decorative) + "Feedback"
  mainStack_.append(
      *kit::MakeSectionHeader("mail-message-new-symbolic", T_("feedback", "Feedback")));

  auto* card = MakeCard(12);

  // a. intro line (supporting text: 12 muted, wraps)
  auto* intro = Gtk::make_managed<Gtk::Label>(
      T_("site_app_support_intro",
         "Send us your feedback directly, or join the community for direct support."));
  intro->add_css_class("ur-caption");
  intro->set_xalign(0);
  intro->set_wrap(true);
  card->append(*intro);

  card->append(*kit::MakeDivider());

  // c. the star rating (windows RatingControl defaults: 5 stars, clearable,
  //    nothing customized) with its caption rendered AFTER the stars
  auto* ratingRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  for (int i = 0; i < kStarCount; ++i) {
    auto* star = Gtk::make_managed<Gtk::Button>();
    star->add_css_class("flat");
    star->set_valign(Gtk::Align::CENTER);
    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name("non-starred-symbolic");
    icon->set_pixel_size(20);
    icon->add_css_class("dim-label");
    star->set_child(*icon);
    // icon-only button: it must carry a name. No store key exists for the
    // per-star names (the windows RatingControl names its stars natively), so
    // a language-neutral "n/5" stands in.
    kit::SetAccessibleLabel(*star, Glib::ustring(std::to_string(i + 1)) + "/5");
    star->signal_clicked().connect([this, i] { SetRating(i + 1); });
    ratingRow->append(*star);
    starIcons_.push_back(icon);
  }
  auto* ratingCaption =
      Gtk::make_managed<Gtk::Label>(T_("how_are_we_doing", "How are we doing?"));
  ratingCaption->add_css_class("ur-caption");
  ratingCaption->set_valign(Gtk::Align::CENTER);
  ratingCaption->set_margin_start(4);
  ratingRow->append(*ratingCaption);
  card->append(*ratingRow);

  card->append(*kit::MakeDivider());

  // e. the multi-line feedback box: header label above the field, underline
  //    input vocabulary, MinHeight 96
  auto* fieldColumn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* fieldLabel =
      Gtk::make_managed<Gtk::Label>(T_("anything_else", "Anything else?"));
  fieldLabel->add_css_class("ur-input-label");
  fieldLabel->set_xalign(0);
  fieldColumn->append(*fieldLabel);
  feedbackView_ = Gtk::make_managed<Gtk::TextView>();
  feedbackView_->add_css_class("ur-input-multi");
  feedbackView_->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
  feedbackView_->set_size_request(-1, 96);
  kit::SetAccessibleLabel(*feedbackView_, T_("anything_else", "Anything else?"));
  fieldColumn->append(*feedbackView_);
  card->append(*fieldColumn);

  // f. include-logs checkbox — the content doubles as its accessible name (it
  //    shipped once on windows with NO content; never repeat that)
  includeLogs_ = Gtk::make_managed<Gtk::CheckButton>(
      T_("feedback_include_logs", "Attach logs to feedback (optional)"));
  card->append(*includeLogs_);

  card->append(*kit::MakeDivider());

  // h. Send — right-aligned; the content is a box, so the button carries an
  //    explicit accessible name
  sendButton_ = Gtk::make_managed<Gtk::Button>();
  sendButton_->add_css_class("suggested-action");  // the card-page primary role
  sendButton_->set_halign(Gtk::Align::END);
  auto* sendContent = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* sendIcon = Gtk::make_managed<Gtk::Image>();
  sendIcon->set_from_icon_name("mail-send-symbolic");  // windows E724 (send)
  sendIcon->set_pixel_size(14);
  kit::MarkDecorative(*sendIcon);
  sendContent->append(*sendIcon);
  auto* sendLabel = Gtk::make_managed<Gtk::Label>(T_("send", "Send"));
  sendLabel->set_valign(Gtk::Align::CENTER);
  sendContent->append(*sendLabel);
  sendButton_->set_child(*sendContent);
  kit::SetAccessibleLabel(*sendButton_, T_("send", "Send"));
  sendButton_->signal_clicked().connect([this] { OnSendFeedback(); });
  card->append(*sendButton_);

  mainStack_.append(*card);

  // The windows page carries an in-flow SupportInfo InfoBar directly below
  // the card; on linux the shell's window snackbar is that surface — the page
  // emits through on_snackbar (see the header).
}

void SupportPage::BuildSideStack() {
  sideStack_.set_valign(Gtk::Align::START);
  sideStack_.set_hexpand(true);

  // section header: person glyph (windows E939, decorative) + "Support"
  sideStack_.append(
      *kit::MakeSectionHeader("avatar-default-symbolic", T_("support", "Support")));

  auto* card = MakeCard(12);

  // a. the contact sentence with REAL inline hyperlinks: markdown [label](url)
  //    spans become pango <a> runs — tab-focusable, opened in the default
  //    handler (mailto: included); a malformed URL degrades to plain text
  auto* contact = Gtk::make_managed<Gtk::Label>();
  contact->add_css_class("ur-body");  // 14px, wraps
  contact->set_xalign(0);
  contact->set_wrap(true);
  contact->set_markup(MarkdownLinksToPango(
      T_("if_the_problem_persists_contact_us_at_support_ur",
         "If the problem persists, contact us at [support@ur.io](mailto:support@ur.io) "
         "or [join our Discord](https://discord.com/invite/RUNZXMwPRK) for direct "
         "support.")));
  card->append(*contact);

  card->append(*kit::MakeDivider());

  // c. the protocol link: 12px, flush left, no padding
  auto* protocol = Gtk::make_managed<Gtk::Label>();
  protocol->add_css_class("ur-caption");
  protocol->set_xalign(0);
  protocol->set_halign(Gtk::Align::START);
  protocol->set_markup(
      "<a href=\"https://ur.xyz\">" +
      Glib::Markup::escape_text(
          T_("learn_more_protocol_page", "Learn more at the protocol page")) +
      "</a>");
  card->append(*protocol);

  sideStack_.append(*card);
}

// ---- the rating -------------------------------------------------------------

void SupportPage::SetRating(int value) {
  // clearable, the RatingControl default: picking the current value clears
  // back to unset (-1)
  rating_ = (rating_ == value) ? -1 : value;
  PaintStars();
}

void SupportPage::PaintStars() {
  for (int i = 0; i < kStarCount; ++i) {
    const bool filled = rating_ > i;
    starIcons_[static_cast<size_t>(i)]->set_from_icon_name(
        filled ? "starred-symbolic" : "non-starred-symbolic");
    if (filled) {
      starIcons_[static_cast<size_t>(i)]->remove_css_class("dim-label");
    } else {
      starIcons_[static_cast<size_t>(i)]->add_css_class("dim-label");
    }
  }
}

// ---- the send flow ----------------------------------------------------------

void SupportPage::OnSendFeedback() {
  if (sending_) return;

  // 1. Session guard FIRST. The historical bug being guarded: no guard plus
  //    an unconditional success once rendered a 401 as "Thanks for the
  //    feedback!". Warning severity -> persists.
  if (!host_.IsLoggedIn()) {
    Snack(T_("please_login_to_urnetwork", "Please login to URnetwork"), true);
    return;
  }

  // 2. Build the args. An untouched rating reports -1 and that -1 is sent
  //    as-is (windows parity, flagged to the owner). Empty text => needs
  //    omitted entirely.
  urnet::FeedbackSendArgs args{};
  args.star_count = static_cast<int64_t>(rating_);
  const Glib::ustring text = feedbackView_->get_buffer()->get_text();
  if (!text.empty()) {
    urnet::FeedbackSendNeeds needs{};
    needs.other = text.raw();  // UTF-8
    args.needs = needs;
  }
  const bool attachLogs = includeLogs_->get_active();  // captured NOW, pre-async

  // 3. Disable Send — the only in-flight gating; no spinner.
  sending_ = true;
  sendButton_->set_sensitive(false);

  // 4. Api::sendFeedback; the callback fires on an SDK thread — marshal via
  //    PostToMain and drop it if the epoch moved (Load()/dtor).
  auto epoch = epoch_;
  const uint64_t seen = *epoch_;
  host_.api().sendFeedback(
      std::optional<urnet::FeedbackSendArgs>(args),
      [this, epoch, seen, attachLogs](std::optional<urnet::FeedbackSendResult> result,
                                      std::optional<std::string> err) {
        PostToMain([this, epoch, seen, attachLogs, result = std::move(result),
                    err = std::move(err)] {
          if (*epoch != seen) return;  // stale: a newer Load() owns the page
          sending_ = false;
          sendButton_->set_sensitive(true);  // on EVERY path

          // 5. Success test: FeedbackSendResult carries no error field, only
          //    an optional feedback_id — ok = no transport error AND a result
          //    arrived. Never report success unconditionally.
          const bool ok = !err.has_value() && result.has_value();
          if (!ok) {
            // Failure: the transport error VERBATIM when non-empty (often the
            // user's only diagnostic), else the store fallback. Persists.
            // Nothing is cleared; no upload happens.
            const std::string detail = err.value_or(std::string());
            g_warning("support: sendFeedback failed: %s",
                      detail.empty() ? "(no transport error text)" : detail.c_str());
            Snack(detail.empty()
                      ? Glib::ustring(T_("error_sending_feedback",
                                         "There was an error sending your feedback. "
                                         "Please try again later."))
                      : Glib::ustring(detail),
                  true);
            return;
          }

          // Success: timed snackbar; clear the text and the checkbox. The
          // star rating is deliberately NOT reset (windows parity).
          Snack(T_("thanks_for_the_feedback", "Thanks for the feedback!"), false);
          feedbackView_->get_buffer()->set_text("");
          includeLogs_->set_active(false);

          // 6. Logs go up ONLY after the server accepted the feedback, keyed
          //    by the SERVER-issued id — a client-minted id correlates with
          //    nothing.
          const std::string feedbackId =
              result->feedback_id ? *result->feedback_id : std::string();
          if (attachLogs && !feedbackId.empty()) UploadLogs(feedbackId);
        });
      });
}

void SupportPage::UploadLogs(const std::string& feedbackId) {
  // The include-logs contract (apple FeedbackView parity): only from here,
  // only when the box was ticked, only with the server's feedback id.
  if (feedbackId.empty() || !host_.hasDevice()) {
    g_message("support: skipping log upload (no feedback id or no device)");
    return;
  }
  try {
    host_.device().uploadLogs(
        feedbackId,
        [](std::optional<urnet::UploadLogsResult> result,
           std::optional<std::string> err) {
          // Failure is silent BY DESIGN here and only here: the feedback
          // itself WAS accepted — telling the user their report failed would
          // be false. Log-only; no widget touch, so no marshal needed.
          if (err.has_value()) {
            g_warning("support: uploadLogs transport failure: %s", err->c_str());
          } else if (result && result->error) {
            g_warning("support: uploadLogs failed: %s", result->error->message.c_str());
          }
        });
  } catch (const std::exception& e) {
    // uploadLogs can also throw synchronously (C-call failure)
    g_warning("support: uploadLogs threw: %s", e.what());
  } catch (...) {
    g_warning("support: uploadLogs threw");
  }
}

void SupportPage::Snack(const Glib::ustring& message, bool error) {
  if (on_snackbar) {
    on_snackbar(message, error);
  } else {
    g_warning("support: snackbar unbound; dropping message: %s", message.c_str());
  }
}

}  // namespace urnw
