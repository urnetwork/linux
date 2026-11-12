// SPDX-License-Identifier: MPL-2.0
#include "ClaimRelay.hpp"

#include <glib.h>

#include <cstdio>
#include <string_view>

#include "BittensorKeyText.hpp"
#include "I18n.hpp"
#include "PaneKit.hpp"
#include "SnLedgerPresentation.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

constexpr int kSheetWidth = 480;
constexpr int kSheetMaxHeight = 560;
constexpr double kRaoPerUnit = 1e9;

Gtk::Label* MakeText(const Glib::ustring& text, const char* cssClass) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(true);
  label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  return label;
}

Gtk::Label* MakeStrong(const Glib::ustring& text) {
  auto* label = MakeText(text, "ur-value");
  label->add_css_class("ur-key-strong");
  return label;
}

// "0.0125": a TAO figure in rao of TAO, as the relay's policy states it
std::string FormatTaoFigure(int64_t rao) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.4f", static_cast<double>(rao) / kRaoPerUnit);
  return text;
}

// "3", "1.5" or "0.0420": an estimate, as precise as its size needs, for
// "about 3 SN25α"
std::string FormatApproxAlpha(int64_t rao) {
  const double value = static_cast<double>(rao) / kRaoPerUnit;
  char text[32];
  if (10 <= value) {
    std::snprintf(text, sizeof(text), "%.0f", value);
  } else if (1 <= value) {
    std::snprintf(text, sizeof(text), "%.1f", value);
  } else {
    std::snprintf(text, sizeof(text), "%.4f", value);
  }
  return text;
}

// The date Claim now acts again, in the reader's own date format.
std::string FormatDate(int64_t millis) {
  GDateTime* date = g_date_time_new_from_unix_local(millis / 1000);
  if (date == nullptr) return {};
  gchar* text = g_date_time_format(date, "%x");
  g_date_time_unref(date);
  std::string out = text != nullptr ? text : "";
  g_free(text);
  return out;
}

// Why a balance is still pending: a title and, for some reasons, a line more.
void AppendReason(Gtk::Box& into, const std::string& reason, int64_t thresholdTaoRao,
                  int64_t thresholdRao) {
  if (reason == urnet::SnClaimReasonUnderThreshold) {
    into.append(*MakeStrong(T_("sn_claim_relay_under_threshold", "Below the automatic threshold.")));
    if (0 < thresholdTaoRao) {
      into.append(*MakeText(
          0 < thresholdRao
              ? Glib::ustring(Format(
                    T_("sn_claim_relay_under_threshold_body",
                       "It's claimed for you once it's worth about {0} TAO, about {1} SN25α at "
                       "today's price. Waiting costs nothing: this credit never expires."),
                    FormatTaoFigure(thresholdTaoRao), FormatApproxAlpha(thresholdRao)))
              : Glib::ustring(Format(
                    T_("sn_claim_relay_under_threshold_no_price",
                       "It's claimed for you once it's worth about {} TAO. Waiting costs nothing: "
                       "this credit never expires."),
                    FormatTaoFigure(thresholdTaoRao))),
          "ur-row-note"));
    }
  } else if (reason == urnet::SnClaimReasonQueued) {
    into.append(*MakeStrong(T_("sn_claim_relay_queued", "URnetwork claims this soon.")));
  } else if (reason == urnet::SnClaimReasonCapReached) {
    // the budget renews at 00:00 UTC, and a claim goes out once a day's budget
    // reaches it: no day is promised
    into.append(*MakeStrong(T_("sn_claim_relay_cap",
                               "Today's claim budget is used up, so this waits. The budget "
                               "renews each day at 00:00 UTC. Nothing is lost: this credit "
                               "never expires.")));
  } else if (reason == urnet::SnClaimReasonRefused) {
    into.append(*MakeStrong(T_("sn_claim_relay_refused",
                               "The last claim didn't go through. URnetwork tries again later.")));
  } else if (reason == urnet::SnClaimReasonRelayNotRunning ||
             reason == urnet::SnClaimReasonRelayPaused) {
    into.append(*MakeStrong(
        reason == urnet::SnClaimReasonRelayNotRunning
            ? T_("sn_claim_relay_not_running", "Automatic claims aren't running right now.")
            : T_("sn_claim_relay_paused", "URnetwork has paused claims for now.")));
    into.append(*MakeText(
        T_("sn_claim_relay_waits", "Your SN25α waits in the vault, and this credit never expires."),
        "ur-row-note"));
  }
}

