// SPDX-License-Identifier: MPL-2.0
#include "ReferralRoyalty.hpp"

#include <algorithm>

#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>

#include "I18n.hpp"
#include "RuntimePaths.hpp"
#include "Ui.hpp"

namespace urnw {

namespace {
ReferralTerms g_referralTerms;
}  // namespace

const ReferralTerms& CurrentReferralTerms() { return g_referralTerms; }
void SetCurrentReferralTerms(const ReferralTerms& terms) { g_referralTerms = terms; }

namespace {

constexpr int kSheetWidth = 400;   // matches the account referral sheets
constexpr int kFrogLargePx = 128;  // the crowning / refer sheets
constexpr int kFrogSmallPx = 96;   // the royal-welcome panel inside a sheet

// The frog mascot (the same crowned frog the royalty badge shows). Resolved
// through the RuntimePaths ladder; a miss keeps the words and drops the image.
std::string FrogPath() {
  static const std::string path = ResolveRuntimePath(
      UR_PKGDATADIR "/ReferralFrog.png", G_FILE_TEST_IS_REGULAR, "assets/ReferralFrog.png");
  return path;
}

void AppendFrog(Gtk::Box& box, int pixelSize) {
  const std::string frog = FrogPath();
  if (frog.empty()) return;
  auto* image = Gtk::make_managed<Gtk::Image>();
  image->set(frog);
  image->set_pixel_size(pixelSize);
  image->set_halign(Gtk::Align::CENTER);
  box.append(*image);
}

// A centered wrapped label in the referral gold (pango attribute, not a CSS
// class, for the same cascade reason AccountPage::SetToned exists).
Gtk::Label* MakeGoldLabel(const Glib::ustring& text, int sizePt) {
  auto* label = Gtk::make_managed<Gtk::Label>();
  label->set_markup("<span foreground='" + HexForMarkup(kReferralGoldLight) +
                    "' size='" + std::to_string(sizePt * PANGO_SCALE) + "'><b>" +
                    Glib::Markup::escape_text(text) + "</b></span>");
  label->set_wrap(true);
  label->set_justify(Gtk::Justification::CENTER);
  label->set_halign(Gtk::Align::CENTER);
  return label;
}

Gtk::Label* MakeBodyLabel(const Glib::ustring& text) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->set_wrap(true);
  label->set_justify(Gtk::Justification::CENTER);
  label->set_halign(Gtk::Align::CENTER);
  return label;
}

// A modal gold sheet window in the app's sheet idiom. One-shot: closing hides
// it, and the hide schedules an idle delete (deleting inside the hide signal
// itself would pull the window out from under the emission).
Gtk::Window* MakeGoldSheet(Gtk::Window& parent, const Glib::ustring& title) {
  auto* sheet = new Gtk::Window();
  sheet->set_transient_for(parent);
  sheet->set_modal(true);
  sheet->set_title(title);
  sheet->set_default_size(kSheetWidth, -1);
  sheet->set_resizable(false);
  sheet->add_css_class("ur-sheet");
  AddEscapeToClose(*sheet);
  sheet->set_hide_on_close(true);
  sheet->signal_hide().connect([sheet] {
    Glib::signal_idle().connect_once([sheet] { delete sheet; });
  });
  return sheet;
}

// The code + a copy button, and the copy-the-invite action. `owner` provides
// the clipboard.
void AppendCodeActions(Gtk::Box& box, Gtk::Window& owner, const std::string& referralCode) {
  if (referralCode.empty()) return;

  auto* hint = Gtk::make_managed<Gtk::Label>(
      T_("refer_friends_code_hint", "Share your code. Friends enter it when they sign up."));
  hint->add_css_class("dim-label");
  hint->add_css_class("caption");
  hint->set_wrap(true);
  hint->set_justify(Gtk::Justification::CENTER);
  hint->set_halign(Gtk::Align::CENTER);
  box.append(*hint);

  auto* codeRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  codeRow->set_halign(Gtk::Align::CENTER);
  auto* code = Gtk::make_managed<Gtk::Label>();
  code->set_markup("<span foreground='" + HexForMarkup(kReferralGoldLight) +
                   "' size='" + std::to_string(16 * PANGO_SCALE) + "'><b>" +
                   Glib::Markup::escape_text(referralCode) + "</b></span>");
  code->set_selectable(true);
  codeRow->append(*code);
  auto* copyCode = Gtk::make_managed<Gtk::Button>(T_("copy", "Copy"));
  copyCode->add_css_class("flat");
  copyCode->signal_clicked().connect([&owner, copyCode, referralCode] {
    owner.get_clipboard()->set_text(referralCode);
    copyCode->set_label(T_("copied", "Copied!"));
  });
  codeRow->append(*copyCode);
  box.append(*codeRow);

  // linux has no share sheet: "share" copies the invite message, the same
  // convention as the windows account menu
  auto* share = Gtk::make_managed<Gtk::Button>(T_("share", "Share"));
  share->add_css_class("suggested-action");
  share->set_halign(Gtk::Align::CENTER);
  share->signal_clicked().connect([&owner, share, referralCode] {
    owner.get_clipboard()->set_text(Format(
        T_("referral_share_message",
           "Join me on URnetwork! Get the app and enter referral code {} when you sign up."),
        referralCode));
    share->set_label(T_("copied", "Copied!"));
  });
  box.append(*share);
}

}  // namespace

