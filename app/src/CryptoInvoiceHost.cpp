// SPDX-License-Identifier: MPL-2.0
#include "CryptoInvoiceHost.hpp"

#include <exception>
#include <utility>

#include <glib.h>

#include "CryptoInvoiceStorage.hpp"
#include "SdkHost.hpp"
#include "Ui.hpp"  // PostToMain

namespace urnw {
namespace {

crypto_pay::InvoiceView NoInvoice() {
  crypto_pay::InvoiceView view;
  view.phase = crypto_pay::kPhaseNone;
  return view;
}

// The controller's state as the screens read it, with the SDK's answers
// beside it. The controller already drops a URI that fails the check; it is
// checked again here because the screens must never show or open one that
// fails it, whatever produced the state. The QR draws the state's qr text
// (urnet::cryptoPaymentQrText); a coin without a payment URI scheme (TAO) is
// paid by hand.
crypto_pay::InvoiceView ViewOf(const urnet::CryptoInvoiceState& state) {
  crypto_pay::InvoiceView view;
  view.phase = state.phase;
  view.intentId = state.intent_id;
  view.asset = state.asset;
  view.plan = state.plan;
  view.itemId = state.item_id;
  view.address = state.address;
  view.amount = state.amount;
  view.ticker = state.ticker.empty() ? urnet::cryptoAssetTicker(state.asset) : state.ticker;
  view.uri = state.uri;
  view.qrText = state.qr_text;
  view.memo = state.memo;
  view.priceUsd = state.price_usd;
  view.expiresAt = state.expires_at;
  view.secondsLeft = state.seconds_left;
  view.status = state.status;
  view.reviewReason = state.review_reason;
  view.received = state.received;
  view.remaining = state.remaining;
  view.confirmations = state.confirmations;
  view.confirmationsRequired = state.confirmations_required;
  view.refreshing = state.refreshing;
  view.errorCode = state.error_code;
  view.errorMessage = state.error_message;
  view.uriChecked =
      !state.uri.empty() && urnet::checkCryptoPaymentUri(state.asset, state.uri, state.address);
  if (const auto wallets = urnet::cryptoWalletSuggestions(state.asset)) {
    view.wallets.assign(wallets->begin(), wallets->end());
  }
  view.noPaymentLink =
      crypto_pay::IsCoin(state.asset) && urnet::cryptoPaymentUriScheme(state.asset).empty();
  return view;
}

}  // namespace

CryptoInvoiceHost::CryptoInvoiceHost(SdkHost& host) : host_(host), state_(NoInvoice()) {}

CryptoInvoiceHost::~CryptoInvoiceHost() {
  subscribers_.clear();
  CloseController();
}

std::string CryptoInvoiceHost::CurrentNetworkId() {
  if (!host_.IsLoggedIn()) return std::string();
  const auto byJwt = host_.ParseByJwt();
  if (byJwt && byJwt->NetworkId) return *byJwt->NetworkId;
  return std::string();
}

void CryptoInvoiceHost::SignIn() {
  if (!host_.IsLoggedIn()) return;
  // A repeated sign-in transition (the session reconnecting, a second auth
  // event) keeps the controller and only resumes, so an invoice starting or
  // on screen is not thrown away. A new controller is for a new Api (another
  // network server) or a network the stored jwt names differently; the jwt is
  // read without waiting on a command, so an unknown network (empty) keeps
  // the controller.
  const std::string networkId = CurrentNetworkId();
  const uint64_t api = host_.api().handle();
  if (vc_ && api == apiHandle_ && (networkId.empty() || networkId == networkId_)) {
    if (networkId_.empty()) networkId_ = networkId;
    Resume();
    return;
  }
  CloseController();
  networkId_ = networkId;
  apiHandle_ = api;
  storageDir_ = host_.CryptoInvoiceStorageDir();
  // owner-only, like the SDK's local-state directory; a directory that cannot
  // be made keeps the invoice in memory for this run only
  const std::string dir = crypto_pay::EnsureStorageDir(storageDir_) ? storageDir_ : std::string();
  if (dir.empty()) {
    g_warning("crypto invoice: no storage directory; a started invoice lasts this run only");
  }
  try {
    // The controller reads the network from the Api's jwt itself, and resumes
    // only an invoice saved for that network.
    vc_ = urnet::newCryptoInvoiceViewController(host_.api(), dir);
    auto epoch = epoch_;
    const uint64_t issued = *epoch;
    // fires on an SDK thread: marshal only, and never touch this host there
    sub_ = vc_->addCryptoInvoiceListener([this, epoch, issued] {
      PostToMain([this, epoch, issued] {
        if (*epoch != issued) return;  // closed (sign-out, quit) since
        Refresh();
      });
    });
    if (0 < visibleViewers_) vc_->start();
    vc_->resume();
  } catch (const std::exception& e) {
    g_warning("crypto invoice: the controller could not be opened: %s", e.what());
    sub_.reset();
    vc_.reset();
  }
  Refresh();
}

void CryptoInvoiceHost::Close() {
  CloseController();
  storageDir_.clear();
  Refresh();
}

void CryptoInvoiceHost::SignOut() {
  // the directory of the session that is leaving (the server bound now)
  const std::string dir = storageDir_.empty() ? host_.CryptoInvoiceStorageDir() : storageDir_;
  CloseController();
  // what the SDK's sign-out does to its local-state directory: the saved
  // invoice (with its status token) and the remembered method go
  const size_t removed = crypto_pay::WipeStorage(dir);
  if (0 < removed) g_message("crypto invoice: cleared %zu saved file(s) at sign-out", removed);
  storageDir_.clear();
  Refresh();
}

void CryptoInvoiceHost::CloseController() {
  ++*epoch_;
  sub_.reset();
  if (vc_) {
    try {
      // the saved invoice stays saved
      vc_->close();
    } catch (const std::exception& e) {
      g_warning("crypto invoice: close failed: %s", e.what());
    }
    vc_.reset();
  }
  networkId_.clear();
  apiHandle_ = 0;
}

bool CryptoInvoiceHost::Resume() {
  if (!vc_) return false;
  bool showing = false;
  try {
    showing = vc_->resume();
  } catch (const std::exception& e) {
    g_warning("crypto invoice: resume failed: %s", e.what());
  }
  Refresh();
  return showing;
}

bool CryptoInvoiceHost::StartInvoice(const std::string& asset, const std::string& plan) {
  if (!vc_) return false;
  try {
    // no item (a plan, not a data pack), and no storefront on the desktop:
    // the server resolves the regional tier, as it does for the balance
    vc_->startInvoice(asset, plan, "", "");
  } catch (const std::exception& e) {
    g_warning("crypto invoice: start failed: %s", e.what());
    return false;
  }
  Refresh();
  return true;
}

void CryptoInvoiceHost::Requote() {
  if (!vc_) return;
  try {
    vc_->requote();
  } catch (const std::exception& e) {
    g_warning("crypto invoice: requote failed: %s", e.what());
  }
  Refresh();
}

void CryptoInvoiceHost::Dismiss() {
  if (!vc_) return;
  try {
    vc_->dismiss();
  } catch (const std::exception& e) {
    g_warning("crypto invoice: dismiss failed: %s", e.what());
  }
  Refresh();
}

std::string CryptoInvoiceHost::PaymentMethod() const {
  if (!vc_) return std::string();
  try {
    return vc_->getPaymentMethod();
  } catch (const std::exception& e) {
    g_warning("crypto invoice: reading the payment method failed: %s", e.what());
    return std::string();
  }
}

void CryptoInvoiceHost::SetPaymentMethod(const std::string& method) {
  if (!vc_) return;
  try {
    vc_->setPaymentMethod(method);
  } catch (const std::exception& e) {
    g_warning("crypto invoice: saving the payment method failed: %s", e.what());
  }
}

void CryptoInvoiceHost::AddVisibleViewer() {
  if (visibleViewers_++ != 0 || !vc_) return;
  try {
    vc_->start();  // polls at once, then on the status's cadence, and ticks
  } catch (const std::exception& e) {
    g_warning("crypto invoice: start failed: %s", e.what());
  }
}

void CryptoInvoiceHost::RemoveVisibleViewer() {
  if (visibleViewers_ == 0) return;
  if (--visibleViewers_ != 0 || !vc_) return;
  try {
    vc_->stop();  // nothing runs while no view shows the invoice
  } catch (const std::exception& e) {
    g_warning("crypto invoice: stop failed: %s", e.what());
  }
}

int CryptoInvoiceHost::Subscribe(std::function<void()> changed) {
  const int token = nextToken_++;
  subscribers_[token] = std::move(changed);
  return token;
}

void CryptoInvoiceHost::Unsubscribe(int token) { subscribers_.erase(token); }

void CryptoInvoiceHost::Refresh() {
  crypto_pay::InvoiceView next = NoInvoice();
  if (vc_) {
    try {
      if (const auto state = vc_->getState()) next = ViewOf(*state);
    } catch (const std::exception& e) {
      // keep what the screens show; the next change reads again
      g_warning("crypto invoice: reading the state failed: %s", e.what());
      return;
    }
  }
  state_ = std::move(next);
  // a copy: a subscriber may unsubscribe itself, or another, while it runs
  const std::map<int, std::function<void()>> subscribers = subscribers_;
  for (const auto& [token, changed] : subscribers) {
    if (subscribers_.count(token) == 1 && changed) changed();
  }
}

}  // namespace urnw
