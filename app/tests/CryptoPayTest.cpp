// The payment-method toggle and the crypto invoice's rules (CryptoPay.hpp,
// PaymentQr.hpp; mmm/ZCASH-CONTRACT.md §6, mmm/BTCPAY.md §17.6): the method
// row's selection over the SDK's coin selector, the coin chosen last in each
// tab that a tap hands the SDK, and its plan rules; what the invoice shows in
// each state and for each coin, with the SDK's answers for the coin as the
// host carries them (its lines, its payment link, its memo, its steps); the
// renewal notice's window, the QR's encoding and sizing, and the marks' svg
// reading. The rules the SDK owns (the tabs, the coin a tap chooses, the
// lines, the memo, the steps, the URI check, the event product) are the SDK's
// own tests', which replay the shared vectors. The screens that show them
// need GTK and the SDK; CryptoPayWiringTest.cpp reads those.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "CryptoInvoiceStorage.hpp"
#include "CryptoPay.hpp"
#include "PaymentQr.hpp"

#ifndef UR_ASSETS_DIR
#define UR_ASSETS_DIR ""
#endif

namespace {

namespace cp = urnw::crypto_pay;

using Tabs = std::vector<cp::PaymentTabView>;

// A coin of the selector as the SDK lays it out.
cp::PaymentCoinView MakeCoin(const char* asset, const char* label, const char* labelKey,
                             const char* iconKey, bool available) {
  cp::PaymentCoinView coin;
  coin.asset = asset;
  coin.label = label;
  coin.labelKey = labelKey;
  coin.iconKey = iconKey;
  coin.available = available;
  return coin;
}

// The SDK's selector (urnet::cryptoPaymentTabs) for a balance that sells
// `listed`, with no unavailable list: none at all without a coin; else the
// main three tabs always, and More coins while one of its coins is listed,
// each coin available while it is.
Tabs SelectorFor(const std::vector<std::string>& listed) {
  Tabs tabs;
  if (listed.empty()) return tabs;
  const auto has = [&listed](const char* asset) {
    return std::find(listed.begin(), listed.end(), asset) != listed.end();
  };
  const auto single = [&](const char* asset, const char* label, const char* labelKey,
                          const char* iconKey) {
    cp::PaymentTabView tab;
    tab.id = asset;
    tab.label = label;
    tab.labelKey = labelKey;
    tab.iconKeys = {iconKey};
    tab.coins = {MakeCoin(asset, label, labelKey, iconKey, has(asset))};
    tab.available = has(asset);
    tabs.push_back(tab);
  };
  single(cp::kAssetUsdcSolana, "USDC on Solana", "pay_method_usdc_solana", "coin_usdc");
  single(cp::kAssetZec, "Zcash", "pay_method_zcash", "coin_zec");
  single(cp::kAssetXmr, "Monero", "pay_method_monero", "coin_xmr");
  if (has(cp::kAssetTao) || has(cp::kAssetBtc) || has(cp::kAssetEth) || has(cp::kAssetSol)) {
    cp::PaymentTabView more;
    more.id = cp::kTabMoreCoins;
    more.label = "More coins";
    more.labelKey = "pay_method_more_coins";
    more.iconKeys = {"coin_tao", "coin_btc", "coin_eth", "coin_sol"};
    more.coins = {MakeCoin(cp::kAssetTao, "TAO", "pay_method_tao", "coin_tao", has(cp::kAssetTao)),
                  MakeCoin(cp::kAssetBtc, "Bitcoin", "pay_method_bitcoin", "coin_btc",
                           has(cp::kAssetBtc)),
                  MakeCoin(cp::kAssetEth, "ETH", "pay_method_eth", "coin_eth", has(cp::kAssetEth)),
                  MakeCoin(cp::kAssetSol, "SOL", "pay_method_sol", "coin_sol", has(cp::kAssetSol))};
    more.available = true;
    tabs.push_back(more);
  }
  return tabs;
}

// The whole file, or "" when it cannot be read.
std::string ReadAsset(const std::string& name) {
  std::ifstream in(std::string(UR_ASSETS_DIR) + "/" + name, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// A status step as the SDK hands it over (urnet::CryptoPaymentStep).
cp::Step MakeStep(const char* status, const char* progress, int64_t confirmations = 0,
                  int64_t confirmationsRequired = 0) {
  cp::Step step;
  step.status = status;
  step.progress = progress;
  step.confirmations = confirmations;
  step.confirmationsRequired = confirmationsRequired;
  return step;
}

// An open, payable Monero invoice as the host hands it over: the SDK's
// answers for Monero beside the state (its network line, no warning and no
// note, a payment link, no memo, its four steps at pending, and no wallets:
// urnet::cryptoWalletSuggestions names TAO's alone).
cp::InvoiceView OpenXmr() {
  cp::InvoiceView v;
  v.phase = "open";
  v.intentId = "0b6f0000-0000-4000-8000-000000000001";
  v.asset = "xmr";
  v.plan = "yearly";
  v.address = "8" + std::string(94, 'A');
  v.amount = "0.072387";
  v.ticker = "XMR";
  v.uri = "monero:" + v.address + "?tx_amount=0.072387&tx_description=URnetwork%20Pro%20yearly";
  v.qrText = v.uri;  // urnet::cryptoPaymentQrText: the URI its check passed
  v.expiresAt = "2026-10-20T18:30:00Z";
  v.secondsLeft = 1799;
  v.status = "pending";
  v.remaining = "0.072387";
  v.confirmationsRequired = 1;
  v.steps = {MakeStep("pending", "current"), MakeStep("seen", "upcoming"),
             MakeStep("confirming", "upcoming", 0, 1), MakeStep("paid", "upcoming")};
  v.uriChecked = true;
  v.lines.networkLine = {"crypto_network_monero", "Monero"};
  return v;
}

// "pending=done seen=current confirming=upcoming 0/1 paid=upcoming": each
// step's status, how it is drawn, and the confirming step's counts.
std::string Kinds(const std::vector<cp::Step>& steps) {
  std::string out;
  for (const cp::Step& step : steps) {
    if (!out.empty()) out += " ";
    out += step.status + "=" + step.progress;
    if (step.status == "confirming") {
      out += " " + std::to_string(step.confirmations) + "/" + std::to_string(step.confirmationsRequired);
    }
  }
  return out;
}

}  // namespace

// The SDK lays the selector out (urnet::cryptoPaymentTabs): USDC on Solana,
// Zcash, Monero, then More coins with TAO, Bitcoin, ETH and SOL, each its own
// mark; the row is Card and those tabs.
UR_TEST(CryptoPay_TheSelectorIsTheSdksTabs) {
  const Tabs all = SelectorFor({"usdc_solana", "zec", "xmr", "tao", "btc", "eth", "sol"});
  std::string ids;
  std::string assets;
  for (const cp::PaymentTabView& tab : all) {
    ids += (ids.empty() ? "" : ",") + tab.id;
    for (const cp::PaymentCoinView& coin : tab.coins) assets += (assets.empty() ? "" : ",") + coin.asset;
  }
  UR_EXPECT_TRUE_MSG(ids, ids == "usdc_solana,zec,xmr,more_coins");
  UR_EXPECT_TRUE_MSG(assets, assets == "usdc_solana,zec,xmr,tao,btc,eth,sol");
  UR_EXPECT_TRUE(all[3].iconKeys ==
                 std::vector<std::string>({"coin_tao", "coin_btc", "coin_eth", "coin_sol"}));
  UR_EXPECT_TRUE(cp::TabOpensCoins(all[3]) && !cp::TabOpensCoins(all[0]));
  const cp::PaymentCoinView* eth = cp::FindCoin(all, "eth");
  UR_EXPECT_TRUE(eth != nullptr && eth->labelKey == "pay_method_eth");
  UR_EXPECT_TRUE(cp::FindCoin(all, "doge") == nullptr);
  // the row rebuilds only for a selector that changed
  UR_EXPECT_TRUE(SelectorFor({"xmr"}) == SelectorFor({"xmr"}));
  UR_EXPECT_TRUE(SelectorFor({"xmr"}) != SelectorFor({"xmr", "tao"}));
}

// No coin listed: no tabs, no row, and the surface looks as it did before the
// toggle; the main three tabs while no More coins coin is listed.
UR_TEST(CryptoPay_TheRowShowsOnlyWithACoin) {
  UR_EXPECT_FALSE(cp::MethodRowShown(SelectorFor({})));
  UR_EXPECT_TRUE(cp::MethodRowShown(SelectorFor({"usdc_solana"})));
  UR_EXPECT_TRUE(cp::MethodRowShown(SelectorFor({"tao"})));
  UR_EXPECT_EQ(size_t{3}, SelectorFor({"xmr"}).size());
  UR_EXPECT_EQ(size_t{4}, SelectorFor({"sol"}).size());
}

// The remembered method while it can be paid with now, else Card: a coin the
// server stopped selling, or one that cannot be quoted (greyed in its place).
UR_TEST(CryptoPay_ARememberedCoinTheServerDroppedFallsBackToCard) {
  const Tabs tabs = SelectorFor({"usdc_solana", "xmr", "tao", "sol"});
  UR_EXPECT_TRUE(cp::MethodSelection(tabs, "xmr") == "xmr");
  UR_EXPECT_TRUE(cp::MethodSelection(tabs, "sol") == "sol");
  UR_EXPECT_TRUE(cp::MethodSelection(tabs, "card") == "card");
  UR_EXPECT_TRUE(cp::MethodSelection(tabs, "zec") == "card");
  UR_EXPECT_TRUE(cp::MethodSelection(tabs, "btc") == "card");
  UR_EXPECT_TRUE(cp::MethodSelection(tabs, "") == "card");
  UR_EXPECT_TRUE(cp::MethodSelection(tabs, "doge") == "card");
  UR_EXPECT_TRUE(cp::MethodSelection(tabs, "more_coins") == "card");
  UR_EXPECT_TRUE(cp::MethodSelection(SelectorFor({}), "usdc_solana") == "card");
  UR_EXPECT_TRUE(cp::MethodOffered(tabs, "card") && cp::MethodOffered(tabs, "tao"));
  UR_EXPECT_FALSE(cp::MethodOffered(tabs, "eth"));
}

// A tap on a tab asks the SDK for its coin (urnet::cryptoPaymentTabCoin) with
// the coin chosen last in that tab: kept per tab, so a choice in another tab
// never replaces it. One "chosen last anywhere" sent USDC to More coins after
// SOL, and More coins fell back to TAO.
UR_TEST(CryptoPay_TheCoinChosenLastIsKeptPerTab) {
  const Tabs all = SelectorFor({"usdc_solana", "zec", "xmr", "tao", "btc", "eth", "sol"});
  cp::LastChosenCoins chosen;
  UR_EXPECT_TRUE(chosen.In("more_coins").empty());
  chosen.Choose(all, "sol");  // in the More coins list
  UR_EXPECT_TRUE(chosen.In("more_coins") == "sol");
  chosen.Choose(all, "usdc_solana");  // then USDC on Solana's tab
  UR_EXPECT_TRUE(chosen.In("usdc_solana") == "usdc_solana");
  UR_EXPECT_TRUE_MSG(chosen.In("more_coins"), chosen.In("more_coins") == "sol");
  // Card, nothing, a coin no tab holds, a tab and a ticker change nothing
  for (const char* none : {"card", "", "doge", "more_coins", "XMR"}) chosen.Choose(all, none);
  UR_EXPECT_EQ(size_t{2}, chosen.byTab.size());
  UR_EXPECT_TRUE(chosen.In("more_coins") == "sol");
  chosen.Choose(all, "btc");  // a later choice in More coins replaces SOL
  UR_EXPECT_TRUE(chosen.In("more_coins") == "btc");
  chosen.Choose(SelectorFor({"xmr"}), "eth");  // a selector without More coins
  UR_EXPECT_TRUE(chosen.In("more_coins") == "btc");
  chosen.Choose(SelectorFor({"xmr"}), "xmr");
  UR_EXPECT_TRUE(chosen.In("xmr") == "xmr");
  UR_EXPECT_TRUE(chosen.In("zec").empty());

  const Tabs taoAndSol = SelectorFor({"tao", "sol"});  // Bitcoin and ETH not quotable
  UR_EXPECT_FALSE(taoAndSol[0].available);  // USDC not listed: greyed
  UR_EXPECT_TRUE(taoAndSol[3].available);
  UR_EXPECT_TRUE(cp::JoinNames({"TAO", "", "Bitcoin", "ETH", "SOL"}) == "TAO, Bitcoin, ETH, SOL");
  // a coin's network line is part of the selector: the row rebuilds when it changes
  Tabs lined = all;
  lined[3].coins[0].networkLine = {"crypto_network_tao", "Bittensor network"};
  UR_EXPECT_TRUE(lined != all);
}

// Monthly is card only; a coin buys the yearly plan, or the welcome offer.
UR_TEST(CryptoPay_ACoinBuysYearlyOrTheOffer) {
  UR_EXPECT_TRUE(cp::MonthlyAllowed("card"));
  for (const char* coin : {"usdc_solana", "zec", "xmr", "tao", "btc", "eth", "sol"}) {
    UR_EXPECT_FALSE(cp::MonthlyAllowed(coin));
    UR_EXPECT_TRUE_MSG(coin, cp::IsCoin(coin));
  }
  UR_EXPECT_FALSE(cp::IsCoin("card"));
  UR_EXPECT_FALSE(cp::IsCoin(""));
  UR_EXPECT_FALSE(cp::IsCoin("XMR"));
  UR_EXPECT_FALSE(cp::IsCoin("doge"));
  UR_EXPECT_FALSE(cp::IsCoin("more_coins"));
  UR_EXPECT_TRUE(std::string(cp::CoinPlan(false)) == "yearly");
  UR_EXPECT_TRUE(std::string(cp::CoinPlan(true)) == "yearly_onboarding");
}

// No renewal or trial wording while a coin is selected (§6.1): under the
// yearly price, the offer's "then $40/year" goes for a coin; "Save N%" and the
// monthly equivalent stay; Card keeps everything.
UR_TEST(CryptoPay_ACoinShowsNoRenewalLine) {
  using Lines = std::vector<std::string>;
  const std::string then = "then $40/year";
  const std::string save = "Save 33%";
  const std::string equivalent = "\xE2\x89\x88 $3.34/month \xC2\xB7 billed once a year";
  // the welcome offer: the renewal price under the first-year price
  UR_EXPECT_TRUE(cp::YearlyCardLines(then, true, equivalent, false) == Lines({then, equivalent}));
  UR_EXPECT_TRUE(cp::YearlyCardLines(then, true, equivalent, true) == Lines({equivalent}));
  UR_EXPECT_TRUE(cp::YearlyCardLines(then, true, "", true).empty());
  // no offer: the saving is not renewal wording
  UR_EXPECT_TRUE(cp::YearlyCardLines(save, false, equivalent, true) == Lines({save, equivalent}));
  UR_EXPECT_TRUE(cp::YearlyCardLines(save, false, "", false) == Lines({save}));
  UR_EXPECT_TRUE(cp::YearlyCardLines("", false, "", true).empty());
}

// (The events' store and product are the SDK's, urnet::cryptoAssetMarket and
// urnet::cryptoPaymentEventProduct: CryptoPayWiringTest.cpp reads
// PaymentMethodEventStore and PaymentMethodEventProduct. Each coin's network
// line, its warning and TAO's QR note are the SDK's too, as the host carries
// them: CatalogLookupTest.cpp holds their keys and English to the catalog.)

// A refused start: unavailable stays on the coin, a guest converts, the rest
// is the surface's own refusal line.
UR_TEST(CryptoPay_StartRefusals) {
  UR_EXPECT_TRUE(cp::StartRefusalFor("asset_unavailable", true) == cp::StartRefusal::Unavailable);
  UR_EXPECT_TRUE(cp::StartRefusalFor("guest_sign_in_required", true) ==
                 cp::StartRefusal::GuestSignIn);
  for (const char* code : {"rate_limited", "offer_unavailable", "start_failed", "plan_unavailable",
                           "invalid_request"}) {
    UR_EXPECT_TRUE_MSG(code, cp::StartRefusalFor(code, true) == cp::StartRefusal::Other);
  }
  // a transport failure carries no code
  UR_EXPECT_TRUE(cp::StartRefusalFor("", true) == cp::StartRefusal::Other);
  UR_EXPECT_TRUE(cp::StartRefusalFor("", false) == cp::StartRefusal::None);
}

UR_TEST(CryptoPay_Stages) {
  cp::InvoiceView v;
  UR_EXPECT_TRUE(cp::StageOf(v) == cp::InvoiceStage::None);
  v.phase = "none";
  UR_EXPECT_TRUE(cp::StageOf(v) == cp::InvoiceStage::None);
  v.phase = "starting";
  UR_EXPECT_TRUE(cp::StageOf(v) == cp::InvoiceStage::Starting);
  v.phase = "failed";
  UR_EXPECT_TRUE(cp::StageOf(v) == cp::InvoiceStage::Failed);
  v.phase = "open";
  UR_EXPECT_TRUE(cp::StageOf(v) == cp::InvoiceStage::Open);
  v.status = "paid";  // credited: paid whatever the phase reached
  UR_EXPECT_TRUE(cp::StageOf(v) == cp::InvoiceStage::Paid);
  v.status = "";
  v.phase = "paid";
  UR_EXPECT_TRUE(cp::StageOf(v) == cp::InvoiceStage::Paid);
}

// A payment in progress: an open invoice the server still knows.
UR_TEST(CryptoPay_InProgress) {
  cp::InvoiceView v = OpenXmr();
  UR_EXPECT_TRUE(cp::InvoiceInProgress(v));
  for (const char* status : {"seen", "confirming", "review", "expired"}) {
    v.status = status;
    UR_EXPECT_TRUE_MSG(status, cp::InvoiceInProgress(v));
  }
  v.status = "unknown";
  UR_EXPECT_FALSE(cp::InvoiceInProgress(v));
  v.status = "paid";
  UR_EXPECT_FALSE(cp::InvoiceInProgress(v));
  v = OpenXmr();
  v.phase = "starting";
  UR_EXPECT_FALSE(cp::InvoiceInProgress(v));
  v.phase = "none";
  UR_EXPECT_FALSE(cp::InvoiceInProgress(v));
}

UR_TEST(CryptoPay_Formats) {
  UR_EXPECT_TRUE(cp::FormatCountdown(1799) == "29:59");
  UR_EXPECT_TRUE(cp::FormatCountdown(5) == "0:05");
  UR_EXPECT_TRUE(cp::FormatCountdown(3600) == "60:00");
  UR_EXPECT_TRUE(cp::FormatCountdown(0) == "0:00");
  UR_EXPECT_TRUE(cp::FormatCountdown(-3) == "0:00");
  UR_EXPECT_TRUE(cp::AmountWithTicker("0.072387", "XMR") == "0.072387 XMR");
  UR_EXPECT_TRUE(cp::AmountWithTicker("40.012345", "") == "40.012345");
  UR_EXPECT_TRUE(cp::AmountAboveZero("0.000100"));
  UR_EXPECT_FALSE(cp::AmountAboveZero("0"));
  UR_EXPECT_FALSE(cp::AmountAboveZero("0.000000"));
  UR_EXPECT_FALSE(cp::AmountAboveZero(""));
  UR_EXPECT_TRUE(cp::JoinWallets({"btcli", "", "Talisman"}) == "btcli, Talisman");
  UR_EXPECT_TRUE(cp::JoinWallets({}).empty());
}

// The status steps are the SDK's (the state's steps: which there are, how
// each is drawn, the confirming one's counts); the invoice shows them as the
// SDK lists them while they show, and lists none of its own.
UR_TEST(CryptoPay_TheStepsAreTheSdks) {
  cp::InvoiceView v = OpenXmr();
  UR_EXPECT_TRUE(Kinds(cp::PresentInvoice(v).steps) ==
                 "pending=current seen=upcoming confirming=upcoming 0/1 paid=upcoming");
  // a whole payment never reports seen: the SDK draws it done all the same
  v.status = "confirming";
  v.steps = {MakeStep("pending", "done"), MakeStep("seen", "done"), MakeStep("confirming", "current", 3, 6),
             MakeStep("paid", "upcoming")};
  UR_EXPECT_TRUE(Kinds(cp::PresentInvoice(v).steps) ==
                 "pending=done seen=done confirming=current 3/6 paid=upcoming");
  // USDC's two
  v = OpenXmr();
  v.asset = "usdc_solana";
  v.steps = {MakeStep("pending", "current"), MakeStep("paid", "upcoming")};
  UR_EXPECT_TRUE(Kinds(cp::PresentInvoice(v).steps) == "pending=current paid=upcoming");
  // SOL straight to paid: every step done, the confirming one not listed
  v = OpenXmr();
  v.asset = "sol";
  v.status = "paid";
  v.phase = "paid";
  v.steps = {MakeStep("pending", "done"), MakeStep("seen", "done"), MakeStep("paid", "done")};
  UR_EXPECT_TRUE(Kinds(cp::PresentInvoice(v).steps) == "pending=done seen=done paid=done");
  // none from the SDK (an asset it does not know): none shown
  v = OpenXmr();
  v.steps.clear();
  UR_EXPECT_TRUE(cp::PresentInvoice(v).steps.empty());
}

// Pending with time left: everything to pay with, and Pay another way.
UR_TEST(CryptoPay_APendingInvoiceShowsEverythingToPayWith) {
  const cp::InvoicePresentation p = cp::PresentInvoice(OpenXmr());
  UR_EXPECT_TRUE(p.showQr);
  UR_EXPECT_TRUE(p.qrText == OpenXmr().uri);  // the SDK's qr text: the checked URI
  UR_EXPECT_TRUE(p.showOpenWallet);
  UR_EXPECT_FALSE(p.noPaymentLink || p.showByHandLine || !p.noPaymentLinkNote.key.empty());
  UR_EXPECT_TRUE(p.showAmount);
  UR_EXPECT_TRUE(p.amountText == "0.072387 XMR");
  UR_EXPECT_TRUE(p.showAddress);
  UR_EXPECT_FALSE(p.showMemo);  // the SDK shows none for Monero (no memo sent)
  UR_EXPECT_FALSE(p.showNetworkWarning);  // Monero has none
  UR_EXPECT_TRUE(p.showCountdown);
  UR_EXPECT_TRUE(p.countdownText == "29:59");
  UR_EXPECT_FALSE(p.expired);
  UR_EXPECT_TRUE(p.showRefresh && p.refreshEnabled);
  UR_EXPECT_TRUE(Kinds(p.steps) == "pending=current seen=upcoming confirming=upcoming 0/1 paid=upcoming");
  UR_EXPECT_FALSE(p.showRemaining);
  UR_EXPECT_FALSE(p.review);
  // Monero names no wallets (the SDK names TAO's alone): no such line
  UR_EXPECT_FALSE(p.showWallets);
  UR_EXPECT_FALSE(p.zcashNote);
  UR_EXPECT_TRUE(p.notice == cp::InvoiceNotice::None);
  UR_EXPECT_TRUE(p.showPayAnotherWay);
}

// A URI the SDK's check refused is never shown or opened; the amount and the
// address still are, so the invoice can be paid by hand.
UR_TEST(CryptoPay_AnUncheckedUriShowsNoQr) {
  cp::InvoiceView v = OpenXmr();
  v.uriChecked = false;
  cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr || p.showOpenWallet);
  UR_EXPECT_TRUE(p.showAmount && p.showAddress);
  // Requoted on another device (mmm/ZCASH-CONTRACT.md §4.4.1): the controller blanks the
  // URI, which asked for the old amount, and its qr text with it. No QR and no
  // Open in wallet; the new exact amount and the address to send it to by hand.
  v = OpenXmr();
  v.uri.clear();
  v.qrText.clear();
  v.uriChecked = false;
  v.amount = "0.071000";
  p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr || p.showOpenWallet);
  UR_EXPECT_TRUE(p.showAmount && p.amountText == "0.071000 XMR");
  UR_EXPECT_TRUE(p.showAddress && p.showCountdown && p.showRefresh);
  // A coin with a payment link draws its URI and nothing else: never its bare
  // address
  v = OpenXmr();
  v.qrText = v.address;
  p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr);
  UR_EXPECT_TRUE(p.qrText.empty());
}

