// The account section's extender logic (connect/EXTENDER.md K6/K7): the
// settings form's fields, the bootstrap DNS-over-HTTPS servers box and its
// messages, the share screen's reading of build_share, and the import screen's
// decision from decode_share -- all as pure functions so the rules are pinned
// by tests and the dialogs only render what these decided.
//
// Encoding, decoding and applying a share live in the SDK
// (ExtenderViewController), one implementation for every app. NOTHING here
// parses a payload; this is only what the UI does with the SDK's answers.
//
// Header-only and free of GTK and the SDK so the unit tests need no vendored
// headers (tests/ExtenderSharePresentationTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::extender {

// ---- the settings form ------------------------------------------------------

// One field of the settings form. The SDK reports the EFFECTIVE value plus a
// flag saying whether that value is the derived default, so:
//
//   isDefault -> the field is EMPTY (empty means default) and the value it
//                reported is the placeholder, shown as "Default: <value>".
//   override  -> the field holds the override, and the default is whatever was
//                learned the last time the field was at its default.
//
// Carrying the learned default forward is what lets a user who types an
// override still see what they are overriding. A form that opens on an
// override it has never seen defaulted has no placeholder to show yet -- it
// gets one the moment the field is cleared and saved, which is also exactly
// when it matters.
struct SettingsField {
  std::string text;          // what the entry shows (empty = "use the default")
  std::string defaultValue;  // "" when this build has never seen the default

  bool hasDefault() const { return !defaultValue.empty(); }

  friend bool operator==(const SettingsField& a, const SettingsField& b) {
    return a.text == b.text && a.defaultValue == b.defaultValue;
  }
};

inline SettingsField ApplySettingsField(const SettingsField& previous, const std::string& value,
                                        bool isDefault) {
  SettingsField out;
  if (isDefault) {
    out.text.clear();
    out.defaultValue = value;
    return out;
  }
  out.text = value;
  out.defaultValue = previous.defaultValue;
  return out;
}

// The manual bootstrap list is edited as one host per line. Blank lines and
// surrounding whitespace are dropped and duplicates collapse, keeping the
// FIRST occurrence, so the order the user typed survives a save.
inline std::vector<std::string> SplitHostLines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    const bool last = end == std::string::npos;
    if (last) end = text.size();
    size_t a = start, b = end;
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (a < b && space(text[a])) ++a;
    while (b > a && space(text[b - 1])) --b;
    if (a < b) {
      std::string host(text, a, b - a);
      if (std::find(out.begin(), out.end(), host) == out.end()) out.push_back(std::move(host));
    }
    if (last) break;
    start = end + 1;
  }
  return out;
}

inline std::string JoinHostLines(const std::vector<std::string>& hosts) {
  std::string out;
  for (const auto& host : hosts) {
    if (!out.empty()) out.push_back('\n');
    out += host;
  }
  return out;
}

// ---- the bootstrap DNS-over-HTTPS servers (sdk control_doh_ui.go) -----------
//
// `https://<ip literal>/<path>` servers the space's own names (api, connect,
// extender) are looked up through ahead of the built-in ones, for networks
// that block those. A value of the GUI's own network space, so the box works
// with or without a tunnel; the daemon takes the space at its next
// start_tunnel.

// A store key and its English (the gettext msgid), as VlessPresentation.hpp's.
struct Text {
  std::string_view key;
  std::string_view english;
};

// sdk ControlDohError* (URNET_CONTROL_DOH_ERROR_*): each id is the
// localization key of its message, the vless_error_* pattern.
inline constexpr const char* kControlDohErrorUrlInvalid = "control_doh_error_url_invalid";
inline constexpr const char* kControlDohErrorHttpsRequired = "control_doh_error_https_required";
inline constexpr const char* kControlDohErrorIpRequired = "control_doh_error_ip_required";
inline constexpr const char* kControlDohErrorTooMany = "control_doh_error_too_many";

// The country whose preset "Use China resolvers" asks the SDK for
// (urnet::regionalControlDohUrls), the one source of the preset's servers.
inline constexpr const char* kControlDohChinaCountryCode = "cn";

// The servers box back into the list set_control_doh_urls takes: split on line
// breaks (\n and \r), trimmed, blank lines dropped, in order. Not on commas and
// not de-duplicated, unlike the hosts box: every platform splits this box the
// same way, and the SDK drops repeats and normalizes the rest. Shown back one
// per line (JoinHostLines).
inline std::vector<std::string> SplitControlDohLines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find_first_of("\r\n", start);
    const bool last = end == std::string::npos;
    if (last) end = text.size();
    size_t a = start, b = end;
    auto space = [](char c) { return c == ' ' || c == '\t'; };
    while (a < b && space(text[a])) ++a;
    while (b > a && space(text[b - 1])) --b;
    if (a < b) out.emplace_back(text, a, b - a);
    if (last) break;
    start = end + 1;
  }
  return out;
}

