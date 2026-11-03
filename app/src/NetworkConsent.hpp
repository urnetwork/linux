// The network payout consent in the Earnings page's settlement v2 area
// (CONSENT.md §4, amendments A1 and A2): one signature by a Bittensor coldkey
// covers every provider client of the network, so the network's app-only
// providers are paid to that coldkey (VERSION2-FINAL.md §11).
//
// Sign with wallet, and Change wallet, which runs the same steps, in the order
// every app shares:
//   1. the wallet is picked on the shared chooser;
//   2. the user types the coldkey the consent names: a deliberate choice, so
//      nothing is prefilled, and it must pass the SDK's ss58 check before
//      anything is asked;
//   3. SnNetworkConsentChallenge(coldkey) issues the consent message, for a
//      range from the next epoch (A2.1);
//   4. the wallet signs exactly that message through the existing connector
//      (SdkHost::SignBittensorConsent: the SDK's BittensorWalletSession with
//      purpose "consent", on the bridge, bound to the message's coldkey and
//      expiry);
//   5. SnNetworkConsentSubmit stores it, only when the proof is for the issued
//      message and its coldkey (anything else reads sn_consent_failed), and
//      the status is read again.
// Nothing but the issued message is signed or submitted: the wallet is handed
// the challenge as the SDK returned it, and the submit sends that challenge's
// coldkey and message with the wallet's signature.
//
// The status is active while a network consent exists whose through_epoch has
// not passed: in force, or from the next epoch (A2.3). Either way the section
// shows the coldkey with Change wallet and the note that a new wallet takes
// effect from the next epoch, so right after a signing it shows the new
// coldkey that way. With no consent it explains itself and offers Sign with
// wallet.
//
// Three refusals read in the section's own words (A1), each only from the step
// the SDK reports it for: an expired challenge (challenge_expired: the SDK's
// challenge, the session's expiry or the server's submit), a signature the
// server does not verify for the coldkey (signature_mismatch, the submit), and
// a session that is not the network owner's (network_owner_only, the status,
// the challenge and the submit). Text from a wallet's return is never one of
// them (A3). A wallet step another wallet flow took over ends quietly; every
// other failure goes to the page's error surface.
//
// Header-only and free of GTK and the SDK (tests/NetworkConsentTest.cpp): the
// page hands the flow its ports, the SDK's consent calls and the wallet, and
// draws its view.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "WalletBridgeRoute.hpp"

namespace urnw::consent {

// The SDK's stable codes of the refusals the section words itself (A1). Its
// errors read "code: message".
inline constexpr std::string_view kNetworkOwnerOnly = "network_owner_only";
inline constexpr std::string_view kChallengeExpired = "challenge_expired";
inline constexpr std::string_view kSignatureMismatch = "signature_mismatch";

// Whether `error` is the SDK's `code`: the code alone, or "code: message".
constexpr bool HasCode(std::string_view error, std::string_view code) noexcept {
  if (error == code) return true;
  return error.size() >= code.size() + 2 && error.substr(0, code.size()) == code &&
         error.substr(code.size(), 2) == ": ";
}

// The refusal a failed step shows in the section. None: the error is not one
// of them, and goes to the page's error surface.
enum class Notice { None, Expired, Failed, OwnerOnly };

// The step a failure comes from. Each step words only the refusals the SDK
// reports for it (A1), so no text a wallet's return carries can read as
// another step's refusal (A3).
enum class Step { Status, Challenge, Signature, Submit };

constexpr Notice NoticeFor(Step step, std::string_view error) noexcept {
  const bool ownerOnly = HasCode(error, kNetworkOwnerOnly);
  const bool expired = HasCode(error, kChallengeExpired);
  switch (step) {
    case Step::Status:
      return ownerOnly ? Notice::OwnerOnly : Notice::None;
    case Step::Challenge:
      if (expired) return Notice::Expired;
      return ownerOnly ? Notice::OwnerOnly : Notice::None;
    case Step::Signature:
      // the session's own expiry: the owner and the signature's match are
      // the challenge's and the submit's to refuse
      return expired ? Notice::Expired : Notice::None;
    case Step::Submit:
      if (expired) return Notice::Expired;
      if (HasCode(error, kSignatureMismatch)) return Notice::Failed;
      return ownerOnly ? Notice::OwnerOnly : Notice::None;
  }
  return Notice::None;
}

// A wallet step another wallet flow took the bridge from: nothing failed.
constexpr bool IsQuiet(std::string_view error) noexcept {
  return bridge::IsSuperseded(error);
}

// The SDK's SnNetworkConsentStatus. Its generation is not read: it stays 0
// until the server reports it (A2.2).
struct Status {
  bool active = false;
  std::string coldkeySs58;
  int64_t fromEpoch = 0;
  int64_t throughEpoch = 0;
};

// The SDK's SnNetworkConsentChallengeResult: `message` is the exact text the
// wallet signs, for `coldkeySs58`, until `expiresAt` (unix seconds).
struct Challenge {
  std::string message;
  std::string coldkeySs58;
  int64_t fromEpoch = 0;
  int64_t throughEpoch = 0;
  int64_t expiresAt = 0;
};

// The consent session's answer: its proof, the address that signed, the
// message it signed and the sr25519 signature (0x + 128 hex).
struct Signed {
  bool ok = false;
  std::string address;
  std::string message;
  std::string signature;
  std::string error;
};

// The submit's answer; a refusal the server coded reads "code: message".
struct Submitted {
  bool ok = false;
  std::string error;
};

enum class Phase {
  Hidden,       // no session: nothing shows
  Loading,      // the status is being read
  Unavailable,  // the status could not be read
  None,         // no consent
  Active,       // a consent in force or from the next epoch
  Signing,      // a signing in flight, from the challenge to the submit
};

// The sentence the section leads with.
enum class Line {
  Loading,      // loading
  Unavailable,  // something_went_wrong
  None,         // sn_consent_none
  Active,       // sn_consent_active, with the coldkey
  Signing,      // sn_consent_signing
};

// The section's button.
enum class Action {
  None,
  Sign,    // sn_consent_sign
  Change,  // sn_consent_change
};

// What the section shows.
struct View {
  bool visible = false;
  Line line = Line::Loading;
  std::string coldkeySs58;  // the active consent's (the page shortens it)
  bool body = false;        // sn_consent_body, under sn_consent_none
  Action action = Action::None;
  bool changeNote = false;  // sn_consent_change_note, with Change wallet
  Notice notice = Notice::None;