// Expired, as the server says or as the countdown ran out: no stale amount
// to pay, the expired line, and Refresh quote (disabled while one runs).
UR_TEST(CryptoPay_AnExpiredQuoteOffersOnlyARefresh) {
  for (int variant = 0; variant < 2; ++variant) {
    cp::InvoiceView v = OpenXmr();
    if (variant == 0) {
      v.status = "expired";
      v.secondsLeft = 0;
    } else {
      v.secondsLeft = 0;  // still pending on the server
    }
    cp::InvoicePresentation p = cp::PresentInvoice(v);
    UR_EXPECT_TRUE(p.expired);
    UR_EXPECT_FALSE(p.showQr);
    UR_EXPECT_FALSE(p.showAmount);
    UR_EXPECT_FALSE(p.showAddress);
    UR_EXPECT_FALSE(p.showCountdown);
    UR_EXPECT_TRUE(p.showRefresh && p.refreshEnabled);
    UR_EXPECT_TRUE(p.steps.empty());
    UR_EXPECT_TRUE(p.showPayAnotherWay);
    UR_EXPECT_TRUE(p.notice == cp::InvoiceNotice::None);
    v.refreshing = true;
    p = cp::PresentInvoice(v);
    UR_EXPECT_TRUE(p.showRefresh);
    UR_EXPECT_FALSE(p.refreshEnabled);
  }
  // the status result's quote_expired code is the expired line, not a failure
  cp::InvoiceView v = OpenXmr();
  v.status = "expired";
  v.secondsLeft = 0;
  v.errorCode = "quote_expired";
  UR_EXPECT_TRUE(cp::PresentInvoice(v).notice == cp::InvoiceNotice::None);
}

