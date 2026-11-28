// SPDX-License-Identifier: MPL-2.0
#include "ExtenderSection.hpp"

#include <optional>
#include <vector>

#include <glib.h>

#include "HelpButton.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// the pane's prose padding, matching the Account pane's own padded rows
constexpr int kPadY = 10;

// A pane row whose height is its content: the pane's 12px inset and bottom
// hairline come from the kit, so this cannot drift from the rows around it.
struct PaddedRow {
  Gtk::Box* root = nullptr;
  Gtk::Box* content = nullptr;
};

PaddedRow MakePaddedRow(int padY) {
  PaddedRow out;
  out.root = kit::MakePaneRow(0);
  out.content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  out.content->set_hexpand(true);
  out.content->set_margin_top(padY);
  out.content->set_margin_bottom(padY);
  if (auto* inner = dynamic_cast<Gtk::Box*>(out.root->get_first_child())) {
    inner->append(*out.content);
  }
  return out;
}

Gtk::Label* MakeFieldLabel(const Glib::ustring& text) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class("ur-input-label");
  label->set_xalign(0);
  return label;
}

Gtk::Label* MakeHint(const Glib::ustring& text) {
  auto* hint = Gtk::make_managed<Gtk::Label>(text);
  hint->add_css_class("ur-caption");
  hint->add_css_class("dim-label");
  hint->set_xalign(0);
  hint->set_wrap(true);
  return hint;
}

// The placeholder of a field that is at (or was last seen at) its default:
// "Default: extender.ur.network". A field whose default this build has never
// seen gets no placeholder rather than an invented one.
void ApplyPlaceholder(Gtk::Entry& entry, const extender::SettingsField& field) {
  if (!field.hasDefault()) {
    entry.set_placeholder_text({});
    return;
  }
  entry.set_placeholder_text(
      Format(T_("extender_default_value", "Default: {}"), field.defaultValue));
}

}  // namespace

ExtenderSection::ExtenderSection(SdkHost& host)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 0), host_(host) {
  set_hexpand(true);
  // the '?' says what an extender is
  auto header = kit::MakePaneGroupHeader(T_("extenders", "Extenders"));
  header.trailing->append(*MakeHelpButton(HelpTopic::Extender));
  append(*header.root);
  BuildForm(*this);
  BuildControlDoh(*this);
  BuildAdvanced(*this);
  BuildActions(*this);
  ApplyEnabled();
}

ExtenderSection::~ExtenderSection() { *alive_ = false; }

void ExtenderSection::BuildForm(Gtk::Box& host) {
  auto row = MakePaddedRow(kPadY);

  auto* title = Gtk::make_managed<Gtk::Label>(T_("extender_settings", "Extender settings"));
  title->add_css_class("ur-input-label");
  title->set_xalign(0);
  row.content->append(*title);

  row.content->append(*MakeFieldLabel(T_("extender_dns_name", "Extender DNS name")));
  dnsName_ = Gtk::make_managed<Gtk::Entry>();
  dnsName_->add_css_class("ur-input");
  kit::SetAccessibleLabel(*dnsName_, T_("extender_dns_name", "Extender DNS name"));
  dnsName_->signal_activate().connect([this] { Save(); });
  row.content->append(*dnsName_);

  row.content->append(*MakeFieldLabel(T_("gossip_url", "Gossip URL")));
  gossipUrl_ = Gtk::make_managed<Gtk::Entry>();
  gossipUrl_->add_css_class("ur-input");
  kit::SetAccessibleLabel(*gossipUrl_, T_("gossip_url", "Gossip URL"));
  gossipUrl_->signal_activate().connect([this] { Save(); });
  row.content->append(*gossipUrl_);

  row.content->append(*MakeFieldLabel(T_("extender_hosts", "Extender hosts")));
  hosts_ = Gtk::make_managed<Gtk::TextView>();
  hosts_->set_monospace(true);
  hosts_->set_wrap_mode(Gtk::WrapMode::CHAR);
  hosts_->add_css_class("ur-card-bordered");
  kit::SetAccessibleLabel(*hosts_, T_("extender_hosts", "Extender hosts"));
  auto* hostsScroll = Gtk::make_managed<Gtk::ScrolledWindow>();
  hostsScroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  hostsScroll->set_min_content_height(72);
  hostsScroll->set_child(*hosts_);
  row.content->append(*hostsScroll);
  row.content->append(*MakeHint(
      T_("extender_hosts_hint",
         "One hostname or IP address per line. These are added to the discovered extenders.")));

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  save_ = Gtk::make_managed<Gtk::Button>(T_("save", "Save"));
  save_->add_css_class("suggested-action");
  save_->signal_clicked().connect([this] { Save(); });
  actions->append(*save_);
  row.content->append(*actions);

  status_ = Gtk::make_managed<Gtk::Label>();
  status_->set_xalign(0);
  status_->set_wrap(true);
  row.content->append(*status_);

  host.append(*row.root);
}

