// The sign-out the sdk reports when the server rejects this app's sign-in
// (Api.addAuthLogoutListener, and the bound DeviceRemote's), as the app follows
// it: one sign-out for one rejection, and on the sign-in page the notice its
// cause allows (REVOKE-UI-FINAL.md §5).
//
// The cause. The sdk sets it before its listeners run (Api.getAuthLogoutCause,
// Device.getAuthLogoutCause) and a new sign-in clears it, so each listener
// reads it on the sdk thread it runs on and carries it in its report.
// kSessionRevoked is the server's confirmation that this sign-in session was
// revoked: signed out from another device. Every other sign-out reports "",
// this app's own included: an explicit sign-out fires no listener at all, and
// the sdk reports revoking this session from the Sessions list without a
// cause.
//
// Once. Each listener that hears a rejection reports it: the GUI's Api, and
// then the DeviceRemote bound to that same Api, which signs out because its
// Api did (it may report after the app has already signed out on the first
// report). The daemon's DeviceLocal is another process, and its sign-out
// reaches nothing here; the GUI's own requests carry the GUI's credential, so
// the server refuses the next of them with the same cause. A report names the
// sign-in it was heard in, and only a report heard while signed in, whose
// sign-in is still the one signed in, signs out: the first report of a
// rejection does, and the others, a report heard while signed out, and one
// that lands after any sign-out or a new sign-in, do nothing.
//
// Pure C++17, no GTK or sdk (tests/AuthLogoutTest.cpp). SdkHost keeps the
// Tracker and MainWindow the SignInNotice.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace urnw::auth_logout {

// The sdk's AuthLogoutCauseSessionRevoked; SdkHost.cpp asserts the two equal.
inline constexpr const char* kSessionRevoked = "session_revoked";

// One sign-out the sdk reported: the cause its listener read, and the sign-in
// it was heard in (Tracker; 0 when heard while signed out).
struct Report {
  std::string cause;
  uint64_t signIn = 0;
};

// Whether the sign-in page says "This session was signed out from another
// device.": for the trusted cause only. Any other cause, "" included, keeps
// the generic sign-out.
inline bool SignedOutRemotely(const std::string& cause) { return cause == kSessionRevoked; }

// The sign-in that reports are heard in and checked against. Hear runs on the
// sdk's threads, the rest where SdkHost changes the sign-in; safe for
// concurrent use.
class Tracker {
 public:
  // A listener heard a sign-out, with the cause it read.
  Report Hear(std::string cause) const { return Report{std::move(cause), signedIn_.load()}; }

  // Whether the report signs the app out: it was heard while signed in, and
  // that sign-in is still the one signed in.
  bool SignsOut(const Report& report) const {
    return report.signIn != 0 && report.signIn == signedIn_.load();
  }

  // A new sign-in: a fresh one, or the one a launch or a server switch finds
  // stored. Every report heard before it is spent.
  void SignedIn() { signedIn_.store(next_.fetch_add(1) + 1); }

  // A sign-out of any kind. Every report heard before it is spent, and one
  // heard until the next sign-in signs nothing out.
  void SignedOut() { signedIn_.store(0); }

 private:
  std::atomic<uint64_t> next_{0};
  std::atomic<uint64_t> signedIn_{0};  // the sign-in now; 0 while signed out
};

// The sign-in page's notice for the sign-out a report made: set as the report
// signs out, shown once as the sign-out reaches the sign-in page, dropped by a
// sign-in before then. Main loop only.
class SignInNotice {
 public:
  void Arm(const std::string& cause) { signedOutRemotely_ = SignedOutRemotely(cause); }

  // Whether to say the session was signed out from another device, now; never
  // again for the same sign-out.
  bool Take() { return std::exchange(signedOutRemotely_, false); }

  void Drop() { signedOutRemotely_ = false; }

 private:
  bool signedOutRemotely_ = false;
};

}  // namespace urnw::auth_logout