// Something arrived but not enough: what is left, where to send it, no QR
// for the full amount, and no way to walk away from the money.
UR_TEST(CryptoPay_APartialPaymentAsksForTheRest) {
  cp::InvoiceView v = OpenXmr();
  v.status = "seen";
  v.received = "0.050000";
  v.remaining = "0.022387";
  v.steps = {MakeStep("pending", "done"), MakeStep("seen", "current"),
             MakeStep("confirming", "upcoming", 0, 1), MakeStep("paid", "upcoming")};
  const cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(p.showRemaining);
  UR_EXPECT_TRUE(p.remainingText == "0.022387 XMR");
  UR_EXPECT_TRUE(p.showAddress);
  UR_EXPECT_FALSE(p.showQr);
  UR_EXPECT_FALSE(p.showAmount);
  UR_EXPECT_FALSE(p.showCountdown);
  UR_EXPECT_FALSE(p.showRefresh);
  UR_EXPECT_FALSE(p.expired);
  UR_EXPECT_TRUE(Kinds(p.steps) == "pending=done seen=current confirming=upcoming 0/1 paid=upcoming");
  UR_EXPECT_FALSE(p.showPayAnotherWay);
  // nothing left: no remaining line
  v.remaining = "0.000000";
  UR_EXPECT_FALSE(cp::PresentInvoice(v).showRemaining);
}