// Claim now queued (the mocks' 4c), as the relay runs it (owner, 2026-11-11;
// BTEASY.md, Claim now and the daily cap): taps go out ahead of the automatic
// claims, the oldest first, as each day's budget allows, so no day is promised;
// and the coldkey's limit starts when URnetwork pays this claim, not at the tap,
// so the next Claim now opens the relay's period after it goes through. No
// period, no second line.
Glib::ustring ClaimNowQueuedNote(int64_t periodDays) {
  Glib::ustring note = T_("sn_claim_relay_requested",
                          "Claim now is queued. It goes out ahead of automatic claims, in the "
                          "order requests came in, as each day's budget allows.");
  if (0 < periodDays) {
    // the period is a count: each language reads it in its own plural form
    const unsigned long count = static_cast<unsigned long>(periodDays);
    note += "\n";
    note += Glib::ustring(Format(
        TN_("sn_claim_relay_requested_period",
            "Your next Claim now opens a day after this claim goes through.",
            "Your next Claim now opens {} days after this claim goes through.", count),
        periodDays));
  }
  return note;
}

// Under Claim now: who pays and how often, or why it can't act now. ""
// when there is nothing to say.
Glib::ustring ClaimNowNote(bool available, const std::string& reason, int64_t nextAtMillis,
                           int64_t periodDays, int64_t floorTaoRao, bool eachColdkey) {
  if (available) {
    if (periodDays <= 0) return {};
    // the period is a count: each language reads it in its own plural form
    const unsigned long count = static_cast<unsigned long>(periodDays);
    return eachColdkey
               ? Glib::ustring(Format(
                     TN_("sn_claim_relay_claim_now_paid_each",
                         "Paid by URnetwork, once a day for each coldkey.",
                         "Paid by URnetwork, once every {} days for each coldkey.", count),
                     periodDays))
               : Glib::ustring(Format(TN_("sn_claim_relay_claim_now_paid",
                                          "Claim now is paid by URnetwork, once a day.",
                                          "Claim now is paid by URnetwork, once every {} days.", count),
                                      periodDays));
  }
  if (reason == urnet::SnClaimNowReasonUnderFloor && 0 < floorTaoRao) {
    return Format(T_("sn_claim_relay_under_floor", "Claim now needs a balance worth at least {} TAO."),
                  FormatTaoFigure(floorTaoRao));
  }
  if (reason == urnet::SnClaimNowReasonRequested) return ClaimNowQueuedNote(periodDays);
  if (reason == urnet::SnClaimNowReasonLimit && 0 < nextAtMillis) {
    const std::string date = FormatDate(nextAtMillis);
    if (!date.empty()) {
      return Format(T_("sn_claim_relay_again_on", "Your next Claim now opens on {}."), date);
    }
  }
  return {};
}

// Who claims (while the relay does), and the pending pill with why it waits.
void AppendWhoAndWhy(Gtk::Box& into, const urnet::SnClaimStatusResult& status) {
  if (ClaimRelayClaims(status)) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name("view-refresh-symbolic");
    icon->add_css_class("ur-key-icon-info");
    kit::MarkDecorative(*icon);
    row->append(*icon);
    row->append(*MakeStrong(T_("sn_claim_relay_for_you", "URnetwork claims this for you")));
    into.append(*row);
  }
  if (0 < status.claimable_rao) {
    auto* pending = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* pill = Gtk::make_managed<Gtk::Label>(T_("pending_data_key", "Pending"));
    pill->add_css_class("ur-earn-tag");
    pill->set_valign(Gtk::Align::START);
    pending->append(*pill);
    auto* reason = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    reason->set_hexpand(true);
    AppendReason(*reason, status.reason, status.automatic_threshold_tao_rao,
                 status.automatic_threshold_rao);
    pending->append(*reason);
    into.append(*pending);
  }
}

Gtk::Button* MakeLink(const Glib::ustring& text, const std::function<void()>& action) {
  auto* button = Gtk::make_managed<Gtk::Button>(text);
  button->add_css_class("flat");
  button->add_css_class("ur-key-link");
  button->set_halign(Gtk::Align::START);
  button->signal_clicked().connect([action] {
    if (action) action();
  });
  return button;
}

// A row's Transaction link (snledger::RelayRow): the explorer page it names,
// opened only as a web page. "" when there is none to open.
std::string RowTransactionLink(const std::string& url) {
  const std::string link = snledger::TransactionLink(url);
  if (link.empty() || !g_uri_is_valid(link.c_str(), G_URI_FLAGS_NONE, nullptr)) return {};
  return link;
}

