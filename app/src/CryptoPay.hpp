// The payment-method toggle (Card · USDC on Solana · Zcash · Monero · More
// coins) and the native crypto invoice (mmm/ZCASH-CONTRACT.md §6, mmm/ZCASH.md
// §0.1 and §0.3, mmm/BTCPAY.md §17.6), the half that needs no GTK and no SDK,
// so tests/CryptoPayTest.cpp pins it.
//
// The SDK owns the money: which coins the server enabled and how the widget
// lays them out (urnet::cryptoPaymentTabs over the balance result's
// crypto_assets: the tabs, their order, their coins, their labels and which
// coins can be paid with now), the invoice itself
// (urnet::CryptoInvoiceViewController: quote, countdown, re-quote, status, the
// saved invoice, the text its QR code holds) and the payment URI check
// (urnet::checkCryptoPaymentUri). This header only decides what the screens
// show from those answers:
//
//   * the method row: Card first, then the SDK's tabs, USDC on Solana, Zcash,
//     Monero and More coins, which opens a list of TAO, Bitcoin, ETH and SOL;
//     no row at all with no coin listed, so the surface looks exactly as
//     before; a coin that cannot be paid with now stays in its place, greyed,
//     and a remembered one falls back to Card;
//   * a coin buys the yearly plan, or the welcome offer (yearly_onboarding)
//     while it is active. Monthly is card only: picking a coin greys the
//     monthly card and moves the selection to yearly;
//   * the invoice, top to bottom: the network line, the QR (the SDK's qr text:
//     a URI that passed the SDK's check, or TAO's address alone) and Open in
//     wallet (only for such a URI), both only while the quote can still be
//     paid, the exact amount, the address with the coin's network warning and
//     the memo of a coin on Solana, the countdown with Refresh quote (the
//     expired line once it ran out), the status steps (waiting -> payment seen
//     -> confirming n of N -> paid; USDC is never seen or confirming, so it
//     has two), what is left to send after a partial payment, the review copy
//     alone for a payment in review, the wallets that work, and Zcash's
//     exchange note. A coin with no payment link (TAO) says to send exactly
//     this amount to this address, its Copy buttons say what they copy, and
//     its QR says it holds only the address;
//   * the renewal notice of a one-time crypto year (§2.2 crypto_term), and the
//     "does not renew" reading of a crypto store.
//
// Copy lives with the screens (T_ calls in PaymentMethodRow.cpp,
// CryptoInvoiceView.cpp, UpgradeSheet.cpp, Onboarding.cpp and AccountPage.cpp)
// except the per-id tables below, whose {key, English} pairs the catalog test
// reads like any lookup. The tabs' and coins' labels are the SDK's, a store key
// with its English, looked up as it hands them out (PaymentMethodRow.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::crypto_pay {

// ---- ids (the SDK's crypto_pay.go constants and the server's vocabulary) ----

// A method is Card or a coin; a coin is named by its asset id.
inline constexpr const char* kMethodCard = "card";            // urnet::PaymentMethodCard
inline constexpr const char* kAssetUsdcSolana = "usdc_solana";  // urnet::CryptoAssetUsdcSolana
inline constexpr const char* kAssetZec = "zec";                // urnet::CryptoAssetZec
inline constexpr const char* kAssetXmr = "xmr";                // urnet::CryptoAssetXmr
inline constexpr const char* kAssetTao = "tao";                // urnet::CryptoAssetTao
inline constexpr const char* kAssetBtc = "btc";                // urnet::CryptoAssetBtc
inline constexpr const char* kAssetEth = "eth";                // urnet::CryptoAssetEth
inline constexpr const char* kAssetSol = "sol";                // urnet::CryptoAssetSol

// The selector's tab of the coins beyond the main three, which opens a list of
// them (urnet::CryptoPaymentTabMoreCoins). A tab of one coin is named by its
// asset.
inline constexpr const char* kTabMoreCoins = "more_coins";