// Confirming: the count (the confirming step's own, the SDK's), and nothing
// left to do.
UR_TEST(CryptoPay_ConfirmingShowsTheCount) {
  cp::InvoiceView v = OpenXmr();
  v.status = "confirming";
  v.confirmations = 0;
  v.confirmationsRequired = 10;
  v.steps = {MakeStep("pending", "done"), MakeStep("seen", "done"), MakeStep("confirming", "current", 0, 10),
             MakeStep("paid", "upcoming")};
  const cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(Kinds(p.steps) == "pending=done seen=done confirming=current 0/10 paid=upcoming");
  UR_EXPECT_EQ(size_t{4}, p.steps.size());
  if (p.steps.size() == 4) {
    UR_EXPECT_EQ(int64_t{0}, p.steps[2].confirmations);
    UR_EXPECT_EQ(int64_t{10}, p.steps[2].confirmationsRequired);
  }
  UR_EXPECT_FALSE(p.showQr || p.showAmount || p.showAddress || p.showRefresh || p.showWallets);
  UR_EXPECT_FALSE(p.showPayAnotherWay);
}

// Review: the review copy and nothing else to do.
UR_TEST(CryptoPay_ReviewShowsOnlyTheReviewCopy) {
  cp::InvoiceView v = OpenXmr();
  v.status = "review";
  v.reviewReason = "underpaid";
  v.errorCode = "underpaid";
  const cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(p.review);
  UR_EXPECT_TRUE(p.steps.empty());
  UR_EXPECT_FALSE(p.showQr || p.showAmount || p.showAddress || p.showCountdown || p.showRefresh ||
                  p.showRemaining || p.showWallets || p.zcashNote || p.expired);
  UR_EXPECT_TRUE(p.notice == cp::InvoiceNotice::None);
  UR_EXPECT_FALSE(p.showPayAnotherWay);
}

