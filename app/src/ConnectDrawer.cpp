// SPDX-License-Identifier: MPL-2.0
#include "ConnectDrawer.hpp"

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

void SetDnsRowState(Gtk::Label* dot, Gtk::Label* state, bool enabled) {
  if (enabled) {
    dot->remove_css_class("ur-dot-off");
    dot->add_css_class("ur-dot-on");
    state->add_css_class("ur-value-on");
    state->remove_css_class("dim-label");
    state->set_text(T_("on", "On"));
  } else {
    dot->remove_css_class("ur-dot-on");
    dot->add_css_class("ur-dot-off");
    state->remove_css_class("ur-value-on");
    state->add_css_class("dim-label");
    state->set_text(T_("off", "Off"));
  }
}

// whole-card click -> open the detail sheet (with hover feedback + pointer)
void MakeCardTappable(Gtk::Box& card, std::function<void()> open) {
  card.add_css_class("ur-card-tappable");
  SetPointerCursor(card);
  auto gesture = Gtk::GestureClick::create();
  gesture->signal_released().connect(
      [open = std::move(open)](int, double, double) { open(); });
  card.add_controller(gesture);
}

}  // namespace

ConnectDrawer::ConnectDrawer(SdkHost& host, Gtk::Window& parent,
                             SubscriptionBalanceStore& balance)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 16), host_(host), parent_(parent), balance_(balance) {
  EnsureDrawerCss();

  BuildInsufficientBanner();
  BuildControlsCard();
  BuildClientStatsCard();
  BuildLocalStatsCard();
  BuildDnsCard();
  BuildBlockerCard();
  BuildPlanCard();

  contractsSheet_ = std::make_unique<ContractsSheet>(parent_, host_);
  splitRulesSheet_ = std::make_unique<SplitRulesSheet>(parent_, host_);
  dnsSheet_ = std::make_unique<DnsSheet>(parent_, host_);
  redeemSheet_ = std::make_unique<RedeemCodeSheet>(parent_, host_, balance_);
  upgradeSheet_ = std::make_unique<UpgradeSheet>(parent_, host_, balance_);

  // intro: the drawer slides up while its cards fade in, staggered (macOS
  // motion parity; first appearance only)
  signal_map().connect([this] {
    if (introPlayed_) return;
    introPlayed_ = true;
    AnimateEntrance(*this, 0, /*slideOffset=*/16, 300);
    unsigned delay = 40;
    for (Gtk::Widget* card = get_first_child(); card; card = card->get_next_sibling()) {
      AnimateEntrance(*card, delay, 0, 300);
      delay += 60;
    }
  });

  RefreshAll();
}

// ---- build -----------------------------------------------------------------

// Out of balance and not Pro (mac ConnectActions:58-70 swaps the connect
// button for this CTA): a tappable banner into the upgrade flow.
void ConnectDrawer::BuildInsufficientBanner() {
  insufficientBanner_ = MakeCard(4);
  auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* icon = Gtk::make_managed<Gtk::Image>();
  icon->set_from_icon_name("dialog-warning-symbolic");
  icon->add_css_class("ur-error-text");
  header->append(*icon);
  auto* title = Gtk::make_managed<Gtk::Label>(T_("insufficient_balance", "Insufficient balance"));
  title->add_css_class("heading");
  title->set_xalign(0);
  title->set_hexpand(true);
  header->append(*title);
  auto* chevron = Gtk::make_managed<Gtk::Image>();
  chevron->set_from_icon_name("go-next-symbolic");
  chevron->add_css_class("dim-label");
  header->append(*chevron);
  insufficientBanner_->append(*header);
  auto* body = Gtk::make_managed<Gtk::Label>(
      T_("insufficient_balance_message", "Add balance or a plan to keep connecting."));
  body->add_css_class("dim-label");
  body->add_css_class("caption");
  body->set_xalign(0);
  body->set_wrap(true);
  insufficientBanner_->append(*body);
  MakeCardTappable(*insufficientBanner_, [this] { OpenUpgrade(); });
  insufficientBanner_->set_visible(false);
  append(*insufficientBanner_);
}

