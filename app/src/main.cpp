// SPDX-License-Identifier: MPL-2.0
//
// Single-process entry point. Owns the SdkHost (the in-process VPN core), the
// GTK4 window, and the D-Bus tray. Closing the window hides to the tray and the
// tunnel keeps running (the Windows/macOS "keep connected" behavior); with no
// tray it minimizes instead (TrayPolicy.hpp). Quit from the tray is the only
// real exit. One instance runs per session: a later launch hands itself to it
// and exits, and one that meets it quitting waits for it to end, then starts
// (InstanceHandover.hpp).
#include <adwaita.h>
#include <glib.h>
#include <glib-unix.h>
#include <giomm/file.h>
#include <glibmm/miscutils.h>
#include <gtkmm/application.h>

#include <vector>

#include <clocale>
#include <csignal>
#include <functional>
#include <memory>
#include <cstdlib>
#include <string>

#include "I18n.hpp"
#include "InstanceHandover.hpp"
#include "LaunchAtStartup.hpp"
#include "MainWindow.hpp"
#include "QuitSequence.hpp"
#include "RuntimePaths.hpp"
#include "SdkHost.hpp"
#include "SingleInstance.hpp"
#include "StartupFailure.hpp"
#include "Tray.hpp"
#include "TrayPolicy.hpp"
#include "UrTheme.hpp"

// Where the message catalogs are installed: meson passes the configured
// localedir (see meson.build). The fallback is the FHS default, so the file
// still builds standalone.
#ifndef UR_LOCALEDIR
#define UR_LOCALEDIR "/usr/share/locale"
#endif

namespace {

std::string EnsureDir(const std::string& base, const char* leaf) {
  std::string dir = base + "/" + leaf;
  g_mkdir_with_parents(dir.c_str(), 0700);
  return dir;
}

// The launcher's autostart flag, read once and unset so nothing this process
// starts inherits it (InstanceHandover.hpp).
bool TakeAutostartFlag() {
  const bool autostart =
      urnw::instance::IsAutostartFlag(g_getenv(urnw::instance::kAutostartEnvironment));
  g_unsetenv(urnw::instance::kAutostartEnvironment);
  return autostart;
}

}  // namespace