// Paid: every step done, as the SDK draws it (the confirming one at its full
// count).
UR_TEST(CryptoPay_PaidIsEveryStepDone) {
  cp::InvoiceView v = OpenXmr();
  v.status = "paid";
  v.phase = "paid";
  v.steps = {MakeStep("pending", "done"), MakeStep("seen", "done"), MakeStep("confirming", "done", 1, 1),
             MakeStep("paid", "done")};
  const cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(Kinds(p.steps) == "pending=done seen=done confirming=done 1/1 paid=done");
  UR_EXPECT_FALSE(p.showQr || p.showAmount || p.showRefresh || p.showPayAnotherWay);
}

// A coin that cannot be quoted now, an invoice the server no longer knows, a
// refused re-quote: the line under the invoice, and a way out.
UR_TEST(CryptoPay_FailuresUnderTheInvoice) {
  cp::InvoiceView v = OpenXmr();
  v.status = "expired";
  v.secondsLeft = 0;
  v.errorCode = "asset_unavailable";
  cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(p.notice == cp::InvoiceNotice::Unavailable);
  UR_EXPECT_TRUE(p.showPayAnotherWay);

  v = OpenXmr();
  v.status = "unknown";
  p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(p.notice == cp::InvoiceNotice::Failed);
  UR_EXPECT_TRUE(p.showPayAnotherWay);
  UR_EXPECT_FALSE(p.showQr || p.showAmount || p.showRefresh);
  UR_EXPECT_TRUE(p.steps.empty());

  v = OpenXmr();
  v.errorCode = "rate_limited";
  p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(p.notice == cp::InvoiceNotice::Failed);
  // the quote it had is still good
  UR_EXPECT_TRUE(p.showQr && p.showAmount);
}