  bool operator==(const View& other) const {
    return visible == other.visible && line == other.line && coldkeySs58 == other.coldkeySs58 &&
           body == other.body && action == other.action && changeNote == other.changeNote &&
           notice == other.notice;
  }
};

// The section's state over its loads and signings. Runs on one thread (the
// page's main loop); every port answers once, and an answer for a load or a
// signing that a later Load, Reset or Start replaced is dropped.
class Flow {
 public:
  using StatusDone = std::function<void(std::optional<Status> status, std::string error)>;
  using ChallengeDone =
      std::function<void(std::optional<Challenge> challenge, std::string error)>;
  using SignDone = std::function<void(Signed signed_)>;
  using SubmitDone = std::function<void(Submitted submitted)>;

  struct Ports {
    // SnNetworkConsentStatus
    std::function<void(StatusDone done)> status;
    // the SDK's local ss58 check of a typed coldkey (ValidateSs58): no request
    std::function<bool(const std::string& address)> validSs58;
    // SnNetworkConsentChallenge
    std::function<void(const std::string& coldkeySs58, ChallengeDone done)> challenge;
    // the consent session: `walletId` signs challenge.message
    std::function<void(const std::string& walletId, const Challenge& challenge, SignDone done)>
        sign;
    // SnNetworkConsentSubmit
    std::function<void(const std::string& coldkeySs58, const std::string& message,
                       const std::string& signature, SubmitDone done)>
        submit;
  };

  explicit Flow(Ports ports) : ports_(std::move(ports)) {}
  // the ports' answers point at this object
  Flow(const Flow&) = delete;
  Flow& operator=(const Flow&) = delete;

  // the view changed
  std::function<void()> on_changed;
  // a failure the section does not word itself, for the page's error surface
  std::function<void(const std::string& error)> on_error;

  // Signed in (the page's Load): the status is read. A signing in flight is
  // dropped with whatever it was waiting for.
  void Load() {
    ++generation_;
    status_.reset();
    notice_ = Notice::None;
    phase_ = Phase::Loading;
    Changed();
    ReadStatus();
  }

  // Signed out: nothing shows, and nothing in flight counts any more.
  void Reset() {
    ++generation_;
    status_.reset();
    notice_ = Notice::None;
    walletId_.clear();
    phase_ = Phase::Hidden;
    Changed();
  }

