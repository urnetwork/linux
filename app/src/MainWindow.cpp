// SPDX-License-Identifier: MPL-2.0
#include "MainWindow.hpp"
#include "DataInfo.hpp"
#include "ProUpgradeReaction.hpp"

#include "SsoBridge.hpp"

#include <adwaita.h>
#include <glib.h>

#include <cstdio>

#include <glibmm/datetime.h>

#include "AppPrefs.hpp"
#include "BittensorWalletFlow.hpp"
#include "ReferralRoyalty.hpp"
#include "BrandIcons.hpp"
#include "DaemonUnreachableCopy.hpp"
#include "DisplayText.hpp"
#include "FailsafeNotice.hpp"
#include "Formatters.hpp"
#include "UrTheme.hpp"
#include "I18n.hpp"
#include "LocationSelection.hpp"
#include "StatusStripPresentation.hpp"
#include "TrayPolicy.hpp"
#include "Ui.hpp"
#include "WindowGeometry.hpp"

namespace urnw {

namespace {
constexpr const char* kOnboardingPendingKey = "onboarding_pending";
}  // namespace
namespace {

// a user auth is an email or a phone number (light shape check gating the
// discovery call; the server is the real validator — Windows/mac parity)
bool LooksLikeUserAuth(const std::string& value) {
  if (value.find('@') != std::string::npos) return value.size() >= 3;
  size_t digits = 0;
  for (char c : value) {
    if ('0' <= c && c <= '9') ++digits;
  }
  return digits >= 7;
}

// Shown and not minimized — deliberately NOT focus (windows §7.15: the
// presentation gate stops every animation, chart, poll and carousel when the
// window is hidden, but "focus loss deliberately does NOT stop them"). Gating
// on is_active() froze the login carousel the moment the user clicked any
// other window, which reads as "the login screen is a static image".
constexpr bool WindowPresentationShouldRun(bool visible, bool mapped) {
  return visible && mapped;
}

static_assert(WindowPresentationShouldRun(true, true));
static_assert(!WindowPresentationShouldRun(true, false));
static_assert(!WindowPresentationShouldRun(false, true));

// The copy for a daemon that ANSWERED and refused on authorization grounds.
//
// THE POINT OF THIS FUNCTION: none of these may ever reach the user as "The
// URnetwork system service is not running. Install or start it, then try
// again." The service is running — it is running well enough to have made a
// policy decision about this account and told us so. Sending the user to
// `systemctl start` for a polkit denial is the single worst outcome available
// here, which is why the authorization verdict is checked BEFORE the generic
// failure copy and why it is carried on its own enum rather than folded into
// DaemonUnreachableReason (ControlClient.hpp says why at length).
//
// `detail` is the daemon's own message, appended only where it adds a fact the
// sentence cannot carry.
Glib::ustring DaemonAuthRefusalCopy(DaemonAuthOutcome outcome, const std::string& detail) {
  switch (outcome) {
    case DaemonAuthOutcome::Denied:
      return T_("daemon_auth_denied",
                "This device's policy does not allow this account to connect through "
                "URnetwork. An administrator can change that.");
    case DaemonAuthOutcome::ChallengeRequired:
      return T_("daemon_auth_required",
                "Connecting from this session needs administrator permission. Press "
                "Connect again to be asked, or use the session at this device's screen.");
    case DaemonAuthOutcome::Dismissed:
      // DELIBERATELY EMPTY, and this is the whole behaviour, not an omission.
      // The user was shown the polkit dialog and closed it: they changed their
      // mind. Rendering a banner (or emitting a g_warning) for a decision the
      // user just made is the product's own documented mistake —
      // docs/parity/settings.md:130, "User-declined elevation = SILENCE (the
      // user changed their mind), not an error." Any copy here is a bug.
      return {};
    case DaemonAuthOutcome::Unavailable:
      return T_("daemon_auth_unavailable",
                "This device has no polkit authorization service, so the URnetwork "
                "system service falls back to the 'urnetwork' group. Add your user to "
                "it and sign out and back in.");
    case DaemonAuthOutcome::CheckFailed: {
      Glib::ustring notice =
          T_("daemon_auth_check_failed",
             "The URnetwork system service could not check whether this account is "
             "allowed to connect, so it did not connect. Try again.");
      // The daemon's detail names the actual failure (bus error, polkitd gone,
      // an unreadable /proc entry) and there is no way to guess it from here.
      if (!detail.empty()) notice += " (" + detail + ")";
      return notice;
    }
    case DaemonAuthOutcome::TimedOut:
      return T_("daemon_auth_timeout",
                "The permission request was not answered, so nothing was connected. "
                "Press Connect to try again.");
    case DaemonAuthOutcome::NotTunnelOwner:
      return T_("daemon_tunnel_owned_by_other_user",
                "Another user on this device is connected through URnetwork. Disconnect "
                "it from their session, or take it over with administrator permission.");
    case DaemonAuthOutcome::None:
    case DaemonAuthOutcome::Authorized:
      // Not a refusal: the caller must not have asked (IsAuthRefusal gates it)
      // and falls through to the ordinary failure copy.
      break;
  }
  return {};
}

}  // namespace

MainWindow::MainWindow(SdkHost& host) : host_(host), balance_(host) {
  set_title("URnetwork");
  // The size the last run left, or the desktop default (windows shell parity:
  // 1120x820dip, min 400x480). The preview harness always opens at the default.
  window_geometry::Size size;
  const bool restore = g_getenv("URNETWORK_PREVIEW_UI") == nullptr;
  if (restore) {
    size = window_geometry::SizeToOpenAt(prefs::Get<int64_t>(window_geometry::kWidthKey, 0),
                                         prefs::Get<int64_t>(window_geometry::kHeightKey, 0));
  }
  set_default_size(size.width, size.height);
  set_size_request(window_geometry::kMinWidth, window_geometry::kMinHeight);
  if (restore && prefs::Get<bool>(window_geometry::kMaximizedKey, false)) maximize();
  // Saved again shortly after every resize and maximize as well, not only at
  // close and Quit: a logout or a SIGTERM ends the app with neither.
  if (restore) {
    const auto saveSoon = [this] {
      geometrySave_.disconnect();
      geometrySave_ = Glib::signal_timeout().connect(
          [this] {
            SaveGeometry();
            return false;
          },
          window_geometry::kSaveDebounceMillis);
    };
    property_default_width().signal_changed().connect(saveSoon);
    property_default_height().signal_changed().connect(saveSoon);
    property_maximized().signal_changed().connect(saveSoon);
  }

  BuildChrome();
  BuildLogin();
  BuildPasswordStep();
  BuildSeedphraseStep();
  BuildInstantStep();
  BuildHome();
  BuildAuthPages();
  // URNW_ONBOARDING_PREVIEW=1 opens the onboarding flow over whatever is
  // showing, for design review without an account (no data behind it)
  if (const char* preview = g_getenv("URNW_ONBOARDING_PREVIEW"); preview && *preview) {
    const std::string tag(preview);
    const int step = std::max(1, atoi(preview));
    Glib::signal_timeout().connect_once([this, tag, step] {
      if (!onboarding_) {
        onboarding_ = std::make_unique<OnboardingWindow>(*this, host_, balance_);
        onboarding_->on_guest_sign_in_required = [this] { OnOnboardingGuestSignInRequired(); };
      }
      // URNW_ONBOARDING_PREVIEW_OFFER=1 seeds a sample welcome offer (no
      // session behind the preview, so nothing is issued): the offer card on
      // the plan page and the offer page print the sample's numbers
      if (const char* sample = g_getenv("URNW_ONBOARDING_PREVIEW_OFFER"); sample && *sample) {
        urnet::OnboardingOffer offer;
        offer.state = "active";
        offer.percent_off = 25;
        offer.months_free = 3;
        offer.first_year_usd = 30;
        offer.regular_year_usd = 40;
        offer.tier = "standard";
        offer.currency = "USD";
        offer.expires_at =
            Glib::DateTime::create_now_utc().add_days(5).format_iso8601();
        balance_.SetOffer(offer);
      }
      // "offer" reviews the urnetwork://onboarding/offer destination: the
      // offer page on its own
      if (tag == "offer") {
        onboarding_->OpenOffer();
      } else {
        onboarding_->OpenAt(step);
      }
    }, 800);
  }
  // AdwToastOverlay across the page stack: hosts the window's toasts (the
  // balance recovery's reconnecting notice; the detail sheets carry their own
  // overlays; see Ui.hpp ShowToast).
  GtkWidget* toastOverlay = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toastOverlay), GTK_WIDGET(stack_.gobj()));
  // Home's first entrance runs on the page crossfade's duration (ApplyAuthState);
  // every other swap of this stack passes no transition.
  stack_.set_transition_duration(motion::kBaseMs);
  // The Pro celebration wraps everything: the page stack (with its toasts)
  // sits in the mosaic container, and the confetti overlay floats above it.
  // Both are inert until a flight starts (ProCelebration.hpp).
  proPixelateBin_ = Gtk::make_managed<PixelateBin>(proFlightClock_);
  proPixelateBin_->SetChild(*Glib::wrap(toastOverlay));
  auto* windowOverlay = Gtk::make_managed<Gtk::Overlay>();
  windowOverlay->set_child(*proPixelateBin_);
  proCelebration_ = Gtk::make_managed<ProCelebrationOverlay>(proFlightClock_);
  proCelebration_->on_frame = [this] {
    if (proPixelateBin_) proPixelateBin_->queue_draw();
  };
  windowOverlay->add_overlay(*proCelebration_);
  windowOverlay->set_measure_overlay(*proCelebration_, false);
  set_child(*windowOverlay);

  // Track window visibility (tray app: closing hides to tray). Skip window-widget
  // updates while hidden and resync when shown, so a hidden window doesn't churn
  // on high-frequency SDK updates. Live stats and the balance poll follow this
  // same gate (the balance store resyncs itself on show).
  auto reconcilePresentation = [this] {
    const bool presentationActive =
        WindowPresentationShouldRun(get_visible(), get_mapped());
    if (windowVisible_ == presentationActive) return;
    windowVisible_ = presentationActive;
    host_.SetPresentationActive(windowVisible_);
    balance_.SetWindowVisible(windowVisible_);
    UpdateCarouselRunning();
    if (connectPage_) connectPage_->SetPresentationActive(windowVisible_);
    if (earningsPage_) earningsPage_->SetPresentationActive(windowVisible_);
    // the Sessions controller's foreground (its visibility is the page's map)
    if (sessionsPage_) sessionsPage_->SetPresentationActive(windowVisible_);
    if (developerPage_) developerPage_->SetPresenting(windowVisible_);
    if (windowVisible_) {
      // RE-READ, never replay: SetPresentationActive(true) above has just
      // reopened the connect controller, so the reading the window was last
      // pushed describes a moment when there was no controller to ask.
      ApplyConnectReading(host_.CurrentConnectReading());
      ApplyStats(lastStats_);
    }
  };
  property_visible().signal_changed().connect(reconcilePresentation);
  signal_map().connect(reconcilePresentation);
  signal_unmap().connect(reconcilePresentation);
  // Focus is NOT part of the presentation gate (see above), but the purchase
  // confirmation poll also pauses on it: a hosted checkout leaves this window
  // visible behind the browser while the user pays (UPGRADE.md D1).
  TrackAppFocus();
  // The window reveal (Hero Bloom): plays once per show on the signed-out
  // frame; hiding mid-reveal must never leave a hero pinned at 0.92 —
  // CancelToFinal on unmap.
  signal_map().connect([this] {
    if (!host_.IsLoggedIn() && stack_.get_visible_child_name() == "login") {
      RunSignedOutReveal();
    }
  });
  signal_unmap().connect([this] { SettleReveal(); });
  // the carousel runs only on the initial login step
  stack_.property_visible_child_name().signal_changed().connect(
      [this] { UpdateCarouselRunning(); });

  // Balance/plan changes land on the GTK loop already (the store marshals);
  // fan out to the upgrade sheet's states, the pages and the balance gate.
  balance_.SetChangedHandler([this] {
    if (upgradeSheet_) upgradeSheet_->OnBalanceChanged();
    // a converted guest's purchase continues once the server stops reporting a guest
    guestUpgrade_.Poll(balance_.IsGuest());
    ApplyStatusStripDetails();  // the strip's Network field names a guest
    UpdateBalanceNotice();  // a Pro upgrade or a settled poll moves the gate
    // Earnings gates its upgrade door and its plan-flavoured copy on the
    // plan's two bits.
    if (earningsPage_) earningsPage_->SetBalanceState(balance_.IsPro(), balance_.IsGuest());
    // Account paints its whole plan pane from ONE relayed snapshot — the page
    // never touches the store, which is window-owned and shared with the
    // balance warning and the upgrade sheet.
    if (accountPage_) {
      AccountBalance snapshot;
      snapshot.usedByteCount = balance_.UsedByteCount();
      snapshot.pendingByteCount = balance_.PendingByteCount();
      snapshot.availableByteCount = balance_.AvailableByteCount();
      snapshot.startBalanceByteCount = balance_.StartBalanceByteCount();
      snapshot.isPro = balance_.IsPro();
      snapshot.guest = balance_.IsGuest();
      snapshot.subscriptionStoreFamily = balance_.SubscriptionStoreFamily();
      snapshot.loaded = balance_.HasFetched();
      snapshot.confirming = balance_.IsPolling();
      snapshot.timedOut = balance_.PurchaseConfirmationTimedOut();
      accountPage_->ApplyBalance(snapshot);
    }
    // The Refer and earn page paints its card from the same store.
    if (referralsPage_) referralsPage_->OnBalanceChanged();
    // The free -> Pro upgrade (ProUpgradeReaction.hpp): the provide control
    // mode stands, and the Pro celebration plays once per purchase. The store
    // confirms the flip after checkout (the upgrade sheet's success state
    // reads the same snapshot), and the flight plays over whatever is on screen.
    if (balance_.DidDetectUpgradeToPro() && !proCelebrated_) {
      const std::string provideControlMode = host_.GetProvideControlMode();
      const ProUpgradeReaction reaction =
          ReactToProUpgrade(true, proCelebrated_, provideControlMode);
      if (reaction.provideControlMode != provideControlMode) {
        host_.SetProvideControlMode(reaction.provideControlMode);
        // the connect page's picker shows the mode the reaction holds
        if (connectPage_) connectPage_->Resync();
      }
      if (reaction.celebrate) {
        proCelebrated_ = true;
        LaunchProCelebration();
      }
    }
  });

  // Referral celebrations (the king-frog gold moments): the first referral
  // gets the full-screen crowning sheet; later batches get the gold snackbar.
  // The store only polls while the window is visible, so the celebration
  // always has a window to land in.
  balance_.SetReferralCelebrationHandler([this](const ReferralCelebration& celebration) {
    if (celebration.isFirst) {
      ShowReferralCelebrationSheet(*this, celebration.joined, balance_.ReferralCode());
      return;
    }
    if (shell_) {
      shell_->snackbar().Show(
          Format(TN_("referral_toast_joined",
                     "A friend joined with your code! +{1} GiB/day, for life.",
                     "{0} friends joined with your code! +{1} GiB/day each, for life.",
                     celebration.joined),
                 celebration.joined, kReferralGiBPerDay),
          kit::Snackbar::Severity::Gold);
    }
  });

  // Auth-state transitions are marshaled onto the GTK loop and flip the view.
  host_.SetAuthStateHandler([this](bool loggedIn) {
    PostToMain([this, loggedIn] { ApplyAuthState(loggedIn); });
  });
  // The server rejected the stored auth (e.g. the client was removed, or this
  // session was signed out from another device): log out and return to the
  // login panel (OnAuthLogout). Logout() fires the auth-state handler.
  host_.SetAuthInvalidHandler([this](auth_logout::Report report) {
    PostToMain([this, report] { OnAuthLogout(report); });
  });
  host_.SetJwtRefreshedHandler([this] {
    PostToMain([this] { balance_.OnJwtRefreshed(); });
  });
  host_.SetBittensorManualHandler([this](SdkHost::BittensorManualRequest request) {
    ShowBittensorManualSheet(request);
  });
  host_.SetOnboardingLinkHandler([this](const std::string& url) {
    PostToMain([this, url] { HandleOnboardingLink(url); });
  });
  // THE CONNECTION FEED, AND IT IS NOT GATED ON VISIBILITY. That asymmetry —
  // this push ungated beside a stats push gated on windowVisible_ — is how two
  // copies of one fact came to describe two different moments. There is one
  // copy now, and it carries every field, so there is nothing left to age
  // independently.
  host_.SetConnectReadingHandler([this](ConnectReading reading) {
    PostToMain([this, reading] { ApplyConnectReading(reading); });
  });
  // Live stats (provider count / throughput / provide). Same visibility gate:
  // cache always, but only touch widgets while the window is shown.
  host_.SetStatsHandler([this](const LiveStats& stats) {
    PostToMain([this, stats] {
      lastStats_ = stats;
      if (windowVisible_) ApplyStats(stats);
    });
  });
  // Connect drawer feed (charts / block actions / dns / blocker / controls).
  // Same visibility gate: dropped while hidden, resynced on show.
  host_.SetDrawerEventHandler([this](DrawerEvent event) {
    PostToMain([this, event] {
      // Panes B/C of Home read this feed. Without it they would only ever
      // refresh on a stats push: block actions, block stats, overrides,
      // contracts, DNS settings, blocker, routeLocal and location changes
      // would never reach the page at all.
      if (windowVisible_ && connectPage_) connectPage_->OnHostEvent(event);
      // ...and so do the earnings page's provider and extender statistics and
      // its read-only extender row (EXTENDER.md N7, O5), under the same gate
      if (windowVisible_ && earningsPage_) earningsPage_->OnHostEvent(event);
      // the location chooser's pinned peers and its sections, while it is open
      if (windowVisible_ && (event == DrawerEvent::Peers || event == DrawerEvent::Locations) &&
          locationsSheet_ && locationsSheet_->is_visible()) {
        locationsSheet_->Refresh();
      }
      if (event == DrawerEvent::ProviderSelection) {
        // a wheel step (or any other app-side selection) landing back from the
        // SDK view controller
        if (windowVisible_ && providerLocationsSheet_ && providerLocationsSheet_->get_visible()) {
          providerLocationsSheet_->Refresh();
        }
      }
      if (event == DrawerEvent::ProviderLocations || event == DrawerEvent::DeviceLifecycle) {
        // Deliberately NOT gated on window visibility: the location override
        // must keep following the connect window while the app sits in the
        // tray, or it would report a provider we stopped using.
        SyncLocationOverrideTarget();
        if (windowVisible_ && providerLocationsSheet_ && providerLocationsSheet_->get_visible()) {
          providerLocationsSheet_->Refresh();
        }
      }
      if (event == DrawerEvent::ProviderIdentities) {
        // A provider's e2e session verifying (or dropping) changes only the
        // identicon badge, not the location rows, so the locations sheet must
        // refresh on this event too or a newly sealed provider would show no
        // badge until an unrelated location change forced a rebuild.
        if (windowVisible_ && providerLocationsSheet_ && providerLocationsSheet_->get_visible()) {
          providerLocationsSheet_->Refresh();
        }
      }
    });
  });

  // Device-location override (GeoClue static source). Built unconditionally at
  // startup so its cleanup of an override left by a previous run always
  // happens. The GUI keeps the state machine; the privileged write goes to
  // urnetworkd over the shared control channel (DaemonGeoClueWriter) — this
  // process never needs root.
  locationOverride_ = std::make_unique<GeoClueLocationOverride>(
      std::make_unique<DaemonGeoClueWriter>(host_.Control()));

  // The location picks (Network page, location chooser) connect through the
  // host directly; the host asks the same gate.
  host_.SetConnectGate(
      [this](std::function<void()> retry) { return ConnectBlockedByBalance(std::move(retry)); });
  // ...and a pick starts the tunnel when there is none, through the same start
  // path as the Connect button (its notices, its gate), to the row's location.
  host_.SetRowConnect([this](const std::optional<urnet::ConnectLocation>& location) {
    if (connectPage_) connectPage_->ClearDisconnectIntent();
    StartTunnelUi("location row", location);
  });

  if (host_.IsLoggedIn()) {
    // AUTO-CONNECT IS OPT IN, DEFAULT OFF. Being signed in is not a request to
    // connect: this ran on every launch of a signed-in account and brought the
    // tunnel up — urnet0, capture routes, DNS — before the user touched
    // anything. The preference is device-local (AppPrefs, not the account
    // preferences API: launch behaviour is per-device and must not wait on a
    // network round trip) and gates ONLY this call.
    //
    // Nothing about StartTunnelUi changes, and no other path to it moves. The
    // post-login handlers still connect on a fresh sign-in — that is a user
    // action with an obvious intent, not a launch.
    if (prefs::Get<bool>(prefs::kConnectOnLaunchKey, false)) StartTunnelUi("connect on launch");
    ApplyAuthState(true);
  } else {
    ApplyAuthState(false);
  }

  // Verification hook for the Google / Apple sign-in round trip (with URNETWORK_SSO_SIMULATE,
  // WalletConnect.cpp): URNETWORK_SSO_AUTOSTART=<google|apple> presses that
  // pill shortly after the window shows, so the whole return path — minted
  // state + nonce, the simulated return, the login call, the error line the
  // server's rejection of an unsigned token paints — runs without a click and
  // lands in a URNETWORK_SHOOT frame. Debug only; inert without SIMULATE.
  if (const char* provider = g_getenv("URNETWORK_SSO_AUTOSTART");
      provider && g_getenv("URNETWORK_SSO_SIMULATE")) {
    const std::string p(provider);
    Glib::signal_timeout().connect_once([this, p] { OnSso(p); }, 1200);
  }

  // The preview harness (windows --preview-ui): URNETWORK_PREVIEW_UI=<tag>
  // renders the signed-in shell with NO session — API loads are skipped (no
  // jwt, no balance poll) and every panel settles on its real empty state.
  // The only way most screens are reviewable without an account.
  if (const char* preview = g_getenv("URNETWORK_PREVIEW_UI")) {
    stack_.set_visible_child("home");
    const std::string tag(preview);
    // The preview harness reviews FOLDS as much as pages: a destination's
    // widest lane only exists above its threshold, and the default 1120 is
    // below three of them.
    if (const char* w = g_getenv("URNETWORK_PREVIEW_WIDTH")) {
      const int width = std::atoi(w);
      if (width > 0) set_default_size(width, 900);
    }
    // Preview mode is a page-level contract, not a shell one: a page in
    // preview must skip its API reads and paint the state it would settle on,
    // rather than sit on a spinner forever with no session behind it.
    if (earningsPage_) earningsPage_->SetPreviewMode(true);
    if (accountPage_) accountPage_->SetPreviewMode(true);
    if (referralsPage_) referralsPage_->SetPreviewMode(true);
    if (sessionsPage_) sessionsPage_->SetPreviewMode(true);
    // DEFERRED to idle, and guarded: a destination's Load() runs API/SDK
    // reads, and in preview there is no session — an exception escaping the
    // WINDOW CONSTRUCTOR would take the process down before anything renders
    // (measured). Navigating after the window exists is also what the real
    // app does; nothing may load from inside the constructor.
    // A short TIMEOUT rather than an idle: the connect canvas keeps the frame
    // clock busy, and a default-priority idle can starve behind redraws for
    // the whole life of the process (observed on macOS: the shoot fired with
    // the Connect page still up and this navigation never logged).
    Glib::signal_timeout().connect_once([this, tag] {
      g_message("preview: navigating to '%s'", tag.c_str());
      try {
        if (shell_ && !tag.empty() && tag != "1") shell_->Navigate(tag);
        if (tag == "account" && accountPage_) accountPage_->ShowPreviewState();
        if (tag == "referrals" && referralsPage_) referralsPage_->ShowPreviewState();
        if (tag == "sessions" && sessionsPage_) sessionsPage_->ShowPreviewState();
        if (tag == "wallet" && earningsPage_) shell_->Navigate("earnings");
        if ((tag == "earnings" || tag == "wallet") && earningsPage_) {
          // ORDER MATTERS: the empty settle is what a no-session preview looks
          // like, and the sample paints OVER it. Reversed, SettleAllEmpty wipes
          // the sample back to dashes.
          earningsPage_->ShowPreviewState();
          if (g_getenv("URNETWORK_PREVIEW_SAMPLE")) earningsPage_->ApplyPreviewSample();
          if (tag == "wallet") earningsPage_->ShowPreviewSnackbar();
          // the claim dialog over the sample's attached-wallet layer
          if (g_getenv("URNETWORK_PREVIEW_CLAIM")) earningsPage_->ShowPreviewClaimDialog();
        }
      } catch (const std::exception& e) {
        g_warning("preview: navigate to '%s' failed: %s", tag.c_str(), e.what());
      } catch (...) {
        g_warning("preview: navigate to '%s' failed: non-std exception", tag.c_str());
      }
      g_message("preview: navigate done");
    }, 100);
  }
}