// USDC: the memo with its own Copy, while the SDK says it shows (the server
// sent one), and the network-only warning.
UR_TEST(CryptoPay_UsdcShowsTheMemoAndTheNetworkWarning) {
  cp::InvoiceView v = OpenXmr();
  v.asset = "usdc_solana";
  v.ticker = "USDC";
  v.amount = "40.012345";
  v.memo = "9xQeWvG816bUx9EPjHmaT23yvVM2ZWbrrpZb9PusVFin";
  v.showsMemo = true;  // urnet::cryptoPaymentShowsMemo
  v.uri = "solana:" + v.address + "?amount=40.012345";
  v.qrText = v.uri;
  v.lines.networkLine = {"crypto_network_usdc", "USDC on Solana"};
  v.lines.networkWarning = {
      "usdc_solana_network_only",
      "Send USDC on the Solana network only. USDC sent on any other network will not arrive."};
  v.steps = {MakeStep("pending", "current"), MakeStep("paid", "upcoming")};
  cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(p.showMemo);
  UR_EXPECT_TRUE(p.showNetworkWarning);
  UR_EXPECT_TRUE(p.networkWarning == v.lines.networkWarning);
  UR_EXPECT_TRUE(p.amountText == "40.012345 USDC");
  UR_EXPECT_TRUE(Kinds(p.steps) == "pending=current paid=upcoming");
  UR_EXPECT_FALSE(p.zcashNote);
  v.memo.clear();
  v.showsMemo = false;
  UR_EXPECT_FALSE(cp::PresentInvoice(v).showMemo);
  // a memo the SDK does not show stays hidden, whatever the state says
  v.memo = "Rsample";
  UR_EXPECT_FALSE(cp::PresentInvoice(v).showMemo);
  // and one it shows waits for a payable quote
  v.showsMemo = true;
  v.secondsLeft = 0;
  UR_EXPECT_FALSE(cp::PresentInvoice(v).showMemo);
}

// Zcash: the exchange note while there is something to send.
UR_TEST(CryptoPay_ZcashShowsTheExchangeNote) {
  cp::InvoiceView v = OpenXmr();
  v.asset = "zec";
  v.ticker = "ZEC";
  UR_EXPECT_TRUE(cp::PresentInvoice(v).zcashNote);
  v.status = "confirming";
  UR_EXPECT_FALSE(cp::PresentInvoice(v).zcashNote);
}

// TAO has no payment link (mmm/BTCPAY.md §17.6): the SDK keeps no URI and its
// qr text is the address alone. The QR holds the address, with the note saying
// so; nothing opens in a wallet; the invoice says to send exactly this amount
// to this address, Copy amount and Copy address, with the Bittensor network's
// warning.
UR_TEST(CryptoPay_TaoIsPaidByHand) {
  cp::InvoiceView v = OpenXmr();
  v.asset = "tao";
  v.ticker = "TAO";
  v.amount = "0.123456";
  v.address = "5" + std::string(47, 'T');
  v.uri.clear();
  v.uriChecked = false;
  v.qrText = v.address;
  v.noPaymentLink = true;  // urnet::cryptoPaymentUriScheme("tao") is ""
  v.wallets = {"btcli", "Talisman", "Nova", "SubWallet", "Polkadot.js", "Ledger", "Tangem"};
  v.lines.networkLine = {"crypto_network_tao", "Bittensor network"};
  v.lines.networkWarning = {"crypto_invoice_tao_network_only",
                            "Send only TAO on the Bittensor network to this address. TAO sent on any "
                            "other network won't be credited."};
  v.lines.noPaymentLinkNote = {"crypto_invoice_tao_no_payment_link",
                               "TAO has no payment link, so this QR code holds only the address. Enter "
                               "the exact amount in your wallet."};
  cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(p.showQr && p.qrText == v.address);
  UR_EXPECT_TRUE(p.noPaymentLinkNote == v.lines.noPaymentLinkNote);
  UR_EXPECT_FALSE(p.showOpenWallet);
  UR_EXPECT_TRUE(p.noPaymentLink && p.showByHandLine);
  UR_EXPECT_TRUE(p.showAmount && p.amountText == "0.123456 TAO");
  UR_EXPECT_TRUE(p.showAddress && p.showNetworkWarning);
  UR_EXPECT_TRUE(p.networkWarning.key == "crypto_invoice_tao_network_only");
  UR_EXPECT_FALSE(p.showMemo || p.zcashNote);
  // the one coin the SDK names wallets for, its names joined untranslated
  UR_EXPECT_TRUE(p.showCountdown && p.showWallets);
  UR_EXPECT_TRUE(p.walletsText == "btcli, Talisman, Nova, SubWallet, Polkadot.js, Ledger, Tangem");
  // the QR holds the address or nothing
  v.qrText = "tao:" + v.address;
  p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr);
  UR_EXPECT_TRUE(p.noPaymentLinkNote.key.empty());
  UR_EXPECT_TRUE(p.showAmount && p.showAddress && p.showByHandLine);
  v.qrText.clear();
  p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr);
  UR_EXPECT_TRUE(p.showAmount && p.showAddress);
  // only a coin the SDK says has no payment link draws its bare address
  v.qrText = v.address;
  v.noPaymentLink = false;
  p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr || p.showByHandLine || !p.noPaymentLinkNote.key.empty());
  v.noPaymentLink = true;
  // a top-up: the address and its warning for the rest, never the QR or the
  // full amount
  v.status = "seen";
  v.received = "0.12";
  v.remaining = "0.003456";
  p = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(p.showAddress && p.showNetworkWarning);
  UR_EXPECT_FALSE(p.showQr || p.showAmount || p.showByHandLine);
  UR_EXPECT_TRUE(p.remainingText == "0.003456 TAO");
  // expired: nothing to send
  v.status = "pending";
  v.secondsLeft = 0;
  p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr || p.showAmount || p.showByHandLine || !p.noPaymentLinkNote.key.empty());
  // nor the wallets to send it with: the line shows only while there is
  // something to send (payable, or a top-up), so not once the quote expired,
  // while confirming, in review or before the first status, whatever the SDK
  // names
  UR_EXPECT_FALSE(p.showWallets);
  v.secondsLeft = 1799;
  for (const char* status : {"confirming", "review", ""}) {
    v.status = status;
    UR_EXPECT_TRUE_MSG(status, !cp::PresentInvoice(v).showWallets);
  }
}

