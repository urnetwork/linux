// The signed-in network's crypto invoice, held for every surface of the GUI
// (mmm/ZCASH-CONTRACT.md §4.4, §6.2): one urnet::CryptoInvoiceViewController per
// sign-in, which owns the quote, the countdown, the re-quote, the status polls
// and the saved invoice; this only carries it onto the GTK loop.
//
//   * SignIn (MainWindow::ApplyAuthState) opens the controller on the Api and
//     Resume()s the invoice saved for the jwt's network, so a payment started
//     before a quit shows again at launch ("Payment in progress" on Account).
//     A real sign-out (SdkHost::Logout) closes it and clears its directory
//     (CryptoInvoiceStorage.hpp), with the saved invoice's status token and
//     the remembered method; the window's signed-out state only closes it, so
//     a switch to another network server keeps this one's saved invoice.
//   * Nothing polls in the background (mmm/ZCASH.md §0.3): the controller runs
//     only while a view shows the invoice. Every view that shows it counts
//     itself visible (AddVisibleViewer, from its map) and gone
//     (RemoveVisibleViewer, from its unmap); the first starts the controller,
//     the last stops it.
//   * The controller's listener fires on an SDK thread. It only marshals
//     (PostToMain); State() is read and the subscribers run on the GTK loop,
//     and a change that lands after a sign-out or the host's end is dropped
//     (epoch).
//   * State() is crypto_pay::InvoiceView: the controller's state (its status
//     steps among it) plus the SDK's answers beside it: the payment URI check,
//     the wallets that work, whether the coin has a payment link, its lines
//     and whether its memo shows.
//
// The last payment method chosen on this device is the controller's
// (Get/SetPaymentMethod, its .payment_method), so a sign-out forgets it with
// the invoice. Main loop only. Owned by SdkHost (SdkHost::CryptoInvoice).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <urnetwork_sdk.hpp>

#include "CryptoPay.hpp"

namespace urnw {

class SdkHost;

class CryptoInvoiceHost {
 public:
  explicit CryptoInvoiceHost(SdkHost& host);
  ~CryptoInvoiceHost();
  CryptoInvoiceHost(const CryptoInvoiceHost&) = delete;
  CryptoInvoiceHost& operator=(const CryptoInvoiceHost&) = delete;

  // A sign-in (or a launch signed in): the controller for the jwt's network,
  // and its saved invoice. Again for the same network only resumes.
  void SignIn();
  // The window shows no session (a sign-out, or a switch to a network server
  // this device is not signed in to): the controller ends; what it saved
  // stays, for that server's next sign-in.
  void Close();
  // A real sign-out (SdkHost::Logout): the controller ends and its directory
  // is cleared, as the SDK's sign-out clears its local-state directory.
  void SignOut();
  // The saved invoice of the signed-in network, if any (Account appearing).
  // True when an invoice is on screen after the call.
  bool Resume();

  // Starts an invoice for `asset` and a plan (crypto_pay::CoinPlan). False,
  // with nothing started, when there is no controller (no session).
  bool StartInvoice(const std::string& asset, const std::string& plan);
  void Requote();
  // "Pay another way", or giving up on a failed invoice: with nothing
  // received the saved invoice goes; once money arrived it is only hidden.
  void Dismiss();

  // The method last chosen on this device ("" when none), and remembering one.
  std::string PaymentMethod() const;
  void SetPaymentMethod(const std::string& method);

  // A view started or stopped showing the invoice.
  void AddVisibleViewer();
  void RemoveVisibleViewer();

  const crypto_pay::InvoiceView& State() const { return state_; }

  // Runs `changed` on the GTK loop after every change of State(). The token
  // unsubscribes; subscribers outlive nothing (each unsubscribes in its
  // destructor, and this host outlives the windows: SdkHost owns it).
  int Subscribe(std::function<void()> changed);
  void Unsubscribe(int token);

 private:
  // Reads the controller's state and tells the subscribers.
  void Refresh();
  // Ends the controller (the saved invoice stays saved), without telling.
  void CloseController();
  // The signed-in network's id from the stored jwt, "" without a session.
  std::string CurrentNetworkId();

  SdkHost& host_;
  std::optional<urnet::CryptoInvoiceViewController> vc_;
  std::optional<urnet::Sub> sub_;
  std::string networkId_;   // the network the controller was opened for ("" unknown)
  uint64_t apiHandle_ = 0;  // the Api it was opened on (a server switch replaces it)
  std::string storageDir_;  // where it saves, cleared at sign-out
  // bumped by every close and by the destructor: a change posted before
  // either is dropped without touching this host
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);
  int visibleViewers_ = 0;
  crypto_pay::InvoiceView state_;
  std::map<int, std::function<void()>> subscribers_;
  int nextToken_ = 1;
};

}  // namespace urnw
