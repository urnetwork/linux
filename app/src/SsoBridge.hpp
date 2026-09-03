// The ur.io/sso browser bridge contract for Google and Apple sign-in on hosts
// with no native provider flow (LOGIN_STACK_SPEC, 2026-09-02): the app opens
//   https://ur.io/sso?provider=<google|apple>&redirect_link=urnetwork://sso
//                     &state=<attempt>&nonce=<attempt>
// and the page comes back through the urnetwork:// scheme with
//   urnetwork://sso?provider=<p>&auth_jwt=<identity token>&state=<state>
//   urnetwork://sso?provider=<p>&error=<message>&state=<state>
// Two checks make a return usable: the echoed `state` must be the one this
// app minted (no stray or replayed return can start a login), and the identity
// token's `nonce` claim must be the one sent with it (the token was issued for
// THIS attempt, not lifted from another sign-in). Header-only and pure (no
// GTK, no SDK, no glib) so the unit test binary covers both checks anywhere.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace urnw::sso {

inline constexpr const char* kBridgeUrl = "https://ur.io/sso";
inline constexpr const char* kRedirectLink = "urnetwork://sso";
inline constexpr const char* kReturnHost = "sso";  // the host of the return url

inline constexpr const char* kProviderGoogle = "google";
inline constexpr const char* kProviderApple = "apple";

namespace detail {

inline std::string PercentEncode(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

inline int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::string PercentDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = HexValue(s[i + 1]);
      const int lo = HexValue(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += (s[i] == '+') ? ' ' : s[i];
  }
  return out;
}

// base64url (RFC 4648 §5, unpadded or padded) -> bytes; nullopt on a bad char
inline std::optional<std::vector<uint8_t>> Base64UrlDecode(const std::string& in) {
  std::vector<uint8_t> out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=') break;
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-' || c == '+') v = 62;
    else if (c == '_' || c == '/') v = 63;
    else return std::nullopt;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

inline std::string Base64UrlEncode(const std::string& in) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (unsigned char c : in) {
    acc = (acc << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += kAlphabet[(acc >> bits) & 0x3F];
    }
  }
  if (bits > 0) out += kAlphabet[(acc << (6 - bits)) & 0x3F];
  return out;
}

}  // namespace detail

// The bridge url for one attempt. `state` and `nonce` are minted per attempt
// by the caller and kept in memory until the return.
inline std::string BridgeUrl(const std::string& provider, const std::string& state,
                             const std::string& nonce) {
  return std::string(kBridgeUrl) + "?provider=" + detail::PercentEncode(provider) +
         "&redirect_link=" + detail::PercentEncode(kRedirectLink) +
         "&state=" + detail::PercentEncode(state) + "&nonce=" + detail::PercentEncode(nonce);
}

inline std::map<std::string, std::string> ParseQuery(const std::string& query) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i <= query.size()) {
    const auto amp = query.find('&', i);
    const std::string pair =
        query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    const auto eq = pair.find('=');
    if (eq != std::string::npos) {
      out[detail::PercentDecode(pair.substr(0, eq))] = detail::PercentDecode(pair.substr(eq + 1));
    }
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return out;
}

// What the bridge sent back (the query of urnetwork://sso?...)
struct Return {
  std::string provider;
  std::string authJwt;
  std::string state;
  std::string error;
};

inline Return ParseReturn(const std::string& query) {
  auto params = ParseQuery(query);
  Return r;
  r.provider = params.count("provider") ? params["provider"] : std::string();
  r.authJwt = params.count("auth_jwt") ? params["auth_jwt"] : std::string();
  r.state = params.count("state") ? params["state"] : std::string();
  r.error = params.count("error") ? params["error"] : std::string();
  return r;
}

// One string claim of a JWT payload — decoded, never verified: the server
// verifies the signature; the app only reads the nonce it minted back.
inline std::optional<std::string> JwtClaimString(const std::string& jwt,
                                                 const std::string& claim) {
  const auto first = jwt.find('.');
  if (first == std::string::npos) return std::nullopt;
  const auto second = jwt.find('.', first + 1);
  const std::string payload =
      jwt.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
  const auto bytes = detail::Base64UrlDecode(payload);
  if (!bytes) return std::nullopt;
  const auto json = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) return std::nullopt;
  const auto it = json.find(claim);
  if (it == json.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

struct Verdict {
  bool ok = false;
  std::string error;
};

// Accept a return only for the attempt in flight: same provider, the minted
// state echoed, a token present, and the token minted for this nonce.
inline Verdict CheckReturn(const Return& r, const std::string& provider,
                           const std::string& state, const std::string& nonce) {
  if (state.empty()) return {false, "no sign-in in progress"};
  if (r.state != state) return {false, "unexpected sign-in return"};
  if (!r.error.empty()) return {false, r.error};
  if (r.provider != provider) return {false, "unexpected sign-in provider"};
  if (r.authJwt.empty()) return {false, "sign-in returned no token"};
  const auto claim = JwtClaimString(r.authJwt, "nonce");
  if (!claim || *claim != nonce) return {false, "sign-in token did not match this attempt"};
  return {true, std::string()};
}

// A token shaped like an identity token, for the simulated round trip
// (URNETWORK_SSO_SIMULATE) and the tests: unsigned, the server rejects it.
inline std::string SimulatedIdentityToken(const std::string& nonce, const std::string& issuer) {
  const std::string header = detail::Base64UrlEncode(R"({"alg":"none","typ":"JWT"})");
  const nlohmann::json payload = {{"iss", issuer}, {"sub", "simulated"}, {"nonce", nonce}};
  return header + "." + detail::Base64UrlEncode(payload.dump()) + ".";
}

}  // namespace urnw::sso