// The coins this app has words for (the network lines and warnings below), in
// the SDK's order: every coin the SDK lays out.
inline constexpr const char* kCoins[] = {kAssetUsdcSolana, kAssetZec, kAssetXmr, kAssetTao,
                                         kAssetBtc,        kAssetEth, kAssetSol};

// The plans a coin sells (mmm/ZCASH-CONTRACT.md §1 "item"; decision 6: no coin sells
// monthly).
inline constexpr const char* kPlanYearly = "yearly";
inline constexpr const char* kPlanYearlyOnboarding = "yearly_onboarding";

// The invoice's phase (urnet::CryptoInvoiceState.Phase, §4.4).
inline constexpr const char* kPhaseNone = "none";
inline constexpr const char* kPhaseStarting = "starting";
inline constexpr const char* kPhaseOpen = "open";
inline constexpr const char* kPhasePaid = "paid";
inline constexpr const char* kPhaseFailed = "failed";

// The invoice's status (§2.5), first match wins on the server.
inline constexpr const char* kStatusUnknown = "unknown";
inline constexpr const char* kStatusPaid = "paid";
inline constexpr const char* kStatusReview = "review";
inline constexpr const char* kStatusConfirming = "confirming";
inline constexpr const char* kStatusSeen = "seen";
inline constexpr const char* kStatusExpired = "expired";
inline constexpr const char* kStatusPending = "pending";

// The purchase error codes the crypto endpoints add (§2.7), and the one every
// purchase endpoint shares for a legacy guest network.
inline constexpr const char* kErrorAssetUnavailable = "asset_unavailable";
inline constexpr const char* kErrorQuoteExpired = "quote_expired";
inline constexpr const char* kErrorUnderpaid = "underpaid";
inline constexpr const char* kErrorGuestSignInRequired = "guest_sign_in_required";

// The store family urnet::classifySubscriptionStore gives a one-time crypto
// renewal (every coin's market; §4.3): it never renews.
inline constexpr const char* kStoreFamilyCrypto = "crypto";

// A store key and its English, the gettext msgid (I18n.hpp T_).
struct Text {
  const char* key;
  const char* english;
};

// A coin of this app's (kCoins): not Card, nothing, a ticker or a tab.
inline bool IsCoin(std::string_view method) {
  return std::any_of(std::begin(kCoins), std::end(kCoins),
                     [method](const char* coin) { return method == coin; });
}

// The invoice's first line: the network to send on, named before anything
// else (the USDC panel's lesson: "which network do I select?"). The More coins
// list names it under each coin too. {nullptr, nullptr} for an id this app
// does not know.
inline Text NetworkLine(std::string_view asset) {
  if (asset == kAssetUsdcSolana) return {"crypto_network_usdc", "USDC on Solana"};
  if (asset == kAssetZec) return {"crypto_network_zcash", "Zcash, shielded"};
  if (asset == kAssetXmr) return {"crypto_network_monero", "Monero"};
  if (asset == kAssetTao) return {"crypto_network_tao", "Bittensor network"};
  if (asset == kAssetBtc) return {"crypto_network_bitcoin", "Bitcoin network"};
  if (asset == kAssetEth) return {"crypto_network_eth", "Ethereum mainnet"};
  if (asset == kAssetSol) return {"crypto_network_sol", "Solana (native SOL)"};
  return {nullptr, nullptr};
}

