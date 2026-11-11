// The payment-method toggle (Card · USDC on Solana · Zcash · Monero · More
// coins) and the native crypto invoice (mmm/ZCASH-CONTRACT.md §6, mmm/ZCASH.md
// §0.1 and §0.3, mmm/BTCPAY.md §17.6), the half that needs no GTK and no SDK,
// so tests/CryptoPayTest.cpp pins it.
//
// The SDK owns the money: which coins the server enabled and how the widget
// lays them out (urnet::cryptoPaymentTabs over the balance result's
// crypto_assets and crypto_unavailable_assets: the tabs, their order, their
// coins, their labels and which coins can be paid with now), the coin a tap
// on a tab chooses (urnet::cryptoPaymentTabCoin), the invoice itself
// (urnet::CryptoInvoiceViewController: quote, countdown, re-quote, status and
// its steps, the saved invoice, the text its QR code holds), the payment URI
// check (urnet::checkCryptoPaymentUri, the whole gate), and every coin's own
// lines and rules (urnet::cryptoAssetLinesFor, cryptoPaymentShowsMemo,
// cryptoPaymentUriScheme, cryptoPaymentEventProduct). The host carries those
// answers into the views (CryptoInvoiceHost.cpp, SubscriptionBalance.cpp), and
// this header only decides what the screens show from them:
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
//     the memo when the SDK says it shows (the server sent one: USDC on
//     Solana), the countdown with Refresh quote (the expired line once it ran
//     out), the SDK's status steps (waiting -> payment seen -> confirming n of
//     N -> paid; two for USDC; each drawn done, current or upcoming), what is
//     left to send after a partial payment, the review copy alone for a
//     payment in review, the wallets that work, and Zcash's exchange note. A
//     coin with no payment link (TAO) says to send exactly this amount to this
//     address, its Copy buttons say what they copy, and its QR says it holds
//     only the address;
//   * the renewal notice of a one-time crypto year (§2.2 crypto_term), and the
//     "does not renew" reading of a crypto store.
//
// Copy lives with the screens (T_ calls in PaymentMethodRow.cpp,
// CryptoInvoiceView.cpp, UpgradeSheet.cpp, Onboarding.cpp and AccountPage.cpp).
// The tabs' and coins' labels and each coin's lines are the SDK's, a store key
// with its English, looked up as it hands them out (PaymentMethodRow.cpp
// LabelText); tests/CatalogLookupTest.cpp lists them (SdkCryptoLabels).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
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

// The coins this app takes, in the SDK's order: every coin the SDK lays out,
// and the methods that are a coin (IsCoin). Everything else about a coin is
// the SDK's: its label and mark, its lines, its payment URI and the check in
// front of it, its status steps and the product its events name.
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

// How one of the invoice's status steps is drawn (the state's steps'
// progress): urnet::CryptoPaymentStepDone, ...Current, ...Upcoming.
inline constexpr const char* kStepDone = "done";
inline constexpr const char* kStepCurrent = "current";
inline constexpr const char* kStepUpcoming = "upcoming";

// The purchase error codes the crypto endpoints add (§2.7), and the one every
// purchase endpoint shares for a legacy guest network.
inline constexpr const char* kErrorAssetUnavailable = "asset_unavailable";
inline constexpr const char* kErrorQuoteExpired = "quote_expired";
inline constexpr const char* kErrorUnderpaid = "underpaid";
inline constexpr const char* kErrorGuestSignInRequired = "guest_sign_in_required";

// The store family urnet::classifySubscriptionStore gives a one-time crypto
// renewal (every coin's market; §4.3): it never renews.
inline constexpr const char* kStoreFamilyCrypto = "crypto";

// A coin of this app's (kCoins): not Card, nothing, a ticker or a tab.
inline bool IsCoin(std::string_view method) {
  return std::any_of(std::begin(kCoins), std::end(kCoins),
                     [method](const char* coin) { return method == coin; });
}

