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
#include "SdkHost.hpp"
#include "Tray.hpp"

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
  bindtextdomain(GETTEXT_PACKAGE, UR_LOCALEDIR);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");  // the catalogs are UTF-8
  textdomain(GETTEXT_PACKAGE);

  auto host = std::make_shared<urnw::SdkHost>();
  const std::string storageDir = EnsureDir(Glib::get_user_data_dir(), "urnetwork");
  // glibmm on core24 (the 2.68 ABI series) has no Glib::get_user_state_dir wrapper;
  // call the C g_get_user_state_dir() (glib 2.72+) directly for XDG_STATE_HOME.
  const std::string logDir = EnsureDir(g_get_user_state_dir(), "urnetwork");
  if (!host->Initialize(storageDir, logDir)) {
    g_printerr("failed to initialize SDK\n");
    return 1;
  }

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
    adw_init();  // libadwaita stylesheet + platform integration
    // the brand visual system is dark (mac app parity); the Ui.cpp stylesheet
    // layers the exact palette on top
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                       ADW_COLOR_SCHEME_FORCE_DARK);

    window = std::make_shared<urnw::MainWindow>(*host);
    app->add_window(*window);

    tray = std::make_shared<urnw::Tray>();
    tray->on_activate = [&] { window->present(); };
    tray->on_show = [&] { window->present(); };
    tray->on_toggle_connect = [&] { window->ToggleConnect(); };
    tray->on_quit = [&] {
      host->Logout();  // tears down tunnel + IoLoop cleanly
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