// The warning beside the address: the network to send on, and that anything
// sent on another is not credited. {nullptr, nullptr} for a coin without one
// (Zcash and Monero).
inline Text NetworkWarning(std::string_view asset) {
  if (asset == kAssetUsdcSolana) {
    return {"usdc_solana_network_only",
            "Send USDC on the Solana network only. USDC sent on any other network will not arrive."};
  }
  if (asset == kAssetTao) {
    return {"crypto_invoice_tao_network_only",
            "Send only TAO on the Bittensor network to this address. TAO sent on any other network "
            "won't be credited."};
  }
  if (asset == kAssetBtc) {
    return {"crypto_invoice_bitcoin_network_only",
            "Send only Bitcoin on the Bitcoin network to this address. Bitcoin sent on any other "
            "network won't be credited."};
  }
  if (asset == kAssetEth) {
    return {"crypto_invoice_eth_network_only",
            "Send only ETH on Ethereum mainnet to this address. Tokens and ETH on other networks, "
            "such as Base or Arbitrum, won't be credited."};
  }
  if (asset == kAssetSol) {
    return {"crypto_invoice_sol_network_only",
            "Send only SOL on the Solana network to this address. Tokens, including USDC, won't be "
            "credited."};
  }
  return {nullptr, nullptr};
}

// Under the QR of a coin with no payment link: it holds only the address.
// {nullptr, nullptr} for any other coin.
inline Text NoPaymentLinkNote(std::string_view asset) {
  if (asset == kAssetTao) {
    return {"crypto_invoice_tao_no_payment_link",
            "TAO has no payment link, so this QR code holds only the address. Enter the exact "
            "amount in your wallet."};
  }
  return {nullptr, nullptr};
}

// ---- the method row -----------------------------------------------------------

// A coin of the selector (urnet::CryptoPaymentCoin, read by
// SubscriptionBalanceStore::PaymentTabs).
struct PaymentCoinView {
  std::string asset;
  std::string label;     // in English: the msgid its key is looked up with
  std::string labelKey;  // the store key of its label
  std::string iconKey;   // its mark: the installed coins/<iconKey>.svg
  // payable now. A coin that is not stays in its place and cannot be chosen:
  // its label is greyed and says "Temporarily unavailable", and its mark stays
  // at full contrast (Solana's and Bittensor's brand rules).
  bool available = false;
};

inline bool operator==(const PaymentCoinView& a, const PaymentCoinView& b) {
  return a.asset == b.asset && a.label == b.label && a.labelKey == b.labelKey &&
         a.iconKey == b.iconKey && a.available == b.available;
}
inline bool operator!=(const PaymentCoinView& a, const PaymentCoinView& b) { return !(a == b); }

// A tab of the selector (urnet::CryptoPaymentTab).
struct PaymentTabView {
  std::string id;  // the asset of a tab of one coin, or kTabMoreCoins
  std::string label;
  std::string labelKey;
  // the marks the tab shows, in order: for More coins each coin's, as separate
  // marks with space between them, never one badge
  std::vector<std::string> iconKeys;
  std::vector<PaymentCoinView> coins;  // in the list's order
  bool available = false;              // one of its coins is
};

inline bool operator==(const PaymentTabView& a, const PaymentTabView& b) {
  return a.id == b.id && a.label == b.label && a.labelKey == b.labelKey &&
         a.iconKeys == b.iconKeys && a.coins == b.coins && a.available == b.available;
}
inline bool operator!=(const PaymentTabView& a, const PaymentTabView& b) { return !(a == b); }

// The row shows only while the SDK lays a selector out: its tabs are empty when
// no coin the server lists sells the surface's plan, and the surface then looks
// exactly as before the toggle (§6.1).
inline bool MethodRowShown(const std::vector<PaymentTabView>& tabs) { return !tabs.empty(); }

// The coin `asset` among the tabs, or nullptr.
inline const PaymentCoinView* FindCoin(const std::vector<PaymentTabView>& tabs,
                                       std::string_view asset) {
  for (const PaymentTabView& tab : tabs) {
    for (const PaymentCoinView& coin : tab.coins) {
      if (coin.asset == asset) return &coin;
    }
  }
  return nullptr;
}

// Whether the row offers `method` now: Card always, a coin of this app's while
// the selector has it available.
inline bool MethodOffered(const std::vector<PaymentTabView>& tabs, std::string_view method) {
  if (method == kMethodCard) return true;
  if (!IsCoin(method)) return false;
  const PaymentCoinView* coin = FindCoin(tabs, method);
  return coin != nullptr && coin->available;
}