// Whether a row links its claim's transaction, which the claimed banner then
// leaves out.
bool RowsLinkATransaction(const std::vector<snledger::RelayRow>& rows) {
  for (const snledger::RelayRow& row : rows) {
    if (row.status == snledger::RelayStatus::Claimed && !RowTransactionLink(row.txUrl).empty()) {
      return true;
    }
  }
  return false;
}

// URnetwork's claim, the latest (4d): the store's one sentence for it on every
// app, sn_claim_relay_claimed, with what it paid and the coldkey it is on, or
// Claimed alone without an amount or a coldkey to name; then its transaction,
// unless `transaction` is false: the sheet's rows link it.
// The sentence names SN25α, so the amount carries no symbol.
void AppendClaimedBanner(Gtk::Box& into, const urnet::SnClaimStatusResult& status,
                         const std::function<void(const std::string& url)>& openLink,
                         bool transaction = true) {
  auto* banner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  banner->add_css_class("ur-key-notice");
  banner->add_css_class("ur-key-notice-green");
  auto* icon = Gtk::make_managed<Gtk::Image>();
  icon->set_from_icon_name("emblem-ok-symbolic");
  icon->add_css_class("ur-key-icon-green");
  icon->set_valign(Gtk::Align::START);
  kit::MarkDecorative(*icon);
  banner->append(*icon);
  auto* lines = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
  lines->set_hexpand(true);
  const int64_t amountRao = status.claim_amount_rao.value_or(0);
  const std::string coldkey = status.coldkey_ss58.value_or(std::string());
  if (0 < amountRao && !coldkey.empty()) {
    const Glib::ustring text = Format(T_("sn_claim_relay_claimed",
                                         "Claimed by URnetwork. {0} SN25α is on your key, {1}."),
                                      urnet::formatAlphaAmount(amountRao), urnet::shortSs58(coldkey));
    auto* sentence = MakeText(text, "ur-body");
    sentence->set_tooltip_text(coldkey);
    kit::SetAccessibleLabel(
        *sentence, text + ", " + Glib::ustring(T_("bittensor_wallet", "Bittensor wallet")) + ", " + coldkey);
    lines->append(*sentence);
  } else {
    lines->append(*MakeStrong(T_("claim_confirmed", "Claimed")));
  }
  banner->append(*lines);
  into.append(*banner);
  const std::string link = transaction ? ClaimTransactionLink(status) : std::string();
  if (!link.empty()) {
    auto* transaction = MakeLink(T_("transaction", "Transaction"), [openLink, link] {
      if (openLink) openLink(link);
    });
    transaction->set_tooltip_text(link);
    into.append(*transaction);
  }
}

// After URnetwork's claim: when Claim now is available again for this
// coldkey, the SDK's date. "" without one still to come.
Glib::ustring ClaimedAgainNote(const urnet::SnClaimStatusResult& status) {
  const int64_t nextAtMillis = status.claim_now_next_at_millis.value_or(0);
  if (nextAtMillis <= g_get_real_time() / 1000) return {};
  const std::string date = FormatDate(nextAtMillis);
  if (date.empty()) return {};
  return Format(T_("sn_claim_relay_again_on", "Your next Claim now opens on {}."), date);
}

}  // namespace

bool ClaimRelayClaims(const urnet::SnClaimStatusResult& status) {
  return status.reason != urnet::SnClaimReasonRelayNotRunning &&
         status.reason != urnet::SnClaimReasonRelayPaused;
}

std::string ClaimTransactionLink(const urnet::SnClaimStatusResult& status) {
  const std::string link = snledger::TransactionLink(status.claim_tx_url.value_or(std::string()));
  if (link.empty() || !g_uri_is_valid(link.c_str(), G_URI_FLAGS_NONE, nullptr)) return {};
  return link;
}

Glib::ustring LedgerPoolName(snledger::PoolName pool, int64_t noId) {
  switch (pool) {
    case snledger::PoolName::None:
      return {};
    case snledger::PoolName::Numbered:
      return Format(T_("sn_ledger_pool", "Pool {}"), noId);
    case snledger::PoolName::Provider:
      return T_("sn_ledger_provider_pool", "Provider pool");
    case snledger::PoolName::Ctf:
      return T_("sn_ctf_pool", "Capture the flag");
  }
  return {};
}

