// SPDX-License-Identifier: MPL-2.0
#include "ControlDohSection.hpp"

#include <glib.h>

#include <optional>

#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"
#include "UrTheme.hpp"

namespace urnw {
namespace {

constexpr int kSheetWidth = 460;

// A presentation text through the catalog. An empty key is no text (a save
// now has no note line): an empty msgid is never looked up.
Glib::ustring Tr(const extender::Text& text) {
  if (text.key.empty()) return {};
  return g_dpgettext2(GETTEXT_PACKAGE, std::string(text.key).c_str(),
                      std::string(text.english).c_str());
}

Gtk::Label* MakeHint(const Glib::ustring& text) {
  auto* hint = Gtk::make_managed<Gtk::Label>(text);
  hint->add_css_class("ur-caption");
  hint->add_css_class("dim-label");
  hint->set_xalign(0);
  hint->set_wrap(true);
  return hint;
}

}  // namespace

ControlDohSection::ControlDohSection(SdkHost& host, bool withTitle)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 8), host_(host) {
  set_hexpand(true);

  if (withTitle) {
    auto* title = Gtk::make_managed<Gtk::Label>(
        T_("control_doh_urls", "Bootstrap DNS-over-HTTPS servers"));
    title->add_css_class("ur-input-label");
    title->set_xalign(0);
    append(*title);
  }
  // The privacy disclosure is part of the description, so it is always shown.
  append(*MakeHint(T_("control_doh_urls_description", "URnetwork looks up the names of its own servers over DNS-over-HTTPS. If the built-in servers are blocked on your network, add servers that work there. URnetwork tries them first, and they can see these lookups.")));

  // One server per line. No placeholder: an empty box means the built-in
  // servers alone, and the hint under it shows the shape of a line.
  urls_ = Gtk::make_managed<Gtk::TextView>();
  urls_->set_monospace(true);
  urls_->set_wrap_mode(Gtk::WrapMode::CHAR);
  urls_->add_css_class("ur-card-bordered");
  kit::SetAccessibleLabel(*urls_, T_("control_doh_urls", "Bootstrap DNS-over-HTTPS servers"));
  urls_->get_buffer()->signal_changed().connect([this] {
    // a user edit: the last save's verdict no longer describes the box
    if (!applying_) ClearResult();
  });
  auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroll->set_min_content_height(72);
  scroll->set_child(*urls_);
  append(*scroll);
  append(*MakeHint(T_("control_doh_urls_hint", "One URL per line, starting with https:// and an IP address, such as https://223.5.5.5/dns-query.")));

  china_ = Gtk::make_managed<Gtk::Button>(T_("control_doh_use_china", "Use China resolvers"));
  china_->set_halign(Gtk::Align::START);
  china_->signal_clicked().connect([this] { UseChinaResolvers(); });
  append(*china_);
  append(*MakeHint(T_("control_doh_use_china_hint", "Fills in the AliDNS and DNSPod servers, which are reachable in mainland China.")));

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  reset_ = Gtk::make_managed<Gtk::Button>(T_("control_doh_urls_reset", "Use built-in servers only"));
  reset_->signal_clicked().connect([this] {
    // the built-in servers alone, saved at once: an empty list clears
    if (!loaded_) return;
    SetBox("");
    Save({});
  });
  actions->append(*reset_);
  save_ = Gtk::make_managed<Gtk::Button>(T_("save", "Save"));
  save_->add_css_class("suggested-action");
  save_->signal_clicked().connect([this] {
    Save(extender::SplitControlDohLines(urls_->get_buffer()->get_text().raw()));
  });
  actions->append(*save_);
  append(*actions);

  // A result line is a plain label, as the extender form's status is: its tone
  // comes from kit::ApplySupportingText.
  status_ = Gtk::make_managed<Gtk::Label>();
  status_->set_xalign(0);
  status_->set_wrap(true);
  status_->set_visible(false);
  append(*status_);
  note_ = MakeHint({});
  note_->set_visible(false);
  append(*note_);

  ApplyEnabled();
}