// The selection a surface opens with: the method remembered on this device
// while the row offers it, else Card (a remembered coin that cannot be paid
// with now, or that the server switched off, falls back to Card).
inline std::string MethodSelection(const std::vector<PaymentTabView>& tabs,
                                   const std::string& remembered) {
  return MethodOffered(tabs, remembered) ? remembered : std::string(kMethodCard);
}

// A tab opens a list of its coins (More coins); a tab of one coin is that coin.
inline bool TabOpensCoins(const PaymentTabView& tab) { return tab.id == kTabMoreCoins; }

// The coin choosing `tab` selects: `wanted` (the coin chosen there last, or
// remembered) while it is one of the tab's available coins, else the tab's
// first available coin; "" when none is, and the tab cannot be chosen.
inline std::string TabCoinSelection(const PaymentTabView& tab, const std::string& wanted) {
  for (const PaymentCoinView& coin : tab.coins) {
    if (coin.available && coin.asset == wanted && IsCoin(coin.asset)) return coin.asset;
  }
  for (const PaymentCoinView& coin : tab.coins) {
    if (coin.available && IsCoin(coin.asset)) return coin.asset;
  }
  return std::string();
}

// The names the More coins tab stands for ("TAO, Bitcoin, ETH, SOL"), for
// pay_method_more_coins_a11y: its marks may say what its label does not.
inline std::string JoinNames(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) {
    if (name.empty()) continue;
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out;
}

// Monthly is card only (urnet::paymentMethodAllowsPlan(method, "monthly")).
inline bool MonthlyAllowed(std::string_view method) { return !IsCoin(method); }

// The yearly plan card's lines between its price and its trial line, top to
// bottom, for the selected method (§6.1: no renewal or trial wording while a
// coin is selected). Card shows them all. A coin buys one year once: the
// offer's renewal line ("then $40/year", `secondaryRenews`) goes, while "Save
// N%" and the monthly equivalent stay. The trial line itself is the card's
// alone (PlanPicker hides it for a coin). Empty texts are no line.
inline std::vector<std::string> YearlyCardLines(const std::string& secondary, bool secondaryRenews,
                                                const std::string& equivalent, bool coin) {
  std::vector<std::string> lines;
  if (!secondary.empty() && !(coin && secondaryRenews)) lines.push_back(secondary);
  if (!equivalent.empty()) lines.push_back(equivalent);
  return lines;
}

// The plan a coin buys: the welcome offer while the surface shows it, the
// yearly plan otherwise. It is also the one item the row asks
// urnet::paymentMethods about, so a coin is listed only when it sells exactly
// what the plan card prices.
inline const char* CoinPlan(bool offerActive) {
  return offerActive ? kPlanYearlyOnboarding : kPlanYearly;
}

// What a refused start leads to (§6.1). asset_unavailable keeps the coin
// selectable with "Temporarily unavailable" under the row; a legacy guest
// goes to the conversion, like a refused card checkout; anything else is the
// surface's own refusal line (ServerRefusalText.hpp).
enum class StartRefusal { None, Unavailable, GuestSignIn, Other };

inline StartRefusal StartRefusalFor(const std::string& errorCode, bool failed) {
  if (errorCode == kErrorAssetUnavailable) return StartRefusal::Unavailable;
  if (errorCode == kErrorGuestSignInRequired) return StartRefusal::GuestSignIn;
  if (!errorCode.empty() || failed) return StartRefusal::Other;
  return StartRefusal::None;
}

// ---- the invoice --------------------------------------------------------------