// StartTunnel + render the daemon session state. Each failure is a DISTINCT
// actionable line (MIGRATION.md; APPIMAGE.md §11b): "service not running",
// "service out of date", "app out of date" and "builds differ" are different
// problems with different fixes, and none of them may render as a blank or a
// zero — the same doctrine as the gray "discovery disabled" the RPC-hosted
// stats use.
//
// A REFUSAL IS NOT AN ABSENCE. "The service is not running" and "the service
// ran your request and said no" are opposite facts, and the second one arrives
// through TunnelStartResult::Failed with an authorization verdict on the
// control client (DaemonAuthOutcome). It is rendered from its own copy table
// below and never through the DaemonUnreachable arm.
TunnelStartResult MainWindow::StartTunnelUi(const char* reason) {
  return StartTunnelUi(reason, host_.SelectedLocation());
}

TunnelStartResult MainWindow::StartTunnelUi(const char* reason,
                                            const std::optional<urnet::ConnectLocation>& target) {
  // Out of balance, a new connection is not started at all: no tunnel, no
  // routes, the upgrade path instead. Every caller (the Connect press, connect
  // on launch, the post-sign-in connect) passes through here, and a stale
  // balance read repeats the whole start, connect included, once it lands.
  if (ConnectBlockedByBalance([this, reason, target] { StartTunnelUi(reason, target); })) {
    return TunnelStartResult::Failed;
  }
  // Snapshot the reply counter BEFORE the attempt. LastAuthOutcome() describes
  // the last reply this client processed, and StartTunnel() has failure paths
  // that never send anything (not signed in; unusable device-rpc key material).
  // Without this guard a polkit denial from an earlier Connect would be
  // rendered under an unrelated failure — a confidently wrong sentence, which
  // is worse than the generic one. Only a verdict this attempt produced counts.
  const uint64_t replySerialBefore = host_.Control().ReplySerial();
  // what the daemon said about the last session does not describe this one
  ForgetDaemonStatus();
  const TunnelStartResult result = host_.StartTunnel(reason);
  const DaemonAuthOutcome authOutcome = host_.Control().ReplySerial() != replySerialBefore
                                            ? host_.Control().LastAuthOutcome()
                                            : DaemonAuthOutcome::None;
  // The notice goes to ConnectPage::SetDaemonNotice, the surface the user
  // sees: a daemon failure must never look like a silent no-op of the press.
  Glib::ustring notice;
  switch (result) {
    case TunnelStartResult::Started:
      break;  // an empty notice clears it
    case TunnelStartResult::DaemonUnreachable:
      // "Unreachable" is three different problems with three different fixes.
      // Collapsing them into "not running" actively misleads: against a
      // group-gated daemon the service IS running and the user simply is not
      // in the urnetwork group, and telling them to start a running service
      // sends them nowhere. Every arm here is a transport failure — the daemon
      // was never reached — which is what separates them from the
      // authorization refusals under Failed.
      {
        const auto reason = host_.Control().LastUnreachableReason();
        const auto copy = CopyForDaemonUnreachableReason(reason);
        notice = T_(copy.key, copy.english);
        if (reason == DaemonUnreachableReason::Other) {
          // LastTunnelError() is EnsureSession's own out-param, which already
          // carries strerror for this case.
          const std::string detail = host_.LastTunnelError();
          if (!detail.empty()) notice += " (" + detail + ")";
        }
      }
      break;
    case TunnelStartResult::DaemonTooOld:
      notice = T_("daemon_too_old",
                  "The URnetwork system service is out of date. Update it to connect.");
      break;
    case TunnelStartResult::AppTooOld:
      notice = T_("app_too_old_for_daemon",
                  "This app is older than the installed URnetwork system service. Update "
                  "the app to connect.");
      break;
    case TunnelStartResult::SdkMismatch:
      notice = T_("daemon_sdk_mismatch",
                  "The app and the URnetwork system service are different builds. Update "
                  "both to the same version.");
      break;
    case TunnelStartResult::Failed: {
      const std::string error = host_.LastTunnelError();
      // AUTHORIZATION FIRST. A refusal arrives here — the daemon was reached,
      // hello succeeded, and it answered a verb with a code — so without this
      // branch a polkit denial would render as the daemon's raw diagnostic, and
      // a daemon that omitted one would render as "Could not start the
      // connection", which says nothing about the only thing the user can act
      // on. TunnelStartResult has no enumerator for these (SdkHost.hpp is
      // outside this change), so the verdict is read from the control client
      // instead; see the serial guard at the top of this function.
      if (IsAuthRefusal(authOutcome)) {
        notice = DaemonAuthRefusalCopy(authOutcome, error);
        break;
      }
      notice = error.empty() ? Glib::ustring(T_("tunnel_start_failed",
                                                "Could not start the connection"))
                             : Glib::ustring(error);
      break;
    }
  }
  // Empty means "say nothing", and for DaemonAuthOutcome::Dismissed that is the
  // required behaviour, not an accident: no banner AND no g_warning for a user
  // who closed the password dialog themselves.
  if (!notice.empty()) g_warning("connect: %s", notice.c_str());
  if (connectPage_) connectPage_->SetDaemonNotice(notice);

  // A TUNNEL WITHOUT A DESTINATION CARRIES NOTHING, AND LOOKS EXACTLY LIKE ONE
  // THAT DOES. This is the "Connect doesn't actually forward real traffic" bug,
  // measured on the owner's machine: urnet0 up, 31 capture routes installed,
  // DNS pinned, egress witness passing, UI green — and 166 seconds with not one
  // [rel], [contract], [multi] or firstload line in the daemon journal, because
  // no provider session existed. The instant ConnectBestAvailable ran, the
  // relay came up 0.6s later.
  //
  // The cause was pairing: StartTunnelUi() has TEN call sites and exactly ONE
  // of them (ToggleConnect) also called host_.ConnectBestAvailable(). Every
  // other path — including the constructor's auto-start for an already
  // signed-in account, which is what most launches take — brought the tunnel up
  // with no destination. Pairing them here rather than at nine call sites is
  // the point: a rule that has to be remembered ten times is a rule that will
  // be missed again.
  //
  // The target is respected: a user who has chosen a specific provider must
  // not be silently moved to "best available", which the Connect button did
  // while the provider row above it named their choice. Only no choice, or a
  // choice of best available, asks the SDK to pick.
  if (result == TunnelStartResult::Started) {
    if (IsBestAvailableSelected(target)) {
      g_message("connect: no destination selected, choosing the best available");
      host_.ConnectBestAvailable();
    } else {
      g_message("connect: routing to the selected provider");
      host_.Connect(target);
    }
  }
  return result;
}

// ---- window chrome ----------------------------------------------------------
// The 48px integrated title bar (windows R1): a 20px app icon and the
// PP NeueBit wordmark at the left; the strip is the drag region (CSD).
void MainWindow::BuildChrome() {
  auto* header = Gtk::make_managed<Gtk::HeaderBar>();
  header->set_show_title_buttons(true);
  auto* brand = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* icon = Gtk::make_managed<Gtk::Image>();
  // The LOGO, loaded from the asset — not by icon name. A name lookup needs
  // an installed icon theme and renders a blank white square when it misses
  // (build tree, or an AppImage whose theme dirs the host does not know),
  // which is exactly how this shipped blank.
  if (auto logo = BrandLogoTexture()) {
    icon->set(logo);
  } else {
    icon->set_from_icon_name(kAppIconName);
  }
  icon->set_pixel_size(20);
  brand->append(*icon);
  // the product name is never translated (the store marks it so)
  auto* wordmark = Gtk::make_managed<Gtk::Label>("URnetwork");
  wordmark->add_css_class("ur-wordmark");
  brand->append(*wordmark);
  // the brand beat: the wordmark joins the reveal at 120ms, opacity-only
  brandBin_ = Gtk::make_managed<motion::MotionBin>();
  brandBin_->set_child(*brand);
  header->pack_start(*brandBin_);
  // suppress the centered window title: the wordmark at the left IS the title
  header->set_title_widget(*Gtk::make_managed<Gtk::Label>(""));
  set_titlebar(*header);
}

