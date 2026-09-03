// SPDX-License-Identifier: MPL-2.0
#include "HomeShell.hpp"

#include "I18n.hpp"
#include "UrMotion.hpp"

namespace urnw {
namespace {
constexpr int kNavExpandedWidth = 220;  // windows OpenPaneLength
constexpr int kNavCompactWidth = 48;    // the compact rail
}  // namespace

HomeShell::HomeShell() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 0) {
  // ---- the left rail --------------------------------------------------------
  navRail_.add_css_class("ur-nav");
  navRail_.set_size_request(kNavExpandedWidth, -1);
  navRail_.set_vexpand(true);

  navPrimary_.set_margin_top(8);
  navPrimary_.set_vexpand(true);
  navRail_.append(navPrimary_);
  navFooter_.set_margin_bottom(8);
  navRail_.append(navFooter_);

  // primary items, in the windows order (tag -> store key -> Segoe Fluent
  // glyph the drawn NavIcon reproduces: E80F Home, E774 Globe, E8C7 Wallet,
  // E77B Contact; footer E897 Help, EBE8 DeveloperTools, E713 Settings)
  MakeNavItem(navPrimary_, "connect", NavIcon::Kind::Home, T_("connect", "Connect"));
  MakeNavItem(navPrimary_, "network", NavIcon::Kind::Globe, T_("network", "Network"));
  MakeNavItem(navPrimary_, "earnings", NavIcon::Kind::Wallet, T_("earnings", "Earnings"));
  MakeNavItem(navPrimary_, "account", NavIcon::Kind::Person, T_("account", "Account"));
  // footer: support, [developer — inserted by Advanced Mode], settings
  MakeNavItem(navFooter_, "support", NavIcon::Kind::Help, T_("support", "Support"));
  MakeNavItem(navFooter_, "settings", NavIcon::Kind::Gear, T_("settings", "Settings"));

  append(navRail_);

  // ---- the content column ---------------------------------------------------
  // the standing session-mode notice (never closable; persists across
  // destinations); empty = hidden
  modeNotice_.add_css_class("ur-mode-notice");
  modeNotice_.set_xalign(0);
  modeNotice_.set_wrap(true);
  modeNotice_.set_visible(false);
  contentColumn_.append(modeNotice_);

  stack_.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
  stack_.set_transition_duration(motion::kBaseMs);  // the page-swap default
  stack_.set_vexpand(true);
  stack_.set_hexpand(true);
  contentColumn_.append(stack_);

  // ---- the status strip -----------------------------------------------------
  statusStrip_.add_css_class("ur-status-strip");
  stateField_ = kit::MakeStatusField("", /*withDot=*/true, T_("connect", "Connect"));
  statusStrip_.append(*stateField_.root);
  statusStrip_.append(*kit::MakeStatusSeparator());
  providerField_ =
      kit::MakeStatusField(T_("selected_provider", "Selected provider"), false);
  statusStrip_.append(*providerField_.root);
  statusStrip_.append(*kit::MakeStatusSeparator());
  trafficField_ = kit::MakeStatusField("", false, T_("site_app_no_traffic", "No traffic yet"));
  statusStrip_.append(*trafficField_.root);

  // the 4 Advanced fields ride the SAME row and drop entirely with the mode
  advancedFields_.append(*kit::MakeStatusSeparator());
  networkField_ = kit::MakeStatusField(T_("network", "Network"), false);
  advancedFields_.append(*networkField_.root);
  advancedFields_.append(*kit::MakeStatusSeparator());
  sessionField_ = kit::MakeStatusField("Session", false);
  advancedFields_.append(*sessionField_.root);
  advancedFields_.append(*kit::MakeStatusSeparator());
  routesField_ = kit::MakeStatusField("Routes", false);
  advancedFields_.append(*routesField_.root);
  advancedFields_.append(*kit::MakeStatusSeparator());
  rpcField_ = kit::MakeStatusField("RPC", false);
  advancedFields_.append(*rpcField_.root);
  advancedFields_.append(*kit::MakeStatusSeparator());
  rawField_ = kit::MakeStatusField("Raw", false);
  advancedFields_.append(*rawField_.root);
  advancedFields_.set_visible(false);
  statusStrip_.append(advancedFields_);
  contentColumn_.append(statusStrip_);

  // snackbar overlays the content bottom-center (windows AccountSnackbar,
  // MaxWidth 480)
  contentOverlay_.set_child(contentColumn_);
  contentOverlay_.add_overlay(snackbar_.root());
  contentOverlay_.set_hexpand(true);
  append(contentOverlay_);

  PaintSelection();
}