// urnet::CryptoInvoiceState as the screens read it, plus the answers the host
// asks the SDK for beside it: whether the URI passed
// urnet::checkCryptoPaymentUri, urnet::cryptoWalletSuggestions, and whether
// the coin has no payment link (urnet::cryptoPaymentUriScheme is empty: TAO).
struct InvoiceView {
  std::string phase;  // none | starting | open | paid | failed
  std::string intentId;
  std::string asset;
  std::string plan;
  std::string itemId;
  std::string address;
  std::string amount;  // exact decimal string, never a float
  std::string ticker;  // USDC | ZEC | XMR | TAO | BTC | ETH | SOL
  std::string uri;     // built by the server; shown and opened, never built here
  // what the QR code holds (urnet::cryptoPaymentQrText): the URI once the
  // SDK's check passed it, or for a coin with no payment link the address
  // alone; "" for no QR code
  std::string qrText;
  std::string memo;  // usdc_solana and sol: the memo for a transfer sent by hand
  double priceUsd = 0;
  std::string expiresAt;
  int64_t secondsLeft = 0;  // 0 once expired
  std::string status;       // §2.5
  std::string reviewReason;
  std::string received;
  std::string remaining;
  int64_t confirmations = 0;
  int64_t confirmationsRequired = 0;
  bool refreshing = false;  // a re-quote is in flight
  std::string errorCode;    // the last start or re-quote failure
  std::string errorMessage;
  // beside the state
  bool uriChecked = false;
  std::vector<std::string> wallets;
  bool noPaymentLink = false;
};

enum class InvoiceStage { None, Starting, Open, Paid, Failed };

inline InvoiceStage StageOf(const InvoiceView& invoice) {
  // a credited invoice is paid whatever phase the state reached
  if (invoice.phase == kPhasePaid || invoice.status == kStatusPaid) return InvoiceStage::Paid;
  if (invoice.phase == kPhaseStarting) return InvoiceStage::Starting;
  if (invoice.phase == kPhaseOpen) return InvoiceStage::Open;
  if (invoice.phase == kPhaseFailed) return InvoiceStage::Failed;
  return InvoiceStage::None;
}

// Whether the invoice belongs on screen as a payment still going on: the
// "payment in progress" entry, and the upgrade sheet opening onto it.
inline bool InvoiceInProgress(const InvoiceView& invoice) {
  return StageOf(invoice) == InvoiceStage::Open && invoice.status != kStatusUnknown;
}

// m:ss, minutes unbounded ("29:59", "0:05", "60:00"); never negative.
inline std::string FormatCountdown(int64_t seconds) {
  if (seconds < 0) seconds = 0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(seconds / 60),
                static_cast<long long>(seconds % 60));
  return buf;
}

// "0.072387 XMR": the exact amount and its ticker.
inline std::string AmountWithTicker(const std::string& amount, const std::string& ticker) {
  if (ticker.empty()) return amount;
  return amount + " " + ticker;
}

// A decimal string above zero ("0.000100" is, "0" and "0.000000" are not).
inline bool AmountAboveZero(const std::string& amount) {
  return std::any_of(amount.begin(), amount.end(), [](char c) { return '1' <= c && c <= '9'; });
}

// The wallet names as one list ("Cake, Feather, Monerujo").
inline std::string JoinWallets(const std::vector<std::string>& wallets) {
  std::string out;
  for (const std::string& wallet : wallets) {
    if (wallet.empty()) continue;
    if (!out.empty()) out += ", ";
    out += wallet;
  }
  return out;
}

// The status steps: waiting -> payment seen -> confirming n of N -> paid.
enum class StepKind { Waiting, Seen, Confirming, Paid };
enum class StepState { Done, Current, Upcoming };
struct Step {
  StepKind kind;
  StepState state;
};