namespace {

// URButton (android URButton.kt via windows UrButtonBaseStyle): one component,
// two styles. PRIMARY = BlueMedium/white; SECONDARY = white/black.
Gtk::Button* MakeUrButton(const Glib::ustring& label, bool primary) {
  auto* button = Gtk::make_managed<Gtk::Button>(label);
  button->add_css_class("ur-btn");
  button->add_css_class(primary ? "ur-btn-primary" : "ur-btn-secondary");
  return button;
}

// A SECONDARY pill with a leading brand mark (the wallet / auth-code buttons).
Gtk::Button* MakeUrIconButton(BrandIcon::Kind kind, const Glib::ustring& label) {
  auto* button = Gtk::make_managed<Gtk::Button>();
  button->add_css_class("ur-btn");
  button->add_css_class("ur-btn-secondary");
  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  content->set_halign(Gtk::Align::CENTER);
  content->append(*Gtk::make_managed<BrandIcon>(kind));
  auto* text = Gtk::make_managed<Gtk::Label>(label);
  content->append(*text);
  button->set_child(*content);
  // the content is an icon + text box, not a string: name it for a11y
  gtk_accessible_update_property(GTK_ACCESSIBLE(button->gobj()),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
  return button;
}

// A login tile (LOGIN_STACK_SPEC): a SECONDARY pill in square form, the
// brand mark over a small caption. Rows of four are laid out by MakeTileRows.
Gtk::Button* MakeUrTileButton(BrandIcon::Kind kind, const Glib::ustring& caption) {
  auto* button = Gtk::make_managed<Gtk::Button>();
  button->add_css_class("ur-btn");
  button->add_css_class("ur-btn-secondary");
  button->add_css_class("ur-tile");
  button->set_hexpand(true);
  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  content->set_halign(Gtk::Align::CENTER);
  content->set_valign(Gtk::Align::CENTER);
  content->append(*Gtk::make_managed<BrandIcon>(kind, 22));
  auto* text = Gtk::make_managed<Gtk::Label>(caption);
  text->add_css_class("ur-tile-caption");
  content->append(*text);
  button->set_child(*content);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button->gobj()),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, caption.c_str(), -1);
  return button;
}

// Four tiles per row, each row's tiles stretched to fill it (a homogeneous
// row: a last row of two is two half-width tiles), the rows as wide as the
// full-width pills above.
Gtk::Box* MakeTileRows(const std::vector<Gtk::Button*>& tiles) {
  auto* rows = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  Gtk::Box* row = nullptr;
  int inRow = 0;
  for (Gtk::Button* tile : tiles) {
    if (!row || inRow == 4) {
      row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      row->set_homogeneous(true);
      rows->append(*row);
      inRow = 0;
    }
    row->append(*tile);
    ++inRow;
  }
  return rows;
}

// Wrap a widget in a MotionBin (a reveal ring / translated element).
urnw::motion::MotionBin* WrapInBin(Gtk::Widget& child) {
  auto* bin = Gtk::make_managed<urnw::motion::MotionBin>();
  bin->set_child(child);
  return bin;
}

}  // namespace

// The initial step, in the login stack's order (LOGIN_STACK_SPEC, shared by
// every app): carousel hero, then up to three full-width pills — Google,
// Apple, Create Instant Account — then the remaining ways in as square icon
// tiles four per row (secret key, auth code, Bittensor, Solana), then "or",
// the email/phone field and Get started. Google and Apple sign in through the
// provider's own web flow in the browser (Linux has no native provider flow). There is
// deliberately NO heading (the carousel supplies the headline) and NO guest
// button (superseded by seedphrase accounts). Wide (>=1000dip) the carousel
// moves to an art pane beside a fixed 544dip form column; narrow it rides
// atop the single column.
void MainWindow::BuildLogin() {
  loginPanel_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  loginPanel_->set_margin(16);
  loginPanel_->set_valign(Gtk::Align::CENTER);

  // the hero carousel in its motion wrapper (the reveal's HERO)
  carousel_ = Gtk::make_managed<LoginCarousel>();
  carousel_->ApplyStrings();
  heroBin_ = Gtk::make_managed<motion::MotionBin>();
  heroBin_->set_child(*carousel_);
  heroBin_->set_size_request(-1, 200);  // the narrow slot's cap
  loginPanel_->append(*heroBin_);

  // ---- the three full-width pills ----------------------------------------
  auto* pillGroup = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  auto* google = MakeUrIconButton(BrandIcon::Kind::Google,
                                  T_("sign_in_with_google", "Sign in with Google"));
  google->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnGoogle));
  pillGroup->append(*google);
  auto* apple = MakeUrIconButton(BrandIcon::Kind::Apple,
                                 T_("sign_in_with_apple", "Sign in with Apple"));
  apple->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnApple));
  pillGroup->append(*apple);
  auto* instant =
      MakeUrButton(T_("create_instant_account", "Create Instant Account"), false);
  instant->signal_clicked().connect([this] {
    loginError_.set_text("");
    creatingInstant_ = false;
    if (instantTerms_) {
      instantTerms_->set_active(false);
      instantTerms_->set_sensitive(true);
    }
    if (instantError_) instantError_->set_text("");
    if (instantCreate_) instantCreate_->set_sensitive(false);
    stack_.set_visible_child("instant");
  });
  pillGroup->append(*instant);
  walletBin_ = WrapInBin(*pillGroup);
  loginPanel_->append(*walletBin_);

  // ---- the tiles: the less common ways in ----------------------------------
  auto* secretKey =
      MakeUrTileButton(BrandIcon::Kind::Key, T_("login_tile_secret_key", "Seed"));
  secretKey->signal_clicked().connect([this] {
    loginError_.set_text("");
    if (seedphraseView_) seedphraseView_->get_buffer()->set_text("");
    if (seedphraseError_) seedphraseError_->set_text("");
    OnSeedphraseChanged();
    stack_.set_visible_child("seedphrase");
    if (seedphraseView_) seedphraseView_->grab_focus();
  });
  auto* authCode = MakeUrTileButton(BrandIcon::Kind::AuthCode, T_("auth_code", "Auth code"));
  authCode->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnUseCode));
  auto* bittensor = MakeUrTileButton(BrandIcon::Kind::Bittensor, T_("bittensor", "Bittensor"));
  bittensor->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnBittensor));
  // ONE Solana tile, as android has: the bridge needs a provider up front,
  // so this presents a Phantom/Solflare chooser
  auto* solana = MakeUrTileButton(BrandIcon::Kind::Solana, T_("solana", "Solana"));
  solana->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnSolanaChooser));
  auto* tiles = MakeTileRows({secretKey, authCode, bittensor, solana});
  secondaryBin_ = WrapInBin(*tiles);
  loginPanel_->append(*secondaryBin_);

  auto* orDivider = Gtk::make_managed<Gtk::Label>(T_("or", "or"));
  orDivider->add_css_class("dim-label");
  orBin_ = WrapInBin(*orDivider);
  loginPanel_->append(*orBin_);

  // ---- email / phone (URTextInput: a label above the underlined field) -----
  auto* emailGroup = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto* emailLabel = Gtk::make_managed<Gtk::Label>(T_("user_auth_label", "Email or phone"));
  emailLabel->add_css_class("ur-input-label");
  emailLabel->set_xalign(0);
  emailGroup->append(*emailLabel);
  email_.add_css_class("ur-input");
  email_.set_placeholder_text(
      T_("user_auth_input_placeholder", "Enter your email or phone number"));
  email_.signal_changed().connect([this] {
    loginError_.set_text("");
    if (getStartedBtn_) {
      getStartedBtn_->set_sensitive(!discoveringLogin_ &&
                                    !TrimWhitespace(email_.get_text()).empty());
    }
  });
  email_.signal_activate().connect(sigc::mem_fun(*this, &MainWindow::OnGetStarted));
  emailGroup->append(email_);
  emailGroupBin_ = WrapInBin(*emailGroup);
  loginPanel_->append(*emailGroupBin_);

  // disabled until the field has something in it: an enabled primary button
  // over an empty field promises an action that cannot happen
  getStartedBtn_ = MakeUrButton(T_("get_started", "Get started"), /*primary=*/true);
  getStartedBtn_->set_sensitive(false);
  getStartedBtn_->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnGetStarted));
  getStartedBin_ = WrapInBin(*getStartedBtn_);
  loginPanel_->append(*getStartedBin_);

  // URInlineErrorText: a line of coral body text, not an info bar
  loginError_.add_css_class("ur-error-text");
  loginError_.set_wrap(true);
  loginError_.set_xalign(0);
  loginPanel_->append(loginError_);

  // "Change Network API" — the very last thing on the screen, bottom LEFT,
  // small and muted, reading as plain text rather than another pill: it is a
  // developer/fork affordance, not a sign-in option. A real button (windows:
  // a HyperlinkButton), so it is focusable and announced — never a bare label
  // with a click gesture.
  auto* networkServerLink = Gtk::make_managed<Gtk::Button>(
      T_("change_network_api", "Change Network API"));
  networkServerLink->add_css_class("ur-quiet-link");
  networkServerLink->set_halign(Gtk::Align::START);
  networkServerLink->set_margin_top(8);
  networkServerLink->signal_clicked().connect([this] {
    loginError_.set_text("");
    networkServerSheet_ = std::make_unique<NetworkServerSheet>(*this, host_);
    networkServerSheet_->on_applied = [this] {
      // a switch re-derives the Api and the LocalState: the flow starts over
      // on whatever the new server says about this client
      email_.set_text("");
      loginError_.set_text("");
      loginUserAuth_.clear();
      stack_.set_visible_child("login");
    };
    networkServerSheet_->present();
  });
  loginPanel_->append(*networkServerLink);

  loginAffordances_ = {getStartedBtn_, google, apple, instant, secretKey, authCode, bittensor, solana};

  // ---- wide | narrow assembly (the app-wide 1000dip breakpoint) ------------
  auto* clamp = Gtk::make_managed<Gtk::Box>();  // host for the adw clamp below
  GtkWidget* adwClamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(adwClamp), 512);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(adwClamp), 512);
  adw_clamp_set_child(ADW_CLAMP(adwClamp), GTK_WIDGET(loginPanel_->gobj()));
  gtk_widget_set_hexpand(adwClamp, TRUE);
  clamp->append(*Glib::wrap(adwClamp));
  clamp->set_hexpand(true);

  auto* formScroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  formScroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  formScroller->set_child(*clamp);
  formScroller->set_hexpand(true);
  loginFormColumn_ = formScroller;

  // the wide art pane: empty until the breakpoint reparents the hero here
  artPane_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  artPane_->set_margin(24);
  artPane_->set_hexpand(true);
  artPane_->set_vexpand(true);
  artPane_->set_visible(false);

  auto* loginGrid = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  loginGrid->append(*artPane_);
  loginGrid->append(*formScroller);

  GtkWidget* bpBin = adw_breakpoint_bin_new();
  gtk_widget_set_size_request(bpBin, 360, 300);  // breakpoint bins need a floor
  adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(bpBin), GTK_WIDGET(loginGrid->gobj()));
  AdwBreakpoint* wide = adw_breakpoint_new(adw_breakpoint_condition_new_length(
      ADW_BREAKPOINT_CONDITION_MIN_WIDTH, 1000, ADW_LENGTH_UNIT_SP));
  g_signal_connect(wide, "apply", G_CALLBACK(+[](AdwBreakpoint*, gpointer data) {
                     static_cast<MainWindow*>(data)->ApplyLoginBreakpoint(true);
                   }),
                   this);
  g_signal_connect(wide, "unapply", G_CALLBACK(+[](AdwBreakpoint*, gpointer data) {
                     static_cast<MainWindow*>(data)->ApplyLoginBreakpoint(false);
                   }),
                   this);
  adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(bpBin), wide);  // takes ownership

  stack_.add(*Glib::wrap(bpBin), "login");
}

// Wide login (windows ApplyBreakpoint Task 2a): move the hero between the
// narrow column's top slot and the art pane. A reparent detaches the carousel
// mid-animation, so it lands a clean slide and re-derives its metrics after.
void MainWindow::ApplyLoginBreakpoint(bool wide) {
  if (wideLogin_ == wide || !heroBin_) return;
  wideLogin_ = wide;
  g_object_ref(heroBin_->gobj());
  if (wide) {
    loginPanel_->remove(*heroBin_);
    heroBin_->set_size_request(-1, -1);
    heroBin_->set_vexpand(true);
    heroBin_->set_hexpand(true);
    artPane_->append(*heroBin_);
    artPane_->set_visible(true);
    loginFormColumn_->set_hexpand(false);
    loginFormColumn_->set_size_request(544, -1);
  } else {
    artPane_->remove(*heroBin_);
    artPane_->set_visible(false);
    heroBin_->set_vexpand(false);
    heroBin_->set_hexpand(false);
    heroBin_->set_size_request(-1, 200);
    loginPanel_->prepend(*heroBin_);
    loginFormColumn_->set_hexpand(true);
    loginFormColumn_->set_size_request(-1, -1);
  }
  g_object_unref(heroBin_->gobj());
  carousel_->HostReparented();
}

void MainWindow::size_allocate_vfunc(int width, int height, int baseline) {
  Gtk::ApplicationWindow::size_allocate_vfunc(width, height, baseline);
  if (width <= 0 || width == pageWidthDip_) return;
  pageWidthDip_ = width;
  // DEFERRED: a fold changes child visibility, and queueing a resize from
  // inside an allocation is how you get a layout loop. Idle is one frame late
  // and correct.
  Glib::signal_idle().connect_once([this, width] { ApplyPageBreakpoint(width); });
}

// The signed-in breakpoint fan-out. Every destination owns its own thresholds
// (Connect folds at 1000/640, Network/Settings/Earnings at their own); the
// window only reports the width it was actually given.
// Every few seconds while we believe a tunnel exists, ask the daemon what it
// thinks. A protective teardown publishes tunnel_state=Error with a reason and a
// code, and until now NOTHING in the app ever read it: the only Status() calls
// are on a connect press, the stale-device check, and the kill-switch read-back.
// A user sitting idle would keep a green "Connected" while the daemon had
// already stopped the session and possibly armed the kill switch.
bool MainWindow::PollDaemonHealth() {
  // On a worker (SdkHost::RequestDaemonStatus): a daemon that accepts the
  // socket but no longer answers used to hold this window for the control
  // client's 30 s timeout on every tick. A read still in flight skips the
  // tick. The one reply serves every follow-up in ApplyDaemonHealth, which
  // while disconnected used to read the status once each.
  const uint64_t epoch = daemonStatusEpoch_;
  // `this` outlives the reply: main.cpp holds the window until main returns,
  // past app->run() and with it the last main-loop dispatch
  host_.RequestDaemonStatus([this, epoch](std::optional<ctl::StatusReply> status) {
    // a start, a Disconnect or a sign-in or -out since the read: its reply
    // describes a session that is not this one (a "stopped" read before a
    // start is no stop of the session it started)
    if (epoch != daemonStatusEpoch_) return;
    ApplyDaemonHealth(status);
  });
  return true;
}

void MainWindow::ForgetDaemonStatus() {
  daemonStatus_.reset();
  ++daemonStatusEpoch_;
  ApplyStatusStripDetails();
}