// The message of an SDK error id: its own for the four the SDK names, and
// "Something went wrong." for anything else -- an id this build does not know
// (a newer SDK) and the C ABI's URNET_ERROR_ID_INTERNAL alike (what an
// error-id function answers when the call could not run at all: a handle that
// did not resolve, json that did not decode), neither of which is about the
// url the user typed -- never a raw key on screen. No copy of the internal id
// is kept here: the SDK header is its one source (tests/ErrorIdInternalTest.cpp).
inline Text ControlDohErrorText(std::string_view errorId) {
  static constexpr Text kErrors[] = {
      {kControlDohErrorUrlInvalid, "Enter a full URL, such as https://223.5.5.5/dns-query."},
      {kControlDohErrorHttpsRequired, "The URL must start with https://."},
      {kControlDohErrorIpRequired,
       "Use an IP address, not a host name, such as https://223.5.5.5/dns-query."},
      {kControlDohErrorTooMany, "Too many servers. Remove some and save again."},
  };
  for (const Text& error : kErrors) {
    if (error.key == errorId) return error;
  }
  return {"something_went_wrong", "Something went wrong."};
}

// What the lines under Save say for the SDK's answer to set_control_doh_urls.
// Only an empty answer is saved. The VPN runs in urnetworkd, in its own copy of
// the network space, but the app's device remote hands a running tunnel the
// saved servers in place (sdk device_remote_ingress_dns.go), so a save needs no
// next-connect note. Any id, URNET_ERROR_ID_INTERNAL included, is that error's
// message, and nothing was saved.
struct ControlDohSaveOutcome {
  bool saved = false;
  Text message;
  Text note;  // an empty key: no second line
};

inline ControlDohSaveOutcome ControlDohSaveOutcomeFor(std::string_view errorId) {
  ControlDohSaveOutcome out;
  if (!errorId.empty()) {
    out.message = ControlDohErrorText(errorId);
    return out;
  }
  out.saved = true;
  out.message = {"control_doh_urls_saved", "Bootstrap DNS-over-HTTPS servers saved"};
  return out;
}

// ---- the share screen -------------------------------------------------------

struct SharePresentation {
  bool ready = false;           // a payload arrived and is renderable
  std::string text;             // the ur-ext:1:... payload
  int64_t count = 0;            // addresses the payload carries
  bool includesSettings = false;
  bool canCopy = false;         // the copy button
  bool canRenderCode = false;   // the QR canvas
};

// A share with NO addresses is still a legitimate answer (a device that has
// discovered nothing yet), and it must render as a zero count rather than as a
// failure -- but an EMPTY payload has nothing to encode or copy, so the code
// and the copy button stand down while the count still reads.
//
// `canRenderCode` means "there is something to hand the encoder", NOT "a code
// will appear": whether the payload actually fits is only known once the
// encoder has run, which is what ShareCodeStateFor below resolves.
inline SharePresentation SharePresentationFor(bool haveResult, const std::string& text,
                                              int64_t count, bool includesSettings) {
  SharePresentation out;
  if (!haveResult) return out;
  out.ready = true;
  out.text = text;
  out.count = std::max<int64_t>(count, 0);
  out.includesSettings = includesSettings;
  out.canCopy = !text.empty();
  out.canRenderCode = !text.empty();
  return out;
}

// What the share sheet shows where the code would be. Four distinct readings,
// none of them a blank square -- and the last two are the reason this is not
// just `canRenderCode`: a 48-address share WITH a settings block can exceed
// even version 40-H, and "the encoder refused it" has to be a sentence on the
// screen rather than a line in the journal, because the copyable text below is
// then the only way to move the addresses.
enum class ShareCodeState {
  // no answer at all (no device): the sheet says something went wrong
  Unavailable,
  // an answer carrying nothing: the "0 extenders" count is the whole reading
  // and the code area stands down without a message of its own
  Empty,
  // a payload the encoder refused: share_extenders_too_large, pointing at the
  // copyable text
  TooLarge,
  // the code itself
  Code,
};

// `encoded` is what the encoder actually managed with `share.text`.
inline ShareCodeState ShareCodeStateFor(const SharePresentation& share, bool encoded) {
  if (!share.ready) return ShareCodeState::Unavailable;
  if (!share.canRenderCode) return ShareCodeState::Empty;
  return encoded ? ShareCodeState::Code : ShareCodeState::TooLarge;
}

// The localization key id for the line under (or instead of) the code, and the
// English source that key carries. Empty for the states that need no sentence.
inline const char* ShareCodeMessageKey(ShareCodeState state) {
  switch (state) {
    case ShareCodeState::Unavailable: return "something_went_wrong";
    case ShareCodeState::TooLarge: return "share_extenders_too_large";
    case ShareCodeState::Empty:
    case ShareCodeState::Code: return "";
  }
  return "";
}