// The steps an asset goes through. USDC is reported pending, paid or expired
// only (the scanner sees confirmed transfers), so it has no seen or
// confirming step to wait on.
inline std::vector<Step> StepsFor(const std::string& asset, const std::string& status) {
  std::vector<StepKind> kinds;
  if (asset == kAssetUsdcSolana) {
    kinds = {StepKind::Waiting, StepKind::Paid};
  } else {
    kinds = {StepKind::Waiting, StepKind::Seen, StepKind::Confirming, StepKind::Paid};
  }
  StepKind current = StepKind::Waiting;
  if (status == kStatusSeen) current = StepKind::Seen;
  if (status == kStatusConfirming) current = StepKind::Confirming;
  if (status == kStatusPaid) current = StepKind::Paid;
  std::vector<Step> steps;
  bool reached = false;
  for (const StepKind kind : kinds) {
    if (kind == current) {
      // paid is the last step: it reads as done, not as something to wait on
      steps.push_back({kind, kind == StepKind::Paid ? StepState::Done : StepState::Current});
      reached = true;
    } else {
      steps.push_back({kind, reached ? StepState::Upcoming : StepState::Done});
    }
  }
  return steps;
}

// The line under the invoice when something went wrong with it: a coin that
// cannot be quoted now (asset_unavailable on a re-quote), or an invoice the
// server no longer knows or a re-quote it refused. An expired quote and an
// underpayment are not failures here: the expired line and the review copy
// say those.
enum class InvoiceNotice { None, Unavailable, Failed };

// What the invoice shows, top to bottom (§6.2).
struct InvoicePresentation {
  // the coin's network warning, under the address ({nullptr, nullptr}: none)
  bool showNetworkWarning = false;
  Text networkWarning{nullptr, nullptr};
  bool showQr = false;         // the QR
  std::string qrText;          // what it encodes while it shows
  bool showOpenWallet = false; // a checked URI to hand to a wallet
  // a coin with no payment link (TAO): "Send exactly this amount to this
  // address." while payable, Copy amount and Copy address on the buttons, and
  // under its QR (the address alone) the note saying so
  bool noPaymentLink = false;
  bool showByHandLine = false;
  Text noPaymentLinkNote{nullptr, nullptr};
  bool showAmount = false;    // Send exactly, the amount and Copy
  std::string amountText;     // "0.072387 XMR"
  bool showAddress = false;   // To this address, the address and Copy
  bool showMemo = false;      // the memo of a coin on Solana, and Copy
  bool showCountdown = false; // Quote expires in m:ss
  std::string countdownText;  // m:ss
  bool expired = false;       // the expired line
  bool showRefresh = false;   // Refresh quote
  bool refreshEnabled = false;
  std::vector<Step> steps;    // empty: no steps (expired, review, failed)
  int64_t confirmations = 0;
  int64_t confirmationsRequired = 0;
  bool showRemaining = false;  // "{amount} more to send"
  std::string remainingText;   // "0.010000 XMR"
  bool review = false;         // the review copy, and nothing else to do
  bool showWallets = false;
  std::string walletsText;
  bool zcashNote = false;
  InvoiceNotice notice = InvoiceNotice::None;
  bool showPayAnotherWay = false;  // only while nothing was received
};