// the pure rules (SnLedgerPresentation.hpp) read the SDK's own words
static_assert(std::string_view(snledger::kKindPool) == urnet::SnLedgerPoolKindPool,
              "snledger::kKindPool is the SDK's SnLedgerPoolKindPool");
static_assert(std::string_view(snledger::kKindCtf) == urnet::SnLedgerPoolKindCtf,
              "snledger::kKindCtf is the SDK's SnLedgerPoolKindCtf");
static_assert(snledger::kCtfPoolIdBase == urnet::SnLedgerCtfPoolIdBase,
              "snledger::kCtfPoolIdBase is the SDK's SnLedgerCtfPoolIdBase");
static_assert(std::string_view(snledger::kRoleProvider) == urnet::SnCtfRoleProvider,
              "snledger::kRoleProvider is the SDK's SnCtfRoleProvider");
static_assert(std::string_view(snledger::kRoleExtender) == urnet::SnCtfRoleExtender,
              "snledger::kRoleExtender is the SDK's SnCtfRoleExtender");

std::vector<snledger::UnavailablePool> UnavailablePoolsOf(
    const std::optional<urnet::SnLedgerUnavailablePoolList>& pools) {
  std::vector<snledger::UnavailablePool> out;
  if (!pools) return out;
  for (const urnet::SnLedgerUnavailablePool& pool : *pools) {
    snledger::UnavailablePool row;
    row.noId = pool.no_id;
    row.kind = pool.kind;
    out.push_back(row);
  }
  return out;
}

std::vector<snledger::RelayAccount> RelayAccountsOf(const urnet::SnClaimStatusResult& status) {
  std::vector<snledger::RelayAccount> out;
  if (!status.accounts) return out;
  for (const urnet::SnClaimRelayAccount& account : *status.accounts) {
    snledger::RelayAccount row;
    row.coldkeySs58 = account.coldkey_ss58;
    row.noId = account.no_id;
    row.kind = account.kind;
    row.claimableRao = account.claimable_rao;
    row.claimTxHash = account.claim_tx_hash.value_or(std::string());
    row.claimTxUrl = account.claim_tx_url.value_or(std::string());
    row.claimAmountRao = account.claim_amount_rao.value_or(0);
    out.push_back(row);
  }
  return out;
}

void AppendRelayRows(Gtk::Box& into, const std::vector<snledger::RelayRow>& rows,
                     const std::function<void(const std::string& url)>& openLink) {
  if (rows.empty()) return;
  auto* list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  for (const snledger::RelayRow& row : rows) {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    Glib::ustring name = LedgerPoolName(row.pool, row.noId);
    if (!row.coldkeySs58.empty()) name += " · " + urnet::shortSs58(row.coldkeySs58);
    auto* title = MakeText(name, "ur-body");
    title->set_hexpand(true);
    if (!row.coldkeySs58.empty()) title->set_tooltip_text(row.coldkeySs58);
    line->append(*title);
    if (row.status != snledger::RelayStatus::Unavailable && 0 < row.amountRao) {
      auto* amount = MakeText(urnet::formatAlpha(row.amountRao), "ur-value");
      amount->set_wrap(false);
      amount->set_valign(Gtk::Align::CENTER);
      line->append(*amount);
    }
    Glib::ustring status;
    switch (row.status) {
      case snledger::RelayStatus::Pending:
        status = T_("pending_data_key", "Pending");
        break;
      case snledger::RelayStatus::UnderFloor:
        status = T_("sn_ledger_pool_under_floor", "Below the claim minimum");
        break;
      case snledger::RelayStatus::Claimed:
        status = T_("claim_confirmed", "Claimed");
        break;
      case snledger::RelayStatus::Unavailable:
        status = T_("sn_ledger_pool_unavailable", "Unavailable right now");
        break;
    }
    auto* pill = Gtk::make_managed<Gtk::Label>(status);
    pill->add_css_class("ur-earn-tag");
    pill->set_valign(Gtk::Align::CENTER);
    line->append(*pill);
    list->append(*line);
    const std::string link =
        row.status == snledger::RelayStatus::Claimed ? RowTransactionLink(row.txUrl) : std::string();
    if (!link.empty()) {
      auto* transaction = MakeLink(T_("transaction", "Transaction"), [openLink, link] {
        if (openLink) openLink(link);
      });
      transaction->set_tooltip_text(link);
      list->append(*transaction);
    }
  }
  into.append(*list);
}

