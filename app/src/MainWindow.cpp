// SPDX-License-Identifier: MPL-2.0
#include "MainWindow.hpp"

#include "SsoBridge.hpp"

#include <adwaita.h>
#include <glib.h>

#include <cstdio>

#include "AppPrefs.hpp"
#include "ReferralRoyalty.hpp"
#include "BrandIcons.hpp"
#include "DaemonUnreachableCopy.hpp"
#include "Formatters.hpp"
#include "UrTheme.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

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
  // The desktop default (windows shell parity): 1120x820dip opens wide of the
  // 1000dip breakpoint so the brand art shows on first launch; min 400x480.
  set_default_size(1120, 820);
  set_size_request(400, 480);

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
    const int step = std::max(1, atoi(preview));
    Glib::signal_timeout().connect_once([this, step] {
      if (!onboarding_) {
        onboarding_ = std::make_unique<OnboardingWindow>(*this, host_, balance_);
      }
      onboarding_->OpenAt(step);
    }, 800);
  }
  // AdwToastOverlay across the page stack: hosts the drawer PQI panel's
  // copied-to-clipboard toasts (the detail sheets carry their own overlays;
  // see Ui.hpp ShowToast).
  GtkWidget* toastOverlay = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toastOverlay), GTK_WIDGET(stack_.gobj()));
  gtk_window_set_child(GTK_WINDOW(gobj()), toastOverlay);

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
    if (developerPage_) developerPage_->SetPresenting(windowVisible_);
    if (windowVisible_) {
      status_.set_text(lastStatus_);
      // RE-READ, never replay: SetPresentationActive(true) above has just
      // reopened the connect controller, so the reading the window was last
      // pushed describes a moment when there was no controller to ask.
      ApplyConnectReading(host_.CurrentConnectReading());
      ApplyStats(lastStats_);
      if (drawer_) drawer_->RefreshAll();  // drawer events are dropped while hidden
    }
  };
  property_visible().signal_changed().connect(reconcilePresentation);
  signal_map().connect(reconcilePresentation);
  signal_unmap().connect(reconcilePresentation);
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
  // fan out to the drawer's plan card, banner, and the upgrade sheet states.
  balance_.SetChangedHandler([this] {
    if (drawer_) drawer_->OnBalanceChanged();
    // Earnings gates its upgrade door and its plan-flavoured copy on the same
    // two bits the drawer's plan card reads.
    if (earningsPage_) earningsPage_->SetBalanceState(balance_.IsPro(), balance_.IsGuest());
    // Account paints its whole plan pane from ONE relayed snapshot — the page
    // never touches the store, which is window-owned and shared with the
    // drawer, the balance warning and the upgrade sheet.
    if (accountPage_) {
      AccountBalance snapshot;
      snapshot.usedByteCount = balance_.UsedByteCount();
      snapshot.pendingByteCount = balance_.PendingByteCount();
      snapshot.availableByteCount = balance_.AvailableByteCount();
      snapshot.startBalanceByteCount = balance_.StartBalanceByteCount();
      snapshot.isPro = balance_.IsPro();
      snapshot.guest = balance_.IsGuest();
      snapshot.loaded = balance_.HasFetched();
      snapshot.confirming = balance_.IsPolling();
      snapshot.timedOut = balance_.PurchaseConfirmationTimedOut();
      accountPage_->ApplyBalance(snapshot);
    }
    // The Refer and earn page paints its card from the same store.
    if (referralsPage_) referralsPage_->OnBalanceChanged();
    // The free -> Pro upgrade side effect (mac MainView reacts to
    // didDetectUpgradeToPro): reset provide mode to never at the upgrade,
    // exactly once — the user can opt back in afterward and that sticks.
    if (balance_.DidDetectUpgradeToPro() && !provideResetOnUpgrade_) {
      provideResetOnUpgrade_ = true;
      host_.ResetProvideToNever();
      SyncProvideControlMode();  // reflect it in the home controls
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
  // The server rejected the stored auth (e.g. the client was removed): log out
  // and return to the login panel. Logout() fires the auth-state handler.
  host_.SetAuthInvalidHandler([this] {
    PostToMain([this] { host_.Logout(); });
  });
  host_.SetJwtRefreshedHandler([this] {
    PostToMain([this] { balance_.OnJwtRefreshed(); });
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
      if (windowVisible_ && drawer_) drawer_->OnHostEvent(event);
      // Panes B/C of Home read the same feed the legacy drawer does. Without
      // this, they only ever refresh on a stats push: block actions, block
      // stats, overrides, contracts, DNS settings, blocker, routeLocal and
      // location changes would never reach the page at all.
      if (windowVisible_ && connectPage_) connectPage_->OnHostEvent(event);
      if (event == DrawerEvent::Peers || event == DrawerEvent::DeviceLifecycle) {
        RefreshPeersStatus();
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
    if (prefs::Get<bool>(prefs::kConnectOnLaunchKey, false)) StartTunnelUi();
    ApplyAuthState(true);
  } else {
    ApplyAuthState(false);
  }

  // Verification hook for the ur.io/sso round trip (with URNETWORK_SSO_SIMULATE,
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
TunnelStartResult MainWindow::StartTunnelUi(bool connectDestination) {
  // Snapshot the reply counter BEFORE the attempt. LastAuthOutcome() describes
  // the last reply this client processed, and StartTunnel() has failure paths
  // that never send anything (not signed in; unusable device-rpc key material).
  // Without this guard a polkit denial from an earlier Connect would be
  // rendered under an unrelated failure — a confidently wrong sentence, which
  // is worse than the generic one. Only a verdict this attempt produced counts.
  const uint64_t replySerialBefore = host_.Control().ReplySerial();
  const TunnelStartResult result = host_.StartTunnel();
  const DaemonAuthOutcome authOutcome = host_.Control().ReplySerial() != replySerialBefore
                                            ? host_.Control().LastAuthOutcome()
                                            : DaemonAuthOutcome::None;
  // ONE text, TWO sinks. These strings used to be written only to
  // daemonStatusLabel_, which lives in the legacy single-column home that the
  // nav shell never shows — so every daemon failure was invisible and pressing
  // Connect looked like a silent no-op. ConnectPage::SetDaemonNotice is the
  // surface the user actually sees; the legacy label is kept in sync until the
  // legacy column is deleted.
  Glib::ustring notice;
  switch (result) {
    case TunnelStartResult::Started:
      break;  // empty notice clears both
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
  daemonStatusLabel_.set_text(notice);
  daemonStatusLabel_.set_visible(!notice.empty());
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
  // SelectedLocation() is respected: a user who has chosen a specific provider
  // must not be silently moved to "best available". Only an empty selection
  // asks the SDK to pick.
  if (result == TunnelStartResult::Started && connectDestination) {
    // Honour an explicit choice. ConnectBestAvailable() always asks the SDK to
    // pick, so using it unconditionally would silently move a user off the
    // provider they selected.
    if (const auto selected = host_.SelectedLocation(); selected.has_value()) {
      g_message("connect: routing to the selected provider");
      host_.Connect(selected);
    } else {
      g_message("connect: no destination selected, choosing the best available");
      host_.ConnectBestAvailable();
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
// ur.io/sso browser bridge (Linux has no native provider flow). There is
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
  if (!connected_) {
    // Nothing claimed, nothing to contradict — but the DNS verdict from the
    // last session must not outlive it on screen.
    if (drawer_) drawer_->SetTunnelDnsState(false, false, {});
    return true;
  }
  const auto status = host_.Control().Status();
  if (!status) return true;      // unreachable is StartTunnelUi's business
  // Feed the drawer the daemon's own DNS verdict. Until this existed, the DNS
  // card was drawn entirely from the SDK's resolver PREFERENCES and could sit
  // green while dns_applied was false — the daemon knew, said so in
  // dns_detail, and nothing on screen read it.
  if (drawer_) {
    drawer_->SetTunnelDnsState(true, status->dns_applied, status->dns_detail);
  }
  if (status->tunnel_state != ctl::TunnelState::Error &&
      status->tunnel_state != ctl::TunnelState::Stopped) {
    return true;
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
    return true;
  }
  // The daemon stopped carrying traffic without us asking. Say so, verbatim —
  // the daemon composes the plain-language reason (including whether the machine
  // is now blocked and how to lift it), and inventing our own wording here would
  // be a third place that can disagree about what happened.
  const Glib::ustring detail =
      status->error.empty()
          ? Glib::ustring(T_("tunnel_stopped_unexpectedly", "The connection stopped."))
          : Glib::ustring(status->error);
  g_warning("connect: the daemon stopped the session (%s): %s",
            status->error_code.empty() ? "no code" : status->error_code.c_str(),
            status->error.c_str());
  ApplyConnectReading(DaemonTunnelGoneReading());
  if (connectPage_) connectPage_->SetDaemonNotice(detail);
  daemonStatusLabel_.set_text(detail);
  daemonStatusLabel_.set_visible(true);
  return true;
}

void MainWindow::ApplyPageBreakpoint(int widthDip) {
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
void MainWindow::UpdateCarouselRunning() {
  if (!carousel_) return;
  carousel_->SetActive(windowVisible_ && stack_.get_visible_child_name() == "login");
}

// The signed-out Hero Bloom (motion-overhaul spec §2.1): the hero springs
// 0.92 -> 1 under a 500ms fade while the rings unfold around it on the 40ms
// stagger grid. Delays and directions are the spec's signed-out table.
void MainWindow::RunSignedOutReveal() {
  using namespace motion;
  if (!ShouldAnimate()) return;
  if (!heroBin_) return;
  SettleReveal();  // a reveal still running from a previous show settles first
  ArmHeroBloom(*heroBin_);
  StartHeroBloom(*heroBin_);
  // the brand beat: the wordmark joins mid-hero-settle — the signed-out table's
  // AppTitleBar row (+8 -> rises up, delay 120)
  if (brandBin_) RiseIn(*brandBin_, Rise::Up, kDist8, kBrandBeatMs);
  RiseIn(*walletBin_, Rise::Down, kDist12, 240);
  RiseIn(*secondaryBin_, Rise::Down, kDist12, 280);
  RiseIn(*orBin_, Rise::Down, kDist8, 300);
  RiseIn(*emailGroupBin_, Rise::Down, kDist8, 320);
  RiseIn(*getStartedBin_, Rise::Down, kDist8, 360);
}

// CancelToFinal: every pose the reveal ever writes is either animated back to
// settled or restored right here — never left stranded (the settle invariant).
void MainWindow::SettleReveal() {
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
        Format(T_("seedphrase_word_count_warning",
                  "That's {} words — a seedphrase is 12 or 24 words"),
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
      StartTunnelUi();  // auth handler flips the view
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

  // optional referral code (android/apple instant-account parity): the server
  // links the referral on the guest/seedphrase create path too
  instantReferralToggle_ =
      Gtk::make_managed<Gtk::Button>(T_("add_referral_code", "Add referral code"));
  instantReferralToggle_->add_css_class("flat");
  instantReferralToggle_->set_halign(Gtk::Align::START);
  instantReferralToggle_->signal_clicked().connect([this] {
    instantReferralRevealer_->set_reveal_child(!instantReferralRevealer_->get_reveal_child());
  });
  scaffold.card->append(*instantReferralToggle_);

  instantReferralRevealer_ = Gtk::make_managed<Gtk::Revealer>();
  auto* instantReferralBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  auto* instantReferralRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  instantReferralEntry_ = Gtk::make_managed<Gtk::Entry>();
  instantReferralEntry_->set_placeholder_text(
      T_("enter_a_bonus_referral_code", "Enter a bonus referral code"));
  instantReferralEntry_->set_hexpand(true);
  instantReferralEntry_->signal_changed().connect([this] {
    // retyping invalidates the previous validation
    instantReferralValid_ = false;
    instantReferralSupporting_->set_text("");
    instantReferralApplied_->set_visible(false);
    instantReferralApply_->set_sensitive(
        !TrimWhitespace(instantReferralEntry_->get_text()).empty());
  });
  instantReferralRow->append(*instantReferralEntry_);
  instantReferralApply_ = Gtk::make_managed<Gtk::Button>(T_("apply_bonus", "Apply bonus"));
  instantReferralApply_->set_sensitive(false);
  instantReferralApply_->signal_clicked().connect(
      sigc::mem_fun(*this, &MainWindow::OnInstantValidateReferral));
  instantReferralRow->append(*instantReferralApply_);
  instantReferralBox->append(*instantReferralRow);
  instantReferralSupporting_ = Gtk::make_managed<Gtk::Label>();
  instantReferralSupporting_->add_css_class("ur-error-text");
  instantReferralSupporting_->set_xalign(0);
  instantReferralSupporting_->set_wrap(true);
  instantReferralBox->append(*instantReferralSupporting_);
  instantReferralRevealer_->set_child(*instantReferralBox);
  scaffold.card->append(*instantReferralRevealer_);

  // referral accepted: the gold king-frog line
  instantReferralApplied_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  {
    auto* appliedLabel = Gtk::make_managed<Gtk::Label>();
    appliedLabel->set_markup("<span foreground='" + HexForMarkup(kReferralGoldLight) + "'>" +
                             Glib::Markup::escape_text(
                                 T_("referral_bonus_applied_2", "Referral Bonus applied")) +
                             "</span>");
    appliedLabel->add_css_class("caption");
    instantReferralApplied_->append(*appliedLabel);
  }
  instantReferralApplied_->set_visible(false);
  scaffold.card->append(*instantReferralApplied_);

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

void MainWindow::OnInstantValidateReferral() {
  if (validatingInstantReferral_) return;
  const std::string code = TrimWhitespace(instantReferralEntry_->get_text());
  if (code.empty()) return;
  validatingInstantReferral_ = true;
  instantReferralApply_->set_sensitive(false);
  instantReferralSupporting_->set_text("");

  host_.ValidateReferralCode(code, [this](bool ok, bool valid, bool capped) {
    PostToMain([this, ok, valid, capped] {
      validatingInstantReferral_ = false;
      instantReferralApply_->set_sensitive(
          !TrimWhitespace(instantReferralEntry_->get_text()).empty());
      instantReferralValid_ = ok && valid && !capped;
      if (instantReferralValid_) {
        // the royal welcome: the gold king-frog moment for the referred
        instantReferralRevealer_->set_reveal_child(false);
        instantReferralApplied_->set_visible(true);
        instantReferralToggle_->set_label(T_("edit_referral_code", "Edit referral code"));
        ShowRoyalWelcomeSheet(*this);
      } else if (ok && capped) {
        instantReferralSupporting_->set_text(
            T_("referral_code_capped", "This code has been used up"));
      } else if (ok) {
        instantReferralSupporting_->set_text(
            T_("invalid_referral_code", "This code is not valid"));
      } else {
        instantReferralSupporting_->set_text(
            T_("something_went_wrong", "Something went wrong."));
      }
    });
  });
}

void MainWindow::OnInstantSubmit() {
  if (creatingInstant_ || !instantTerms_ || !instantTerms_->get_active()) return;
  creatingInstant_ = true;
  instantCreate_->set_sensitive(false);
  instantTerms_->set_sensitive(false);
  instantError_->set_text("");

  const std::string referralCode =
      instantReferralValid_ ? TrimWhitespace(instantReferralEntry_->get_text())
                            : std::string();
  host_.CreateInstantAccount(referralCode, [this](SdkHost::InstantAccount account) {
    PostToMain([this, account = std::move(account)]() mutable {
      creatingInstant_ = false;
      instantTerms_->set_sensitive(true);
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
            StartTunnelUi();  // auth handler flips the view
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

void MainWindow::RefreshPeersStatus() {
  // ALL connected devices (online, provide or not); the chooser's peers
  // section stays provide-filtered (connectable only)
  const int peerCount = static_cast<int>(host_.ConnectedPeerCount());
  const Rgba dotColor = 0 < peerCount ? kUrGreen : kUrAmber;
  peersStatusDot_.set_markup("<span foreground='" + HexForMarkup(dotColor) + "'>●</span>");
  peersStatusText_.set_text(
      Format(TN_("network_peer_count", "You have {} other device online",
                 "You have {} other devices online", peerCount),
             peerCount));
}

void MainWindow::BuildHome() {
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  box->set_margin(24);
  box->set_valign(Gtk::Align::START);

  status_.add_css_class("title-2");
  box->append(status_);

  // daemon session problems, right under the status so the reason reads with
  // the state. Gray (the app's unavailable treatment), wrapped, never a blank
  // — hidden entirely while the session is healthy.
  daemonStatusLabel_.add_css_class("dim-label");
  daemonStatusLabel_.set_wrap(true);
  daemonStatusLabel_.set_justify(Gtk::Justification::CENTER);
  daemonStatusLabel_.set_visible(false);
  box->append(daemonStatusLabel_);

  connectBtn_.add_css_class("suggested-action");
  connectBtn_.add_css_class("pill");
  // The legacy column's own button. It is labelled from connected_ (ApplyConnectReading
  // below), not from the page's reading, so it has no action to carry and takes
  // the ask-the-page path — and `sigc::mem_fun` can no longer name an overloaded
  // member anyway.
  connectBtn_.signal_clicked().connect([this] { ToggleConnect(); });
  box->append(connectBtn_);

  // network peers status line, right under the connect button: a dot (green when
  // peers are online, red at zero) + "{n} peers", always shown. Tapping opens the
  // location chooser (owned by the drawer).
  auto* peersRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  peersRow->set_halign(Gtk::Align::CENTER);
  peersStatusDot_.set_valign(Gtk::Align::CENTER);
  peersRow->append(peersStatusDot_);
  peersStatusText_.add_css_class("dim-label");
  peersRow->append(peersStatusText_);
  peersRow->add_css_class("ur-card-tappable");
  SetPointerCursor(*peersRow);
  WireCardPressFeedback(*peersRow);
  auto peersGesture = Gtk::GestureClick::create();
  peersGesture->signal_released().connect(
      [this](int, double, double) { if (drawer_) drawer_->OpenLocationChooser(); });
  peersRow->add_controller(peersGesture);
  box->append(*peersRow);
  // discoverability (apple/android parity): whether this device is itself
  // connectable as a same-network peer
  discoverableLabel_.add_css_class("dim-label");
  box->append(discoverableLabel_);
  RefreshPeersStatus();

  // live stats (macOS parity): provider window size, throughput, provide.
  // The provider count doubles as the entry point into the provider-locations
  // sheet; the gesture is installed once and gated on the live connected state
  // in ApplyStats, so it is inert while disconnected, reconnecting, or showing
  // the insufficient-balance copy.
  providerCountLabel_.add_css_class("dim-label");
  {
    auto gesture = Gtk::GestureClick::create();
    gesture->signal_released().connect([this](int, double, double) {
      if (providerCountClickable_) OpenProviderLocations();
    });
    providerCountLabel_.add_controller(gesture);
  }
  throughputLabel_.add_css_class("dim-label");
  provideStatsLabel_.add_css_class("dim-label");
  box->append(providerCountLabel_);
  box->append(throughputLabel_);
  box->append(provideStatsLabel_);

  // provide control mode picker (apple/android parity): Auto | Always |
  // Network | Never. "Network" is the private provider — the provider is
  // always on, but provides only to same-network peers, never publicly.
  auto* provideRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  provideRow->set_halign(Gtk::Align::CENTER);
  provideModeDot_.set_valign(Gtk::Align::CENTER);
  provideRow->append(provideModeDot_);
  provideRow->append(*Gtk::make_managed<Gtk::Label>(T_("provide", "Provide")));
  auto* provideSegmented = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  provideSegmented->add_css_class("linked");
  provideAuto_ = Gtk::make_managed<Gtk::ToggleButton>(T_("auto", "Auto"));
  provideAlways_ = Gtk::make_managed<Gtk::ToggleButton>(T_("always", "Always"));
  provideNetwork_ = Gtk::make_managed<Gtk::ToggleButton>(T_("network", "Network"));
  provideNever_ = Gtk::make_managed<Gtk::ToggleButton>(T_("never", "Never"));
  provideAlways_->set_group(*provideAuto_);
  provideNetwork_->set_group(*provideAuto_);
  provideNever_->set_group(*provideAuto_);
  // default matches the stored default ("never" — providing is opt-in); the
  // real state syncs in ApplyAuthState. Set before the signal connections
  // below so construction doesn't fire an apply.
  provideNever_->set_active(true);
  for (Gtk::ToggleButton* button :
       {provideAuto_, provideAlways_, provideNetwork_, provideNever_}) {
    provideSegmented->append(*button);
    // toggled fires for the deactivated button too; apply once on the activation
    button->signal_toggled().connect([this, button] {
      if (button->get_active()) ApplyProvideControlMode();
    });
  }
  provideRow->append(*provideSegmented);
  box->append(*provideRow);

  // connect drawer (macOS ConnectActions parity): connection controls, the
  // three stats cards, the block-ads-and-trackers toggle, and the plan +
  // usage card (with the upgrade + redeem flows behind it)
  drawer_ = Gtk::make_managed<ConnectDrawer>(host_, *this, balance_);
  drawer_->on_create_account = [this] {
    // guest -> full account: the create page in upgrade-guest mode
    NavigateCreate(CreateNetworkPage::Mode::UpgradeGuest, "", /*fromHome=*/true);
  };
  box->append(*drawer_);

  box->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  auto* signOut = Gtk::make_managed<Gtk::Button>(T_("sign_out", "Sign out"));
  signOut->add_css_class("destructive-action");
  signOut->signal_clicked().connect([this] { host_.Logout(); });
  box->append(*signOut);

  auto* protocolLink = Gtk::make_managed<Gtk::LinkButton>(
      "https://ur.xyz", T_("uses_ur_protocol", "Uses the UR Protocol"));
  protocolLink->add_css_class("dim-label");
  box->append(*protocolLink);

  // AdwClamp caps the column at the drawer's max width (600, macOS parity)
  // while still shrinking with a narrow window; the scroller makes the taller
  // home view usable at any height.
  GtkWidget* clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 600);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 600);
  adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(box->gobj()));

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_child(*Glib::wrap(clamp));

  // ---- the signed-in nav shell (windows NavigationView home) ---------------
  // The old single-column home rides inside it as the Connect page until the
  // Windows-parity ConnectPage lands; the other destinations register as they
  // are built. The shell owns nav/status-strip/mode-notice chrome.
  shell_ = Gtk::make_managed<HomeShell>();
  // The Windows-parity three-pane Home (docs/parity/connect-page.md). The
  // legacy single-column drawer stays in the tree under "connect-legacy"
  // until every drawer surface has been relocated into panes B/C.
  connectPage_ = Gtk::make_managed<ConnectPage>(host_);
  // on_connect_action, NOT on_toggle_connect: the press carries the action that
  // wrote the label the user actually clicked. Binding the void toggle here is
  // what left the window re-deriving the action from a stricter reading, and a
  // button reading "Disconnect" starting a tunnel.
  connectPage_->on_connect_action = [this](bool disconnect) { ToggleConnect(disconnect); };
  connectPage_->on_open_locations = [this] {
    if (drawer_) drawer_->OpenLocationChooser();
  };
  // "Connected to N providers" -> the provider sheet. MainWindow owns it
  // because the GeoClue location override must keep following the window
  // while the sheet is closed.
  connectPage_->on_open_provider_locations = [this] { OpenProviderLocations(); };
  shell_->SetPage("connect", *connectPage_);
  shell_->SetPage("connect-legacy", *scroller);
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
  shell_->SetPage("earnings", *earningsPage_);
  accountPage_ = Gtk::make_managed<AccountPage>(host_);
  accountPage_->on_snackbar = [this](const Glib::ustring& message, bool error) {
    if (shell_) {
      shell_->snackbar().Show(message, error ? kit::Snackbar::Severity::Error
                                             : kit::Snackbar::Severity::Success);
    }
  };
  // Same guest fork as Earnings: a guest has no account to hang a plan on.
  accountPage_->on_open_upgrade = [this] {
    if (balance_.IsGuest()) {
      NavigateCreate(CreateNetworkPage::Mode::UpgradeGuest, "", /*fromHome=*/true);
    } else if (drawer_) {
      drawer_->OpenUpgrade();
    }
  };
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
    StartTunnelUi();  // auth handler flips the view
  };
  createPage_->on_verify = [this](std::string userAuth) { NavigateVerify(userAuth); };
  createPage_->on_back = [this] {
    stack_.set_visible_child(createPageFromHome_ ? "home" : "login");
  };
  stack_.add(*wrapInScroller(*createPage_), "create");

  verifyPage_ = Gtk::make_managed<VerifyPage>(host_);
  verifyPage_->on_success = [this] {
    prefs::Set(kOnboardingPendingKey, true);  // a verified sign-up is a new network
    StartTunnelUi();  // auth handler flips the view
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
          StartTunnelUi();  // auth handler flips the view
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
        // the login sent a fresh numeric code; route into the verify page —
        // and SAY a code was sent (windows raises the same informational)
        NavigateVerify(loginUserAuth_);
        if (verifyPage_) {
          verifyPage_->ShowNotice(
              T_("verification_code_sent", "Check your email/phone for a verification code."));
        }
      } else if (!r.ok) {
        passwordError_.set_text(r.error.empty() ? T_("sign_in_failed", "Sign in failed")
                                                : r.error);
      } else {
        StartTunnelUi();  // auth handler flips the view
      }
    });
  });
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
              self->StartTunnelUi();
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
  host_.SignInWithSolana(provider, [this](AuthResult r) { OnWalletAuth(r); });
}

void MainWindow::OnGoogle() { OnSso(sso::kProviderGoogle); }

void MainWindow::OnApple() { OnSso(sso::kProviderApple); }

// Google / Apple: the browser carries the provider's sign-in (the ur.io/sso
// bridge); the identity token returns on urnetwork://sso and lands in
// OnWalletAuth like the wallet flows.
void MainWindow::OnSso(const std::string& provider) {
  loginError_.set_text("");
  SetLoginBusy(true);
  host_.SignInWithSso(provider, [this](AuthResult r) { OnWalletAuth(r); });
}

void MainWindow::OnBittensor() {
  SetLoginNotice(T_("opening_bittensor_wallet_in_browser",
                    "Opening your Bittensor wallet in the browser…"));
  SetLoginBusy(true);
  host_.SignInWithBittensor([this](AuthResult r) { OnWalletAuth(r); });
}

// Shared tail of both wallet sign-ins (the SDK callback thread lands here).
void MainWindow::OnWalletAuth(const AuthResult& result) {
  PostToMain([this, result] {
    SetLoginBusy(false);
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
        SetLoginError(result.error.c_str());
      } else if (result.sso) {
        SetLoginError(T_("there_was_an_error_logging_in", "There was an error logging in"));
      } else {
        SetLoginError(T_("wallet_sign_in_failed", "Wallet sign-in failed"));
      }
    } else {
      loginError_.set_text("");
      StartTunnelUi();  // auth handler flips the view
    }
  });
}

// picker -> host: apply the active segment's mode. The sync guard keeps the
// programmatic set_active in SyncProvideControlMode from writing back.
void MainWindow::ApplyProvideControlMode() {
  if (syncingProvideMode_) return;
  std::string mode = "never";
  if (provideAuto_ && provideAuto_->get_active()) {
    mode = "auto";
  } else if (provideAlways_ && provideAlways_->get_active()) {
    mode = "always";
  } else if (provideNetwork_ && provideNetwork_->get_active()) {
    mode = "network";
  }
  host_.SetProvideControlMode(mode);
}

// host -> picker: reflect the device/persisted mode ("manual" and any unknown
// value land on Never, matching the SDK's conservative default case).
void MainWindow::SyncProvideControlMode() {
  const std::string mode = host_.GetProvideControlMode();
  syncingProvideMode_ = true;
  if (mode == "auto" && provideAuto_) {
    provideAuto_->set_active(true);
  } else if (mode == "always" && provideAlways_) {
    provideAlways_->set_active(true);
  } else if (mode == "network" && provideNetwork_) {
    provideNetwork_->set_active(true);
  } else if (provideNever_) {
    provideNever_->set_active(true);
  }
  syncingProvideMode_ = false;
}

// The post-sign-up onboarding: shown once, only after a network was created
// on this machine (never for an existing account signing in).
void MainWindow::OpenOnboardingIfPending() {
  if (!prefs::Get<bool>(kOnboardingPendingKey, false)) return;
  Glib::signal_timeout().connect_once([this] {
    if (!prefs::Get<bool>(kOnboardingPendingKey, false)) return;
    if (!onboarding_) {
      onboarding_ = std::make_unique<OnboardingWindow>(*this, host_, balance_);
      onboarding_->on_finished = [] { prefs::Set(kOnboardingPendingKey, false); };
    }
    onboarding_->Open();
  }, 600);
}

void MainWindow::ApplyAuthState(bool loggedIn) {
  stack_.set_visible_child(loggedIn ? "home" : "login");
  // a fresh session (either way) re-arms the once-only Pro-upgrade provide
  // reset; the balance store's detection flag resets in Start()/Stop() below
  provideResetOnUpgrade_ = false;
  if (loggedIn) {
    SyncProvideControlMode();
    ApplyConnectReading(host_.CurrentConnectReading());
    // (re)seed the balance/plan store from the (possibly new) jwt: login,
    // guest upgrade, and app start all land here
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
    balance_.Stop();
    if (earningsPage_) earningsPage_->Load();  // settles every panel on empty
    if (settingsPage_) settingsPage_->Load();
    // Account carries account-SUBJECT state (name, login methods, referral
    // code, the departed plan): a sign-out must wipe it, not merely reload it.
    if (accountPage_) accountPage_->ResetForSignOut();
    if (referralsPage_) referralsPage_->ResetForSignOut();
    // sign-out lands back on the initial step with a clean login flow
    loginUserAuth_.clear();
    password_.set_text("");
    passwordError_.set_text("");
    loginError_.set_text("");
  }
}

// The tray has no button in front of the user, so it must ask the page — the
// same reading that writes the on-screen label, never a second opinion. With no
// page yet (the login view), connected_ is the only answer there is.
void MainWindow::ToggleConnect() {
  // THE TRAY'S ENTRY POINT, and it must decide from what the TRAY IS SHOWING.
  // The tray's label is set from on_connected_change, i.e. from connected_
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
    host_.Disconnect();
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
  // false: this path issues its own connect immediately below, deliberately
  // unconditional so a Connect press also self-heals a stale session.
  if (StartTunnelUi(/*connectDestination=*/false) != TunnelStartResult::Started) return;
  host_.ConnectBestAvailable();
  // the connect-reading feed reflects the real state as it changes
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
  readingApplied_ = true;
  const bool wasConnected = connected_;
  reading_ = reading;
  const health::Signals signals = reading.ToSignals(/*disconnectRequested=*/false);
  const health::Reading view = health::Render(signals);
  // "There is a session to disconnect from." Exactly what SdkHost::Connected()
  // used to be asked for, now asked once: the tray's label, the tray's action
  // and this window's press logging all read this one bit, so the menu can no
  // longer say "Connect" over a press that disconnects.
  connected_ = view.action == health::Action::Disconnect;
  connectBtn_.set_label(connected_ ? T_("disconnect", "Disconnect") : T_("connect", "Connect"));
  // The strip's raw status field carries the controller's OWN token now
  // (CONNECTING/CONNECTED/CONNECT_FAILED), not the two-word destination
  // vocabulary the old push could produce.
  lastStatus_ = reading.rawStatus.empty() ? std::string("DISCONNECTED") : reading.rawStatus;
  // the status strip's state field: dot color per state (§8.1 connect dots).
  // Green only for the state the hero calls Connected — the strip used to go
  // green the moment a destination was picked.
  if (shell_) {
    shell_->SetStatusState(
        lastStatus_, view.state == health::State::Connected ? "#87FB67" : "#2A60FF");
  }
  if (windowVisible_) status_.set_text(lastStatus_);
  if (connectPage_) connectPage_->ApplyConnectReading(reading);
  if (on_connected_change && (connected_ != wasConnected || !trayConnectedPushed_)) {
    trayConnectedPushed_ = true;
    on_connected_change(connected_);
  }
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
  // the drawer surfaces the insufficient-balance banner (upgrade flow CTA)
  if (drawer_) drawer_->SetInsufficientBalance(stats.insufficientBalance);
  if (connectPage_) connectPage_->ApplyStats(stats);
  // the status strip: provider + traffic (+ the Advanced raw field)
  if (shell_) {
    shell_->SetStatusProvider(T_("best_available_provider", "Best available provider"));
    if (stats.connected) {
      shell_->SetStatusTraffic("↓ " + rate(stats.downBitsPerSecond) +
                               "  ↑ " + rate(stats.upBitsPerSecond));
    } else {
      shell_->SetStatusTraffic(T_("site_app_no_traffic", "No traffic yet"));
    }
    shell_->SetStatusRaw(stats.connectionStatus);
    shell_->SetStatusSession(host_.hasDevice() ? "tunnel" : "none");
  }
  // the provider-locations sheet opens only from a genuine connection: not
  // while reconnecting, and not behind the insufficient-balance copy, where the
  // label is not a provider count at all
  providerCountClickable_ = stats.connected && !stats.insufficientBalance;
  providerCountLabel_.set_cursor(providerCountClickable_ ? Gdk::Cursor::create("pointer")
                                                         : Glib::RefPtr<Gdk::Cursor>());
  if (stats.insufficientBalance) {
    providerCountLabel_.set_text(T_("insufficient_balance_add_balance_or_plan",
                                    "Insufficient balance — add balance or a plan"));
    throughputLabel_.set_text("");
  } else if (stats.connected) {
    // the catalog carries the plural forms; never inflect here
    providerCountLabel_.set_text(
        Format(TN_("connected_provider_count", "Connected to {} provider",
                   "Connected to {} providers", static_cast<unsigned long>(stats.providerCount)),
               stats.providerCount));
    // arrows + rates: no translatable text
    throughputLabel_.set_text("↓ " + rate(stats.downBitsPerSecond) + "   ↑ " +
                              rate(stats.upBitsPerSecond));
  } else {
    providerCountLabel_.set_text("");
    throughputLabel_.set_text("");
  }
  std::string provide;
  if (stats.provideEnabled) {
    provide = stats.providePaused
                  ? std::string(T_("providing_paused", "Providing (paused)"))
                  : Format(TN_("providing_client_count", "Providing to {} client",
                               "Providing to {} clients",
                               static_cast<unsigned long>(stats.provideClients)),
                           stats.provideClients);
  }
  provideStatsLabel_.set_text(provide);

  // provide indicator (apple parity). The effective provide mode is a bit
  // set (0 none, 1 network, 2 friends-and-family, 3 public) — per-case only.
  // "●" = solid dot (Network tier), "◉" = dot with outer ring (Public tier).
  const char* provideGlyph = "●";
  Rgba provideColor = kUrCoral;
  switch (stats.provideMode) {
    case 3:  // public
      provideGlyph = "◉";
      provideColor = stats.providePaused ? kUrAmber : kUrGreen;
      break;
    case 1:  // network (also Auto while idle)
    case 2:  // friends-and-family
      provideColor = kUrGreen;
      break;
    default:
      break;
  }
  provideModeDot_.set_markup("<span foreground='" + HexForMarkup(provideColor) + "'>" +
                             provideGlyph + "</span>");

  // discoverability line (apple/android parity): a paused device stays
  // discoverable — pause stops public provide only
  discoverableLabel_.set_text(
      stats.provideEnabled && stats.provideHasNetworkKey
          ? T_("device_discoverable", "This device is discoverable")
          : T_("device_not_discoverable", "Enable provide mode to make this device discoverable"));
}

}  // namespace urnw