void MainWindow::ApplyDaemonHealth(const std::optional<ctl::StatusReply>& status) {
  // the strip's Routes, with a session or without one, when the kill
  // switch's floor can be armed with no tunnel
  if (status) {
    daemonStatus_ = status;
    ApplyStatusStripDetails();
  }
  if (!connected_) {
    // A countdown belongs to a live tunnel, and there is none.
    if (connectPage_) connectPage_->SetFailsafeArmed(false);
    // A daemon that did not answer the worker is asked again next tick: a
    // read here would hold the window for as long as the worker's took.
    if (status) {
      // The network country the daemon reads (P052), for this process's own
      // dials: from the sign-in screen on, signed in or not.
      host_.FollowDaemonNetworkCountry(*status);
      // The outcome of a feedback's log upload, while one is pending.
      host_.FollowDaemonLogUpload(*status);
      // A Reset extenders the daemon refused during a tunnel bring-up, sent
      // again once the bring-up settled.
      host_.FollowDaemonExtenderReset(*status);
      // Disconnected is when the provider-only device is the provider: start
      // it after a launch without auto-connect, bring it back after a service
      // restart or an unexpected drop, and stop one the mode no longer wants.
      // Served by this reply, so a steady tick reads nothing on the main loop.
      host_.ReconcileProvider("health poll", *status);
    }
    // What the daemon holds while this window holds no session: the tray's
    // recovery items, and the stop of a session this window saw end, which
    // Windows explains in exactly this state. The connect feed can report the
    // disconnect before any poll reads the stop, or the daemon can be
    // restarting, so the explanation waits until a status is read.
    PushTrayRecovery(status ? failsafe_notice::TrayRecoveryFor(/*windowConnected=*/false, *status)
                            : failsafe_notice::TrayRecovery{});
    if (status && stopExplanationOwed_) {
      stopExplanationOwed_ = false;
      if (const auto failsafe =
              failsafe_notice::StoppedCopy(status->stop_reason, status->kill_switch)) {
        if (connectPage_) connectPage_->SetDaemonNotice(T_(failsafe->key, failsafe->english));
      }
    }
    return;
  }
  if (!status) return;  // unreachable is StartTunnelUi's business
  // The window holds the session, so its own Disconnect is the recovery.
  PushTrayRecovery(failsafe_notice::TrayRecoveryFor(/*windowConnected=*/true, *status));
  host_.FollowDaemonNetworkCountry(*status);
  host_.FollowDaemonExtenderReset(*status);
  host_.FollowDaemonLogUpload(*status);
  // The dead-tunnel failsafe's countdown on the live tunnel, warned about
  // before the daemon turns it off, never after.
  if (connectPage_) {
    connectPage_->SetFailsafeArmed(
        failsafe_notice::ShowsArmedWarning(status->failsafe_armed, status->tunnel_state));
  }
  if (status->tunnel_state != ctl::TunnelState::Error &&
      status->tunnel_state != ctl::TunnelState::Stopped) {
    return;
  }
  // …UNLESS WE ARE THE ONES WHO ASKED. A user disconnect ends with
  // SdkHost::Disconnect -> control_.StopTunnel(), so the daemon's very next
  // status reads Stopped — indistinguishable, from here, from the protective
  // teardown this poll exists to surface. Reporting it would answer a Disconnect
  // press with "The connection stopped.", a scary notice for the thing the user
  // just asked for. The page holds the intent (it is what renders
  // "Disconnecting…"), so it is the one that can tell the two apart; settle the
  // window's own state and say nothing.
  if (connectPage_ && connectPage_->DisconnectPending()) {
    ApplyConnectReading(DaemonTunnelGoneReading());
    stopExplanationOwed_ = false;
    return;
  }
  // The daemon stopped carrying traffic without us asking. Say so, verbatim —
  // the daemon composes the plain-language reason (including whether the machine
  // is now blocked and how to lift it), and inventing our own wording here would
  // be a third place that can disagree about what happened.
  //
  // The dead-tunnel failsafe is the exception, as on Windows: its two outcomes
  // have store strings in the user's language, chosen by the kill switch the
  // daemon reports now. A kill switch that could not be armed keeps the
  // daemon's sentence, which says what is left behind.
  Glib::ustring detail =
      status->error.empty()
          ? Glib::ustring(T_("tunnel_stopped_unexpectedly", "The connection stopped."))
          : Glib::ustring(status->error);
  if (const auto failsafe =
          failsafe_notice::StoppedCopy(status->stop_reason, status->kill_switch)) {
    detail = T_(failsafe->key, failsafe->english);
  }
  g_warning("connect: the daemon stopped the session (%s): %s",
            status->error_code.empty() ? "no code" : status->error_code.c_str(),
            status->error.c_str());
  ApplyConnectReading(DaemonTunnelGoneReading());
  // Explained here: the disconnected poll owes nothing more for this stop.
  stopExplanationOwed_ = false;
  if (connectPage_) connectPage_->SetDaemonNotice(detail);
}

void MainWindow::PushTrayRecovery(const failsafe_notice::TrayRecovery& recovery) {
  if (trayRecoveryPushed_ && recovery == trayRecovery_) return;
  trayRecoveryPushed_ = true;
  trayRecovery_ = recovery;
  if (on_tray_recovery_change) on_tray_recovery_change(recovery);
}

// The tray's recovery items act on the daemon directly: the window holds no
// session for its own Disconnect to end. Neither does anything once the window
// holds one again, since the item is withdrawn then.
void MainWindow::ForceTunnelOff() {
  if (connected_ || !trayRecovery_.forceTunnelOff) return;
  g_message("tray: forcing the daemon's tunnel off (recovery)");
  std::string error;
  if (!host_.Control().StopTunnel(&error)) {
    g_warning("tray: stop_tunnel failed: %s", error.empty() ? "no detail" : error.c_str());
  }
  // A status read that started before the stop describes the tunnel it
  // stopped, and would offer this item again for a tick: drop its reply.
  ++daemonStatusEpoch_;
  PollDaemonHealth();
}

void MainWindow::LiftKillSwitch() {
  if (connected_ || !trayRecovery_.liftKillSwitch) return;
  g_message("tray: turning the kill switch off (unblock this machine)");
  // All three legs, as the Settings switch turns it off, so the next connect
  // does not arm it again. The daemon's write runs on a worker; the next poll
  // reads its outcome and withdraws the item.
  host_.SetKillSwitch(false);
}

void MainWindow::ApplyPageBreakpoint(int widthDip) {
  // the shell first: its rail takes its width out of the room the pages get
  if (shell_) shell_->ApplyBreakpoint(widthDip);
  if (connectPage_) connectPage_->ApplyBreakpoint(widthDip);
  if (networkPage_) networkPage_->ApplyBreakpoint(widthDip);
  if (settingsPage_) settingsPage_->ApplyBreakpoint(widthDip);
  if (earningsPage_) earningsPage_->ApplyBreakpoint(widthDip);
  if (developerPage_) developerPage_->ApplyBreakpoint(widthDip);
  if (supportPage_) supportPage_->ApplyBreakpoint(widthDip);
  if (accountPage_) accountPage_->ApplyBreakpoint(widthDip);
}

// Only the initial step shows the carousel, and only while the window is on
// screen — a tray app spends most of its life hidden, and a slideshow nobody
// can see is pure wakeups.
MainWindow::~MainWindow() {
  geometrySave_.disconnect();
  UntrackAppFocus();
  host_.SetConnectGate(nullptr);  // the gate reads this window
  host_.SetRowConnect(nullptr);   // and so does the row's start path
}

void MainWindow::TrackAppFocus() {
  auto toplevels = Gtk::Window::get_toplevels();
  auto hook = [this, toplevels] {
    for (guint i = 0; i < toplevels->get_n_items(); ++i) {
      GObject* window = static_cast<GObject*>(g_list_model_get_item(toplevels->gobj(), i));
      if (!window) continue;
      // marked on the window itself, so a destroyed window takes its mark along
      if (!g_object_get_data(window, "urnw-app-focus")) {
        g_object_set_data(window, "urnw-app-focus", GINT_TO_POINTER(1));
        g_signal_connect(window, "notify::is-active", G_CALLBACK(&MainWindow::OnToplevelActiveChanged),
                         this);
      }
      g_object_unref(window);
    }
    ScheduleAppFocusSync();
  };
  toplevelsChanged_ = toplevels->signal_items_changed().connect(
      [hook](guint, guint, guint) { hook(); });
  hook();
}

void MainWindow::UntrackAppFocus() {
  // the hooks carry `this`: drop every one before the window goes away
  toplevelsChanged_.disconnect();
  appFocusSync_.disconnect();
  auto toplevels = Gtk::Window::get_toplevels();
  for (guint i = 0; i < toplevels->get_n_items(); ++i) {
    GObject* window = static_cast<GObject*>(g_list_model_get_item(toplevels->gobj(), i));
    if (!window) continue;
    if (g_signal_handlers_disconnect_by_func(
            window, reinterpret_cast<gpointer>(&MainWindow::OnToplevelActiveChanged), this) > 0) {
      g_object_set_data(window, "urnw-app-focus", nullptr);
    }
    g_object_unref(window);
  }
}

void MainWindow::OnToplevelActiveChanged(GObject*, GParamSpec*, gpointer self) {
  static_cast<MainWindow*>(self)->ScheduleAppFocusSync();
}

void MainWindow::ScheduleAppFocusSync() {
  // Moving between two of the app's windows deactivates one before the other
  // activates; reading once on idle keeps that from pausing the poll.
  if (appFocusSync_.connected()) return;
  appFocusSync_ = Glib::signal_idle().connect([this] {
    auto toplevels = Gtk::Window::get_toplevels();
    bool focused = false;
    for (guint i = 0; i < toplevels->get_n_items() && !focused; ++i) {
      GObject* window = static_cast<GObject*>(g_list_model_get_item(toplevels->gobj(), i));
      if (!window) continue;
      focused = gtk_window_is_active(GTK_WINDOW(window));
      g_object_unref(window);
    }
    balance_.SetAppFocused(focused);
    if (const auto away = appFocusAway_.Read(focused, g_get_monotonic_time() / 1000)) {
      OnAppReturned(*away);
    }
    return false;
  });
}

// The browser sign-ins answer only through their deep link, and a browser the
// user closed sends nothing: coming back enables the affordances again. The
// attempt stays armed, so a late return still lands in OnWalletAuth and a new
// click supersedes it (BrowserSignInGate.hpp).
void MainWindow::OnAppReturned(int64_t awayMillis) {
  const bool manualSheetOpen = bittensorManualSheet_ && bittensorManualSheet_->get_visible();
  if (!browserSignIn_.TakeOnReturn(awayMillis, manualSheetOpen)) return;
  SetLoginBusy(false);
  // the "Opening your wallet" progress notice is stale now; an error stays
  if (loginError_.has_css_class("dim-label")) loginError_.set_text("");
}

void MainWindow::UpdateCarouselRunning() {
  if (!carousel_) return;
  carousel_->SetActive(windowVisible_ && stack_.get_visible_child_name() == "login");
}

// The signed-out Hero Bloom (motion-overhaul spec §2.1): the hero springs
// 0.92 -> 1 under a 500ms fade while the rings unfold around it on the 40ms
// stagger grid. Delays and directions are the spec's signed-out table.
//
// The reveal fails silently by design (a wrong choreography is still a
// working window), so it leaves breadcrumbs in the log, in the Windows
// client's words: a "no animations" report is diagnosable from the log alone.
void MainWindow::RunSignedOutReveal() {
  using namespace motion;
  if (!ShouldAnimate()) {
    g_message("reveal: not armed (animations off in GTK)");
    return;
  }
  if (!heroBin_) return;
  SettleReveal();  // a reveal still running from a previous show settles first
  g_message("reveal: armed (signed-out table)");
  ArmHeroBloom(*heroBin_);
  StartHeroBloom(*heroBin_);
  revealStartedUs_ = g_get_monotonic_time();
  // the brand beat: the wordmark joins mid-hero-settle — the signed-out table's
  // AppTitleBar row (+8 -> rises up, delay 120)
  if (brandBin_) RiseIn(*brandBin_, Rise::Up, kDist8, kBrandBeatMs);
  RiseIn(*walletBin_, Rise::Down, kDist12, 240);
  RiseIn(*secondaryBin_, Rise::Down, kDist12, 280);
  RiseIn(*orBin_, Rise::Down, kDist8, 300);
  RiseIn(*emailGroupBin_, Rise::Down, kDist8, 320);
  RiseIn(*getStartedBin_, Rise::Down, kDist8, 360);
  g_message("reveal: started");
}

// CancelToFinal: every pose the reveal ever writes is either animated back to
// settled or restored right here — never left stranded (the settle invariant).
void MainWindow::SettleReveal() {
  // the last rise lands at 360 + kSlowMs; a settle before then cuts one short
  const int64_t revealMs = 360 + motion::kSlowMs;
  if (revealStartedUs_ != 0 && g_get_monotonic_time() - revealStartedUs_ < revealMs * 1000) {
    g_message("reveal: cancel-to-final while armed (hidden or superseded mid-bloom)");
  }
  revealStartedUs_ = 0;
  for (motion::MotionBin* bin : {heroBin_, brandBin_, emailGroupBin_, getStartedBin_,
                                 orBin_, walletBin_, secondaryBin_}) {
    if (bin) bin->settle();
  }
}

// One label, two voices (windows: the muted "Opening your wallet..." progress
// line is not an error and must not read coral).
void MainWindow::SetLoginError(const Glib::ustring& text) {
  loginError_.remove_css_class("dim-label");
  loginError_.add_css_class("ur-error-text");
  loginError_.set_text(text);
}

void MainWindow::SetLoginNotice(const Glib::ustring& text) {
  loginError_.remove_css_class("ur-error-text");
  loginError_.add_css_class("dim-label");
  loginError_.set_text(text);
}

// android disables every sign-in affordance while any one is in flight.
void MainWindow::SetLoginBusy(bool busy) {
  for (Gtk::Widget* widget : loginAffordances_) {
    if (widget) widget->set_sensitive(!busy);
  }
  if (!busy && getStartedBtn_) {
    // Get started also depends on the field having something in it
    getStartedBtn_->set_sensitive(!TrimWhitespace(email_.get_text()).empty());
  }
}

// The password step of the email-first login (mac LoginPasswordView / Windows
// PasswordPanel): discovery said the user auth has a password account. The
// verify routing for an unverified account and the forgot-password reset both
// hang off this step, keyed on the discovered loginUserAuth_.
namespace {

// A sign-in step: a back chevron over the step's fields on a hairlined card —
// a bare column floating on the page reads as an accident (windows parity).
struct StepScaffold {
  Gtk::Box* page;  // the stack child
  Gtk::Box* card;  // append the step's fields here
};

StepScaffold MakeStepScaffold(std::function<void()> onBack, int maxWidth = 420) {
  auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  page->set_margin(24);
  page->set_valign(Gtk::Align::CENTER);
  page->set_halign(Gtk::Align::CENTER);
  page->set_size_request(maxWidth, -1);

  auto* back = Gtk::make_managed<Gtk::Button>();
  back->set_icon_name("go-previous-symbolic");
  back->add_css_class("flat");
  back->set_halign(Gtk::Align::START);
  back->signal_clicked().connect([onBack = std::move(onBack)] { onBack(); });
  page->append(*back);

  auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  card->add_css_class("ur-card-bordered");
  page->append(*card);
  return {page, card};
}

Gtk::Label* MakeStepHeading(const Glib::ustring& text) {
  auto* heading = Gtk::make_managed<Gtk::Label>(text);
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  heading->set_wrap(true);
  return heading;
}

}  // namespace

void MainWindow::BuildPasswordStep() {
  auto scaffold = MakeStepScaffold([this] {
    password_.set_text("");
    passwordError_.set_text("");
    stack_.set_visible_child("login");
  });

  scaffold.card->append(
      *MakeStepHeading(T_("its_nice_to_see_you_again", "It's nice to see you again")));

  // the discovered user auth this password belongs to
  passwordUserAuth_.add_css_class("dim-label");
  passwordUserAuth_.set_xalign(0);
  passwordUserAuth_.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
  scaffold.card->append(passwordUserAuth_);

  password_.add_css_class("ur-input");
  password_.set_show_peek_icon(true);
  password_.property_placeholder_text() = T_("password_label", "Password");
  // Enter submits. The GtkPasswordEntry::activate C signal is GTK 4.0; the
  // gtkmm wrapper (signal_activate) only landed in 4.18 and core24 ships 4.10,
  // so connect at the C level.
  g_signal_connect(password_.gobj(), "activate",
                   G_CALLBACK(+[](GtkPasswordEntry*, gpointer data) {
                     static_cast<MainWindow*>(data)->OnSignIn();
                   }),
                   this);
  scaffold.card->append(password_);

  signInBtn_ = Gtk::make_managed<Gtk::Button>(T_("sign_in", "Sign in"));
  signInBtn_->add_css_class("suggested-action");
  signInBtn_->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnSignIn));
  scaffold.card->append(*signInBtn_);

  // password reset flow (Api::authPasswordReset behind the reset page)
  auto* forgotRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  auto* forgotLabel =
      Gtk::make_managed<Gtk::Label>(T_("forgot_password", "Forgot your password?"));
  forgotLabel->add_css_class("ur-caption");
  forgotRow->append(*forgotLabel);
  auto* forgot = Gtk::make_managed<Gtk::Button>(T_("reset_it", "Reset it."));
  forgot->add_css_class("flat");
  forgot->signal_clicked().connect([this] {
    passwordError_.set_text("");
    resetPage_->Configure(loginUserAuth_);
    stack_.set_visible_child("reset");
  });
  forgotRow->append(*forgot);
  scaffold.card->append(*forgotRow);

  passwordError_.add_css_class("ur-error-text");
  passwordError_.set_wrap(true);
  passwordError_.set_xalign(0);
  scaffold.card->append(passwordError_);

  stack_.add(*scaffold.page, "password");
}

// ---- seedphrase sign-in (macOS LoginSeedphraseView / windows parity) --------

namespace {
// how many whitespace-separated words are in `value` — counting rather than
// splitting: the phrase is a credential and is not copied around here
size_t CountWords(const std::string& value) {
  size_t count = 0;
  bool inWord = false;
  for (unsigned char c : value) {
    const bool space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (!space && !inWord) ++count;
    inWord = !space;
  }
  return count;
}
constexpr size_t kShortSeedphraseWords = 12;
constexpr size_t kLongSeedphraseWords = 24;
}  // namespace