void ConnectDrawer::BuildControlsCard() {
  auto* card = MakeCard(12);

  // selected location, read-only (the connect button above drives connection)
  auto* locationRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
  locationDot_ = Gtk::make_managed<Gtk::Label>();
  locationDot_->set_valign(Gtk::Align::CENTER);
  locationRow->append(*locationDot_);
  auto* locationColumn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  locationColumn->set_hexpand(true);
  locationName_ =
      Gtk::make_managed<Gtk::Label>(T_("best_available_provider", "Best available provider"));
  locationName_->set_xalign(0);
  locationName_->set_ellipsize(Pango::EllipsizeMode::END);
  locationColumn->append(*locationName_);
  locationCaption_ = Gtk::make_managed<Gtk::Label>();
  locationCaption_->add_css_class("dim-label");
  locationCaption_->add_css_class("caption");
  locationCaption_->set_xalign(0);
  locationColumn->append(*locationCaption_);
  locationRow->append(*locationColumn);
  card->append(*locationRow);

  auto* optionsCaption = Gtk::make_managed<Gtk::Label>(T_("connect_options", "Connect options"));
  optionsCaption->add_css_class("dim-label");
  optionsCaption->add_css_class("caption");
  optionsCaption->set_xalign(0);
  card->append(*optionsCaption);

  // window type picker: Auto | Web | Streaming
  auto* modeRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* modeLabel = Gtk::make_managed<Gtk::Label>(T_("connection_mode", "Connection Mode"));
  modeLabel->set_xalign(0);
  modeLabel->set_hexpand(true);
  modeRow->append(*modeLabel);
  auto* segmented = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  segmented->add_css_class("linked");
  modeAuto_ = Gtk::make_managed<Gtk::ToggleButton>(T_("window_type_auto", "Auto"));
  modeWeb_ = Gtk::make_managed<Gtk::ToggleButton>(T_("window_type_quality", "Web"));
  modeStreaming_ = Gtk::make_managed<Gtk::ToggleButton>(T_("window_type_speed", "Streaming"));
  modeWeb_->set_group(*modeAuto_);
  modeStreaming_->set_group(*modeAuto_);
  modeAuto_->set_active(true);
  for (Gtk::ToggleButton* button : {modeAuto_, modeWeb_, modeStreaming_}) {
    segmented->append(*button);
    // toggled fires for the deactivated button too; apply once on the activation
    button->signal_toggled().connect([this, button] {
      if (button->get_active()) ApplyControls();
    });
  }
  modeRow->append(*segmented);
  card->append(*modeRow);

  // fixed ip: window size pinned to [1,1]; only meaningful with a profile
  auto* fixedRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* fixedLabel = Gtk::make_managed<Gtk::Label>(T_("fixed_ip", "Fixed IP"));
  fixedLabel->set_xalign(0);
  fixedLabel->set_hexpand(true);
  fixedRow->append(*fixedLabel);
  fixedIpSwitch_ = Gtk::make_managed<Gtk::Switch>();
  fixedIpSwitch_->set_valign(Gtk::Align::CENTER);
  fixedIpSwitch_->set_sensitive(false);  // Auto disables it
  fixedIpSwitch_->property_active().signal_changed().connect([this] { ApplyControls(); });
  fixedRow->append(*fixedIpSwitch_);
  card->append(*fixedRow);

  // strong anonymization is the inverted allowDirect binding
  auto* anonRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* anonLabel =
      Gtk::make_managed<Gtk::Label>(T_("strong_anonymization", "Strong Anonymization"));
  anonLabel->set_xalign(0);
  anonLabel->set_hexpand(true);
  anonRow->append(*anonLabel);
  anonSwitch_ = Gtk::make_managed<Gtk::Switch>();
  anonSwitch_->set_valign(Gtk::Align::CENTER);
  anonSwitch_->set_active(true);  // profile nil -> allowDirect false
  anonSwitch_->property_active().signal_changed().connect([this] { ApplyControls(); });
  anonRow->append(*anonSwitch_);
  card->append(*anonRow);

  append(*card);
}

void ConnectDrawer::BuildClientStatsCard() {
  auto* card = MakeCard(12);
  card->append(*MakeCardHeader(T_("client_statistics", "Client statistics")));
  remoteChart_ = Gtk::make_managed<TransferChart>(T_("remote", "Remote"),
                                                  TransferChart::Route::Remote, kUrGreen, kUrPink);
  card->append(*remoteChart_);
  blockChart_ = Gtk::make_managed<TransferChart>(T_("blocked", "Blocked"),
                                                 TransferChart::Route::Block, kUrCoral,
                                                 kUrMutedCoral);
  card->append(*blockChart_);
  MakeCardTappable(*card, [this] { contractsSheet_->Open(); });
  append(*card);
}