void ExtenderSection::BuildControlDoh(Gtk::Box& host) {
  // Its own row under the extender settings, with its own Save and result:
  // the servers are written to the GUI's own space through SdkHost, not
  // through the view controller.
  auto row = MakePaddedRow(kPadY);
  controlDoh_ = Gtk::make_managed<ControlDohSection>(host_, /*withTitle=*/true);
  row.content->append(*controlDoh_);
  host.append(*row.root);
}

void ExtenderSection::BuildAdvanced(Gtk::Box& host) {
  auto row = MakePaddedRow(kPadY);
  auto* expander = Gtk::make_managed<Gtk::Expander>(T_("advanced", "Advanced"));
  expander->set_expanded(false);

  auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  body->set_margin_top(8);

  auto* title = Gtk::make_managed<Gtk::Label>(T_("private_extender", "Private extender"));
  title->add_css_class("ur-input-label");
  title->set_xalign(0);
  body->append(*title);
  body->append(*MakeHint(T_("private_extender_hint",
                            "A private extender replaces every discovered extender. Leave it "
                            "empty to use discovery.")));

  body->append(*MakeFieldLabel(T_("private_extender_ip", "IP address")));
  privateIp_ = Gtk::make_managed<Gtk::Entry>();
  privateIp_->add_css_class("ur-input");
  kit::SetAccessibleLabel(*privateIp_, T_("private_extender_ip", "IP address"));
  body->append(*privateIp_);

  body->append(*MakeFieldLabel(T_("private_extender_secret", "Secret")));
  privateSecret_ = Gtk::make_managed<Gtk::Entry>();
  privateSecret_->add_css_class("ur-input");
  // a shared secret is a credential: never on screen by default
  privateSecret_->set_visibility(false);
  privateSecret_->set_input_purpose(Gtk::InputPurpose::PASSWORD);
  kit::SetAccessibleLabel(*privateSecret_, T_("private_extender_secret", "Secret"));
  body->append(*privateSecret_);

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  savePrivate_ = Gtk::make_managed<Gtk::Button>(T_("save", "Save"));
  savePrivate_->signal_clicked().connect([this] { SavePrivateExtender(); });
  actions->append(*savePrivate_);
  body->append(*actions);

  expander->set_child(*body);
  row.content->append(*expander);
  host.append(*row.root);
}

void ExtenderSection::BuildActions(Gtk::Box& host) {
  auto row = MakePaddedRow(kPadY);
  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  share_ = Gtk::make_managed<Gtk::Button>(T_("share_extenders", "Share extenders"));
  share_->signal_clicked().connect([this] {
    if (on_share) on_share();
  });
  actions->append(*share_);
  import_ = Gtk::make_managed<Gtk::Button>(T_("import_extenders", "Import extenders"));
  import_->signal_clicked().connect([this] {
    if (on_import) on_import();
  });
  actions->append(*import_);
  // Not red here: red belongs to the confirmation the page opens.
  reset_ = Gtk::make_managed<Gtk::Button>(T_("reset_extenders", "Reset extenders"));
  reset_->signal_clicked().connect([this] {
    if (on_reset) on_reset();
  });
  actions->append(*reset_);
  row.content->append(*actions);
  host.append(*row.root);
}

void ExtenderSection::ResetExtenders() {
  if (resetting_) return;
  auto alive = alive_;
  const bool started =
      host_.ResetExtenders([this, alive](SdkHost::ExtenderResetOutcome outcome) {
        if (!*alive) return;  // the page is gone; nothing to show
        resetting_ = false;
        FinishReset(outcome);
      });
  if (!started) {
    // no network space to reset, or a reset already in flight
    kit::ApplySupportingText(*status_, T_("something_went_wrong", "Something went wrong."),
                             kit::ValidationState::Invalid);
    Snack(T_("something_went_wrong", "Something went wrong."), true);
    return;
  }
  resetting_ = true;
  ApplyEnabled();
}

void ExtenderSection::FinishReset(const SdkHost::ExtenderResetOutcome& outcome) {
  if (!outcome.reset) {
    ApplyEnabled();
    kit::ApplySupportingText(*status_, T_("something_went_wrong", "Something went wrong."),
                             kit::ValidationState::Invalid);
    Snack(T_("something_went_wrong", "Something went wrong."), true);
    return;
  }
  // The settings from the view controller when a device is bound, the
  // bootstrap DoH servers (a reset keeps them) and the private extender (a
  // reset clears it), all from the GUI's own space.
  Load();
  if (!haveSettings_) {
    // No controller to read the settings back from, and a reset leaves every
    // one at its default: each field empty under the default it last showed,
    // and no manual host.
    dnsField_ = extender::ApplySettingsField(dnsField_, dnsField_.defaultValue,
                                             /*isDefault=*/true);
    gossipField_ = extender::ApplySettingsField(gossipField_, gossipField_.defaultValue,
                                                /*isDefault=*/true);
    dnsName_->set_text(dnsField_.text);
    gossipUrl_->set_text(gossipField_.text);
    hosts_->get_buffer()->set_text("");
    ApplyPlaceholder(*dnsName_, dnsField_);
    ApplyPlaceholder(*gossipUrl_, gossipField_);
  }
  kit::ApplySupportingText(*status_, T_("extenders_reset_done", "Extenders reset"),
                           kit::ValidationState::Valid);
  Snack(T_("extenders_reset_done", "Extenders reset"), false);
}