void AppendClaimRelaySection(Gtk::Box& into, const urnet::SnClaimStatusResult& status,
                             const std::function<void()>& claimNow,
                             const std::function<void()>& selfClaim,
                             const std::function<void(const std::string& url)>& openLink) {
  const bool claims = ClaimRelayClaims(status);
  AppendWhoAndWhy(into, status);
  if (status.shows_claimed) AppendClaimedBanner(into, status, openLink);
  auto* claimNowButton =
      Gtk::make_managed<Gtk::Button>(T_("sn_claim_relay_claim_now", "Claim now"));
  claimNowButton->add_css_class("ur-pane-secondary");
  claimNowButton->signal_clicked().connect([claimNow] {
    if (claimNow) claimNow();
  });
  into.append(*claimNowButton);
  if (claims) {
    claimNowButton->set_sensitive(status.claim_now_available);
    // after URnetwork's claim, when Claim now is available again (4d)
    Glib::ustring note = status.shows_claimed && !status.claim_now_available
                             ? ClaimedAgainNote(status)
                             : Glib::ustring();
    if (note.empty()) {
      note = ClaimNowNote(status.claim_now_available, status.claim_now_reason.value_or(std::string()),
                          status.claim_now_next_at_millis.value_or(0), status.claim_now_period_days,
                          status.transfer_floor_tao_rao, /*eachColdkey=*/false);
    }
    if (!note.empty()) into.append(*MakeText(note, "ur-row-note"));
    return;
  }
  claimNowButton->set_sensitive(false);
  into.append(*MakeText(T_("sn_claim_relay_waits_for_relay",
                           "Claim now waits until claims are running again. To claim right away, "
                           "pay the gas yourself:"),
                        "ur-row-note"));
  if (status.self_claim_available) {
    into.append(*MakeLink(T_("sn_claim_relay_self_claim", "Claim it yourself with your gas key"),
                          selfClaim));
  }
}

ClaimRelaySheet::ClaimRelaySheet(Gtk::Window& parent) {
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("claim_alpha_title", "Claim SN25α"));
  set_default_size(kSheetWidth, -1);
  add_css_class("ur-sheet");

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  column->set_margin(24);
  column->set_size_request(kSheetWidth, -1);
  column->append(*MakeText(T_("sn_ledger_title", "Settlement v2"), "ur-caption"));
  column->append(*MakeText(T_("claim_alpha_title", "Claim SN25α"), "ur-step-heading"));
  content_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  column->append(*content_);
  disclaimer_ = MakeBittensorFundsDisclaimer(/*boxed=*/true);
  column->append(*disclaimer_);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_height(true);
  scroller->set_max_content_height(kSheetMaxHeight);
  scroller->set_child(*column);
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->append(*scroller);
  close_ = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  close_->set_halign(Gtk::Align::END);
  close_->set_margin(16);
  close_->signal_clicked().connect([this] { set_visible(false); });
  root->append(*close_);
  set_child(*root);
}