// ---- the import screen ------------------------------------------------------

enum class ImportDecision {
  // nothing decoded yet: the screen is waiting for a code or a paste
  Waiting,
  // the SDK refused the payload; `messageKey` is its error key id
  Invalid,
  // decoded and applicable as-is
  Ready,
  // decoded, but the code belongs to another operator: importing is refused
  // until "use extender settings" is chosen
  NeedsSettings,
  // decoded and "use extender settings" is on: replacing the dns name, gossip
  // url and root keys is confirmed before it is applied
  Confirm,
};

struct ImportPresentation {
  ImportDecision decision = ImportDecision::Waiting;
  // Localization key id for the line under the count, "" when there is none.
  // A STRING, not a const char*: one of its values is the SDK's own error key,
  // which arrives in a caller-owned string that this result must outlive.
  std::string messageKey;
  // the single argument that line takes (an operator host), "" when none
  std::string messageArg;
  bool showCount = false;
  int64_t count = 0;
  bool showUseSettings = false;   // the switch: only when the code carries settings
  bool canImport = false;         // the Import button
  bool confirmBeforeImport = false;

  friend bool operator==(const ImportPresentation& a, const ImportPresentation& b) {
    return a.decision == b.decision && a.messageKey == b.messageKey &&
           a.messageArg == b.messageArg && a.showCount == b.showCount && a.count == b.count &&
           a.showUseSettings == b.showUseSettings && a.canImport == b.canImport &&
           a.confirmBeforeImport == b.confirmBeforeImport;
  }
};

// Mirrors of the SDK's import error key ids (URNET_EXTENDER_IMPORT_ERROR_*),
// which are localization key ids rather than prose -- the SDK hands the app a
// key and the app looks it up.
inline constexpr const char* kImportErrorInvalid = "import_extenders_invalid";
inline constexpr const char* kImportErrorForeignHost = "import_extenders_foreign_host";
inline constexpr const char* kImportConfirmSettings = "import_extenders_confirm_settings";

// The whole import screen, from one decode_share result plus the state of the
// "use extender settings" switch.
//
// K7's rule: "an import whose network host differs from the space's is refused
// unless 'use extender settings' is chosen, which shows the operator host and
// asks to confirm before replacing the dns name, gossip url and root keys."
// So a foreign host WITHOUT the switch cannot import at all, and applying
// settings -- foreign host or not -- always confirms first, because it is the
// settings replacement that is the consequential act.
//
// A foreign code that carries NO settings block is a dead end by construction:
// the switch that would unblock it has nothing to apply. It says so and offers
// no button, rather than offering one that would fail.
inline ImportPresentation ImportPresentationFor(bool haveResult, bool ok, const std::string& error,
                                                const std::string& networkHost, bool foreignHost,
                                                int64_t count, bool hasSettings,
                                                const std::string& settingsHost, bool useSettings) {
  ImportPresentation out;
  if (!haveResult) return out;
  if (!ok) {
    out.decision = ImportDecision::Invalid;
    // An empty error from a failed decode still has to say something; the
    // generic "this is not an extender share" is the honest fallback.
    out.messageKey = error.empty() ? std::string(kImportErrorInvalid) : error;
    if (out.messageKey == kImportErrorForeignHost) out.messageArg = networkHost;
    return out;
  }
  out.showCount = true;
  out.count = std::max<int64_t>(count, 0);
  out.showUseSettings = hasSettings;

  if (foreignHost && !(hasSettings && useSettings)) {
    out.decision = ImportDecision::NeedsSettings;
    out.messageKey = kImportErrorForeignHost;
    out.messageArg = networkHost;
    out.canImport = false;
    return out;
  }
  if (hasSettings && useSettings) {
    out.decision = ImportDecision::Confirm;
    out.messageKey = kImportConfirmSettings;
    // the operator the settings point at, which is the thing being agreed to;
    // an older SDK that leaves it blank falls back to the code's network host
    out.messageArg = settingsHost.empty() ? networkHost : settingsHost;
    out.canImport = true;
    out.confirmBeforeImport = true;
    return out;
  }
  out.decision = ImportDecision::Ready;
  out.canImport = true;
  return out;
}

// The argument of import_extenders_control_doh_urls: the bootstrap
// DNS-over-HTTPS servers a code's settings block names, joined with ", ".
// Empty, and the line hidden, when the code carries no settings block or its
// block names none -- such a code leaves this device's own servers alone. The
// servers are taken only with the settings, and they are named before the
// switch is turned on, because they will see URnetwork's lookups.
inline std::string ControlDohImportArg(bool hasSettings, const std::vector<std::string>& urls) {
  std::string out;
  if (!hasSettings) return out;
  for (const auto& url : urls) {
    if (!out.empty()) out += ", ";
    out += url;
  }
  return out;
}

}  // namespace urnw::extender