void MainWindow::BuildSeedphraseStep() {
  auto scaffold = MakeStepScaffold([this] {
    // nothing may leave a phrase sitting in the field
    if (seedphraseView_) seedphraseView_->get_buffer()->set_text("");
    stack_.set_visible_child("login");
  }, 440);

  scaffold.card->append(
      *MakeStepHeading(T_("sign_in_with_seedphrase", "Sign in with Seedphrase")));

  // multi-line monospace: a 24-word phrase does not fit on one line and gets
  // pasted with newlines in it. No spellcheck / prediction by construction —
  // GtkTextView has neither, which is exactly right for a credential.
  seedphraseView_ = Gtk::make_managed<Gtk::TextView>();
  seedphraseView_->add_css_class("ur-input-multi");
  seedphraseView_->set_wrap_mode(Gtk::WrapMode::WORD);
  // The phrase is the account's credential: an input method's spellcheck
  // would rewrite BIP-39 words, and one that learns what is typed would keep
  // it. The words stay visible, so the purpose stays free form.
  seedphraseView_->set_input_hints(Gtk::InputHints::NO_SPELLCHECK | Gtk::InputHints::PRIVATE |
                                   Gtk::InputHints::NO_EMOJI);
  seedphraseView_->set_size_request(-1, 120);
  seedphraseView_->get_buffer()->signal_changed().connect(
      sigc::mem_fun(*this, &MainWindow::OnSeedphraseChanged));
  auto* seedScroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  seedScroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  seedScroller->set_child(*seedphraseView_);
  scaffold.card->append(*seedScroller);

  seedphraseCount_ = Gtk::make_managed<Gtk::Label>();
  seedphraseCount_->add_css_class("ur-caption");
  seedphraseCount_->set_xalign(0);
  seedphraseCount_->set_wrap(true);
  scaffold.card->append(*seedphraseCount_);

  seedphraseSubmit_ = Gtk::make_managed<Gtk::Button>(T_("sign_in", "Sign in"));
  seedphraseSubmit_->add_css_class("suggested-action");
  seedphraseSubmit_->set_sensitive(false);
  seedphraseSubmit_->signal_clicked().connect(
      sigc::mem_fun(*this, &MainWindow::OnSeedphraseSubmit));
  scaffold.card->append(*seedphraseSubmit_);

  seedphraseError_ = Gtk::make_managed<Gtk::Label>();
  seedphraseError_->add_css_class("ur-error-text");
  seedphraseError_->set_xalign(0);
  seedphraseError_->set_wrap(true);
  scaffold.card->append(*seedphraseError_);

  stack_.add(*scaffold.page, "seedphrase");
}

void MainWindow::OnSeedphraseChanged() {
  if (!seedphraseView_ || !seedphraseSubmit_ || !seedphraseCount_) return;
  if (seedphraseError_) seedphraseError_->set_text("");
  const size_t words = CountWords(seedphraseView_->get_buffer()->get_text());
  const bool valid = (words == kShortSeedphraseWords || words == kLongSeedphraseWords);
  seedphraseSubmit_->set_sensitive(valid && !seedphraseLoggingIn_);
  if (words == 0 || valid) {
    seedphraseCount_->set_text("");
  } else {
    // the count is the whole diagnostic — "invalid seedphrase" would not tell
    // anyone that they pasted 23 words
    seedphraseCount_->set_text(
        Format(TN_("seedphrase_word_count_warning",
                   "That's {} word — a seedphrase is 12 or 24 words",
                   "That's {} words — a seedphrase is 12 or 24 words",
                   static_cast<unsigned long>(words)),
               static_cast<uint64_t>(words)));
  }
}

void MainWindow::OnSeedphraseSubmit() {
  if (!seedphraseView_ || seedphraseLoggingIn_) return;
  const std::string phrase = seedphraseView_->get_buffer()->get_text();
  const size_t words = CountWords(phrase);
  if (words != kShortSeedphraseWords && words != kLongSeedphraseWords) return;
  seedphraseLoggingIn_ = true;
  seedphraseSubmit_->set_sensitive(false);
  seedphraseError_->set_text("");
  // CLEAR THE FIELD NOW, not on re-entry: a successful sign-in never returns
  // to this step, so "cleared on re-entry" means "never cleared" — and a
  // credential sitting in a readable widget is not a trade worth making.
  seedphraseView_->get_buffer()->set_text("");

  host_.LoginWithSeedphrase(phrase, [this](AuthResult r) {
    PostToMain([this, r] {
      seedphraseLoggingIn_ = false;
      OnSeedphraseChanged();
      if (!r.ok) {
        seedphraseError_->set_text(
            r.error.empty() ? T_("seedphrase_login_failed", "Seedphrase sign-in failed")
                            : r.error.c_str());
        return;
      }
      StartTunnelUi("seedphrase sign-in");  // auth handler flips the view
    });
  });
}

// ---- instant (seedphrase-only) account (macOS CreateNetworkInstantView) -----

void MainWindow::BuildInstantStep() {
  auto scaffold = MakeStepScaffold([this] { stack_.set_visible_child("login"); }, 440);

  scaffold.card->append(
      *MakeStepHeading(T_("create_instant_account", "Create Instant Account")));

  auto* explainer = Gtk::make_managed<Gtk::Label>(
      T_("instant_account_explainer",
         "No email, phone, or password needed. Your account is secured by a seedphrase."));
  explainer->add_css_class("dim-label");
  explainer->set_xalign(0);
  explainer->set_wrap(true);
  scaffold.card->append(*explainer);

  auto* termsRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  instantTerms_ = Gtk::make_managed<Gtk::CheckButton>();
  instantTerms_->set_valign(Gtk::Align::START);
  instantTerms_->signal_toggled().connect([this] {
    if (instantError_) instantError_->set_text("");
    if (instantCreate_) {
      instantCreate_->set_sensitive(instantTerms_->get_active() && !creatingInstant_);
    }
  });
  termsRow->append(*instantTerms_);
  auto* termsText = Gtk::make_managed<Gtk::Label>();
  termsText->set_markup(MarkdownLinksToPango(
      T_("i_agree_to_urnetwork_s_terms_and_services_https",
         "I agree to URnetwork's [Terms and Services](https://ur.io/terms) and "
         "[Privacy Policy](https://ur.io/privacy)")));
  termsText->add_css_class("dim-label");
  termsText->add_css_class("caption");
  termsText->set_wrap(true);
  termsText->set_xalign(0);
  termsText->set_hexpand(true);
  termsRow->append(*termsText);
  scaffold.card->append(*termsRow);

  // the marketing opt-out, on by default (every page that creates a network)
  auto* updatesRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  instantProductUpdates_ = Gtk::make_managed<Gtk::Switch>();
  instantProductUpdates_->set_valign(Gtk::Align::CENTER);
  instantProductUpdates_->set_active(true);
  updatesRow->append(*instantProductUpdates_);
  auto* updatesText = Gtk::make_managed<Gtk::Label>(
      T_("periodic_product_updates", "Periodic product updates"));
  updatesText->add_css_class("dim-label");
  updatesText->add_css_class("caption");
  updatesText->set_wrap(true);
  updatesText->set_xalign(0);
  updatesText->set_hexpand(true);
  updatesRow->append(*updatesText);
  scaffold.card->append(*updatesRow);

  // the optional referral code, always visible above Create Account
  // (android/apple instant-account parity): the server links the referral on
  // the seedphrase create path too
  instantReferralCode_ = Gtk::make_managed<ReferralCodeBox>(host_);
  instantReferralCode_->on_edit = [this] {
    if (instantError_) instantError_->set_text("");
  };
  scaffold.card->append(*instantReferralCode_);

  instantCreate_ = Gtk::make_managed<Gtk::Button>(T_("create_account_2", "Create Account"));
  instantCreate_->add_css_class("suggested-action");
  instantCreate_->set_sensitive(false);
  instantCreate_->signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::OnInstantSubmit));
  scaffold.card->append(*instantCreate_);

  instantError_ = Gtk::make_managed<Gtk::Label>();
  instantError_->add_css_class("ur-error-text");
  instantError_->set_xalign(0);
  instantError_->set_wrap(true);
  scaffold.card->append(*instantError_);

  stack_.add(*scaffold.page, "instant");
}

void MainWindow::OnInstantSubmit() {
  if (creatingInstant_ || !instantTerms_ || !instantTerms_->get_active()) return;
  creatingInstant_ = true;
  instantCreate_->set_sensitive(false);
  instantTerms_->set_sensitive(false);
  instantReferralCode_->set_sensitive(false);
  instantError_->set_text("");

  const std::string referralCode = instantReferralCode_->CreateCode();
  // the marketing opt-out rides on the create call (absent = opted in)
  host_.SetProductUpdatesOptOut(instantProductUpdates_ && !instantProductUpdates_->get_active());
  host_.CreateInstantAccount(referralCode, [this](SdkHost::InstantAccount account) {
    PostToMain([this, account = std::move(account)]() mutable {
      creatingInstant_ = false;
      instantTerms_->set_sensitive(true);
      instantReferralCode_->set_sensitive(true);
      instantCreate_->set_sensitive(instantTerms_->get_active());
      if (!account.ok) {
        instantError_->set_text(account.error.empty()
                                    ? T_("instant_account_failed",
                                         "Could not create the account. Please try again.")
                                    : account.error.c_str());
        return;
      }
      // the phrase is shown BEFORE the device registers: confirming is the
      // only way out of the sheet, and the only path to a session
      seedphraseSheet_ = std::make_unique<SeedphraseSheet>(*this, account.seedphrase);
      seedphraseSheet_->on_confirm = [this] {
        host_.ConfirmInstantAccount([this](AuthResult r) {
          PostToMain([this, r] {
            if (!r.ok) {
              instantError_->set_text(r.error.empty()
                                          ? T_("instant_account_failed",
                                               "Could not create the account. Please try again.")
                                          : r.error.c_str());
              return;
            }
            prefs::Set(kOnboardingPendingKey, true);  // an instant account is a new network
            StartTunnelUi("instant account");  // auth handler flips the view
          });
        });
      };
      seedphraseSheet_->present();
      // this frame held the app's copy of the credential; the sheet has its own
      std::fill(account.seedphrase.begin(), account.seedphrase.end(), '\0');
      account.seedphrase.clear();
    });
  });
}

void MainWindow::BuildHome() {
  // ---- the signed-in nav shell (windows NavigationView home) ---------------
  // Every destination registers as a page; the shell owns the nav, the status
  // strip and the mode-notice chrome.
  shell_ = Gtk::make_managed<HomeShell>();
  // The Windows-parity three-pane Home (docs/parity/connect-page.md).
  connectPage_ = Gtk::make_managed<ConnectPage>(host_);
  // on_connect_action, NOT on_toggle_connect: the press carries the action that
  // wrote the label the user actually clicked. Binding the void toggle here is
  // what left the window re-deriving the action from a stricter reading, and a
  // button reading "Disconnect" starting a tunnel.
  connectPage_->on_connect_action = [this](bool disconnect) { ToggleConnect(disconnect); };
  connectPage_->on_retry_connect = [this] { RetryConnect(); };
  connectPage_->on_open_locations = [this] { OpenLocationChooser(); };
  // "Connected to N providers" -> the provider sheet. MainWindow owns it
  // because the GeoClue location override must keep following the window
  // while the sheet is closed.
  connectPage_->on_open_provider_locations = [this] { OpenProviderLocations(); };
  // the easter egg: five taps on the connected dot play the Pro celebration
  connectPage_->on_connected_icon_tap = [this] { LaunchProCelebration(); };
  // the out-of-balance held alert: the same guest fork as Account's upgrade,
  // and the notification's disconnect-only path
  connectPage_->on_open_upgrade = [this] { OpenUpgrade(); };
  connectPage_->on_balance_disconnect = [this] { DisconnectFromBalanceNotice(); };
  connectPage_->on_open_data_info = [this] { OpenDataInfo(); };
  connectPage_->on_cancel_balance_recovery = [this] { ClearBalanceRecovery(); };
  // The strip's state and provider are the page's own render, as on Windows,
  // so the strip and the Connect page cannot disagree.
  connectPage_->on_status_rendered = [this](const Glib::ustring& text, const std::string& dot) {
    if (shell_) shell_->SetStatusState(text, dot);
  };
  connectPage_->on_location_rendered = [this](const Glib::ustring& text) {
    if (shell_) shell_->SetStatusProvider(text);
  };
  connectPage_->RepublishStatus();
  shell_->SetPage("connect", *connectPage_);
  auto placeholder = [this](const char* tag, const Glib::ustring& title) {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    page->add_css_class("ur-pane");
    page->append(*kit::MakePaneEmptyLine(title));
    shell_->SetPage(tag, *page);
  };
  networkPage_ = Gtk::make_managed<NetworkPage>(host_);
  shell_->SetPage("network", *networkPage_);
  earningsPage_ = Gtk::make_managed<EarningsPage>(host_);
  earningsPage_->on_snackbar = [this](const Glib::ustring& message, bool error) {
    if (shell_) {
      shell_->snackbar().Show(message, error ? kit::Snackbar::Severity::Error
                                             : kit::Snackbar::Severity::Success);
    }
  };
  earningsPage_->sheet_open = [this] { return sheetOpen_; };
  earningsPage_->on_sheet_open_changed = [this](bool open) { sheetOpen_ = open; };
  // the provide mode is changed on the connect page (its provide row); the
  // earnings row is a shortcut there, and so is the idle line's Change. The
  // picker sits in Simple mode's collapsed "More options" group, so it is
  // opened too.
  earningsPage_->on_open_provide_settings = [this] {
    if (shell_) shell_->Navigate("connect");
    if (connectPage_) connectPage_->RevealProvideControls();
  };
  shell_->SetPage("earnings", *earningsPage_);
  accountPage_ = Gtk::make_managed<AccountPage>(host_);
  accountPage_->on_snackbar = [this](const Glib::ustring& message, bool error) {
    if (shell_) {
      shell_->snackbar().Show(message, error ? kit::Snackbar::Severity::Error
                                             : kit::Snackbar::Severity::Success);
    }
  };
  // Same guest fork as Earnings: a guest has no account to hang a plan on.
  // A guest's button reads "Create an account": the conversion is all it asks
  // for, so it does not continue to the upgrade.
  accountPage_->on_open_upgrade = [this] {
    if (balance_.IsGuest()) {
      OpenGuestConversion();
    } else {
      OpenUpgrade();
    }
  };
  // a Pro network's plan label replays the Pro celebration
  accountPage_->on_plan_label_tap = [this] { LaunchProCelebration(); };
  // the data-usage group's info button: the sheet reads the balance store
  accountPage_->on_open_data_info = [this] { OpenDataInfo(); };
  // The redeem sheet needs the balance store (it starts confirmation polling),
  // which the page deliberately does not hold — so the window opens it.
  accountPage_->on_open_redeem = [this] {
    if (!redeemSheet_) redeemSheet_ = std::make_unique<RedeemCodeSheet>(*this, host_, balance_);
    redeemSheet_->Open();
  };
  accountPage_->sheet_open = [this] { return sheetOpen_; };
  accountPage_->on_sheet_open_changed = [this](bool open) { sheetOpen_ = open; };
  shell_->SetPage("account", *accountPage_);
  // The "Refer and earn" page: a destination without a rail item, reached from
  // Account's Referrals row and left through its own "‹ Account".
  referralsPage_ = Gtk::make_managed<ReferralsPage>(host_, balance_);
  referralsPage_->on_snackbar = [this](const Glib::ustring& message, bool error) {
    if (shell_) {
      shell_->snackbar().Show(message, error ? kit::Snackbar::Severity::Error
                                             : kit::Snackbar::Severity::Success);
    }
  };
  referralsPage_->sheet_open = [this] { return sheetOpen_; };
  referralsPage_->on_sheet_open_changed = [this](bool open) { sheetOpen_ = open; };
  referralsPage_->on_back = [this] {
    if (shell_) shell_->Navigate("account");
  };
  accountPage_->on_open_referrals = [this] {
    if (shell_) shell_->Navigate("referrals");
  };
  shell_->SetPage("referrals", *referralsPage_);
  // Account -> Sessions: a destination without a rail item too, reached from
  // Account's Sessions row and left through its own "‹ Account".
  sessionsPage_ = Gtk::make_managed<SessionsPage>(host_);
  sessionsPage_->on_snackbar = [this](const Glib::ustring& message, bool error) {
    if (shell_) {
      shell_->snackbar().Show(message, error ? kit::Snackbar::Severity::Error
                                             : kit::Snackbar::Severity::Success);
    }
  };
  sessionsPage_->sheet_open = [this] { return sheetOpen_; };
  sessionsPage_->on_sheet_open_changed = [this](bool open) { sheetOpen_ = open; };
  sessionsPage_->on_back = [this] {
    if (shell_) shell_->Navigate("account");
  };
  accountPage_->on_open_sessions = [this] {
    if (shell_) shell_->Navigate("sessions");
  };
  shell_->SetPage("sessions", *sessionsPage_);
  // the window may already be presenting: the page's foreground starts there
  sessionsPage_->SetPresentationActive(windowVisible_);
  supportPage_ = Gtk::make_managed<SupportPage>(host_);
  supportPage_->on_snackbar = [this](const Glib::ustring& message, bool error) {
    if (shell_) {
      shell_->snackbar().Show(message, error ? kit::Snackbar::Severity::Error
                                             : kit::Snackbar::Severity::Success);
    }
  };
  shell_->SetPage("support", *supportPage_);
  developerPage_ = Gtk::make_managed<DeveloperPage>(host_);
  shell_->SetPage("developer", *developerPage_);
  settingsPage_ = Gtk::make_managed<SettingsPage>(host_);
  settingsPage_->on_snackbar = [this](const Glib::ustring& message, bool error) {
    if (shell_) {
      shell_->snackbar().Show(message, error ? kit::Snackbar::Severity::Error
                                             : kit::Snackbar::Severity::Success);
    }
  };
  shell_->SetPage("settings", *settingsPage_);
  // the windows tag->load mapping: each destination loads on selection (and
  // again on auth change, through ApplyAuthState)
  shell_->on_navigate = [this](const std::string& tag) {
    // A destination's load must never take the window down: the SDK surface
    // throws urnet::Error on a no-session read, and the preview harness runs
    // with no session by design.
    try {
    if (tag == "network" && networkPage_) {
      networkPage_->SetSelected(true);
      networkPage_->Load();
    } else if (networkPage_) {
      networkPage_->SetSelected(false);
    }
    if (tag == "support" && supportPage_) supportPage_->Load();
    if (tag == "earnings" && earningsPage_) earningsPage_->Load();
    if (tag == "account" && accountPage_) {
      accountPage_->Load();
      balance_.FetchNow();  // the plan pane is painted from the store's snapshot
    }
    if (tag == "referrals" && referralsPage_) {
      referralsPage_->Load();
      balance_.FetchNow();  // the card is painted from the store's referral figures
    }
    if (tag == "sessions" && sessionsPage_) sessionsPage_->Load();
    // Settings owns the account-subject sheets too, so it loads for both tags.
    if ((tag == "settings" || tag == "account") && settingsPage_) settingsPage_->Load();
    if (developerPage_) {
      // the developer poll runs only while selected AND presenting AND advanced
      developerPage_->SetSelected(tag == "developer");
      if (tag == "developer") developerPage_->Load();
    }
    } catch (const std::exception& e) {
      g_warning("destination '%s' load failed: %s", tag.c_str(), e.what());
    }
  };
  shell_->Navigate("connect");
  // 5s: slow enough to cost nothing on a local socket, fast enough that a
  // protective teardown is not sat on.
  Glib::signal_timeout().connect(sigc::mem_fun(*this, &MainWindow::PollDaemonHealth), 5000);

  // The in-app updater. Bind-then-replay, like Advanced Mode below: the
  // handler is bound before Start() so the launch check's outcome cannot be
  // missed, and the current snapshot is replayed so Settings renders the
  // install kind's notice state from the first paint. The checker publishes
  // on the GTK main loop already (PostToMain), so no marshalling here.
  updates_ = std::make_unique<UpdateChecker>();
  developerPage_->SetUpdateChecker(updates_.get());
  settingsPage_->SetUpdateChecker(updates_.get());
  updates_->SetHandler([this](const UpdateChecker::Snapshot& snap) {
    if (settingsPage_) settingsPage_->ApplyUpdate(snap);
    if (developerPage_) developerPage_->ApplyUpdateCheck(snap);
  });
  settingsPage_->ApplyUpdate(updates_->Current());
  updates_->Start();

  // Advanced Mode (D5): bind-then-replay — the handler is bound before the
  // stored value is replayed, so a restored-from-disk true cannot be lost.
  host_.SetAdvancedModeHandler([this](bool on) {
    PostToMain([this, on] {
      if (shell_) shell_->SetAdvancedMode(on);
      if (connectPage_) connectPage_->SetAdvancedMode(on);
      if (settingsPage_) settingsPage_->SetAdvancedMode(on);
      if (developerPage_) developerPage_->SetAdvancedMode(on);
    });
  });
  host_.RefreshAdvancedMode();

  stack_.add(*shell_, "home");
}

