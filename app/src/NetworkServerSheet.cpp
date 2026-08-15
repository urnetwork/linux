// SPDX-License-Identifier: MPL-2.0
#include "NetworkServerSheet.hpp"

#include "I18n.hpp"
#include "NetworkServerUtils.hpp"
#include "NetworkSpaceConfig.hpp"
#include "Ui.hpp"

namespace urnw {

NetworkServerSheet::NetworkServerSheet(Gtk::Window& parent, SdkHost& sdk) : sdk_(sdk) {
  current_ = sdk_.CurrentNetworkServer();

  set_transient_for(parent);
  set_modal(true);
  set_title(T_("change_network_api_title", "Change Network API"));
  set_default_size(440, -1);
  set_resizable(false);
  add_css_class("ur-sheet");
  AddEscapeToClose(*this);

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);

  auto* heading =
      Gtk::make_managed<Gtk::Label>(T_("change_network_api_title", "Change Network API"));
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  box->append(*heading);

  auto* description = Gtk::make_managed<Gtk::Label>(
      T_("network_api_description",
         "Choose the network domain used by the app. Leave the API and connect URLs blank "
         "to derive them from the domain."));
  description->add_css_class("ur-caption");
  description->set_xalign(0);
  description->set_wrap(true);
  box->append(*description);

  // What is in force RIGHT NOW, before anything is typed — the first question
  // anyone opening this sheet has.
  auto currentLine = [box](const std::string& value) {
    auto* line = Gtk::make_managed<Gtk::Label>(value);
    line->add_css_class("ur-caption");
    line->add_css_class("ur-label-faint");
    line->set_xalign(0);
    line->set_wrap(true);
    line->set_selectable(true);
    box->append(*line);
  };
  currentLine(Format(T_("network_api_current_api", "Current API: {}"), current_.apiUrl));
  currentLine(
      Format(T_("network_api_current_connect", "Current connect: {}"), current_.connectUrl));

  auto field = [this, box](Gtk::Entry*& entry, const Glib::ustring& label,
                           const Glib::ustring& help, const std::string& value) {
    auto* group = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    auto* caption = Gtk::make_managed<Gtk::Label>(label);
    caption->add_css_class("ur-input-label");
    caption->set_xalign(0);
    group->append(*caption);
    entry = Gtk::make_managed<Gtk::Entry>();
    entry->add_css_class("ur-input");
    entry->set_text(value);
    entry->set_sensitive(current_.managerAvailable);
    group->append(*entry);
    auto* helpLine = Gtk::make_managed<Gtk::Label>(help);
    helpLine->add_css_class("ur-caption");
    helpLine->set_xalign(0);
    helpLine->set_wrap(true);
    group->append(*helpLine);
    box->append(*group);
  };

  const std::string initialHost = netserver::NormalizeHost(current_.hostName);
  field(hostBox_, T_("network_api_domain_label", "Network domain"),
        T_("network_api_domain_help", "Example: ur.network or your custom domain."),
        initialHost.empty() ? DefaultHost() : initialHost);
  field(apiBox_, T_("network_api_api_url_label", "API URL (optional)"),
        T_("network_api_api_url_help", "Leave blank to derive from the network domain."),
        current_.configuredApiUrl);
  field(connectBox_, T_("network_api_connect_url_label", "Connect URL (optional)"),
        T_("network_api_connect_url_help", "Use wss:// for secure custom connect servers."),
        current_.configuredConnectUrl);

  // AMBER, not danger: this line is ADVISORY — Apply goes ahead with an
  // http:// or ws:// endpoint (a self-hosted deployment behind a local
  // reverse proxy is a real thing people do).
  insecureText_ = Gtk::make_managed<Gtk::Label>(
      T_("network_api_insecure_warning",
         "Warning: one or more custom endpoints are not using HTTPS/WSS. Traffic may be "
         "unencrypted."));
  insecureText_->add_css_class("ur-caption");
  insecureText_->set_xalign(0);
  insecureText_->set_wrap(true);
  {
    Pango::AttrList attrs;
    auto color = Pango::Attribute::create_attr_foreground(0xF5F5, 0xC2C2, 0x4242);  // kUrAmber
    attrs.insert(color);
    insecureText_->set_attributes(attrs);
  }
  insecureText_->set_visible(false);
  box->append(*insecureText_);