void ConnectDrawer::BuildLocalStatsCard() {
  auto* card = MakeCard(12);
  card->append(*MakeCardHeader(T_("local_statistics", "Local statistics")));
  localChart_ = Gtk::make_managed<TransferChart>(T_("local", "Local"), TransferChart::Route::Local,
                                                 kUrGreen, kUrPink);
  card->append(*localChart_);
  splitRuleCount_ = Gtk::make_managed<Gtk::Label>(
      Format(TN_("split_rule_count", "{} split rule", "{} split rules", 0), 0));
  splitRuleCount_->set_xalign(0);
  card->append(*splitRuleCount_);
  MakeCardTappable(*card, [this] { splitRulesSheet_->Open(); });
  append(*card);
}

void ConnectDrawer::BuildDnsCard() {
  auto* card = MakeCard(0);
  card->append(*MakeCardHeader(T_("custom_dns", "Custom DNS")));

  dnsRowsBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  auto addStatusRow = [&](const char* title) {
    DnsStatusRow row;
    auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row.dot = Gtk::make_managed<Gtk::Label>("●");
    row.dot->add_css_class("ur-dot-off");
    row.dot->add_css_class("ur-caption-11");
    row.dot->set_valign(Gtk::Align::CENTER);
    rowBox->append(*row.dot);
    auto* titleLabel = Gtk::make_managed<Gtk::Label>(title);
    titleLabel->set_xalign(0);
    titleLabel->set_hexpand(true);
    rowBox->append(*titleLabel);
    row.state = Gtk::make_managed<Gtk::Label>(T_("off", "Off"));
    row.state->add_css_class("dim-label");
    row.state->add_css_class("caption");
    rowBox->append(*row.state);
    dnsRowsBox_->append(*rowBox);
    return row;
  };
  dnsDohRow_ = addStatusRow(T_("dns_over_https", "DNS over HTTPS"));
  dnsUnencryptedRow_ = addStatusRow(T_("unencrypted_dns", "Unencrypted DNS"));
  dnsLocalRow_ = addStatusRow(T_("local_dns", "Local DNS"));
  dnsFallbackRow_ = addStatusRow(T_("local_dns_fallback", "Local DNS fallback"));
  card->append(*dnsRowsBox_);

  dnsUnavailable_ =
      Gtk::make_managed<Gtk::Label>(T_("dns_settings_unavailable", "DNS settings unavailable"));
  dnsUnavailable_->add_css_class("dim-label");
  dnsUnavailable_->set_xalign(0);
  dnsUnavailable_->set_visible(false);
  card->append(*dnsUnavailable_);

  MakeCardTappable(*card, [this] { dnsSheet_->Open(); });
  append(*card);
}

void ConnectDrawer::BuildBlockerCard() {
  auto* card = MakeCard(0);
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* label =
      Gtk::make_managed<Gtk::Label>(T_("block_ads_and_trackers", "Block ads and trackers"));
  label->set_xalign(0);
  label->set_hexpand(true);
  row->append(*label);
  blockerSwitch_ = Gtk::make_managed<Gtk::Switch>();
  blockerSwitch_->set_valign(Gtk::Align::CENTER);
  blockerSwitch_->property_active().signal_changed().connect([this] {
    if (updatingBlocker_) return;
    // the device applies immediately and persists; with the tunnel down the
    // preference lands in local state and restores at the next device creation
    host_.SetBlockerEnabled(blockerSwitch_->get_active());
  });
  row->append(*blockerSwitch_);
  card->append(*row);
  append(*card);
}

// The plan + usage card (mac ConnectActions:169-204). With no Account surface
// on Linux yet, this card is the balance home: plan, usage bar, daily balance,
// referrals, upgrade, and the redeem entry point.
void ConnectDrawer::BuildPlanCard() {
  auto* card = MakeCard(8);

  auto* planCaption = Gtk::make_managed<Gtk::Label>(T_("plan", "Plan"));
  planCaption->add_css_class("dim-label");
  planCaption->add_css_class("caption");
  planCaption->set_xalign(0);
  card->append(*planCaption);

  auto* planRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  planLabel_ = Gtk::make_managed<Gtk::Label>(T_("free", "Free"));
  planLabel_->add_css_class("title-2");
  planLabel_->set_xalign(0);
  planLabel_->set_hexpand(true);
  planRow->append(*planLabel_);
  // guests first create a full account; free accounts go straight to upgrade
  createAccountBtn_ = Gtk::make_managed<Gtk::Button>(T_("create_account", "Create account"));
  createAccountBtn_->add_css_class("suggested-action");
  createAccountBtn_->set_valign(Gtk::Align::CENTER);
  createAccountBtn_->set_visible(false);
  createAccountBtn_->signal_clicked().connect([this] {
    if (on_create_account) on_create_account();
  });
  planRow->append(*createAccountBtn_);
  getProBtn_ = Gtk::make_managed<Gtk::Button>(T_("become_supporter", "Get UR Pro"));
  getProBtn_->add_css_class("suggested-action");
  getProBtn_->set_valign(Gtk::Align::CENTER);
  getProBtn_->signal_clicked().connect([this] { OpenUpgrade(); });
  planRow->append(*getProBtn_);
  card->append(*planRow);

  usageBar_ = Gtk::make_managed<UsageBar>();
  usageBar_->set_margin_top(8);
  card->append(*usageBar_);

  auto* redeemBtn =
      Gtk::make_managed<Gtk::Button>(T_("redeem_balance_code", "Redeem Balance Code"));
  redeemBtn->add_css_class("flat");
  redeemBtn->set_halign(Gtk::Align::START);
  redeemBtn->set_margin_top(4);
  redeemBtn->signal_clicked().connect([this] { redeemSheet_->Open(); });
  card->append(*redeemBtn);

  append(*card);
}