  // Sign with wallet, or Change wallet, with the wallet the chooser picked and
  // the coldkey the user typed. Refused (false), with nothing asked, until the
  // status is known, while a signing is in flight, without a wallet, and for a
  // coldkey the SDK's ss58 check refuses.
  bool Start(const std::string& walletId, const std::string& coldkeySs58) {
    if ((phase_ != Phase::None && phase_ != Phase::Active) || walletId.empty()) return false;
    if (!ports_.validSs58(coldkeySs58)) return false;
    ++generation_;
    walletId_ = walletId;
    notice_ = Notice::None;
    phase_ = Phase::Signing;
    Changed();
    RequestChallenge(generation_, coldkeySs58);
    return true;
  }

  // a signing is in flight: the bridge is the consent's
  bool Busy() const { return phase_ == Phase::Signing; }
  Phase phase() const { return phase_; }
  // the status read last; none while it is not known (the one-tap key's flow
  // reads it, with its first epoch)
  const std::optional<Status>& status() const { return status_; }

  View Current() const {
    View view;
    view.notice = notice_;
    switch (phase_) {
      case Phase::Hidden:
        return View{};
      case Phase::Loading:
        view.visible = true;
        view.line = Line::Loading;
        break;
      case Phase::Unavailable:
        view.visible = true;
        view.line = Line::Unavailable;
        break;
      case Phase::None:
        view.visible = true;
        view.line = Line::None;
        view.body = true;
        view.action = Action::Sign;
        break;
      case Phase::Active:
        view.visible = true;
        view.line = Line::Active;
        view.coldkeySs58 = status_ ? status_->coldkeySs58 : std::string();
        view.action = Action::Change;
        view.changeNote = true;
        break;
      case Phase::Signing:
        view.visible = true;
        view.line = Line::Signing;
        break;
    }
    return view;
  }

 private:
  void Changed() {
    if (on_changed) on_changed();
  }

  // An active status names its coldkey; one that does not is not readable.
  void ReadStatus() {
    const uint64_t generation = generation_;
    ports_.status([this, generation](std::optional<Status> status, std::string error) {
      if (generation != generation_) return;
      if (!status || (status->active && status->coldkeySs58.empty())) {
        status_.reset();
        phase_ = Phase::Unavailable;
        notice_ = NoticeFor(Step::Status, error);
        Changed();
        return;
      }
      status_ = std::move(status);
      phase_ = status_->active ? Phase::Active : Phase::None;
      Changed();
    });
  }

  void RequestChallenge(uint64_t generation, const std::string& coldkeySs58) {
    ports_.challenge(coldkeySs58, [this, generation](std::optional<Challenge> challenge,
                                                     std::string error) {
      if (generation != generation_) return;
      if (!challenge || challenge->message.empty()) {
        Fail(Step::Challenge, error);
        return;
      }
      Sign(generation, *challenge);
    });
  }

  // the wallet is handed the challenge as it came, and the submit sends its
  // coldkey and message: nothing else is ever signed or stored
  void Sign(uint64_t generation, const Challenge& issued) {
    ports_.sign(walletId_, issued, [this, generation, issued](Signed signed_) {
      if (generation != generation_) return;
      if (!signed_.ok || signed_.signature.empty()) {
        Fail(Step::Signature, signed_.error);
        return;
      }
      // a proof for another message or another coldkey is not this consent:
      // it is never submitted
      if (signed_.message != issued.message || signed_.address != issued.coldkeySs58) {
        Settle(Notice::Failed);
        return;
      }
      ports_.submit(issued.coldkeySs58, issued.message, signed_.signature,
                    [this, generation](Submitted submitted) {
                      if (generation != generation_) return;
                      if (!submitted.ok) {
                        Fail(Step::Submit, submitted.error);
                        return;
                      }
                      // stored: the status shows it, from the next epoch
                      phase_ = Phase::Loading;
                      Changed();
                      ReadStatus();
                    });
    });
  }

  // A step failed: the section is back on the status it had, with the refusal
  // in its own words, or quietly, or with the error on the page's surface.
  void Fail(Step step, const std::string& error) {
    const bool quiet = IsQuiet(error);
    const Notice notice = quiet ? Notice::None : NoticeFor(step, error);
    Settle(notice);
    if (!quiet && notice == Notice::None && on_error) on_error(error);
  }

  // back on the status the section had, with `notice`
  void Settle(Notice notice) {
    phase_ = status_ && status_->active ? Phase::Active : Phase::None;
    notice_ = notice;
    Changed();
  }

  Ports ports_;
  uint64_t generation_ = 0;
  Phase phase_ = Phase::Hidden;
  std::optional<Status> status_;  // the status read last; none while it is not known
  Notice notice_ = Notice::None;
  std::string walletId_;  // the wallet of the signing in flight
};

}  // namespace urnw::consent