// Bitcoin, ETH and native SOL: their links' QR and Open in wallet as Monero's,
// and each network's warning with the address. No wallets line: the SDK names
// TAO's wallets alone (the owner, 2026-11-11), so these name none. No memo:
// the server sends one for USDC alone, SOL included in the "".
UR_TEST(CryptoPay_TheNewCoinsLinksAndWarnings) {
  struct Linked {
    const char* asset;
    const char* ticker;
    std::string uri;
    const char* warning;
  };
  for (const Linked& c : {
           Linked{"btc", "BTC", "bitcoin:bc1qsample?amount=0.00061234", "crypto_invoice_bitcoin_network_only"},
           Linked{"eth", "ETH", "ethereum:0xsample@1?value=12000000000000000", "crypto_invoice_eth_network_only"},
           Linked{"sol", "SOL", "solana:Ssample?amount=0.251234", "crypto_invoice_sol_network_only"},
       }) {
    cp::InvoiceView v = OpenXmr();
    v.asset = c.asset;
    v.ticker = c.ticker;
    v.uri = c.uri;
    v.qrText = c.uri;
    v.wallets.clear();  // urnet::cryptoWalletSuggestions: none for this coin
    v.lines.networkWarning = {c.warning, "Send only this coin on its network."};
    const cp::InvoicePresentation p = cp::PresentInvoice(v);
    UR_EXPECT_TRUE_MSG(c.asset, p.showQr && p.qrText == c.uri && p.showOpenWallet);
    UR_EXPECT_TRUE_MSG(c.asset, !p.noPaymentLink && !p.showByHandLine && p.noPaymentLinkNote.key.empty());
    UR_EXPECT_TRUE_MSG(c.asset, p.showNetworkWarning && p.networkWarning.key == c.warning);
    UR_EXPECT_TRUE_MSG(c.asset, !p.showMemo && !p.zcashNote);
    // a coin the SDK lists no wallets for has no such line, never an empty one
    UR_EXPECT_TRUE_MSG(c.asset, !p.showWallets && p.walletsText.empty());
  }
}

// The marks' files (BrandIcons.cpp CoinMark): an icon key names an installed
// file and nothing else, and SvgPathData reads one plain path out of it.
UR_TEST(CryptoPay_AMarkIsOnePlainPath) {
  for (const char* key : {"coin_tao", "coin_btc", "coin_eth", "coin_sol"}) {
    UR_EXPECT_TRUE_MSG(key, cp::IsMarkKey(key));
  }
  for (const char* key : {"", "../coin_tao", "coin_tao.svg", "Coin_Tao", "coin tao", "coins/tao"}) {
    UR_EXPECT_FALSE(cp::IsMarkKey(key));
  }
  const std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\">\n"
      "<!-- a comment that names <path d=\"M9 9Z\"/> -->\n"
      "<path fill=\"currentColor\" d=\"M14.38 18.415V8.577C14.38 6.119 12.351 4.116 9.893 4.116Z\"/>\n"
      "</svg>\n";
  UR_EXPECT_TRUE(cp::SvgPathData(svg) == "M14.38 18.415V8.577C14.38 6.119 12.351 4.116 9.893 4.116Z");
  UR_EXPECT_TRUE(cp::SvgPathData("<svg><path d=\"M0 0Z\"/><path d=\"M1 1Z\"/></svg>").empty());
  UR_EXPECT_TRUE(cp::SvgPathData("<svg></svg>").empty());
  UR_EXPECT_TRUE(cp::SvgPathData("<svg><path fill=\"x\"/></svg>").empty());
  UR_EXPECT_TRUE(cp::SvgPathData("<svg><path d=\"M0 0'/><text/>\"/></svg>").empty());
  UR_EXPECT_TRUE(cp::SvgPathData("<svg><path d=\"M0 0&amp;Z\"/></svg>").empty());
  UR_EXPECT_TRUE(cp::SvgPathData("<svg><path d=\"M0 0Z").empty());
  UR_EXPECT_TRUE(cp::SvgPathData("<!-- <path d=\"M0 0Z\"/>").empty());
  UR_EXPECT_TRUE(cp::SvgPathData("<svg><pathway/><path\nd=\"M0 0Z\"/></svg>") == "M0 0Z");
  UR_EXPECT_EQ(24.0, cp::kMarkViewBox);
}

// The four marks meson installs are each one plain path on the 24 x 24 box,
// filled nonzero, that SvgPathData reads.
UR_TEST(CryptoPay_TheInstalledMarksReadAsOnePath) {
  for (const char* key : {"coin_tao", "coin_btc", "coin_eth", "coin_sol"}) {
    const std::string svg = ReadAsset(std::string(key) + ".svg");
    UR_EXPECT_TRUE_MSG(key, !svg.empty());
    UR_EXPECT_TRUE_MSG(key, !cp::SvgPathData(svg).empty());
    UR_EXPECT_TRUE_MSG(key, svg.find("viewBox=\"0 0 24 24\"") != std::string::npos);
    UR_EXPECT_TRUE_MSG(key, svg.find("evenodd") == std::string::npos);
  }
}

// A resumed invoice before its first status poll: money may already be on its
// way, so nothing offers to pay, re-quote or walk away from it until the
// server answers (CONTRACT §6.2.1). The SDK lists no steps before the first
// status, so none stand in meanwhile.
UR_TEST(CryptoPay_AResumedInvoiceWaitsForItsStatus) {
  cp::InvoiceView v = OpenXmr();
  v.status = "";
  v.steps.clear();  // the SDK's steps for an unpolled invoice: none
  const cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr || p.showAmount || p.showAddress || p.showMemo || p.showCountdown ||
                  p.showWallets || p.expired);
  UR_EXPECT_FALSE(p.showRefresh);
  UR_EXPECT_FALSE(p.showPayAnotherWay);
  UR_EXPECT_TRUE(p.steps.empty());
  // its quote ran out while the app was closed: still nothing until the
  // server says whether anything arrived
  v.secondsLeft = 0;
  const cp::InvoicePresentation stale = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(stale.expired || stale.showQr || stale.showAmount || stale.showRefresh ||
                  stale.showPayAnotherWay);
  // the first answer: nothing arrived and the quote is gone
  v.status = "expired";
  const cp::InvoicePresentation answered = cp::PresentInvoice(v);
  UR_EXPECT_TRUE(answered.expired && answered.showRefresh && answered.showPayAnotherWay);
  UR_EXPECT_FALSE(answered.showQr || answered.showAmount);
}

// No invoice, or one still starting: nothing to show yet.
UR_TEST(CryptoPay_NothingToShowBeforeTheQuote) {
  cp::InvoiceView v;
  v.phase = "starting";
  v.asset = "xmr";
  const cp::InvoicePresentation p = cp::PresentInvoice(v);
  UR_EXPECT_FALSE(p.showQr || p.showAmount || p.showAddress || p.showCountdown || p.showRefresh ||
                  p.expired || p.review || p.showWallets || p.showPayAnotherWay);
  UR_EXPECT_TRUE(p.steps.empty());
  UR_EXPECT_TRUE(p.notice == cp::InvoiceNotice::None);
}