void ControlDohSection::Load() {
  const std::optional<std::vector<std::string>> urls = host_.GetControlDohUrls();
  loaded_ = urls.has_value();
  if (urls) {
    SetBox(extender::JoinHostLines(*urls));
  } else {
    ShowResult(T_("something_went_wrong", "Something went wrong."), {}, true);
  }
  ApplyEnabled();
}

void ControlDohSection::UseChinaResolvers() {
  if (!loaded_) return;
  // the SDK's preset is the one source of the servers (connect
  // RegionalControlDohUrls), so every app fills in the same four
  const std::vector<std::string> preset =
      SdkHost::RegionalControlDohUrls(extender::kControlDohChinaCountryCode);
  if (preset.empty()) {
    ShowResult(T_("something_went_wrong", "Something went wrong."), {}, true);
    return;
  }
  // The box's contents are replaced and nothing is saved: the user reviews the
  // servers and saves.
  SetBox(extender::JoinHostLines(preset));
  ClearResult();
}

void ControlDohSection::Save(const std::vector<std::string>& urls) {
  if (!loaded_) return;
  const std::optional<std::string> answer = host_.SetControlDohUrls(urls);
  if (!answer) {
    ShowResult(T_("something_went_wrong", "Something went wrong."), {}, true);
    return;
  }
  const extender::ControlDohSaveOutcome outcome = extender::ControlDohSaveOutcomeFor(*answer);
  if (!outcome.saved) {
    // nothing was saved, and the box keeps what was typed
    ShowResult(Tr(outcome.message), {}, true);
    return;
  }
  // Read back what the SDK stored -- normalized, repeats dropped, v4 before v6
  // -- so the box shows what is in force and the next open agrees with it.
  if (const auto stored = host_.GetControlDohUrls()) SetBox(extender::JoinHostLines(*stored));
  ShowResult(Tr(outcome.message), Tr(outcome.note), false);
}

void ControlDohSection::SetBox(const std::string& text) {
  applying_ = true;
  urls_->get_buffer()->set_text(text);
  applying_ = false;
}

void ControlDohSection::ApplyEnabled() {
  urls_->set_sensitive(loaded_);
  china_->set_sensitive(loaded_);
  reset_->set_sensitive(loaded_);
  save_->set_sensitive(loaded_);
}

void ControlDohSection::ShowResult(const Glib::ustring& message, const Glib::ustring& note,
                                   bool error) {
  kit::ApplySupportingText(*status_, message,
                           error ? kit::ValidationState::Invalid : kit::ValidationState::Valid);
  status_->set_visible(!message.empty());
  kit::SetTextOrCollapse(*note_, note);
}

void ControlDohSection::ClearResult() {
  status_->set_text("");
  status_->set_visible(false);
  kit::SetTextOrCollapse(*note_, {});
}

ControlDohSheet::ControlDohSheet(Gtk::Window& parent, SdkHost& host) {
  // The login screen opens this before any page has run: the sheet carries its
  // own vocabulary rather than borrowing a page's.
  EnsureBrandCss();   // .ur-sheet / .ur-step-heading / .ur-caption / the inputs
  EnsureDrawerCss();  // the green of the saved line
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("control_doh_urls", "Bootstrap DNS-over-HTTPS servers"));
  set_default_size(kSheetWidth, -1);
  set_hide_on_close(true);
  add_css_class("ur-sheet");
  AddEscapeToClose(*this);

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);
  auto* heading = Gtk::make_managed<Gtk::Label>(
      T_("control_doh_urls", "Bootstrap DNS-over-HTTPS servers"));
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  heading->set_wrap(true);
  box->append(*heading);
  section_ = Gtk::make_managed<ControlDohSection>(host, /*withTitle=*/false);
  box->append(*section_);
  set_child(*box);
}

ControlDohSheet::~ControlDohSheet() = default;

void ControlDohSheet::Open() {
  section_->Load();
  present();
}

}  // namespace urnw
