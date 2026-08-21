// SPDX-License-Identifier: MPL-2.0
//
// Single-process entry point. Owns the SdkHost (the in-process VPN core), the
// GTK4 window, and the D-Bus tray. Closing the window hides to the tray and the
// tunnel keeps running (the Windows/macOS "keep connected" behavior); Quit from
// the tray is the only real exit.
#include <adwaita.h>
#include <glib.h>
#include <giomm/file.h>
#include <glibmm/miscutils.h>
#include <gtkmm/application.h>

#include <vector>

#include <clocale>
#include <memory>
#include <string>

#include "I18n.hpp"
#include "MainWindow.hpp"
#include "RuntimePaths.hpp"
#include "SdkHost.hpp"
#include "Tray.hpp"
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

  auto host = std::make_shared<urnw::SdkHost>();
  const std::string storageDir = EnsureDir(Glib::get_user_data_dir(), "urnetwork");
  // glibmm on core24 (the 2.68 ABI series) has no Glib::get_user_state_dir wrapper;
  // call the C g_get_user_state_dir() (glib 2.72+) directly for XDG_STATE_HOME.
  const std::string logDir = EnsureDir(g_get_user_state_dir(), "urnetwork");

  // Must match the .desktop StartupWMClass + common-id so the shell associates
  // the window with the app (and the hide-to-tray window keeps its identity).
  // HANDLES_OPEN: the single instance receives urnetwork:// deep links (wallet
  // callbacks) via signal_open — the .desktop registers x-scheme-handler/urnetwork.
  auto app = Gtk::Application::create("network.ur.urnetwork",
                                      Gio::Application::Flags::HANDLES_OPEN);

  // Hold the application so it survives with only the tray (window hidden).
  app->hold();

  std::shared_ptr<urnw::MainWindow> window;
  std::shared_ptr<urnw::Tray> tray;

  app->signal_startup().connect([&] {
    // SDK INIT BELONGS HERE, NOT BEFORE app->run(). GApplication only decides
    // primary-vs-remote inside run(), so anything above it executes in EVERY
    // launch -- including a duplicate that is about to hand off to the running
    // instance and exit. signal_startup is emitted on the PRIMARY only.
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
      g_printerr("failed to initialize SDK\n");
      app->quit();
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
    // the icon NAME "urnetwork" must resolve for the window icon and the
    // tray, wherever the app runs from
    urnw::RegisterBrandIcons();

    window = std::make_shared<urnw::MainWindow>(*host);
    app->add_window(*window);

    tray = std::make_shared<urnw::Tray>();
    tray->on_activate = [&] { window->present(); };
    tray->on_show = [&] { window->present(); };
    tray->on_toggle_connect = [&] { window->ToggleConnect(); };
    tray->on_quit = [&] {
      // teardown WITHOUT Logout(): Logout wipes the stored jwt, and for a
      // guest network that jwt is the only credential — quitting from the
      // tray was permanently destroying guest accounts (and any balance or
      // subscription they had paid for). Shutdown tears down the tunnel +
      // IoLoop cleanly and leaves auth alone.
      host->Shutdown();
      app->release();
      app->quit();
    };
    window->on_connected_change = [&](bool connected) {
      if (tray) tray->SetConnected(connected);
    };

    // Close = hide to tray (tunnel keeps running); Quit from the tray truly exits.
    window->signal_close_request().connect(
        [&]() -> bool {
          window->set_visible(false);
          return true;  // stop the default destroy
        },
        false);
  });

  app->signal_activate().connect([&] {
    if (window) window->present();
  });

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
    Glib::signal_timeout().connect(
        [&app, &window, out, tries]() -> bool {
          if (++*tries > 20) {
            g_message("shoot: gave up (window never laid out)");
            app->quit();
            return false;
          }
          if (!window) return true;
          Gtk::Widget* child = window->get_child();
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

  // urnetwork:// deep links (wallet-connect callbacks) arrive here. GFile keeps
  // the original URI even for a custom scheme; route it into the SDK host.
  app->signal_open().connect(
      [&](const std::vector<Glib::RefPtr<Gio::File>>& files, const Glib::ustring&) {
        for (const auto& f : files) {
          if (f) host->HandleDeepLink(f->get_uri());
        }
        if (window) window->present();
      });

  return app->run(argc, argv);
}