// The sign-up / verify / password-reset pages (AuthViews.cpp) as stack
// children; each page reports back through callbacks and MainWindow routes.
void MainWindow::BuildAuthPages() {
  auto wrapInScroller = [](Gtk::Widget& page) {
    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 480);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 480);
    adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(page.gobj()));
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_child(*Glib::wrap(clamp));
    return scroller;
  };

  createPage_ = Gtk::make_managed<CreateNetworkPage>(host_);
  createPage_->on_success = [this] {
    // a network was just created: the onboarding flow follows the sign-in
    prefs::Set(kOnboardingPendingKey, true);
    StartTunnelUi("network created");  // auth handler flips the view
  };
  createPage_->on_verify = [this](std::string userAuth, VerifySendNotice notice) {
    NavigateVerify(userAuth);
    // a code that was not sent must not leave the page implying it was
    if (notice.kind != VerifySendNoticeKind::Sent) verifyPage_->ShowSendNotice(notice);
  };
  createPage_->on_back = [this] {
    stack_.set_visible_child(createPageFromHome_ ? "home" : "login");
  };
  stack_.add(*wrapInScroller(*createPage_), "create");

  verifyPage_ = Gtk::make_managed<VerifyPage>(host_);
  verifyPage_->on_success = [this] {
    prefs::Set(kOnboardingPendingKey, true);  // a verified sign-up is a new network
    StartTunnelUi("sign-up verified");  // auth handler flips the view
  };
  verifyPage_->on_back = [this] { stack_.set_visible_child("login"); };
  stack_.add(*wrapInScroller(*verifyPage_), "verify");

  resetPage_ = Gtk::make_managed<ResetPasswordPage>(host_);
  resetPage_->on_back = [this] {
    // back to the password step when a discovery routed there (Windows parity)
    stack_.set_visible_child(loginUserAuth_.empty() ? "login" : "password");
  };
  stack_.add(*wrapInScroller(*resetPage_), "reset");
}

void MainWindow::NavigateCreate(CreateNetworkPage::Mode mode, const std::string& userAuth,
                                bool fromHome) {
  createPageFromHome_ = fromHome;
  createPage_->Configure(mode, userAuth);
  stack_.set_visible_child("create");
  // focus the first empty field, not the back chevron (windows EnterCreateStep)
  createPage_->FocusFirstField();
}

void MainWindow::NavigateVerify(const std::string& userAuth) {
  verifyPage_->Configure(userAuth);
  stack_.set_visible_child("verify");
}

// The email-first entry: discover the account for the user auth and route
// (Windows ApplyLoginRouting / mac LoginInitialViewModel.getStarted).
void MainWindow::OnGetStarted() {
  const std::string userAuth = TrimWhitespace(email_.get_text());
  if (discoveringLogin_ || !LooksLikeUserAuth(userAuth)) return;
  discoveringLogin_ = true;
  SetLoginBusy(true);
  loginError_.set_text("");
  host_.StartLogin(userAuth, [this](LoginRouting routing) {
    PostToMain([this, routing] {
      discoveringLogin_ = false;
      SetLoginBusy(false);
      switch (routing.route) {
        case LoginRoute::Login:
          StartTunnelUi("sign-in");  // auth handler flips the view
          break;
        case LoginRoute::Password:
          loginUserAuth_ = routing.userAuth;
          passwordUserAuth_.set_text(loginUserAuth_);
          password_.set_text("");
          passwordError_.set_text("");
          stack_.set_visible_child("password");
          password_.grab_focus();
          break;
        case LoginRoute::Create:
          // a new user: continue into sign-up with the user auth prefilled
          NavigateCreate(CreateNetworkPage::Mode::Password, routing.userAuth,
                         /*fromHome=*/false);
          break;
        case LoginRoute::Verify:
          NavigateVerify(routing.userAuth);
          break;
        case LoginRoute::IncorrectAuth:
          // the account exists under another sign-in method
          loginError_.set_text(
              Format(T_("login_error_auth_allowed", "Please login with one of: {}."),
                     routing.authAllowed));
          break;
        case LoginRoute::Error:
          loginError_.set_text(routing.error.empty()
                                   ? T_("there_was_an_error_logging_in",
                                        "There was an error logging in")
                                   : routing.error);
          break;
      }
    });
  });
}

// The password step's sign-in, against the discovered loginUserAuth_.
void MainWindow::OnSignIn() {
  const std::string password(password_.get_text());
  if (signingIn_ || loginUserAuth_.empty() || password.empty()) return;
  signingIn_ = true;
  if (signInBtn_) signInBtn_->set_sensitive(false);
  passwordError_.set_text("");
  host_.LoginWithPassword(loginUserAuth_, password, [this](AuthResult r) {
    PostToMain([this, r] {
      signingIn_ = false;
      if (signInBtn_) signInBtn_->set_sensitive(true);
      if (r.verification_required) {
        // the login asked for a fresh numeric code; route into the verify
        // page and say whether it was sent (windows raises the same
        // informational)
        NavigateVerify(loginUserAuth_);
        if (verifyPage_) verifyPage_->ShowSendNotice(r.sendNotice);
      } else if (!r.ok) {
        passwordError_.set_text(r.error.empty() ? T_("sign_in_failed", "Sign in failed")
                                                : r.error);
      } else {
        StartTunnelUi("password sign-in");  // auth handler flips the view
      }
    });
  });
}

void MainWindow::OpenGuestConversion() {
  if (!guestConversionSheet_) {
    guestConversionSheet_ = std::make_unique<GuestConversionSheet>(*this, host_, balance_);
    guestConversionSheet_->on_done = [this] {
      guestUpgrade_.ConversionDone();
      if (shell_) {
        shell_->snackbar().Show(
            T_("sign_in_method_added_successfully", "Sign-in method added successfully"),
            kit::Snackbar::Severity::Success);
      }
    };
    // closed, done or not (on_done runs first): a purchase that sent the guest
    // here continues once the guest clears, here or on the balance change
    guestConversionSheet_->signal_hide().connect([this] {
      guestUpgrade_.ConversionClosed();
      guestUpgrade_.Poll(balance_.IsGuest());
    });
  }
  guestConversionSheet_->Open();
}

void MainWindow::DivertGuestToConversion(std::function<void()> checkout) {
  guestUpgrade_.Divert(std::move(checkout));
  OpenGuestConversion();
}

// The server refused the onboarding checkout for a guest network the balance
// had not reported yet: the conversion, then the upgrade once it is done.
void MainWindow::OnOnboardingGuestSignInRequired() {
  DivertGuestToConversion([this] { OpenUpgrade(); });
}

// android presents AuthCodeLoginSheet — a modal with its own field — instead
// of an inline box on the login screen; the desktop equivalent is a dialog.
void MainWindow::OnUseCode() {
  loginError_.set_text("");
  GtkWidget* dialog = adw_message_dialog_new(
      GTK_WINDOW(gobj()), T_("auth_code_login_sheet_header", "Auth code login"), nullptr);
  auto* field = Gtk::make_managed<Gtk::Entry>();
  field->add_css_class("ur-input");
  field->set_placeholder_text(T_("auth_code", "Auth code"));
  adw_message_dialog_set_extra_child(ADW_MESSAGE_DIALOG(dialog), GTK_WIDGET(field->gobj()));
  adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel",
                                   T_("cancel", "Cancel"), "login",
                                   T_("auth_code_login_button_text", "Log in with Auth Code"),
                                   nullptr);
  adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "login",
                                             ADW_RESPONSE_SUGGESTED);
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "login");
  // Enter in the field submits (the default response does not fire from an
  // extra child's activate on its own).
  field->signal_activate().connect([dialog] {
    adw_message_dialog_response(ADW_MESSAGE_DIALOG(dialog), "login");
  });
  struct Ctx {
    MainWindow* self;
    Gtk::Entry* field;
  };
  g_signal_connect_data(
      dialog, "response",
      G_CALLBACK(+[](AdwMessageDialog*, const char* response, gpointer data) {
        auto* ctx = static_cast<Ctx*>(data);
        if (g_strcmp0(response, "login") != 0) return;
        const std::string code = TrimWhitespace(ctx->field->get_text());
        if (code.empty()) return;
        MainWindow* self = ctx->self;
        self->SetLoginBusy(true);
        self->host_.LoginWithCode(code, [self](AuthResult r) {
          PostToMain([self, r] {
            self->SetLoginBusy(false);
            if (!r.ok) {
              self->loginError_.set_text(
                  r.error.empty() ? T_("code_sign_in_failed", "Code sign in failed")
                                  : r.error.c_str());
            } else {
              self->StartTunnelUi("code sign-in");
            }
          });
        });
      }),
      new Ctx{this, field},
      +[](gpointer data, GClosure*) { delete static_cast<Ctx*>(data); }, G_CONNECT_DEFAULT);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ONE Solana button, as android has: the browser bridge needs the provider
// baked into the deeplink it opens, so ask here — the desktop analogue of
// android's Mobile Wallet Adapter picker.
void MainWindow::OnSolanaChooser() {
  loginError_.set_text("");
  GtkWidget* dialog = adw_message_dialog_new(GTK_WINDOW(gobj()),
                                             T_("solana_sign_in", "Sign in with Solana"),
                                             nullptr);
  // wallet names are product names: never translated (the store marks them so)
  adw_message_dialog_add_responses(ADW_MESSAGE_DIALOG(dialog), "cancel",
                                   T_("cancel", "Cancel"), "phantom", "Phantom", "solflare",
                                   "Solflare", nullptr);
  adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dialog), "phantom",
                                             ADW_RESPONSE_SUGGESTED);
  adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dialog), "phantom");
  g_signal_connect(dialog, "response",
                   G_CALLBACK(+[](AdwMessageDialog*, const char* response, gpointer data) {
                     auto* self = static_cast<MainWindow*>(data);
                     if (g_strcmp0(response, "phantom") == 0) {
                       self->OnSolana(WalletConnect::Provider::Phantom);
                     } else if (g_strcmp0(response, "solflare") == 0) {
                       self->OnSolana(WalletConnect::Provider::Solflare);
                     }
                   }),
                   this);
  gtk_window_present(GTK_WINDOW(dialog));
}

void MainWindow::OnSolana(WalletConnect::Provider provider) {
  SetLoginNotice(T_("opening_wallet_in_browser", "Opening your wallet in the browser…"));
  SetLoginBusy(true);
  browserSignIn_.Begin();
  host_.SignInWithSolana(provider, [this](AuthResult r) { OnWalletAuth(r); });
}

void MainWindow::OnGoogle() { OnSso(sso::kProviderGoogle); }

void MainWindow::OnApple() { OnSso(sso::kProviderApple); }

// Google / Apple: the browser carries the provider's own sign-in; the api's
// callback returns the identity token on urnetwork://oauth/<provider> and it
// lands in OnWalletAuth like the wallet flows.
void MainWindow::OnSso(const std::string& provider) {
  loginError_.set_text("");
  SetLoginBusy(true);
  browserSignIn_.Begin();
  host_.SignInWithSso(provider, [this](AuthResult r) { OnWalletAuth(r); });
}

// The Bittensor tile asks which wallet first: Talisman signs on the ur.io
// bridge in the browser (its extension), TAO.com (or any wallet) on the manual
// sheet (it documents no programmatic interface), WalletConnect on the bridge
// page with a QR for Nova, Nightly and other WalletConnect wallets.
// BittensorWalletFlow.hpp.
void MainWindow::OnBittensor() {
  loginError_.set_text("");
  GtkWidget* dialog = NewBittensorWalletChooser(GTK_WINDOW(gobj()));
  g_signal_connect(dialog, "response",
                   G_CALLBACK(+[](AdwMessageDialog*, const char* response, gpointer data) {
                     auto* self = static_cast<MainWindow*>(data);
                     const std::string_view walletId =
                         bittensor::ChosenWallet(response ? response : "");
                     if (!walletId.empty()) self->OnBittensorWallet(std::string(walletId));
                   }),
                   this);
  gtk_window_present(GTK_WINDOW(dialog));
}

void MainWindow::OnBittensorWallet(const std::string& walletId) {
  const std::string transport =
      urnet::bittensorWalletTransportFor(walletId, std::string(bittensor::kPlatform));
  if (transport == bittensor::kTransportBrowserBridge) {
    const bittensor::ContinueText text = bittensor::ContinueTextFor(walletId);
    const std::string key(text.key);
    const std::string english(text.english);
    const char* localized = g_dpgettext2(GETTEXT_PACKAGE, key.c_str(), english.c_str());
    SetLoginNotice(text.takesWalletName
                       ? Format(localized, urnet::bittensorWalletDisplayName(walletId))
                       : std::string(localized));
  }
  SetLoginBusy(true);
  browserSignIn_.Begin();
  host_.SignInWithBittensor(walletId, [this](AuthResult r) { OnWalletAuth(r); });
}