int main(int argc, char** argv) {
  // gettext (GNOME convention): pick up the user's locale, then bind the
  // "urnetwork" domain to the installed catalogs
  // (<localedir>/<locale>/LC_MESSAGES/urnetwork.mo, built from po/*.po, which
  // the localization store generates). GTK sets the locale too, but the SDK
  // host below can already produce user-visible text, so do it first.
  std::setlocale(LC_ALL, "");
  // The catalog dir must be resolved AT RUNTIME (APPIMAGE.md §3a): inside a
  // relocated AppImage the compile-time UR_LOCALEDIR does not exist, so
  // binding it directly would ship the .mo files as dead weight and hand
  // every AppImage user English regardless of locale — silently. Same ladder
  // as every other installed path (RuntimePaths.hpp); on a miss, bind the
  // compile-time dir anyway, which is exactly the old behaviour.
  const std::string localeDir = urnw::ResolveRuntimePath(UR_LOCALEDIR, G_FILE_TEST_IS_DIR);
  bindtextdomain(GETTEXT_PACKAGE, localeDir.empty() ? UR_LOCALEDIR : localeDir.c_str());
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");  // the catalogs are UTF-8
  textdomain(GETTEXT_PACKAGE);

  // The licensed brand faces must be registered BEFORE any GTK/adwaita init:
  // Pango's fc font map snapshots fontconfig when it is first created, and
  // adw_init (below, in startup) is enough to create one — fonts added after
  // that resolve for some lookup paths and silently miss for others.
  urnw::LoadBrandFonts();

  // Must match the .desktop StartupWMClass + common-id so the shell associates
  // the window with the app (and the hide-to-tray window keeps its identity).
  // HANDLES_OPEN: the single instance receives urnetwork:// deep links (wallet
  // callbacks) via signal_open — the .desktop registers x-scheme-handler/urnetwork.
  auto app = Gtk::Application::create(urnw::instance::kBusName,
                                      Gio::Application::Flags::HANDLES_OPEN);
  // The autostart entry's option, which GApplication would otherwise refuse;
  // hidden from --help, as nobody types it.
  app->add_main_option_entry(Gio::Application::OptionType::BOOL, "autostart", '\0', "", "",
                             Glib::OptionEntry::Flags::HIDDEN);

  // Which launch this is, and where it goes. A launch that finds the instance
  // running hands itself over and exits here, and one that finds it exiting
  // waits for it to end and then starts; either way before anything touches
  // the storage or the log directory (InstanceHandover.hpp).
  const urnw::instance::Arguments arguments = urnw::instance::ParseArguments(
      argc > 1 ? std::vector<std::string>(argv + 1, argv + argc) : std::vector<std::string>(),
      TakeAutostartFlag());
  if (arguments.handover) {
    const urnw::instance::Launch launch =
        urnw::instance::LaunchFor(G_APPLICATION(app->gobj()), arguments);
    const urnw::instance::Outcome outcome = urnw::instance::LaunchOnSessionBus(launch);
    switch (outcome) {
      case urnw::instance::Outcome::Start:
      case urnw::instance::Outcome::Fallback:
        break;
      case urnw::instance::Outcome::HandedOver:
        return 0;
      case urnw::instance::Outcome::StillClosing:
      case urnw::instance::Outcome::GaveUp:
        g_warning("launch: not started, %s; start URnetwork again in a moment",
                  urnw::instance::ToString(outcome));
        return 1;
    }
  }

  auto host = std::make_shared<urnw::SdkHost>();
  const std::string storageDir = EnsureDir(Glib::get_user_data_dir(), "urnetwork");
  // glibmm on core24 (the 2.68 ABI series) has no Glib::get_user_state_dir wrapper;
  // call the C g_get_user_state_dir() (glib 2.72+) directly for XDG_STATE_HOME.
  const std::string logDir = EnsureDir(g_get_user_state_dir(), "urnetwork");

  std::shared_ptr<urnw::MainWindow> window;
  std::shared_ptr<urnw::Tray> tray;
  // How the app ends (QuitSequence.hpp): the queued connect commands run
  // first, for at most kDrainMillis, then a Quit's teardown runs off the main
  // loop, and the app ends when it is done or at kAnswerMillis, whichever is
  // first. Nothing on the way blocks the main loop.
  urnw::QuitSequence quit;
  std::function<void(urnw::QuitSequence::Step)> applyQuitStep =
      [&](urnw::QuitSequence::Step step) {
        if (step.startTeardown) {
          // teardown WITHOUT Logout(): Logout wipes the stored jwt, and for a
          // guest network that jwt is the only credential — quitting from the
          // tray was permanently destroying guest accounts (and any balance or
          // subscription they had paid for). Shutdown tears down the tunnel +
          // IoLoop cleanly and leaves auth alone.
          host->ShutdownOffMainLoop([&] { applyQuitStep(quit.OnTeardownDone()); });
        }
        if (step.answer) {
          app->release();
          app->quit();
        }
      };
  // `teardown`: a Quit; a session ending only lets the queued commands run.
  const auto endApp = [&](bool teardown) {
    if (!quit.Begin(teardown)) return;
    // Launches are refused from here on: whatever arrives meanwhile is read,
    // if at all, by an instance about to end. A launch that meets it waits
    // for it to end, then starts.
    urnw::instance::BeginExiting();
    if (window) window->SaveGeometry();  // a quit with the window up never closed it
    // nothing connects behind the ending; a disconnect still queued runs
    host->RefuseConnects();
    Glib::signal_timeout().connect_once([&] { applyQuitStep(quit.OnAnswerCap()); },
                                        urnw::QuitSequence::kAnswerMillis);
    Glib::signal_timeout().connect_once([&] { applyQuitStep(quit.OnDrainCap()); },
                                        urnw::QuitSequence::kDrainMillis);
    host->WhenCommandsDrained([&] { applyQuitStep(quit.OnDrained()); });
  };
  // A logout or a shutdown sends SIGTERM: it used to kill the app at once,
  // with a disconnect the user had just tapped still queued.
  std::function<void()> onSessionEnd = [&] { endApp(/*teardown=*/false); };
  bool startFailed = false;  // main's exit status says so (StartupFailure.hpp)
  // This process's own launch is the first activation; every later one is a
  // launch handed to this instance.
  urnw::instance::Activations activations(arguments.kind);
  // Every launch this instance serves: its links are routed, and the window
  // shows unless it is the autostart entry's.
  const auto serve = [&](const urnw::instance::Launch& launch) {
    for (const std::string& uri : launch.uris) host->HandleDeepLink(uri);
    if (window && urnw::instance::ShowsWindow(launch.kind)) window->present();
  };
  // A hidden window with no tray to bring it back (an autostart launch, or a
  // tray that went away) shows again, minimized, once the tray has had its
  // grace to come back (TrayPolicy.hpp).
  sigc::connection noTrayGrace;
  const auto unreachable = [&] {
    return window && urnw::tray_policy::MustSurface(window->get_visible(), tray && tray->Available());
  };
  const auto keepReachable = [&] {
    noTrayGrace.disconnect();
    if (!unreachable()) return;
    noTrayGrace = Glib::signal_timeout().connect(
        [&]() -> bool {
          if (unreachable()) {
            g_message("tray: no tray to bring the window back; showing it minimized");
            window->minimize();  // before it maps, so it never shows on top
            window->set_visible(true);
          }
          return false;
        },
        urnw::tray_policy::kNoTrayGraceMillis);
  };

  app->signal_startup().connect([&] {
    // SDK INIT BELONGS HERE, NOT BEFORE app->run(). A launch the handover
    // above leaves to GApplication is decided primary-vs-remote only inside
    // run(), so anything above it can execute in a duplicate that is about to
    // hand off to the running instance and exit. signal_startup is emitted on
    // the PRIMARY only.
    //
    // That distinction is not cosmetic. SdkHost::Initialize calls
    // urnet::setLogDir(), which runs the SDK's glog init: it sweeps the log
    // directory down to a keep-N budget and rewrites the
    // urnetwork-gui.{INFO,WARNING,ERROR} symlinks. A tester's bundle caught it
    // -- a second launch deleted three of the seven log files and left
    // urnetwork-gui.INFO, the file any "grab the current log" step follows,
    // naming its own 846-byte stub while the session that was actually running
    // had a 1.5 MB log the symlink no longer pointed at. Initialize also opens
    // the shared storage dir, which a process about to exit has no business
    // touching.
    if (!host->Initialize(storageDir, logDir)) {
      // No window and no tray can come now. Said where a desktop launch can
      // see it, not only on stderr, and the app ends when the dialog closes.
      startFailed = true;
      g_printerr("%s", urnw::startup_failure::StderrLine(host->InitializeError(), logDir).c_str());
      urnw::instance::BeginExiting();
      if (!urnw::startup_failure::Show(*app, host->InitializeError(), logDir)) app->quit();
      return;
    }
    adw_init();  // libadwaita stylesheet + platform integration
    // the brand visual system is dark (mac app parity); the Ui.cpp stylesheet
    // layers the exact palette on top
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                       ADW_COLOR_SCHEME_FORCE_DARK);
    // The licensed brand faces MUST be registered before the first widget —
    // the Pango font map snapshots fontconfig when first used. A wrong or
    // missing face fails silently to the fallback font (windows parity).
    urnw::LoadBrandFonts();
    urnw::EnsureBrandCss();
    // the icon NAME kAppIconName must resolve for the window icon and the
    // tray, wherever the app runs from
    urnw::RegisterBrandIcons();
    // Launch URnetwork on system startup: an autostart entry from before
    // --autostart is brought up to date, so its logins show only the tray;
    // none is made here (LaunchAtStartup.hpp).
    urnw::startup::Locations startupLocations;
    startupLocations.configDir = g_get_user_config_dir();
    urnw::startup::Refresh(urnw::startup::PosixFiles(), startupLocations);

    window = std::make_shared<urnw::MainWindow>(*host);
    app->add_window(*window);

    tray = std::make_shared<urnw::Tray>();
    tray->on_activate = [&] { window->present(); };
    tray->on_show = [&] { window->present(); };
    tray->on_toggle_connect = [&] { window->ToggleConnect(); };
    // the out-of-balance notification's Disconnect button
    app->add_action(urnw::MainWindow::kBalanceNoticeDisconnectAction, [&] {
      if (window) window->DisconnectFromBalanceNotice();
    });
    tray->on_quit = [&] { endApp(/*teardown=*/true); };
    // the session ending, as an ending of its own (onSessionEnd)
    for (const int signum : {SIGTERM, SIGINT, SIGHUP}) {
      g_unix_signal_add(
          signum,
          +[](gpointer data) -> gboolean {
            (*static_cast<std::function<void()>*>(data))();
            return G_SOURCE_CONTINUE;
          },
          &onSessionEnd);
    }
    window->on_tray_state = [&](bool sessionUp, bool proven, const std::string& status,
                                bool hasStatus) {
      if (tray) tray->SetState(sessionUp, proven, status, hasStatus);
    };
    // The recovery items, for when the window holds no session
    // (failsafe_notice::TrayRecovery).
    tray->on_force_tunnel_off = [&] { window->ForceTunnelOff(); };
    tray->on_lift_kill_switch = [&] { window->LiftKillSwitch(); };
    window->on_tray_recovery_change = [&](const urnw::failsafe_notice::TrayRecovery& recovery) {
      if (tray) tray->SetRecovery(recovery.forceTunnelOff, recovery.liftKillSwitch);
    };

    // Close hides to the tray (the tunnel keeps running) while a tray can bring
    // the window back, and minimizes it otherwise; Quit from the tray truly
    // exits.
    window->signal_close_request().connect(
        [&]() -> bool {
          window->SaveGeometry();
          switch (urnw::tray_policy::OnClose(tray && tray->Available())) {
            case urnw::tray_policy::CloseAction::Hide:
              window->set_visible(false);
              // GTK keeps a minimize() asked for earlier in the run (the grace's,
              // or a close with no tray) and applies it at every later map, so
              // the tray's present would map the window minimized. Unmapped, this
              // only drops that request.
              window->unminimize();
              window->NoteHiddenToTray();
              break;
            case urnw::tray_policy::CloseAction::Minimize:
              window->minimize();
              break;
          }
          return true;  // stop the default destroy
        },
        false);
    tray->on_availability_change = [&](bool) { keepReachable(); };

    // Hold the application so it survives with only the tray (window hidden).
    // Held here, on the instance only: held before run(), it kept every
    // GApplication remote (a launch handed to the instance) running forever.
    app->hold();
    // the window and the tray exist: launches handed to this instance are
    // served from here on
    urnw::instance::OpenLaunches(G_APPLICATION(app->gobj()), serve);
    // an autostart launch leaves the window hidden: it waits for the tray
    keepReachable();
  });

  app->signal_activate().connect([&] {
    urnw::instance::Launch launch;
    launch.kind = activations.Next();
    serve(launch);
  });

  // Every way the main loop ends begins the exit, before GtkApplication's own
  // shutdown, which can run the main loop once more to store the clipboard:
  // a launch read then is refused rather than served into a closing instance.
  app->signal_shutdown().connect([] { urnw::instance::BeginExiting(); }, false);

  // Verification hook (the frame-capture half of the windows preview harness):
  // URNETWORK_SHOOT=<out.png> renders the window's content to a PNG ~5s after
  // startup and exits. Compositor-independent (GtkWidgetPaintable -> cairo),
  // so a headless weston in a container can screenshot every --preview-ui
  // destination without touching the user's session.
  if (const char* shootPath = g_getenv("URNETWORK_SHOOT")) {
    const std::string out(shootPath);
    // retry each second until the window is laid out (a headless compositor
    // can map late); give up after ~20 tries
    auto tries = std::make_shared<int>(0);
    // URNETWORK_SHOOT_AFTER=<seconds> holds the first attempt back, for a
    // frame that must be taken after a page has settled (the loading
    // skeletons' ceiling) rather than at the first laid-out tick
    const char* shootAfter = g_getenv("URNETWORK_SHOOT_AFTER");
    const int holdTries = shootAfter ? std::atoi(shootAfter) : 0;
    Glib::signal_timeout().connect(
        [&app, &window, out, tries, holdTries]() -> bool {
          if (++*tries > 20 + holdTries) {
            g_message("shoot: gave up (window never laid out)");
            app->quit();
            return false;
          }
          if (!window || *tries <= holdTries) return true;
          // an onboarding review (URNW_ONBOARDING_PREVIEW) shoots the sheet,
          // once it is up and its opening page change has settled
          const bool wantSheet = g_getenv("URNW_ONBOARDING_PREVIEW") != nullptr;
          Gtk::Window* target = window->PreviewSheet();
          if (wantSheet && (!target || !target->get_mapped() || *tries < 4)) return true;
          if (!target) target = window.get();
          Gtk::Widget* child = target->get_child();
          if (!child) return true;
          const int w = child->get_width();
          const int h = child->get_height();
          if (*tries == 5) {
            g_message("shoot: try 5: mapped=%d visible=%d w=%d h=%d", window->get_mapped(),
                      window->get_visible(), w, h);
          }
          if (w <= 0 || h <= 0) return true;  // not laid out yet
          GdkPaintable* p = gtk_widget_paintable_new(GTK_WIDGET(child->gobj()));
          if (true) {
            GtkSnapshot* snap = gtk_snapshot_new();
            gdk_paintable_snapshot(p, GDK_SNAPSHOT(snap), w, h);
            GskRenderNode* node = gtk_snapshot_free_to_node(snap);
            // NULL = the widget has not produced a frame yet (a heavier page
            // under a software renderer needs a tick or two). Retry rather
            // than quitting on an empty snapshot.
            if (!node) {
              g_object_unref(p);
              return true;
            }
            {
              cairo_surface_t* surface =
                  cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
              cairo_t* cr = cairo_create(surface);
              // the window ground: nodes only carry the widgets' own drawing
              cairo_set_source_rgb(cr, 0x10 / 255.0, 0x10 / 255.0, 0x10 / 255.0);
              cairo_paint(cr);
              gsk_render_node_draw(node, cr);
              cairo_destroy(cr);
              cairo_surface_write_to_png(surface, out.c_str());
              cairo_surface_destroy(surface);
              gsk_render_node_unref(node);
              g_message("shoot: wrote %dx%d to %s", w, h, out.c_str());
            }
          }
          g_object_unref(p);
          app->quit();
          return false;
        },
        1000);
  }

  // urnetwork:// deep links (wallet-connect callbacks) of this process's own
  // launch, or of a launch from before the handover, arrive here. GFile keeps
  // the original URI even for a custom scheme; route it into the SDK host.
  app->signal_open().connect(
      [&](const std::vector<Glib::RefPtr<Gio::File>>& files, const Glib::ustring&) {
        urnw::instance::Launch launch;
        launch.kind = urnw::instance::LaunchKind::Link;
        for (const auto& f : files) {
          if (f) launch.uris.push_back(f->get_uri());
        }
        serve(launch);
      });

  const int status = app->run(argc, argv);
  return startFailed ? urnw::startup_failure::kExitStatus : status;
}