inline InvoicePresentation PresentInvoice(const InvoiceView& invoice) {
  InvoicePresentation p;
  const InvoiceStage stage = StageOf(invoice);
  const bool open = stage == InvoiceStage::Open;
  const std::string& status = invoice.status;
  // Nothing received, as the server said: the only time a re-quote is allowed
  // (§2.6), and the only time walking away from the invoice cannot strand
  // money. A resumed invoice has no status until its first poll (empty), and
  // money may already be on its way then: it shows only its waiting steps
  // until the server answers (CONTRACT §6.2.1).
  const bool nothingReceived = status == kStatusPending || status == kStatusExpired;
  // The quote ran out: the server says so, or the countdown reached zero
  // first. Paying a stale amount is not offered: the amount, the QR and Open
  // in wallet wait for a fresh quote.
  p.expired = open && (status == kStatusExpired ||
                       (status == kStatusPending && invoice.secondsLeft <= 0));
  const bool payable = open && status == kStatusPending && !p.expired;
  // something arrived but not enough: the address stays for the top-up
  const bool partial = open && status == kStatusSeen;

  p.showAmount = payable;
  p.amountText = AmountWithTicker(invoice.amount, invoice.ticker);
  // The QR draws the SDK's qr text: the server's URI, only once the SDK has
  // checked its scheme and address, or for a coin with no payment link the
  // bare address. Open in wallet hands over such a URI alone.
  const bool qrIsUri =
      invoice.uriChecked && !invoice.uri.empty() && invoice.qrText == invoice.uri;
  const bool qrIsAddress = invoice.noPaymentLink && invoice.uri.empty() &&
                           !invoice.address.empty() && invoice.qrText == invoice.address;
  p.showQr = payable && (qrIsUri || qrIsAddress);
  p.qrText = p.showQr ? invoice.qrText : std::string();
  p.showOpenWallet = payable && invoice.uriChecked && !invoice.uri.empty();
  p.noPaymentLink = invoice.noPaymentLink;
  p.showByHandLine = invoice.noPaymentLink && p.showAmount;
  if (p.showQr && qrIsAddress) p.noPaymentLinkNote = NoPaymentLinkNote(invoice.asset);
  p.showAddress = (payable || partial) && !invoice.address.empty();
  p.showMemo = payable && (invoice.asset == kAssetUsdcSolana || invoice.asset == kAssetSol) &&
               !invoice.memo.empty();
  p.networkWarning = NetworkWarning(invoice.asset);
  p.showNetworkWarning = p.showAddress && p.networkWarning.key != nullptr;
  p.showCountdown = payable;
  p.countdownText = FormatCountdown(invoice.secondsLeft);
  p.showRefresh = open && nothingReceived;
  p.refreshEnabled = p.showRefresh && !invoice.refreshing;

  p.review = (open || stage == InvoiceStage::Paid) && status == kStatusReview;
  const bool stepped = status.empty() || status == kStatusPending || status == kStatusSeen ||
                       status == kStatusConfirming || status == kStatusPaid;
  if ((open && stepped && !p.expired) || stage == InvoiceStage::Paid) {
    p.steps = StepsFor(invoice.asset,
                       stage == InvoiceStage::Paid ? std::string(kStatusPaid) : status);
  }
  if (p.review) p.steps.clear();
  p.confirmations = invoice.confirmations;
  p.confirmationsRequired = invoice.confirmationsRequired;
  p.showRemaining = partial && AmountAboveZero(invoice.remaining);
  p.remainingText = AmountWithTicker(invoice.remaining, invoice.ticker);

  const std::string wallets = JoinWallets(invoice.wallets);
  p.showWallets = (payable || partial) && !wallets.empty();
  p.walletsText = wallets;
  p.zcashNote = (payable || partial) && invoice.asset == kAssetZec;

  if (invoice.errorCode == kErrorAssetUnavailable) {
    p.notice = InvoiceNotice::Unavailable;
  } else if (status == kStatusUnknown || stage == InvoiceStage::Failed ||
             (!invoice.errorCode.empty() && invoice.errorCode != kErrorQuoteExpired &&
              invoice.errorCode != kErrorUnderpaid)) {
    p.notice = InvoiceNotice::Failed;
  }
  // Paying another way abandons the invoice: offered while nothing was
  // received, and when the invoice failed. Money on its way (seen,
  // confirming), paid or in review leaves only Close.
  p.showPayAnotherWay = (open && nothingReceived) || status == kStatusUnknown ||
                        stage == InvoiceStage::Failed;
  return p;
}

// ---- the account: a one-time crypto year ---------------------------------------
// (When its renewal notice shows is the SDK's: urnet::cryptoTermNoticeDue,
// term_end_time <= now < grace_end_time, never before.)

// A one-time crypto year does not renew (urnet::classifySubscriptionStore
// "crypto"): the Account plan says so, and nothing offers to manage it.
inline bool StoreDoesNotRenew(const std::string& storeFamily) {
  return storeFamily == kStoreFamilyCrypto;
}