// ---- events ----------------------------------------------------------------

void ConnectDrawer::OnHostEvent(DrawerEvent event) {
  switch (event) {
    case DrawerEvent::DeviceLifecycle:
      RefreshAll();
      break;
    case DrawerEvent::Throughput:
      PullThroughput();
      break;
    case DrawerEvent::BlockActions:
    case DrawerEvent::BlockStats:
      if (splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
      break;
    case DrawerEvent::Overrides:
      RefreshSplitRuleCount();
      if (splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
      break;
    case DrawerEvent::DnsSettings:
      RefreshDnsCard();
      break;
    case DrawerEvent::Blocker:
      RefreshBlocker();
      break;
    case DrawerEvent::Contracts:
      if (contractsSheet_->is_visible()) contractsSheet_->Refresh();
      break;
    case DrawerEvent::Location:
    case DrawerEvent::Profile:
      RefreshControls();
      break;
  }
}

void ConnectDrawer::RefreshAll() {
  RefreshControls();
  PullThroughput();
  RefreshSplitRuleCount();
  RefreshDnsCard();
  RefreshBlocker();
  RefreshPlanCard();
  if (contractsSheet_->is_visible()) contractsSheet_->Refresh();
  if (splitRulesSheet_->is_visible()) splitRulesSheet_->Refresh();
}

// ---- balance / plan ----------------------------------------------------------

void ConnectDrawer::OnBalanceChanged() {
  RefreshPlanCard();
  upgradeSheet_->OnBalanceChanged();
}

void ConnectDrawer::SetInsufficientBalance(bool insufficient) {
  if (insufficientBalance_ == insufficient) return;
  insufficientBalance_ = insufficient;
  RefreshPlanCard();
}

void ConnectDrawer::OpenUpgrade() { upgradeSheet_->Open(); }

void ConnectDrawer::RefreshPlanCard() {
  const bool isPro = balance_.IsPro();
  const bool isGuest = balance_.IsGuest();
  planLabel_->set_text(isGuest ? T_("guest", "Guest")
                               : (isPro ? T_("supporter", "Pro") : T_("free", "Free")));
  createAccountBtn_->set_visible(isGuest);
  getProBtn_->set_visible(!isPro && !isGuest);
  usageBar_->SetData(balance_.UsedByteCount(), balance_.PendingByteCount(),
                     balance_.AvailableByteCount(), balance_.StartBalanceByteCount(),
                     balance_.TotalReferrals());
  // mac gate: contractStatus.insufficientBalance && !isPro && !isPolling
  insufficientBanner_->set_visible(insufficientBalance_ && !isPro && !balance_.IsPolling());
}

// ---- refresh ---------------------------------------------------------------

void ConnectDrawer::RefreshControls() {
  // selected location (read-only; mac parity for the "best available" default)
  const auto location = host_.SelectedLocation();
  const bool bestAvailable =
      !location || (location->connect_location_id &&
                    location->connect_location_id->best_available.value_or(false));
  if (bestAvailable) {
    locationName_->set_text(T_("best_available_provider", "Best available provider"));
    locationDot_->set_markup("<span foreground='" + HexForMarkup(kUrCoral) + "'>●</span>");
    locationCaption_->set_visible(false);
  } else {
    locationName_->set_text(
        location->name.value_or(T_("selected_location", "Selected location")));
    Rgba dotColor{0.5, 0.5, 0.5, 1.0};
    if (location->country_code && !location->country_code->empty()) {
      dotColor = ParseHexColor(urnet::getColorHex(*location->country_code), dotColor);
    }
    locationDot_->set_markup("<span foreground='" + HexForMarkup(dotColor) + "'>●</span>");
    const int providerCount = location->provider_count.value_or(0);
    if (0 < providerCount) {
      locationCaption_->set_text(
          Format(T_("provider_count_int", "{} providers"), providerCount));
      locationCaption_->set_visible(true);
    } else {
      locationCaption_->set_visible(false);
    }
  }

  // performance profile -> controls (profile nil == Auto)
  const auto profile = host_.GetPerformanceProfile();
  updatingControls_ = true;
  if (!profile) {
    modeAuto_->set_active(true);
    fixedIpSwitch_->set_active(false);
    anonSwitch_->set_active(true);  // allowDirect false
  } else {
    if (profile->window_type == urnet::WindowTypeQuality) {
      modeWeb_->set_active(true);
    } else {
      modeStreaming_->set_active(true);
    }
    anonSwitch_->set_active(!profile->allow_direct);
    fixedIpSwitch_->set_active(profile->window_size &&
                               profile->window_size->window_size_min == 1 &&
                               profile->window_size->window_size_max == 1);
  }
  fixedIpSwitch_->set_sensitive(!modeAuto_->get_active());
  updatingControls_ = false;
}

void ConnectDrawer::ApplyControls() {
  if (updatingControls_) return;
  const bool autoMode = modeAuto_->get_active();
  if (autoMode && fixedIpSwitch_->get_active()) {
    // Auto forces Fixed IP off (there is no profile to pin the window size)
    updatingControls_ = true;
    fixedIpSwitch_->set_active(false);
    updatingControls_ = false;
  }
  fixedIpSwitch_->set_sensitive(!autoMode);

  std::optional<urnet::PerformanceProfile> profile;
  if (!autoMode) {
    urnet::PerformanceProfile p;
    p.window_type = modeWeb_->get_active() ? urnet::WindowTypeQuality : urnet::WindowTypeSpeed;
    p.allow_direct = !anonSwitch_->get_active();
    urnet::WindowSizeSettings windowSize{};
    const bool fixed = fixedIpSwitch_->get_active();
    windowSize.window_size_min = fixed ? 1 : 2;
    windowSize.window_size_max = fixed ? 1 : 4;
    p.window_size = windowSize;
    profile = std::move(p);
  }
  host_.SetPerformanceProfile(profile);
}

void ConnectDrawer::PullThroughput() {
  auto points = std::make_shared<const urnet::ThroughputPointList>(
      host_.ThroughputPoints().value_or(urnet::ThroughputPointList()));
  const double window = static_cast<double>(host_.ThroughputWindowSeconds());
  remoteChart_->SetPoints(points, window);
  blockChart_->SetPoints(points, window);
  localChart_->SetPoints(points, window);
}

void ConnectDrawer::RefreshSplitRuleCount() {
  size_t count = 0;
  if (auto overrides = host_.BlockActionOverrides()) {
    for (const auto& override_ : *overrides) {
      // a "split rule" is an override whose route override forces local
      if (override_.RouteOverride && override_.RouteOverride->Local) ++count;
    }
  }
  splitRuleCount_->set_text(
      Format(TN_("split_rule_count", "{} split rule", "{} split rules", count), count));
}

void ConnectDrawer::RefreshDnsCard() {
  const auto settings = host_.GetDnsResolverSettings();
  dnsRowsBox_->set_visible(settings.has_value());
  dnsUnavailable_->set_visible(!settings.has_value());
  if (!settings) return;
  SetDnsRowState(dnsDohRow_.dot, dnsDohRow_.state,
                 settings->EnableRemoteDoh || settings->EnableLocalDoh);
  SetDnsRowState(dnsUnencryptedRow_.dot, dnsUnencryptedRow_.state,
                 settings->EnableRemoteDns || settings->EnableLocalDns);
  SetDnsRowState(dnsLocalRow_.dot, dnsLocalRow_.state,
                 settings->EnableLocalDoh || settings->EnableLocalDns);
  SetDnsRowState(dnsFallbackRow_.dot, dnsFallbackRow_.state, settings->EnableFallback);
}

void ConnectDrawer::RefreshBlocker() {
  updatingBlocker_ = true;
  blockerSwitch_->set_active(host_.GetBlockerEnabled());
  updatingBlocker_ = false;
}

}  // namespace urnw
