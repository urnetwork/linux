// The connection region (internal name: ingress country): where the user says
// they connect from, which picks how the app resolves URnetwork's own names
// (api, connect, extender) -- the ingress DNS. It never touches the traffic
// (egress) DNS of the tunnel, which the Custom DNS sheet edits.
//
// One stored value, the network space's ingress country (sdk
// NetworkSpace.Get/SetIngressCountryCode), read and written by two doors: the
// login screen's optional "Connecting from" control and the Custom DNS
// sheet's "Connection region" row. Both open the same picker. The value is a
// space value, local to the device, and applies in place from the next lookup.
//
// The picker offers exactly the regions the SDK has a recommendation for
// (urnet::ingressCountryCodes), by the locale's country name, then "Other /
// Undisclosed" (""). It shows the resolvers a choice uses.
//
// When a call to URnetwork fails at the network level (no answer at all, not
// an auth or validation refusal), or a signed-in connect finds the platform
// unreachable, the app offers "Can't reach URnetwork -- choose where you're
// connecting from", which opens the picker. It never changes the region by
// itself. Which failures count is the SDK's rule (see the can't reach part).
//
// Pure, with no GTK or SDK dependency, so the unit suite runs it anywhere; the
// GTK sheet (ConnectionRegionSheet) and SdkHost supply the store and names.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace urnw {
namespace region {

// Where the stored connection region lives. SdkHost's network space in the
// app, a fake in the tests. Set answers the value actually stored (an unknown
// code is stored as undisclosed, "").
class Store {
 public:
  virtual ~Store() = default;
  virtual std::string Get() = 0;
  virtual std::string Set(const std::string& countryCode) = 0;
};

struct Option {
  std::string code;  // lower case iso code; "" is Other / Undisclosed
  std::string name;  // what the picker shows
  bool operator==(const Option& other) const {
    return code == other.code && name == other.name;
  }
};

inline std::string Lowercased(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// The picker's rows: the SDK's regions in its order (lower cased, blanks and
// repeats dropped), each named by `displayName`, then Other / Undisclosed.
inline std::vector<Option> Options(const std::vector<std::string>& sdkCodes,
                                   const std::function<std::string(const std::string&)>& displayName,
                                   const std::string& undisclosedName) {
  std::vector<Option> out;
  for (const std::string& raw : sdkCodes) {
    const std::string code = Lowercased(raw);
    if (code.empty()) continue;
    if (std::any_of(out.begin(), out.end(), [&](const Option& o) { return o.code == code; })) {
      continue;
    }
    std::string name = displayName ? displayName(code) : std::string();
    out.push_back(Option{code, name.empty() ? code : name});
  }
  out.push_back(Option{std::string(), undisclosedName});
  return out;
}

// The row of `stored`: a stored code the list no longer offers reads as
// Other / Undisclosed, the last row.
inline size_t SelectedIndex(const std::vector<Option>& options, const std::string& stored) {
  const std::string code = Lowercased(stored);
  for (size_t i = 0; i < options.size(); ++i) {
    if (options[i].code == code) return i;
  }
  return options.empty() ? 0 : options.size() - 1;
}

// The resolvers a region uses, for the picker to show: encrypted (DoH, ahead
// of the defaults) and the unencrypted last resort (URnetwork names only).
// Both empty is "Default resolvers".
struct Resolvers {
  std::vector<std::string> encrypted;
  std::vector<std::string> fallback;
  bool Default() const { return encrypted.empty() && fallback.empty(); }
};

// The picker's state over one store. The login control and the settings row
// each make one over the same store, so they read and write one value.
class Picker {
 public:
  Picker(Store& store, std::vector<Option> options) : store_(store), options_(std::move(options)) {}

  const std::vector<Option>& options() const { return options_; }
  // The stored value, read again on every call (the other door may have saved).
  std::string Stored() const { return store_.Get(); }
  size_t SelectedIndex() const { return region::SelectedIndex(options_, Stored()); }
  const Option& Selected() const { return options_[SelectedIndex()]; }
  // Saves row `index`; answers the stored value. Out of range saves nothing.
  std::string Select(size_t index) {
    if (index >= options_.size()) return Stored();
    return store_.Set(options_[index].code);
  }

 private:
  Store& store_;
  std::vector<Option> options_;
};

// ---- can't reach ------------------------------------------------------------

// Which failures mean URnetwork could not be reached is the SDK's one rule for
// every app (sdk cant_reach.go; support inbox 7956, 7973): a failed call's
// text (urnet::isCantReachError, which SdkHost::NoteApiError asks), and,
// signed in, the connect window's diagnosis turning to the platform being
// unreachable (urnet::cantReachOnStall, which MainWindow::ApplyConnectReading
// asks). Each app used to keep its own list of failure texts, and the lists
// drifted; only Windows raised the prompt for a signed-in stall.

// The prompt's state. It is raised for a failure or a stall the SDK's rule
// found unreachable, and only shows or hides; it holds no store, so it cannot
// change the region (the user picks, in the picker).
class CantReachPrompt {
 public:
  bool visible() const { return visible_; }
  // A call that got no answer, or a signed-in connect that turned
  // unreachable, by the SDK's rule.
  void Raise() { visible_ = true; }
  // A call that got an answer, or the user opened the picker.
  void Dismiss() { visible_ = false; }

 private:
  bool visible_ = false;
};

// Where the window shows a raised prompt, by the page of its stack the user is
// on (MainWindow's stack_ child names). The login page carries the prompt
// under its error line, where it waits for a user who steps back to it until
// the picker opens or the user signs in, so it is set for every signed-out
// page. Only the login page can show that line, though: a sign-in step past it
// (password, seedphrase, instant, create, verify, reset) shows the failure in
// its own error line, and the signed-in home has no such line, so every page
// but the login page also gets a toast over the stack that opens the picker,
// as android and apple raise the prompt over every page.
struct CantReachPlacement {
  bool loginLine = false;  // the login page's own prompt
  bool toast = false;      // a toast over the page stack
};

inline CantReachPlacement PlaceCantReachPrompt(const std::string& visiblePage) {
  CantReachPlacement placement;
  placement.loginLine = visiblePage != "home";
  placement.toast = visiblePage != "login";
  return placement;
}

}  // namespace region
}  // namespace urnw