// The manual sheet (TAO.com) for any Bittensor flow: sign-in, the
// create-network signature, or the Earnings coldkey. One at a time: a newer
// request replaces the sheet (the older flow was superseded already).
void MainWindow::ShowBittensorManualSheet(const SdkHost::BittensorManualRequest& request) {
  if (bittensorManualSheet_) bittensorManualSheet_->set_visible(false);
  bittensorManualSheet_ = std::make_unique<BittensorManualSheet>(*this, host_, request);
  bittensorManualSheet_->present();
}

// Shared tail of both wallet sign-ins (the SDK callback thread lands here).
void MainWindow::OnWalletAuth(const AuthResult& result) {
  PostToMain([this, result] {
    browserSignIn_.Settle();
    SetLoginBusy(false);
    if (!result.ok && bittensor::IsCancelled(result.error)) {
      // the user closed the Bittensor manual sheet: nothing failed
      loginError_.set_text("");
      return;
    }
    if (result.wallet_needs_network) {
      // wallet authenticated but has no network: the host kept the signed
      // wallet_auth; finish sign-up on the create page (name + terms)
      loginError_.set_text("");
      NavigateCreate(CreateNetworkPage::Mode::Wallet, "", /*fromHome=*/false);
    } else if (result.sso_needs_network) {
      // an sso identity with no network: the host kept the identity token
      loginError_.set_text("");
      NavigateCreate(CreateNetworkPage::Mode::Sso, "", /*fromHome=*/false);
    } else if (!result.ok && !result.authAllowed.empty()) {
      // the account exists under other sign-in methods
      SetLoginError(Format(T_("login_error_auth_allowed", "Please login with one of: {}."),
                           result.authAllowed));
    } else if (!result.ok) {
      if (!result.error.empty()) {
        // a pasted signature from another account than the entered address
        // names the wallet; any other refusal reads as sent
        SetLoginError(
            WalletProofRefusalText(result.errorCode, result.error, result.bittensorWalletId));
      } else if (result.sso) {
        SetLoginError(T_("there_was_an_error_logging_in", "There was an error logging in"));
      } else {
        SetLoginError(T_("wallet_sign_in_failed", "Wallet sign-in failed"));
      }
    } else {
      loginError_.set_text("");
      StartTunnelUi("wallet or sso sign-in");  // auth handler flips the view
    }
  });
}

// The post-sign-up onboarding: shown once, only after a network was created
// on this machine (never for an existing account signing in).
void MainWindow::NoteConnected() {
  // connect.first: once per network. The network id keys the memory, so a
  // second account on the same machine gets its own first connect.
  auto byJwt = host_.ParseByJwt();
  const std::string networkId = byJwt && byJwt->NetworkId ? *byJwt->NetworkId : byJwt ? byJwt->NetworkName : "";
  if (networkId.empty()) return;
  const std::string key = "connect_first_" + networkId;
  if (prefs::Get<bool>(key.c_str(), false)) return;
  prefs::Set(key.c_str(), true);
  host_.events().ConnectFirst();
}

void MainWindow::HandleOnboardingLink(const std::string& url) {
  if (!host_.IsLoggedIn() || !shell_) return;
  present();
  std::map<std::string, std::string> query;
  if (const size_t q = url.find('?'); q != std::string::npos) {
    size_t i = q + 1;
    while (i < url.size()) {
      const size_t amp = url.find('&', i);
      const std::string pair = url.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
      if (const size_t eq = pair.find('='); eq != std::string::npos) {
        char* dec = g_uri_unescape_string(pair.substr(eq + 1).c_str(), nullptr);
        query[pair.substr(0, eq)] = dec ? std::string(dec) : pair.substr(eq + 1);
        if (dec) g_free(dec);
      }
      if (amp == std::string::npos) break;
      i = amp + 1;
    }
  }
  switch (ParseOnboardingLink(url)) {
    case OnboardingLink::Connect:
      shell_->Navigate("connect");
      break;
    case OnboardingLink::Widgets:
      // no widgets on the desktop: the Account page is the closest destination
      shell_->Navigate("account");
      break;
    case OnboardingLink::Offer:
      if (balance_.OfferActive()) {
        if (!onboarding_) {
          onboarding_ = std::make_unique<OnboardingWindow>(*this, host_, balance_);
          onboarding_->on_guest_sign_in_required = [this] { OnOnboardingGuestSignInRequired(); };
          onboarding_->on_finished = [] { prefs::Set(kOnboardingPendingKey, false); };
        }
        onboarding_->OpenOffer();
      } else {
        OpenUpgrade();
      }
      break;
    case OnboardingLink::Feedback: {
      shell_->Navigate("support");
      int rating = 0;
      if (auto r = query.find("r"); r != query.end()) rating = std::atoi(r->second.c_str());
      const std::string why = query.count("why") ? query["why"] : "";
      std::string token = query.count("token") ? query["token"] : "";
      if (token.empty() && query.count("t")) token = query["t"];
      if (supportPage_) supportPage_->PrefillFromCampaign(token, rating, why);
      break;
    }
    case OnboardingLink::None:
      break;
  }
}

void MainWindow::OpenOnboardingIfPending() {
  if (!prefs::Get<bool>(kOnboardingPendingKey, false)) return;
  Glib::signal_timeout().connect_once([this] {
    if (!prefs::Get<bool>(kOnboardingPendingKey, false)) return;
    // A repeated logged-in transition (the session reconnecting, a second auth
    // event) must not restart a flow that is already on screen: Open() resets
    // it to page 1. The persisted pending pref is the only gate; it is cleared
    // when the flow hides for any reason (Get connected, Skip, Escape, close).
    if (onboarding_ && onboarding_->get_visible()) return;
    if (!onboarding_) {
      onboarding_ = std::make_unique<OnboardingWindow>(*this, host_, balance_);
      onboarding_->on_guest_sign_in_required = [this] { OnOnboardingGuestSignInRequired(); };
      onboarding_->on_finished = [] { prefs::Set(kOnboardingPendingKey, false); };
    }
    onboarding_->Open();
  }, 600);
}

// One rejection reaches here once per listener that heard it (the Api's, then
// the bound DeviceRemote's): only the first signs out. A report that lands
// after any sign-out, this app's own included, signs nothing out and says
// nothing. The notice is owed before Logout(), whose ApplyAuthState(false)
// shows it.
void MainWindow::OnAuthLogout(const auth_logout::Report& report) {
  if (!host_.SignsOut(report)) {
    g_message("auth: a sign-out report for a sign-in already ended was dropped (cause \"%s\")",
              report.cause.c_str());
    return;
  }
  g_message("auth: the server rejected this sign-in (cause \"%s\"); signing out",
            report.cause.c_str());
  signInNotice_.Arm(report.cause);
  host_.Logout();
}

void MainWindow::ApplyAuthState(bool loggedIn) {
  // Home's first entrance (windows homeRevealed_): the first time Home shows
  // in this window, a sign-in made in it crossfades the login flow into Home,
  // as the shell crossfades its destinations. Every other swap is instant: a
  // launch already signed in, a hidden window, animations off, a later
  // sign-in, and a sign-out (exits stay quiet).
  const bool firstEntrance = loggedIn && !homeRevealed_ && windowVisible_ &&
                             motion::ShouldAnimate() &&
                             stack_.get_visible_child_name() != "home";
  if (loggedIn) homeRevealed_ = true;
  stack_.set_visible_child(loggedIn ? "home" : "login",
                           firstEntrance ? Gtk::StackTransitionType::CROSSFADE
                                         : Gtk::StackTransitionType::NONE);
  // a known out-of-balance state belongs to the session that observed it
  outOfBalance_.Reset();
  ForgetDaemonStatus();
  if (loggedIn) {
    // a sign-in ends what the last sign-out owed the sign-in page
    signInNotice_.Drop();
    ApplyConnectReading(host_.CurrentConnectReading());
    // (re)seed the balance/plan store from the (possibly new) jwt: login and
    // app start land here
    balance_.SetWindowVisible(windowVisible_);
    balance_.Start();
    // A new session invalidates every destination's cache; the one currently
    // on screen reloads now, the rest reload when they are next navigated to.
    if (accountPage_) accountPage_->Load();
    // A login while the window is ALREADY visible fires no presentation change,
    // so panes B/C would otherwise wait for the next DeviceLifecycle event.
    if (connectPage_) connectPage_->Resync();
    if (shell_ && shell_->on_navigate) shell_->on_navigate(shell_->CurrentTag());
    OpenOnboardingIfPending();
  } else {
    // A signed-out window has no session at all: the DEFAULT reading, not a
    // bool poked into a copy of the last one.
    ApplyConnectReading(ConnectReading{});
    // a connect waiting on a balance read belonged to that session, and so
    // does one waiting on the balance itself
    CancelBalanceCheck();
    ClearBalanceRecovery();
    balance_.Stop();
    // a conversion, and the purchase waiting on it, belong to the session
    // that started it
    guestUpgrade_.Clear();
    if (guestConversionSheet_) guestConversionSheet_->set_visible(false);
    // Earnings forgets the departed network's own row, emoji and public
    // switches, then its reload settles every panel on empty
    if (earningsPage_) earningsPage_->ResetForSignOut();
    if (settingsPage_) settingsPage_->Load();
    // Account carries account-SUBJECT state (name, login methods, referral
    // code, the departed plan): a sign-out must wipe it, not merely reload it.
    if (accountPage_) accountPage_->ResetForSignOut();
    if (referralsPage_) referralsPage_->ResetForSignOut();
    // the departed account's sessions and their controller
    if (sessionsPage_) sessionsPage_->ResetForSignOut();
    // Home's activity view starts fresh too: its filters, search and selection
    if (connectPage_) connectPage_->ResetForSignOut();
    // The post-sign-up onboarding belongs to the network just created here: a
    // sign-out before it finished must not show it to the next account signed
    // in (each network starts fresh).
    prefs::Set(kOnboardingPendingKey, false);
    // sign-out lands back on the initial step with a clean login flow
    loginUserAuth_.clear();
    password_.set_text("");
    passwordError_.set_text("");
    loginError_.set_text("");
    // ...saying why, once, when the server confirmed another device signed
    // this session out (OnAuthLogout); the line goes with the next gesture
    if (signInNotice_.Take()) {
      SetLoginNotice(
          T_("sessions_signed_out_remotely", "This session was signed out from another device."));
    }
  }
}

// The tray has no button in front of the user, so it must ask the page — the
// same reading that writes the on-screen label, never a second opinion. With no
// page yet (the login view), connected_ is the only answer there is.
void MainWindow::ToggleConnect() {
  // THE TRAY'S ENTRY POINT, and it must decide from what the TRAY IS SHOWING.
  // The tray's label is set from on_tray_state, i.e. from connected_
  // alone (main.cpp). ConnectPage's button uses a wider predicate — connected
  // OR connecting — so routing the tray through the page's predicate made the
  // two disagree for the whole connecting window: the menu said "Connect"
  // while pressing it disconnected. The page still gets its richer behaviour;
  // it passes its own intent explicitly through ToggleConnect(bool).
  ToggleConnect(connected_);
}

void MainWindow::ToggleConnect(bool disconnect) {
  g_message("connect: toggle pressed (action=%s, connected=%s, hasDevice=%s)",
            disconnect ? "disconnect" : "connect", connected_ ? "yes" : "no",
            host_.hasDevice() ? "yes" : "no");
  if (disconnect) {
    // NOT gated on connected_. That gate is the defect: connected_ is
    // SdkHost::Connected() = sdkConnected && the DeviceRemote still bound to the
    // current control session, while the button says "Disconnect" whenever the
    // page reads connected OR connecting. Whenever those disagreed — the entire
    // connecting phase, and every moment after the daemon dropped the tunnel
    // while the SDK still reported provider sessions — the press fell through to
    // the start path below, and the status line the user got back was
    // "Connecting to providers".
    //
    // Disconnect is safe to run unconditionally: SdkHost::Disconnect asks the
    // view controller to disconnect and then stops the daemon's tunnel, both
    // best-effort, both no-ops when there is nothing to stop.
    // A connect still waiting on a balance read is withdrawn with it, and a
    // connect waiting on the balance is not run by itself after it.
    CancelBalanceCheck();
    ClearBalanceRecovery();
    host_.Disconnect();
    ForgetDaemonStatus();
    // Re-read every window surface once, now. The page is already showing
    // "Disconnecting…" from its own intent; this keeps the tray, the legacy
    // headline and the status strip from holding "Connected" until whatever the
    // SDK sends next — which, on a teardown, may be nothing at all.
    ApplyConnectReading(host_.CurrentConnectReading());
    return;
  }
  // A connect press retires any disconnect the user is no longer waiting on, so
  // the page cannot hold "Disconnecting…" over a connection it has just started.
  // The page's own presses already do this; a TRAY press arrives here without
  // passing through ConnectPage::RelayConnectPress, so it has to be said again.
  if (connectPage_) connectPage_->ClearDisconnectIntent();
  // ALWAYS run the start path and let SdkHost decide whether the existing
  // session can be reused. This used to be gated on !hasDevice(), which meant a
  // stale handle — one bound over a control connection the daemon has since
  // closed, e.g. after a service upgrade — skipped the start entirely and sent
  // the press into a device with nothing behind it. StartTunnel is cheap when
  // the session is genuinely live (one status read) and self-heals when it is
  // not; the caller is not the right place to guess.
  // The press goes where the provider row says: the selected location, or the
  // best available with none. Out of balance, nothing starts
  // (ConnectBlockedByBalance) and this press opens the upgrade path instead.
  // It is immediate, and supersedes a location row click still settling.
  host_.CancelRowConnect("connect press");
  StartTunnelUi("connect press");
  // the connect-reading feed reflects the real state as it changes
}

// Retry, the Failed state's one action, as Windows has it: stop the failed
// session and connect to the same selection again, the manual sequence that
// recovers a window the SDK has given up on. The selection is read before the
// disconnect, which clears the device's.
void MainWindow::RetryConnect() {
  const auto target = host_.SelectedLocation();
  g_message("connect: retry pressed");
  host_.Disconnect();
  ForgetDaemonStatus();
  StartTunnelUi("retry", target);
}

// THE DAEMON'S OWN VERDICT, WRITTEN INTO THE READING. The status poll has just
// learned that urnetworkd is not running our tunnel any more; `tunnelBound` is
// precisely that fact, so it is set here rather than answered with a separate
// boolean beside the reading. (The SDK's next push re-reads it, exactly as the
// old SetConnected(false) was overwritten by the next status push.)
ConnectReading MainWindow::DaemonTunnelGoneReading() {
  // The verdict is recorded IN THE PRODUCER, not stamped onto a copy here.
  // Setting tunnelBound=false on a local copy lasted exactly until the next
  // SDK push (~10/s during a ramp) re-derived it from getters that cannot see
  // the daemon — so the hero flipped back to green over a stopped tunnel. The
  // latch clears on the next start_tunnel and nowhere else.
  host_.NoteDaemonTunnelGone();
  return host_.CurrentConnectReading();
}

// ONE READING IN, EVERY WINDOW SURFACE OUT — and the page gets the SAME value,
// not a boolean summary of it. Nothing below re-derives anything from the SDK:
// one health::Render() call answers the label, the strip and the tray together.
void MainWindow::ApplyConnectReading(const ConnectReading& reading) {
  // The feed fires on every grid step during a connect ramp (~10/s). An
  // unchanged reading must not re-emit the tray's NewIcon/LayoutUpdated DBus
  // pair or rebuild the page's panes underneath the user.
  if (reading == reading_ && readingApplied_) return;
  // The strip's session fields move with the session, not with each grid
  // step. A Disconnect keeps the DeviceRemote, so tunnelBound outlives it.
  const auto sessionUp = [](const ConnectReading& r) {
    return health::SessionUp(r.ToSignals(/*disconnectRequested=*/false));
  };
  const bool sessionChanged = !readingApplied_ || sessionUp(reading) != sessionUp(reading_);
  readingApplied_ = true;
  const bool wasConnected = connected_;
  reading_ = reading;
  const health::Signals signals = reading.ToSignals(/*disconnectRequested=*/false);
  const health::Reading view = health::Render(signals);
  // "There is a session to disconnect from." Exactly what SdkHost::Connected()
  // used to be asked for, now asked once: the tray's label, the tray's action
  // and this window's press logging all read this one bit, so the menu can no
  // longer say "Connect" over a press that disconnects.
  connected_ = view.action != health::Action::Connect;
  // A session this window held has ended: the next disconnected poll reads why.
  if (connected_ != wasConnected) stopExplanationOwed_ = wasConnected;
  if (view.state == health::State::Connected) NoteConnected();
  // The page renders the status strip's state field with its own status row
  // (on_status_rendered).
  if (connectPage_) connectPage_->ApplyConnectReading(reading);
  if (sessionChanged) ApplyStatusStripDetails();
  UpdateBalanceNotice();
  // The tray: its item follows the session, its connected icon means proven
  // (a session still building, held or degraded is not), and its tooltip
  // names the state, all from this one reading. A session the window has not
  // seen a status for keeps its own claim (health::TrayReading).
  const health::Reading tray = health::TrayReading(view, signals, reading.statusObserved);
  const bool proven = health::Proven(tray);
  const std::string status = T_(tray.textKey, tray.textEnglish);
  if (on_tray_state && (connected_ != wasConnected || proven != trayProven_ ||
                        status != trayStatus_ || !trayStatePushed_)) {
    trayStatePushed_ = true;
    trayProven_ = proven;
    trayStatus_ = status;
    on_tray_state(connected_, proven, status);
  }
}