void ExtenderSection::Load() {
  ApplySettings(host_.GetExtenderSettings());

  // The bootstrap DNS-over-HTTPS servers and the legacy private extender read
  // off the GUI's own network space, which exists with or without a tunnel --
  // unlike the settings above them.
  controlDoh_->Load();
  const auto netExtender = host_.GetPrivateExtender();
  privateIp_->set_text(netExtender ? netExtender->ip : std::string());
  privateSecret_->set_text(netExtender ? netExtender->secret : std::string());
}

void ExtenderSection::ApplySettings(const std::optional<urnet::ExtenderSettings>& settings) {
  haveSettings_ = settings.has_value();
  if (settings) {
    dnsField_ = extender::ApplySettingsField(dnsField_, settings->DnsName,
                                             settings->DnsNameDefault);
    gossipField_ = extender::ApplySettingsField(gossipField_, settings->GossipUrl,
                                                settings->GossipUrlDefault);
    dnsName_->set_text(dnsField_.text);
    gossipUrl_->set_text(gossipField_.text);
    hosts_->get_buffer()->set_text(
        extender::JoinHostLines(settings->Hosts.value_or(urnet::StringList())));
  }
  ApplyPlaceholder(*dnsName_, dnsField_);
  ApplyPlaceholder(*gossipUrl_, gossipField_);
  ApplyEnabled();
}

void ExtenderSection::ApplyEnabled() {
  // With no device there is no view controller, so the three settings, the
  // share and the import have nowhere to go. They are DISABLED rather than
  // absent: a section that vanishes reads as a feature this build does not
  // have. The private extender and the bootstrap DNS-over-HTTPS servers are not
  // gated -- they are written to the GUI's own space and need no tunnel, and a
  // network that blocks the built-in DoH servers keeps the tunnel from
  // bootstrapping until the servers are set. Neither is the reset, which resets
  // the GUI's own space and hands the daemon the reset; it waits only for one
  // in flight.
  const bool enabled = haveSettings_;
  dnsName_->set_sensitive(enabled);
  gossipUrl_->set_sensitive(enabled);
  hosts_->set_sensitive(enabled);
  save_->set_sensitive(enabled);
  share_->set_sensitive(enabled);
  import_->set_sensitive(enabled);
  reset_->set_sensitive(!resetting_);
  if (!enabled) {
    kit::ApplySupportingText(
        *status_, T_("extenders_no_session", "Sign in and connect to manage extenders."),
        kit::ValidationState::NotChecked);
  }
}

void ExtenderSection::Save() {
  if (!haveSettings_) return;
  const std::vector<std::string> hosts =
      extender::SplitHostLines(hosts_->get_buffer()->get_text().raw());
  const auto applied = host_.SetExtenderSettings(dnsName_->get_text().raw(),
                                                 gossipUrl_->get_text().raw(), hosts);
  if (!applied) {
    kit::ApplySupportingText(*status_, T_("something_went_wrong", "Something went wrong."),
                             kit::ValidationState::Invalid);
    Snack(T_("something_went_wrong", "Something went wrong."), true);
    return;
  }
  // Repaint from what the SDK ACCEPTED, never from what was typed: a cleared
  // field comes back as the derived default and that is what the placeholder
  // has to say.
  ApplySettings(applied);
  kit::ApplySupportingText(*status_, T_("extender_settings_saved", "Extender settings saved"),
                           kit::ValidationState::Valid);
  Snack(T_("extender_settings_saved", "Extender settings saved"), false);
}

void ExtenderSection::SavePrivateExtender() {
  const std::string ip = privateIp_->get_text().raw();
  const std::string secret = privateSecret_->get_text().raw();
  if (!host_.SetPrivateExtender(ip, secret)) {
    kit::ApplySupportingText(*status_, T_("something_went_wrong", "Something went wrong."),
                             kit::ValidationState::Invalid);
    Snack(T_("something_went_wrong", "Something went wrong."), true);
    return;
  }
  kit::ApplySupportingText(*status_, T_("extender_settings_saved", "Extender settings saved"),
                           kit::ValidationState::Valid);
  Snack(T_("extender_settings_saved", "Extender settings saved"), false);
}

void ExtenderSection::Snack(const Glib::ustring& message, bool error) {
  if (on_snackbar) on_snackbar(message, error);
}

}  // namespace urnw
