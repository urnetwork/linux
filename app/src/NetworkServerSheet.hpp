// Change Network API — point the client at a different network deployment
// (a self-hosted or forked server) instead of the official one, with optional
// explicit api/connect url overrides. The GTK port of the Windows
// NetworkServerSheet (itself iOS Shared/Views/NetworkServerSheet).
//
// Offered from the SIGNED-OUT screen only, as on iOS/Windows: switching
// servers swaps the LocalState and therefore the stored jwt, so it cannot be
// done underneath a live session.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

class NetworkServerSheet : public Gtk::Window {
 public:
  NetworkServerSheet(Gtk::Window& parent, SdkHost& sdk);

  // fired after a successful Apply (the sheet closes itself); the login flow
  // resets to the initial step on the new server
  std::function<void()> on_applied;

 private:
  void ApplyDerivedPlaceholders();  // preview the urls the host would produce
  void UpdateInsecureWarning();     // http:// / ws:// override -> amber advisory
  void Apply(const std::string& host, const std::string& apiUrl,
             const std::string& connectUrl);
  void UseDefault();
  std::string DefaultHost() const;
  void SetStatus(const Glib::ustring& text, bool error);

  SdkHost& sdk_;
  SdkHost::NetworkServer current_;
  Gtk::Entry* hostBox_ = nullptr;
  Gtk::Entry* apiBox_ = nullptr;
  Gtk::Entry* connectBox_ = nullptr;
  Gtk::Label* insecureText_ = nullptr;
  Gtk::Label* statusText_ = nullptr;
};

}  // namespace urnw