void ClaimRelaySheet::Apply(const std::optional<urnet::SnClaimStatusResult>& status,
                            const ClaimNowOutcome& outcome, bool claimingNow,
                            const std::string& fundsDisclaimer) {
  RemoveAllChildren(*content_);
  SetBittensorFundsDisclaimer(*disclaimer_, fundsDisclaimer);
  // URnetwork's claim is the latest (4d): Done takes the footer's place
  const bool claimed = status && !status->error && status->shows_claimed;
  close_->set_visible(!claimed);
  if (!status || status->error) {
    // unavailable, never zero
    content_->append(*MakeText(T_("sn_ledger_unavailable", "Unavailable"), "ur-step-heading"));
    return;
  }
  content_->append(*MakeText(T_("sn_ledger_claimable", "Claimable"), "ur-caption"));
  auto* amount = Gtk::make_managed<Gtk::Label>();
  amount->add_css_class("ur-stat-value");
  amount->set_xalign(0);
  amount->set_markup("<span size='" + std::to_string(36 * PANGO_SCALE) + "'>" +
                     Glib::Markup::escape_text(urnet::formatAlpha(status->claimable_rao)) +
                     "</span>");
  content_->append(*amount);
  const std::string coldkey = status->coldkey_ss58.value_or(std::string());
  if (!coldkey.empty()) {
    auto* lands = MakeText(Format(T_("sn_claim_relay_lands_on",
                                     "URnetwork claims this for you. It pays the gas, and your "
                                     "SN25α lands on {}."),
                                  urnet::shortSs58(coldkey)),
                           "ur-body");
    lands->set_tooltip_text(coldkey);
    content_->append(*lands);
  }
  const auto openLink = [this](const std::string& url) {
    if (on_open_link) on_open_link(url);
  };
  // each pool's account, beside capture the flag or a pool that could not be read
  const std::vector<snledger::RelayRow> rows =
      snledger::RelayRowsFor(RelayAccountsOf(*status), UnavailablePoolsOf(status->unavailable_pools),
                             status->transfer_floor_rao);
  AppendRelayRows(*content_, rows, openLink);
  if (claimed) {
    // the claim with its transaction, where no row links it, Done, and when
    // Claim now is available again; a Claim now tap's answer is spent
    AppendClaimedBanner(*content_, *status, openLink, /*transaction=*/!RowsLinkATransaction(rows));
    auto* done = Gtk::make_managed<Gtk::Button>(T_("done", "Done"));
    done->add_css_class("ur-pane-primary");
    done->signal_clicked().connect([this] { set_visible(false); });
    content_->append(*done);
    const Glib::ustring again = ClaimedAgainNote(*status);
    if (!again.empty()) {
      auto* note = MakeText(again, "ur-row-note");
      note->set_xalign(0.5);
      note->set_justify(Gtk::Justification::CENTER);
      content_->append(*note);
    }
    return;
  }
  // who claims and why it waits, as on the tile; the sheet has its own Claim now
  const bool claims = ClaimRelayClaims(*status);
  AppendWhoAndWhy(*content_, *status);

  switch (outcome.kind) {
    case ClaimNowOutcome::Kind::Requested: {
      // the queued note, with when the next Claim now opens (the relay's period)
      content_->append(*MakeText(ClaimNowQueuedNote(status->claim_now_period_days), "ur-body"));
      auto* queued = Gtk::make_managed<Gtk::Button>(T_("sn_claim_relay_queued_button", "Queued"));
      queued->add_css_class("ur-pane-primary");
      queued->set_sensitive(false);
      content_->append(*queued);
      break;
    }
    case ClaimNowOutcome::Kind::Refused: {
      const Glib::ustring note =
          ClaimNowNote(false, outcome.reason, outcome.nextAtMillis, status->claim_now_period_days,
                       status->transfer_floor_tao_rao, /*eachColdkey=*/true);
      if (!note.empty()) content_->append(*MakeText(note, "ur-row-note"));
      break;
    }
    case ClaimNowOutcome::Kind::Failed: {
      auto* failed = MakeText(outcome.message, "ur-body");
      failed->add_css_class("ur-danger-text");
      content_->append(*failed);
      break;
    }
    case ClaimNowOutcome::Kind::None: {
      if (!claims) {
        content_->append(*MakeText(T_("sn_claim_relay_waits_for_relay",
                                      "Claim now waits until claims are running again. To claim "
                                      "right away, pay the gas yourself:"),
                                   "ur-row-note"));
        break;
      }
      auto* claimNow = Gtk::make_managed<Gtk::Button>();
      auto* inside = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      inside->set_halign(Gtk::Align::CENTER);
      inside->append(*Gtk::make_managed<Gtk::Label>(T_("sn_claim_relay_claim_now", "Claim now")));
      if (claimingNow) {
        auto* spinner = Gtk::make_managed<Gtk::Spinner>();
        spinner->set_spinning(true);
        inside->append(*spinner);
      }
      claimNow->set_child(*inside);
      kit::SetAccessibleLabel(*claimNow, T_("sn_claim_relay_claim_now", "Claim now"));
      claimNow->add_css_class("ur-pane-primary");
      claimNow->set_sensitive(status->claim_now_available && !claimingNow);
      claimNow->signal_clicked().connect([this] {
        if (on_claim_now) on_claim_now();
      });
      content_->append(*claimNow);
      const Glib::ustring note = ClaimNowNote(
          status->claim_now_available, status->claim_now_reason.value_or(std::string()),
          status->claim_now_next_at_millis.value_or(0), status->claim_now_period_days,
          status->transfer_floor_tao_rao, /*eachColdkey=*/true);
      if (!note.empty()) content_->append(*MakeText(note, "ur-row-note"));
      break;
    }
  }
  if (status->self_claim_available) {
    content_->append(*MakeLink(T_("sn_claim_relay_self_claim", "Claim it yourself with your gas key"),
                               [this] {
                                 if (on_self_claim) on_self_claim();
                               }));
  }
}

}  // namespace urnw
