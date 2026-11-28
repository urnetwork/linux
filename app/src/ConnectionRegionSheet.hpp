// The connection region picker ("Where are you connecting from?"), the one
// sheet both doors open: the login screen's "Connecting from" control and the
// Custom DNS sheet's "Connection region" row (ConnectionRegion.hpp has the
// rules). A row saves at once through SdkHost -- the space's ingress country,
// applied in place from the next lookup -- and the sheet shows the resolvers
// the choice uses.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "ConnectionRegion.hpp"
#include "SdkHost.hpp"

namespace urnw {

// SdkHost's network space as the picker's store.
class SdkHostRegionStore : public region::Store {
 public:
  explicit SdkHostRegionStore(SdkHost& host) : host_(host) {}
  std::string Get() override { return host_.GetIngressCountryCode(); }
  std::string Set(const std::string& countryCode) override {
    return host_.SetIngressCountryCode(countryCode);
  }

 private:
  SdkHost& host_;
};

// The locale's name of a country (iso-codes' iso_3166-1 catalog, the names
// every GNOME country list uses), or "" when iso-codes is not installed.
std::string CountryDisplayName(const std::string& countryCode);

// A location's name in the location list and on the connect screen: a country
// in the app's language by CountryDisplayName, the server's name otherwise
// (region::LocationDisplayName). External text: the caller sanitizes it.
std::string LocationDisplayName(const urnet::ConnectLocation& location);

// The picker's rows over the SDK's regions, named in the locale.
std::vector<region::Option> ConnectionRegionOptions();

// The name of the stored region for the doors' labels.
std::string ConnectionRegionName(SdkHost& host);

class ConnectionRegionSheet : public Gtk::Window {
 public:
  ConnectionRegionSheet(Gtk::Window& parent, SdkHost& host);

  // Reads the stored value again and presents.
  void Open();

  // After a save, with the stored value; the doors update their labels.
  std::function<void(const std::string& stored)> on_changed;

 private:
  void Sync();
  void ShowResolvers(const std::string& countryCode);

  SdkHostRegionStore store_;
  region::Picker picker_;
  bool syncing_ = false;
  std::vector<Gtk::CheckButton*> rows_;
  Gtk::Label* resolvers_ = nullptr;
};

}  // namespace urnw