Gtk::Box* MakeRoyalWelcomePanel() {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  AppendFrog(*box, kFrogSmallPx);
  box->append(*MakeGoldLabel(T_("referral_royal_welcome", "A royal welcome!"), 18));
  box->append(*MakeBodyLabel(
      Format(T_("referral_royal_welcome_detail",
                "Referral confirmed — you and your friend each get +{} GiB/day of free "
                "data, for life."),
             CurrentReferralTerms().bonusGibPerDay)));
  return box;
}

void ShowRoyalWelcomeSheet(Gtk::Window& parent) {
  auto* sheet = MakeGoldSheet(parent, T_("referral_royal_welcome", "A royal welcome!"));

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_size_request(kSheetWidth, -1);
  box->append(*MakeRoyalWelcomePanel());
  sheet->set_child(*box);
  sheet->present();

  // hold the moment, then get out of the way (Escape/close still work sooner)
  Glib::signal_timeout().connect_once([sheet] { sheet->close(); }, 2000);
}

void ShowReferralCelebrationSheet(Gtk::Window& parent, int64_t joined,
                                  const std::string& referralCode) {
  auto* sheet = MakeGoldSheet(parent, T_("referral_royalty", "You're referral royalty!"));

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_size_request(kSheetWidth, -1);

  AppendFrog(*box, kFrogLargePx);
  box->append(*MakeGoldLabel(T_("referral_royalty", "You're referral royalty!"), 20));
  box->append(*MakeBodyLabel(Format(
      TN_("referral_celebration_detail",
          "A friend just joined URnetwork with your code. You're both earning +{1} GiB "
          "a day of free data — for life.",
          "{0} friends just joined URnetwork with your code. Each one earns you +{1} "
          "GiB a day of free data — for life.",
          joined),
      joined, CurrentReferralTerms().bonusGibPerDay)));

  AppendCodeActions(*box, *sheet, referralCode);

  auto* close = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  close->set_halign(Gtk::Align::CENTER);
  close->signal_clicked().connect([sheet] { sheet->close(); });
  box->append(*close);

  sheet->set_child(*box);
  sheet->present();
}

void ShowReferSheet(Gtk::Window& parent, int64_t totalReferrals,
                    const std::string& referralCode) {
  const bool crowned = 0 < totalReferrals;

  auto* sheet = MakeGoldSheet(parent, T_("refer_and_earn", "Refer and earn"));

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  box->set_size_request(kSheetWidth, -1);

  AppendFrog(*box, kFrogLargePx);
  box->append(*MakeGoldLabel(crowned ? T_("referral_royalty", "You're referral royalty!")
                                     : T_("refer_friends_header", "Refer friends"),
                             20));
  box->append(*MakeBodyLabel(
      T_("refer_friends_detail",
         "More connections help our community stay anonymous (and help you earn!)")));

  if (crowned) {
    auto* congrats = Gtk::make_managed<Gtk::Label>();
    congrats->set_markup(
        "<span foreground='" + HexForMarkup(kReferralGoldLight) + "'>👑 " +
        Glib::Markup::escape_text(Format(
            TN_("referral_crowned_congrats",
                "A friend has joined — you're earning +{1} GiB/day, for life.",
                "{0} friends have joined — you're earning +{1} GiB/day, for life.",
                totalReferrals),
            totalReferrals,
            CurrentReferralTerms().PaidReferrals(totalReferrals) * CurrentReferralTerms().bonusGibPerDay)) +
        "</span>");
    congrats->set_wrap(true);
    congrats->set_justify(Gtk::Justification::CENTER);
    congrats->set_halign(Gtk::Align::CENTER);
    box->append(*congrats);
  }

  AppendCodeActions(*box, *sheet, referralCode);

  sheet->set_child(*box);
  sheet->present();
}

}  // namespace urnw