// One fixed notification id, so a post replaces and a withdraw always finds it.
constexpr const char* kBalanceNoticeId = "insufficient-balance";

namespace {
// The application the desktop notifications go through. Never the window's
// get_application(): gtkmm's Gtk::Window removes itself from its application
// when it is hidden (its constructor connects the hide signal to
// Application::remove_window) and nothing adds it back when it shows again,
// so after the first hide to the tray every post and withdraw through it went
// nowhere.
Glib::RefPtr<Gio::Application> NotifyingApp() { return Gio::Application::get_default(); }
}  // namespace

constexpr const char* kHideNoticeId = "hidden-to-tray";

// The default size follows the window's size while it is not maximized, so a
// maximized window keeps the size it returns to.
void MainWindow::SaveGeometry() {
  if (g_getenv("URNETWORK_PREVIEW_UI")) return;  // a review's size is not the user's
  // never shown this run (an autostart quit from the tray): nothing was sized,
  // and a maximize asked for at startup is not yet the window's state
  if (!get_realized()) return;
  int width = 0;
  int height = 0;
  get_default_size(width, height);
  nlohmann::json values = {{window_geometry::kMaximizedKey, is_maximized()}};
  if (window_geometry::Plausible(width, height)) {
    values[window_geometry::kWidthKey] = width;
    values[window_geometry::kHeightKey] = height;
  }
  prefs::SetAll(values);
}

// With no default action, a click on the notice activates the app, which
// shows the window.
void MainWindow::NoteHiddenToTray() {
  if (prefs::Get<bool>(tray_policy::kHideNoticeSeenKey, false)) return;
  auto app = NotifyingApp();
  if (!app) return;
  prefs::Set(tray_policy::kHideNoticeSeenKey, true);  // before the send: once ever
  // the product name, never translated, as the tray's own title
  auto notification = Gio::Notification::create("URnetwork");
  notification->set_body(T_("onb_tray_balloon_hide",
                            "Still running — URnetwork closed to the tray. Click its icon there "
                            "to open it again."));
  app->send_notification(kHideNoticeId, notification);
}

// Out of balance with a connection requested, the tunnel holds traffic with no
// provider behind it. Tell the user once per episode, with a Disconnect button;
// the tracker decides, this only talks to GApplication. It never disconnects.
void MainWindow::UpdateBalanceNotice() {
  struct Sink {
    void Post() {
      auto app = NotifyingApp();
      if (!app) return;
      auto notification =
          Gio::Notification::create(T_("insufficient_balance", "Insufficient balance"));
      notification->set_body(
          T_("insufficient_balance_held_notice",
             "Your traffic is held in the tunnel until you upgrade or disconnect."));
      notification->add_button(T_("disconnect", "Disconnect"),
                               std::string("app.") + kBalanceNoticeDisconnectAction);
      app->send_notification(kBalanceNoticeId, notification);
    }
    void Withdraw() {
      if (auto app = NotifyingApp()) app->withdraw_notification(kBalanceNoticeId);
    }
  };
  balance_notice::Signals signals;
  signals.insufficientBalance = reading_.insufficientBalance;
  signals.pro = balance_.IsPro();
  signals.polling = balance_.IsPolling();
  signals.connectRequested = reading_.destinationSelected;
  if (connectPage_) connectPage_->ApplyBalanceNotice(signals);
  Sink sink;
  balanceNotice_.Observe(signals, sink);

  balance_notice::OutOfBalanceLatch::Observation observation;
  observation.insufficientBalance = reading_.insufficientBalance;
  observation.providersConnected =
      reading_.sdk == health::SdkStatus::Connected &&
      health::SessionUp(reading_.ToSignals(/*disconnectRequested=*/false));
  observation.balanceKnown = balance_.HasFetched();
  observation.availableBytes = balance_.AvailableByteCount();
  outOfBalance_.Observe(observation);

  ObserveBalanceRecovery();
}

void MainWindow::DisconnectFromBalanceNotice() {
  // the user's own Disconnect path, disconnect only
  ToggleConnect(/*disconnect=*/true);
}

namespace {
// the clock the balance store stamps its fetches with
int64_t BalanceClockMillis() { return g_get_monotonic_time() / 1000; }
}  // namespace

bool MainWindow::ConnectBlockedByBalance(std::function<void()> retry) {
  // the balance recovery runs a refused press again: it decided on a fresh
  // balance already, and it is not a new press
  if (retryingRefusedConnect_) return false;
  balance_notice::StartConnectInputs in;
  in.insufficientBalance = reading_.insufficientBalance;
  in.latched = outOfBalance_.OutOfBalance();
  in.pro = balance_.IsPro();
  in.polling = balance_.IsPolling();
  in.sessionUp = health::SessionUp(reading_.ToSignals(/*disconnectRequested=*/false));
  in.balance.known = balance_.HasFetched();
  in.balance.pro = balance_.IsPro();
  in.balance.availableBytes = balance_.AvailableByteCount();
  in.balance.openTransferBytes = balance_.PendingByteCount();
  in.balance.fetchedAtMillis = balance_.FetchedAtMillis();
  in.failedCheckAtMillis = balanceCheckFailedAtMillis_;
  in.nowMillis = BalanceClockMillis();
  switch (balance_notice::DecideStartConnect(in)) {
    case balance_notice::StartConnectStep::Start:
      // this press replaces one still waiting on the balance
      ClearBalanceRecovery();
      return false;
    case balance_notice::StartConnectStep::FetchBalance:
      // nothing starts until the balance is read again (or the read gives up)
      g_message("connect: reading the balance before starting");
      CheckBalanceThen(std::move(retry));
      return true;
    case balance_notice::StartConnectStep::Upgrade:
      break;
  }
  // nothing is started; the press opens the way to add balance instead, in
  // front of the user even when it came from the tray with the window hidden
  g_message("connect: not started, the account is out of balance; opening the upgrade path");
  // the refused press waits on the balance and runs again by itself once it is back
  balanceRecovery_.StartRefused(retry, BalanceClockMillis());
  ApplyBalanceRecoveryLines();
  present();
  // this opening, and only this one, says when the free data refreshes
  nextUpgradeFreeRefresh_ = data_info::UpgradeShowsFreeRefresh(true, balance_.IsPro());
  OpenUpgrade();
  nextUpgradeFreeRefresh_ = false;
  return true;
}

void MainWindow::CheckBalanceThen(std::function<void()> retry) {
  // one press, one attempt: a newer press waiting on the same read replaces it
  pendingConnect_ = std::move(retry);
  if (balanceCheckPending_) return;
  balanceCheckPending_ = true;
  const uint64_t check = ++balanceCheck_;
  auto finish = [this, check](bool ok) {
    if (!balanceCheckPending_ || check != balanceCheck_) return;
    balanceCheckPending_ = false;
    balanceCheckTimeout_.disconnect();
    // a failed read lets the connect go ahead (DecideStartConnect)
    balanceCheckFailedAtMillis_ = ok ? -1 : BalanceClockMillis();
    std::function<void()> connect = std::move(pendingConnect_);
    pendingConnect_ = nullptr;
    if (connect) connect();
  };
  balanceCheckTimeout_ = Glib::signal_timeout().connect(
      [finish] {
        finish(false);
        return false;
      },
      static_cast<unsigned>(balance_notice::kBalanceCheckTimeoutMillis));
  balance_.FetchBalanceThen(finish);
}

void MainWindow::CancelBalanceCheck() {
  balanceCheckPending_ = false;
  ++balanceCheck_;
  balanceCheckTimeout_.disconnect();
  pendingConnect_ = nullptr;
  balanceCheckFailedAtMillis_ = -1;
}

void MainWindow::ClearBalanceRecovery() {
  balanceRecovery_.Clear();
  ApplyBalanceRecoveryLines();
}

balance_notice::Signals MainWindow::BalanceSignals() const {
  balance_notice::Signals signals;
  signals.insufficientBalance = reading_.insufficientBalance;
  signals.pro = balance_.IsPro();
  signals.polling = balance_.IsPolling();
  signals.connectRequested = reading_.destinationSelected;
  return signals;
}

balance_notice::AccountBalance MainWindow::CurrentAccountBalance() const {
  balance_notice::AccountBalance balance;
  balance.known = balance_.HasFetched();
  balance.pro = balance_.IsPro();
  balance.availableBytes = balance_.AvailableByteCount();
  balance.openTransferBytes = balance_.PendingByteCount();
  balance.fetchedAtMillis = balance_.FetchedAtMillis();
  return balance;
}

void MainWindow::ApplyBalanceRecoveryLines() {
  if (!connectPage_) return;
  connectPage_->ApplyBalanceRecovery(
      balance_notice::RecoveryLinesFor(BalanceSignals(),
                                       balance_notice::OutOfBalanceKindFor(CurrentAccountBalance()),
                                       balanceRecovery_.State()),
      balance_.PendingByteCount());
}

void MainWindow::ObserveBalanceRecovery() {
  const balance_notice::Signals signals = BalanceSignals();
  auto step = balanceRecovery_.Observe(balance_notice::Gate(signals), signals.connectRequested,
                                       CurrentAccountBalance(), BalanceClockMillis());
  ApplyBalanceRecoveryLines();
  if (step.kind == balance_notice::RecoveryStepKind::None) return;
  g_message("connect: the balance is back; retrying the connect it blocked");
  ShowToast(stack_, T_("insufficient_balance_reconnecting", "Data is available again. Reconnecting…"));
  // past the gate, and not as a new press (which would end the wait and
  // refill the retries): the recovery decided on a fresh balance already
  retryingRefusedConnect_ = true;
  if (step.kind == balance_notice::RecoveryStepKind::Start) {
    if (step.target) step.target();
  } else if (const auto location = host_.SelectedLocation()) {
    host_.Connect(location);
  } else {
    host_.ConnectBestAvailable();
  }
  retryingRefusedConnect_ = false;
}

void MainWindow::OpenDataInfo() {
  if (!dataInfoSheet_) dataInfoSheet_ = std::make_unique<DataInfoSheet>(*this, balance_);
  dataInfoSheet_->Open();
}

void MainWindow::OpenUpgrade() {
  // No purchase for a legacy guest network: whatever was bought would stay on
  // a network with no login (every upgrade entry point lands here). The gate
  // lifts once a sign-in is added (the server's `guest` turns false), and the
  // conversion then continues here to the upgrade it was opening.
  if (balance_.IsGuest()) {
    DivertGuestToConversion([this] { OpenUpgrade(); });
    return;
  }
  if (!upgradeSheet_) {
    upgradeSheet_ = std::make_unique<UpgradeSheet>(*this, host_, balance_);
    // the server refused the checkout for a guest network the balance had not
    // reported yet: the conversion, then the upgrade once it is done
    upgradeSheet_->on_guest_sign_in_required = [this] {
      DivertGuestToConversion([this] { OpenUpgrade(); });
    };
  }
  upgradeSheet_->Open(nextUpgradeFreeRefresh_);
}

void MainWindow::OpenLocationChooser() {
  if (!locationsSheet_) locationsSheet_ = std::make_unique<LocationsSheet>(*this, host_);
  locationsSheet_->Open();
}

void MainWindow::OpenProviderLocations() {
  if (!providerLocationsSheet_) {
    providerLocationsSheet_ =
        std::make_unique<ProviderLocationsSheet>(*this, host_, locationOverride_.get());
  }
  providerLocationsSheet_->Open();
}

void MainWindow::SyncLocationOverrideTarget() {
  if (!locationOverride_) return;
  auto list = host_.ConnectedProviderLocations();
  if (!list) {
    // tunnel down: never report a city we are not exiting through
    locationOverride_->SetTarget(false, nullptr);
    return;
  }
  const std::vector<ProviderLocationRow> rows = MapConnectedProviderLocations(*list);
  const int index = OldestPlottableIndex(rows);
  if (index < 0) {
    locationOverride_->SetTarget(true, nullptr);
    return;
  }
  const ProviderLocationRow& row = rows[static_cast<size_t>(index)];
  LocationOverrideTarget target;
  target.clientId = row.clientId;
  target.label = PlaceLabel(row);
  target.lat = row.lat;
  target.lon = row.lon;
  locationOverride_->SetTarget(true, &target);
}

void MainWindow::ApplyStats(const LiveStats& stats) {
  auto rate = [](int64_t bps) -> std::string {
    double v = static_cast<double>(bps);
    const char* u = "bps";
    if (v >= 1e9) { v /= 1e9; u = "Gbps"; }
    else if (v >= 1e6) { v /= 1e6; u = "Mbps"; }
    else if (v >= 1e3) { v /= 1e3; u = "Kbps"; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, u);
    return buf;
  };
  if (connectPage_) connectPage_->ApplyStats(stats);
  if (earningsPage_) earningsPage_->ApplyProvideState(stats);  // the provide row + gate
  // the status strip: traffic (+ the Advanced raw field); the provider is the
  // Connect page's row (on_location_rendered)
  if (shell_) {
    if (stats.connected) {
      shell_->SetStatusTraffic("↓ " + rate(stats.downBitsPerSecond) +
                               "  ↑ " + rate(stats.upBitsPerSecond));
    } else {
      shell_->SetStatusTraffic(T_("site_app_no_traffic", "No traffic yet"));
    }
    shell_->SetStatusRaw(stats.connectionStatus.empty() ? Glib::ustring(T_("adv_none", "none"))
                                                        : Glib::ustring(stats.connectionStatus));
  }
}

void MainWindow::ApplyStatusStripDetails() {
  if (!shell_) return;
  // signed out there is no jwt to read, and the read would say so on stderr
  auto byJwt = host_.IsLoggedIn() ? host_.ParseByJwt() : std::nullopt;
  // the jwt's network name is filtered for display (DisplayText.hpp)
  const std::string networkName =
      byJwt ? SanitizeExternalDisplayText(byJwt->NetworkName) : std::string();
  shell_->SetStatusNetwork(balance_.IsGuest() || networkName.empty()
                               ? Glib::ustring(T_("guest", "Guest"))
                               : Glib::ustring(networkName));
  // a session to disconnect from: a DeviceRemote still bound over the current
  // control session outlives a Disconnect, and is not one
  const bool haveSession = health::SessionUp(reading_.ToSignals(/*disconnectRequested=*/false));
  switch (status_strip::SessionWordFor(haveSession)) {
    case status_strip::SessionWord::Tunnel:
      shell_->SetStatusSession(T_("adv_mode_tunnel", "tunnel"));
      break;
    case status_strip::SessionWord::None:
      shell_->SetStatusSession(T_("adv_none", "none"));
      break;
  }
  std::optional<status_strip::RouteFacts> facts;
  if (daemonStatus_) {
    facts = status_strip::RouteFacts{daemonStatus_->routes_installed, daemonStatus_->dns_applied,
                                     daemonStatus_->kill_switch == ctl::KillSwitchState::Armed};
  }
  switch (status_strip::RoutesWordFor(haveSession, facts)) {
    case status_strip::RoutesWord::Unknown:
      shell_->SetStatusRoutes(T_("adv_none", "none"));
      break;
    case status_strip::RoutesWord::Off:
      shell_->SetStatusRoutes(T_("off", "Off"));
      break;
    case status_strip::RoutesWord::KillSwitchArmed:
      shell_->SetStatusRoutes(T_("adv_routes_kill_switch_armed", "off, kill switch armed"));
      break;
    case status_strip::RoutesWord::DnsNotApplied:
      shell_->SetStatusRoutes(T_("adv_routes_dns_degraded", "on, dns not applied"));
      break;
    case status_strip::RoutesWord::On:
      shell_->SetStatusRoutes(T_("on", "On"));
      break;
  }
  const std::string rpc = status_strip::RpcText(haveSession, host_.RpcHostPort());
  shell_->SetStatusRpc(rpc.empty() ? Glib::ustring(T_("adv_none", "none")) : Glib::ustring(rpc));
}

// ---- the Pro celebration ----------------------------------------------------
// One flight at a time; the overlay itself declines to start while a flight
// is in the air or when animations are off, so the callers stay simple.
void MainWindow::LaunchProCelebration() {
  if (proCelebration_) proCelebration_->Launch();
}

}  // namespace urnw