// The controller's directory: one per network server, under the SDK's root.
UR_TEST(CryptoPay_StorageDirIsPerServer) {
  UR_EXPECT_TRUE(cp::StorageDirName("api.bringyour.com") == "api.bringyour.com");
  UR_EXPECT_TRUE(cp::StorageDirName("my.host:8443/x") == "my.host_8443_x");
  UR_EXPECT_TRUE(cp::StorageDirName("") == "default");
  UR_EXPECT_TRUE(cp::StorageDirName("..") == "default");
  UR_EXPECT_TRUE(cp::StorageDirName("../etc") == ".._etc");
  UR_EXPECT_TRUE(cp::StorageDir("/data/urnetwork", "ur.io") == "/data/urnetwork/crypto_invoice/ur.io");
  UR_EXPECT_TRUE(cp::StorageDir("", "ur.io").empty());
}

// The sign-out clears every file the controller wrote, whatever their names,
// and leaves the directory itself (owner-only) for the next sign-in.
UR_TEST(CryptoPay_SignOutWipesTheStorage) {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("urnw-crypto-pay-test-" + std::to_string(::getpid()));
  std::error_code ignored;
  fs::remove_all(root, ignored);
  const std::string dir = cp::StorageDir(root.string(), "ur.io");
  UR_EXPECT_TRUE(cp::EnsureStorageDir(dir));
  const fs::perms perms = fs::status(dir).permissions();
  UR_EXPECT_TRUE((perms & fs::perms::all) == fs::perms::owner_all);
  for (const char* name : {".crypto_invoice", ".payment_method", ".crypto_invoice.tmp-123"}) {
    std::ofstream(fs::path(dir) / name) << "{}";
  }
  fs::create_directories(fs::path(dir) / "kept");
  UR_EXPECT_EQ(size_t{3}, cp::WipeStorage(dir));
  UR_EXPECT_FALSE(fs::exists(fs::path(dir) / ".crypto_invoice"));
  UR_EXPECT_FALSE(fs::exists(fs::path(dir) / ".payment_method"));
  UR_EXPECT_TRUE(fs::is_directory(dir));
  UR_EXPECT_TRUE(fs::is_directory(fs::path(dir) / "kept"));
  UR_EXPECT_EQ(size_t{0}, cp::WipeStorage(dir));
  UR_EXPECT_EQ(size_t{0}, cp::WipeStorage((root / "missing").string()));
  UR_EXPECT_EQ(size_t{0}, cp::WipeStorage(""));
  UR_EXPECT_FALSE(cp::EnsureStorageDir(""));
  fs::remove_all(root, ignored);
}

// A one-time crypto year does not renew, and the Stripe portal never manages it.
UR_TEST(CryptoPay_ACryptoStoreDoesNotRenew) {
  UR_EXPECT_TRUE(cp::StoreDoesNotRenew("crypto"));
  for (const char* family : {"stripe", "apple", "google", "other", ""}) {
    UR_EXPECT_FALSE(cp::StoreDoesNotRenew(family));
  }
}

// The server's URIs encode at level M into a code the canvas draws at whole
// pixels per module, at least 3 of them.
UR_TEST(CryptoPay_PaymentUrisEncodeAtAScannableSize) {
  const std::string xmr = "monero:8" + std::string(94, 'B') +
                          "?tx_amount=0.072387&tx_description=URnetwork%20Pro%20yearly%20(welcome%20offer)";
  const std::string zec = "zcash:u1" + std::string(212, 'q') +
                          "?amount=0.123456&message=URnetwork%20Pro%20yearly%20(welcome%20offer)";
  const std::string usdc =
      "solana:" + std::string(44, 'C') +
      "?amount=40.012345&spl-token=EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v&reference=" +
      std::string(44, 'D') + "&label=URnetwork&message=URnetwork%20Pro%20yearly";
  // mmm/BTCPAY.md §17.7: BIP 21, EIP-681, Solana Pay without spl-token, and
  // TAO's address alone (an ss58 address)
  const std::string btc = "bitcoin:bc1q" + std::string(38, 'q') +
                          "?amount=0.00061234&label=URnetwork&message=URnetwork%20Pro%20yearly";
  const std::string eth = "ethereum:0x" + std::string(40, 'a') + "@1?value=12000000000000000";
  const std::string sol = "solana:" + std::string(44, 'S') + "?amount=0.251234&reference=" +
                          std::string(44, 'R') + "&label=URnetwork&message=URnetwork%20Pro%20yearly";
  const std::string tao = "5" + std::string(47, 'T');
  for (const std::string& uri : {xmr, zec, usdc, btc, eth, sol, tao}) {
    const cp::QrModules code = cp::EncodePaymentQr(uri);
    UR_EXPECT_TRUE_MSG(uri, 21 <= code.count && code.count <= 97);  // versions 1..20
    UR_EXPECT_TRUE_MSG(uri, code.dark.size() ==
                                static_cast<size_t>(code.count) * static_cast<size_t>(code.count));
    const int side = cp::QrCanvasSide(code.count);
    UR_EXPECT_TRUE_MSG(uri, cp::kQrMinSide <= side && side <= cp::kQrMaxSide);
    UR_EXPECT_TRUE_MSG(uri, 3 <= cp::QrModulePixels(side, code.count));
    // the finder pattern's corner is dark: a real code, not an empty grid
    UR_EXPECT_TRUE_MSG(uri, code.Dark(0, 0) && code.Dark(code.count - 1, 0));
  }
  UR_EXPECT_EQ(0, cp::EncodePaymentQr("").count);
}

UR_TEST(CryptoPay_QrSizing) {
  UR_EXPECT_EQ(cp::kQrMinSide, cp::QrCanvasSide(0));
  UR_EXPECT_EQ(cp::kQrMinSide, cp::QrCanvasSide(21));  // 29 modules * 4 = 116 -> floor 200
  UR_EXPECT_EQ(260, cp::QrCanvasSide(57));             // 65 * 4
  UR_EXPECT_EQ(cp::kQrMaxSide, cp::QrCanvasSide(97));  // 105 * 4 = 420 -> cap 320
  UR_EXPECT_EQ(4, cp::QrModulePixels(260, 57));
  UR_EXPECT_EQ(3, cp::QrModulePixels(320, 97));
  UR_EXPECT_EQ(0, cp::QrModulePixels(20, 57));
  UR_EXPECT_EQ(0, cp::QrModulePixels(260, 0));
}
