// The url/host normalization the Change Network API sheet applies before
// anything reaches the SDK. Ported one-for-one from the Windows netserver
// namespace (windows:app/src/App/AuthSheets.cpp), itself a port of iOS
// NetworkServerUtils.swift / android NetworkServerSelector.kt, so all four
// clients agree on what "ur.network", "https://api.example.com/" and
// "[2001:db8::1]:8080" each mean. Header-only and pure (no GTK, no SDK) so
// the unit test binary covers it on any machine.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace urnw::netserver {

namespace detail {

inline std::string Trim(const std::string& raw) {
  const auto first = raw.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = raw.find_last_not_of(" \t\r\n");
  return raw.substr(first, last - first + 1);
}

inline std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

inline std::optional<std::string> ExplicitScheme(const std::string& raw) {
  const std::string value = Trim(raw);
  const auto at = value.find("://");
  if (at == std::string::npos) return std::nullopt;
  const std::string scheme = Lower(Trim(value.substr(0, at)));
  if (scheme.empty()) return std::nullopt;
  return scheme;
}

inline std::string NormalizeUrl(const std::string& raw, const char* defaultScheme) {
  std::string value = Trim(raw);
  while (!value.empty() && value.back() == '/') value.pop_back();
  if (value.empty()) return "";
  if (value.find("://") != std::string::npos) return value;
  return std::string(defaultScheme) + "://" + value;
}

}  // namespace detail

inline std::string NormalizeHost(const std::string& raw) {
  std::string value = detail::Lower(detail::Trim(raw));
  if (const auto at = value.find("://"); at != std::string::npos) value = value.substr(at + 3);
  for (const char* sep : {"/", "?", "#"}) {
    if (const auto at = value.find(sep); at != std::string::npos) value = value.substr(0, at);
  }
  if (const auto at = value.find('@'); at != std::string::npos) value = value.substr(at + 1);
  // Strip a trailing :port for an IPv4 host:port or a BRACKETED IPv6 literal.
  // A bare IPv6 address is left alone on purpose: its colons are
  // indistinguishable from a port separator, and mangling one silently points
  // the client at the wrong server. This field is a HOST NAME — the SDK
  // derives "api.<host>" / "connect.<host>" from it, and a port has no
  // meaning in that derivation; a deployment off :443 is reached through the
  // explicit url fields, which preserve ports.
  if (const auto at = value.rfind("]:"); at != std::string::npos) {
    value = value.substr(0, at) + "]";
  } else if (value.find('[') == std::string::npos) {
    // Exactly ONE colon = an IPv4/hostname host:port. Two or more = a bare
    // IPv6 literal, left alone. (The Windows source documents this rule but
    // its code strips after the LAST colon regardless, mangling "2001:db8::1"
    // to "2001:db8:" — found by this module's unit test; backport the fix.)
    if (std::count(value.begin(), value.end(), ':') == 1) {
      value = value.substr(0, value.rfind(':'));
    }
  }
  value = detail::Trim(value);
  while (!value.empty() && value.front() == '.') value.erase(value.begin());
  while (!value.empty() && value.back() == '.') value.pop_back();
  return value;
}

inline std::string NormalizeApiUrl(const std::string& raw) {
  return detail::NormalizeUrl(raw, "https");
}
inline std::string NormalizeConnectUrl(const std::string& raw) {
  return detail::NormalizeUrl(raw, "wss");
}

inline bool HasInsecureScheme(const std::string& raw, const std::string& secureScheme) {
  const auto scheme = detail::ExplicitScheme(raw);
  if (!scheme) return false;  // no scheme = the secure one will be added
  return *scheme != secureScheme;
}

// The url the SDK would derive for a service on `hostName` — what the sheet
// shows as the placeholder for a blank override field.
inline std::string DerivedServiceUrl(const std::string& hostName,
                                     const std::string& migrationHostName,
                                     const std::string& envName, const std::string& scheme,
                                     const std::string& service) {
  const std::string serviceHost = migrationHostName.empty() ? hostName : migrationHostName;
  const std::string serviceHostName = (envName == "main" || envName.empty())
                                          ? service + "." + serviceHost
                                          : envName + "-" + service + "." + serviceHost;
  return scheme + "://" + serviceHostName;
}

}  // namespace urnw::netserver