// One of a coin's lines as the SDK hands it out (urnet::cryptoAssetLinesFor):
// its store key, and its English, the store's byte for byte and so the
// gettext msgid it is looked up with (I18n.hpp T_). A line whose key is "" is
// one the coin does not have (Zcash and Monero have no network warning, and
// only TAO has the note under its QR), and is not shown.
struct CoinLine {
  std::string key;
  std::string english;
};

inline bool operator==(const CoinLine& a, const CoinLine& b) {
  return a.key == b.key && a.english == b.english;
}
inline bool operator!=(const CoinLine& a, const CoinLine& b) { return !(a == b); }

// A coin's own lines (urnet::CryptoAssetLines): the network it is sent on,
// named before anything else (the USDC panel's lesson: "which network do I
// select?") and under the coin in the More coins list; the warning beside its
// address (only this coin, sent on this network, is credited); and for a coin
// with no payment link (TAO) the note under its QR code, which holds the
// address alone.
struct CoinLines {
  CoinLine networkLine;
  CoinLine networkWarning;
  CoinLine noPaymentLinkNote;
};

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
  // the network it is sent on, under it in the More coins list: the SDK's
  // line for it (SubscriptionBalanceStore::PaymentTabs)
  CoinLine networkLine;
};

inline bool operator==(const PaymentCoinView& a, const PaymentCoinView& b) {
  return a.asset == b.asset && a.label == b.label && a.labelKey == b.labelKey &&
         a.iconKey == b.iconKey && a.available == b.available && a.networkLine == b.networkLine;
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

// The coin chosen last in each tab of a row while it is up, by tab id: the
// `lastChosen` a tap on that tab hands the SDK, which chooses it again while
// it can be paid with (urnet::cryptoPaymentTabCoin, PaymentMethodRow.cpp
// TabCoinFor: else the remembered method, else the tab's first). Every choice
// counts: a tap's, the More coins list's, and the row's own selection (the
// remembered method it opens on, or the one choice the onboarding's two rows
// share). Each counts for its own tab alone, so choosing USDC on Solana after
// SOL leaves More coins' SOL in place: a single "chosen last anywhere" lost
// it, and a tap back on More coins fell back to TAO.
struct LastChosenCoins {
  std::map<std::string, std::string> byTab;

  // `coin` was chosen: it is now the coin chosen last in the tab of `tabs` it
  // sits in. Card, nothing, or a coin no tab holds changes nothing.
  void Choose(const std::vector<PaymentTabView>& tabs, const std::string& coin) {
    if (!IsCoin(coin)) return;
    for (const PaymentTabView& tab : tabs) {
      for (const PaymentCoinView& tabCoin : tab.coins) {
        if (tabCoin.asset == coin) byTab[tab.id] = tabCoin.asset;
      }
    }
  }

  // The coin chosen last in the tab `tabId`, "" while none was.
  std::string In(const std::string& tabId) const {
    const auto found = byTab.find(tabId);
    return found == byTab.end() ? std::string() : found->second;
  }
};

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

// One of the invoice's status steps (urnet::CryptoPaymentStep, the state's
// steps): the status it stands for (pending, seen, confirming or paid), how it
// is drawn (kStepDone, kStepCurrent, kStepUpcoming) and the confirming step's
// counts, 0 for the others. The SDK decides each: USDC has two steps, every
// other coin four (confirming only while its required count is known); a step
// the payment is past is done whether the server reported it or skipped it,
// and once paid every step is.
struct Step {
  std::string status;
  std::string progress;
  int64_t confirmations = 0;
  int64_t confirmationsRequired = 0;
};

// urnet::CryptoInvoiceState as the screens read it, its status steps among
// it, plus the answers the host asks the SDK for beside it: whether the URI
// passed urnet::checkCryptoPaymentUri, urnet::cryptoWalletSuggestions, whether
// the coin has no payment link (urnet::cryptoPaymentUriScheme is empty: TAO),
// the coin's lines (urnet::cryptoAssetLinesFor) and whether its memo shows
// (urnet::cryptoPaymentShowsMemo: only when the server sent one).
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
  // the memo for a transfer sent by hand: the server sends one for USDC on
  // Solana alone (its Solana Pay reference), "" for every other coin
  std::string memo;
  double priceUsd = 0;
  std::string expiresAt;
  int64_t secondsLeft = 0;  // 0 once expired
  std::string status;       // §2.5
  std::string reviewReason;
  std::string received;
  std::string remaining;
  int64_t confirmations = 0;
  int64_t confirmationsRequired = 0;
  // the status steps at `status`, as the SDK draws them; none before the
  // first status, in review or unknown
  std::vector<Step> steps;
  bool refreshing = false;  // a re-quote is in flight
  std::string errorCode;    // the last start or re-quote failure
  std::string errorMessage;
  // beside the state
  bool uriChecked = false;
  std::vector<std::string> wallets;
  bool noPaymentLink = false;
  CoinLines lines;
  bool showsMemo = false;
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

// The wallet names as one list ("btcli, Talisman, Nova").
inline std::string JoinWallets(const std::vector<std::string>& wallets) {
  std::string out;
  for (const std::string& wallet : wallets) {
    if (wallet.empty()) continue;
    if (!out.empty()) out += ", ";
    out += wallet;
  }
  return out;
}

// The line under the invoice when something went wrong with it: a coin that
// cannot be quoted now (asset_unavailable on a re-quote), or an invoice the
// server no longer knows or a re-quote it refused. An expired quote and an
// underpayment are not failures here: the expired line and the review copy
// say those.
enum class InvoiceNotice { None, Unavailable, Failed };

// What the invoice shows, top to bottom (§6.2).
struct InvoicePresentation {
  // the coin's network warning, under the address (no key: none)
  bool showNetworkWarning = false;
  CoinLine networkWarning;
  bool showQr = false;         // the QR
  std::string qrText;          // what it encodes while it shows
  bool showOpenWallet = false; // a checked URI to hand to a wallet
  // a coin with no payment link (TAO): "Send exactly this amount to this
  // address." while payable, Copy amount and Copy address on the buttons, and
  // under its QR (the address alone) the note saying so (no key: none)
  bool noPaymentLink = false;
  bool showByHandLine = false;
  CoinLine noPaymentLinkNote;
  bool showAmount = false;    // Send exactly, the amount and Copy
  std::string amountText;     // "0.072387 XMR"
  bool showAddress = false;   // To this address, the address and Copy
  bool showMemo = false;      // the memo, and Copy, while the SDK says it shows
  bool showCountdown = false; // Quote expires in m:ss
  std::string countdownText;  // m:ss
  bool expired = false;       // the expired line
  bool showRefresh = false;   // Refresh quote
  bool refreshEnabled = false;
  // the SDK's steps while they show, each drawn by its progress and the
  // confirming one with its own counts; empty: no steps (expired, review,
  // failed, or none listed)
  std::vector<Step> steps;
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
  if (p.showQr && qrIsAddress) p.noPaymentLinkNote = invoice.lines.noPaymentLinkNote;
  p.showAddress = (payable || partial) && !invoice.address.empty();
  // the SDK's rule (only a memo the server sent), shown only while payable
  p.showMemo = payable && invoice.showsMemo;
  p.networkWarning = invoice.lines.networkWarning;
  p.showNetworkWarning = p.showAddress && !p.networkWarning.key.empty();
  p.showCountdown = payable;
  p.countdownText = FormatCountdown(invoice.secondsLeft);
  p.showRefresh = open && nothingReceived;
  p.refreshEnabled = p.showRefresh && !invoice.refreshing;

  // When the steps show is this app's; what they say is the SDK's (the
  // state's steps: none before the first status poll, so a resumed invoice
  // shows none until the server answers).
  p.review = (open || stage == InvoiceStage::Paid) && status == kStatusReview;
  const bool stepped = status.empty() || status == kStatusPending || status == kStatusSeen ||
                       status == kStatusConfirming || status == kStatusPaid;
  if ((open && stepped && !p.expired) || stage == InvoiceStage::Paid) p.steps = invoice.steps;
  if (p.review) p.steps.clear();
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