HomeShell::NavItem* HomeShell::MakeNavItem(Gtk::Box& parent, const std::string& tag,
                                           NavIcon::Kind icon, const Glib::ustring& label) {
  NavItem item;
  item.tag = tag;
  item.button = Gtk::make_managed<Gtk::Button>();
  item.button->add_css_class("ur-nav-item");
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  item.accent = Gtk::make_managed<Gtk::Box>();
  item.accent->add_css_class("ur-nav-accent");
  item.accent->set_size_request(3, 16);
  item.accent->set_valign(Gtk::Align::CENTER);
  item.accent->set_opacity(0);
  kit::MarkDecorative(*item.accent);
  row->append(*item.accent);
  // drawn, not themed: two of the windows glyphs (globe, wallet) do not exist
  // in Adwaita, and a missing themed icon renders BLANK
  auto* image = Gtk::make_managed<NavIcon>(icon, 16);
  kit::MarkDecorative(*image);
  row->append(*image);
  item.label = Gtk::make_managed<Gtk::Label>(label);
  item.label->set_xalign(0);
  row->append(*item.label);
  item.button->set_child(*row);
  kit::SetAccessibleLabel(*item.button, label);
  item.button->signal_clicked().connect([this, tag] { Navigate(tag); });
  parent.append(*item.button);
  items_.push_back(item);
  return &items_.back();
}

void HomeShell::SetPage(const std::string& tag, Gtk::Widget& page) {
  stack_.add(page, tag);
}

void HomeShell::Navigate(const std::string& tag) {
  if (!stack_.get_child_by_name(tag)) return;
  currentTag_ = tag;
  stack_.set_visible_child(tag);
  PaintSelection();
  if (on_navigate) on_navigate(tag);
}

void HomeShell::PaintSelection() {
  // pages reached FROM a destination (no rail item of their own) keep that
  // destination's item lit: "referrals" belongs to Account
  const std::string railTag = currentTag_ == "referrals" ? "account" : currentTag_;
  for (auto& item : items_) {
    const bool selected = (item.tag == railTag);
    if (selected) {
      item.button->add_css_class("selected");
    } else {
      item.button->remove_css_class("selected");
    }
    if (item.accent) item.accent->set_opacity(selected ? 1.0 : 0.0);
  }
}

void HomeShell::SetAdvancedMode(bool on) {
  if (advanced_ == on) return;
  advanced_ = on;
  advancedFields_.set_visible(on);
  if (on && !developerItem_) {
    // INSERTED into the footer collection ahead of settings, not un-hidden
    auto* settingsButton = items_.empty() ? nullptr : items_.back().button;
    developerItem_ = MakeNavItem(navFooter_, "developer", NavIcon::Kind::DevTools,
                                 T_("developer", "Developer"));
    if (settingsButton) navFooter_.reorder_child_after(*settingsButton,
                                                       *developerItem_->button);
    // NOTE: items_ may have reallocated; repaint from tags, not stale pointers
    developerItem_ = &items_.back();
    PaintSelection();
  } else if (!on && developerItem_) {
    // the user standing on it is navigated away, then the item is REMOVED
    if (currentTag_ == "developer") Navigate("settings");
    navFooter_.remove(*developerItem_->button);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (it->tag == "developer") {
        items_.erase(it);
        break;
      }
    }
    developerItem_ = nullptr;
  }
}

void HomeShell::SetCompactNav(bool compact) {
  if (compact_ == compact) return;
  compact_ = compact;
  navRail_.set_size_request(compact ? kNavCompactWidth : kNavExpandedWidth, -1);
  for (auto& item : items_) {
    if (item.label) item.label->set_visible(!compact);
  }
}

void HomeShell::SetModeNotice(const Glib::ustring& message) {
  modeNotice_.set_text(message);
  modeNotice_.set_visible(!message.empty());
}

void HomeShell::SetStatusState(const Glib::ustring& word, const std::string& dotHex) {
  kit::SetStatusFieldValue(stateField_, word);
  kit::SetStatusFieldDot(stateField_, dotHex);
}
void HomeShell::SetStatusProvider(const Glib::ustring& provider) {
  kit::SetStatusFieldValue(providerField_, provider);
}
void HomeShell::SetStatusTraffic(const Glib::ustring& traffic) {
  kit::SetStatusFieldValue(trafficField_, traffic);
}
void HomeShell::SetStatusNetwork(const Glib::ustring& network) {
  kit::SetStatusFieldValue(networkField_, network);
}
void HomeShell::SetStatusSession(const Glib::ustring& session) {
  kit::SetStatusFieldValue(sessionField_, session);
}
void HomeShell::SetStatusRoutes(const Glib::ustring& routes) {
  kit::SetStatusFieldValue(routesField_, routes);
}
void HomeShell::SetStatusRpc(const Glib::ustring& rpc) {
  kit::SetStatusFieldValue(rpcField_, rpc);
}
void HomeShell::SetStatusRaw(const Glib::ustring& raw) {
  kit::SetStatusFieldValue(rawField_, raw);
}

}  // namespace urnw