  statusText_ = Gtk::make_managed<Gtk::Label>();
  statusText_->add_css_class("ur-caption");
  statusText_->set_xalign(0);
  statusText_->set_wrap(true);
  box->append(*statusText_);

  if (!current_.managerAvailable) {
    // the SDK space manager never came up: say so instead of leaving three
    // dead fields and a button that does nothing
    SetStatus(T_("network_api_manager_unavailable", "Network manager unavailable"), true);
  }

  for (Gtk::Entry* entry : {hostBox_, apiBox_, connectBox_}) {
    entry->signal_changed().connect([this] {
      ApplyDerivedPlaceholders();
      UpdateInsecureWarning();
    });
  }

  auto* useDefault =
      Gtk::make_managed<Gtk::Button>(T_("network_api_use_default", "Use default network"));
  useDefault->set_sensitive(current_.managerAvailable);
  useDefault->signal_clicked().connect([this] { UseDefault(); });
  box->append(*useDefault);

  auto* apply =
      Gtk::make_managed<Gtk::Button>(T_("network_api_apply", "Apply Network API"));
  apply->add_css_class("suggested-action");
  apply->set_sensitive(current_.managerAvailable);
  apply->signal_clicked().connect([this] {
    Apply(std::string(hostBox_->get_text()), std::string(apiBox_->get_text()),
          std::string(connectBox_->get_text()));
  });
  box->append(*apply);

  set_child(*box);
  ApplyDerivedPlaceholders();
  UpdateInsecureWarning();
}

void NetworkServerSheet::ApplyDerivedPlaceholders() {
  const std::string typed = netserver::NormalizeHost(std::string(hostBox_->get_text()));
  const std::string host = typed.empty() ? DefaultHost() : typed;
  // "official" means the PRODUCTION host specifically — only production
  // carries the migration domain; a custom deployment derives off its own name
  const bool official = (host == std::string(kUrHostName));
  const std::string migration = official ? std::string("bringyour.com") : std::string();
  const std::string env(kUrEnvName);

  hostBox_->set_placeholder_text(DefaultHost());
  apiBox_->set_placeholder_text(
      netserver::DerivedServiceUrl(host, migration, env, "https", "api"));
  connectBox_->set_placeholder_text(
      netserver::DerivedServiceUrl(host, migration, env, "wss", "connect"));
}

void NetworkServerSheet::UpdateInsecureWarning() {
  const std::string api(apiBox_->get_text());
  const std::string connect(connectBox_->get_text());
  const bool insecure = (!api.empty() && netserver::HasInsecureScheme(api, "https")) ||
                        (!connect.empty() && netserver::HasInsecureScheme(connect, "wss"));
  insecureText_->set_visible(insecure);
}

void NetworkServerSheet::SetStatus(const Glib::ustring& text, bool error) {
  statusText_->set_text(text);
  if (error) {
    statusText_->add_css_class("ur-error-text");
  } else {
    statusText_->remove_css_class("ur-error-text");
  }
}

void NetworkServerSheet::Apply(const std::string& host, const std::string& apiUrl,
                               const std::string& connectUrl) {
  const std::string normalizedHost = netserver::NormalizeHost(host);
  if (normalizedHost.empty()) {
    SetStatus(T_("network_api_enter_domain", "Enter a network domain"), true);
    return;
  }
  const std::string normalizedApi = netserver::NormalizeApiUrl(apiUrl);
  const std::string normalizedConnect = netserver::NormalizeConnectUrl(connectUrl);

  if (sdk_.ApplyNetworkServer(normalizedHost, normalizedApi, normalizedConnect)) {
    SetStatus(Format(T_("network_api_switched_to", "Switched to {}"), normalizedHost),
              false);
    if (on_applied) on_applied();
    close();
    return;
  }
  SetStatus(T_("network_api_manager_unavailable", "Network manager unavailable"), true);
}

void NetworkServerSheet::UseDefault() {
  // current_.defaultHostName, NOT the compiled-in constant: on a session
  // started against a test network (URNETWORK_NETWORK_HOST) "Use default
  // network" must not silently move the client to PRODUCTION.
  const std::string host = DefaultHost();
  hostBox_->set_text(host);
  apiBox_->set_text("");
  connectBox_->set_text("");
  Apply(host, "", "");
}

std::string NetworkServerSheet::DefaultHost() const {
  return current_.defaultHostName.empty() ? std::string(kUrHostName)
                                          : current_.defaultHostName;
}

}  // namespace urnw