// ---- the coins' marks (mmm/BTCPAY.md §17.6) -------------------------------------
// A coin's mark is the installed coins/<icon key>.svg (meson.build installs the
// four of More coins under the pkgdatadir), one path filled nonzero on a 24 x
// 24 box, drawn with GskPath (BrandIcons.cpp CoinMark).

inline constexpr double kMarkViewBox = 24;

// An icon key that names an installed file: lowercase letters, digits and
// underscores (the SDK's CryptoIconKey* values), never a path.
inline bool IsMarkKey(std::string_view key) {
  if (key.empty()) return false;
  for (const char c : key) {
    if (!(('a' <= c && c <= 'z') || ('0' <= c && c <= '9') || c == '_')) return false;
  }
  return true;
}

// The d attribute of an svg's one <path> element (comments skipped), or ""
// when it has none or several, or when the data holds anything but letters,
// digits, spaces, points, commas and signs: only path data reaches
// gsk_path_parse.
inline std::string SvgPathData(std::string_view svg) {
  const auto startsAt = [&svg](size_t pos, std::string_view what) {
    return pos <= svg.size() && svg.substr(pos, what.size()) == what;
  };
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  std::string_view data;
  int paths = 0;
  size_t pos = 0;
  while (pos < svg.size()) {
    if (startsAt(pos, "<!--")) {
      const size_t end = svg.find("-->", pos + 4);
      if (end == std::string_view::npos) return std::string();
      pos = end + 3;
      continue;
    }
    const bool element = startsAt(pos, "<path") && pos + 5 < svg.size() &&
                         (isSpace(svg[pos + 5]) || svg[pos + 5] == '/' || svg[pos + 5] == '>');
    if (!element) {
      ++pos;
      continue;
    }
    const size_t close = svg.find('>', pos + 5);
    if (close == std::string_view::npos) return std::string();
    ++paths;
    const std::string_view tag = svg.substr(pos + 5, close - (pos + 5));
    for (size_t at = 0; at + 4 <= tag.size(); ++at) {
      if (!isSpace(tag[at]) || tag.substr(at + 1, 3) != "d=\"") continue;
      const size_t end = tag.find('"', at + 4);
      if (end == std::string_view::npos) return std::string();
      data = tag.substr(at + 4, end - (at + 4));
      break;
    }
    pos = close + 1;
  }
  if (paths != 1 || data.empty()) return std::string();
  for (const char c : data) {
    const bool allowed = ('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') ||
                         ('a' <= c && c <= 'z') || c == ' ' || c == '.' || c == ',' || c == '-' ||
                         c == '+';
    if (!allowed) return std::string();
  }
  return std::string(data);
}

// ---- the QR -------------------------------------------------------------------

// The quiet zone the QR spec asks for, in modules on each side.
inline constexpr int kQrQuietModules = 4;
// Pixels a module aims for, and the canvas bounds. A Monero subaddress URI is
// about 160 bytes (version 10 at level M, 57 modules); a Zcash unified
// address with two receivers about 260 (version 14, 73 modules).
inline constexpr int kQrTargetPxPerModule = 4;
inline constexpr int kQrMinSide = 200;
inline constexpr int kQrMaxSide = 320;

// The canvas side a code of `moduleCount` modules wants, in px.
inline int QrCanvasSide(int moduleCount) {
  if (moduleCount <= 0) return kQrMinSide;
  const int total = moduleCount + 2 * kQrQuietModules;
  return std::clamp(total * kQrTargetPxPerModule, kQrMinSide, kQrMaxSide);
}

// Whole pixels per module for a `side` canvas: an integer module size, or the
// edges antialias and a dense code stops scanning (ExtenderShareSheet.cpp
// measured it). 0 when the code does not fit at all.
inline int QrModulePixels(int side, int moduleCount) {
  if (side <= 0 || moduleCount <= 0) return 0;
  return side / (moduleCount + 2 * kQrQuietModules);
}

}  // namespace urnw::crypto_pay
