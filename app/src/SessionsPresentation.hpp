// Account -> Sessions, decided without GTK or the SDK: what a session row says,
// which logo and colour it wears, what the screen shows in each state of the
// shared controller (sdk ClientSessionViewController), and what a sign-out
// asks before it is sent (REVOKE-UI-FINAL.md §3-§5). SessionsPage.cpp copies
// the controller's ClientSessionSnapshot into the plain Snapshot below on the
// GTK main loop and renders what these functions return; it holds no rule of
// its own (tests/SessionsPresentationTest.cpp).
//
// Every string is a store key and its English (the gettext msgid), which the
// page looks up; tests/CatalogLookupTest.cpp reads each {key, English} pair
// here. The metadata the server sends (place names, versions) is shown as
// plain text, never markup.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "DisplayText.hpp"
#include "MdiGlyphs.hpp"

namespace urnw::sessions {

// A store key and its English.
struct Text {
  const char* key = "";
  const char* english = "";
};

// ---- the controller's snapshot, as plain data --------------------------------

// sdk SessionLastUsed: the server's last observed authenticated use.
struct LastUse {
  int64_t unixSeconds = 0;  // UTC Unix SECONDS
  std::string city;         // GeoLite2 names; empty when unknown
  std::string region;
  std::string country;
  std::string countryCode;  // lower-case ISO alpha-2, or empty
  std::string deviceType;   // android ios macos windows linux web cli server unknown
  std::string appVersion;   // empty when unknown
};

// sdk NetworkSessionInfo, the fields a row reads.
struct Session {
  std::string sessionId;
  bool current = false;
  std::string kind;                // how it signed in (password, google, ...)
  int64_t createUnixSeconds = 0;   // CreateTime; 0 when the sdk sent none
  std::optional<LastUse> lastUse;  // nullopt: no use observed
};

// sdk ClientSessionError: the flags, never the server's words (§5).
// `sessionRevoked` stands for a trustworthy session-revoked cause, the only
// thing that may say "signed out from another device". The sdk's error carries
// no such cause at 1e8f3b5f, so the page leaves it false and a refused
// credential reads the generic sign-in line.
struct Error {
  bool retryable = false;
  bool signInRequired = false;
  bool unsupported = false;
  bool sessionRevoked = false;
};

// sdk ClientSessionAction: one sign-out, in flight or settled.
struct Action {
  std::string sessionId;  // "" for the bulk action
  bool loading = false;   // the request is out
  bool pending = false;   // accepted (202), enforcement not confirmed yet
  std::optional<Error> error;
};

// sdk ClientSessionSnapshot.
struct Snapshot {
  std::vector<Session> sessions;  // in the controller's display order
  std::string currentSessionId;
  std::string legacyCoverage;  // "partial" while older sign-ins are not listed
  bool loaded = false;
  bool loading = false;
  bool refreshing = false;
  bool supported = true;
  std::optional<Action> bulkAction;
  std::vector<Action> actions;
  std::optional<Error> error;
};

// ---- the words ------------------------------------------------------------------

inline constexpr Text kTitleText{"sessions_title", "Sessions"};
inline constexpr Text kThisSessionText{"sessions_this_session", "This session"};
inline constexpr Text kLastUsedText{"sessions_last_used", "Last used {}"};
inline constexpr Text kLastUseUnavailableText{"sessions_last_use_unavailable",
                                              "Last use unavailable"};
inline constexpr Text kSignedInText{"sessions_signed_in", "Signed in {}"};
inline constexpr Text kIdText{"sessions_id", "ID {}"};
inline constexpr Text kCopyIdText{"sessions_copy_id", "Copy session ID"};
inline constexpr Text kCopiedText{"copied", "Copied!"};
inline constexpr Text kSignOutText{"sign_out", "Sign out"};
inline constexpr Text kSignOutNamedText{"sessions_sign_out_accessibility", "Sign out {}"};
inline constexpr Text kSignOutOthersText{"sessions_sign_out_all_others",
                                         "Sign out all other sessions"};
inline constexpr Text kCancelText{"cancel", "Cancel"};
inline constexpr Text kSigningOutText{"sessions_signing_out", "Signing out…"};
inline constexpr Text kActionFailedText{"sessions_action_failed",
                                        "Couldn't sign out this session. Try again."};
// The bulk sign-out's own failure: the row's line says "this session".
inline constexpr Text kOthersFailedText{"sessions_sign_out_others_failed",
                                        "Couldn't sign out the other sessions. Try again."};
inline constexpr Text kRefreshText{"refresh", "Refresh"};
inline constexpr Text kLoadingText{"loading", "Loading..."};
inline constexpr Text kTryAgainText{"try_again", "Try again"};
inline constexpr Text kRefreshFailedText{"sessions_refresh_failed",
                                         "Couldn't refresh. Showing the last list."};
inline constexpr Text kLastUsedHelpText{
    "sessions_last_used_help",
    "Last used is the most recent sign-in activity the server saw. It can lag a few minutes, "
    "and the location is approximate."};
inline constexpr Text kLegacyNoteText{
    "sessions_legacy_note",
    "Sign-ins from older app versions appear here once they renew. To end every sign-in, "
    "change your sign-in details."};
// the header's way back, "‹ Account"
inline constexpr Text kAccountText{"account", "Account"};

// The separator between a line's parts.
inline constexpr const char* kSeparator = " · ";

// What the platform lends the texts: the catalog (T_), the store's {}
// substitution (I18n.hpp FormatArgs), and the renderings of a time
// (Formatters.hpp RelativeTime, LocalDate, LocalDateTime).
struct Words {
  std::function<std::string(const Text&)> lookup;
  std::function<std::string(const std::string& pattern, const std::vector<std::string>& args)>
      format;
  std::function<std::string(int64_t secondsAgo)> relativeTime;
  std::function<std::string(int64_t unixSeconds)> date;
  std::function<std::string(int64_t unixSeconds)> dateTime;
};

// ---- labels, logos and colours (§3.2, §3) ----------------------------------------

// A session's device: the label and the logo.
struct Device {
  Text label;
  MdiGlyph glyph = MdiGlyph::DeviceUnknown;
};

// §3.2. An empty, unknown or unexpected type is "Unknown device" with the
// question mark; iOS and macOS share the Apple logo.
inline Device DeviceFor(const std::string& deviceType) {
  using G = MdiGlyph;
  if (deviceType == "android") return {{"sessions_device_android", "Android"}, G::DeviceAndroid};
  if (deviceType == "ios") return {{"sessions_device_ios", "iOS"}, G::DeviceApple};
  if (deviceType == "macos") return {{"sessions_device_macos", "macOS"}, G::DeviceApple};
  if (deviceType == "windows") return {{"sessions_device_windows", "Windows"}, G::DeviceWindows};
  if (deviceType == "linux") return {{"sessions_device_linux", "Linux"}, G::DeviceLinux};
  if (deviceType == "web") return {{"sessions_device_web", "Web"}, G::DeviceWeb};
  if (deviceType == "cli") return {{"sessions_device_cli", "Command line"}, G::DeviceCli};
  if (deviceType == "server") return {{"sessions_device_server", "Server"}, G::DeviceServer};
  return {{"sessions_device_unknown", "Unknown device"}, G::DeviceUnknown};
}

// The device a session last used (none observed: unknown).
inline Device DeviceOf(const Session& session) {
  return DeviceFor(session.lastUse ? session.lastUse->deviceType : std::string());
}

// §3.2: how the session signed in. Legacy kinds, and any kind this build does
// not know, have no label: the row leaves the method out.
inline std::optional<Text> MethodFor(const std::string& kind) {
  if (kind == "password") return Text{"sessions_kind_password", "Password"};
  if (kind == "verify") return Text{"sessions_kind_verify", "Verification code"};
  if (kind == "apple") return Text{"sessions_kind_apple", "Apple"};
  if (kind == "google") return Text{"sessions_kind_google", "Google"};
  if (kind == "sso") return Text{"sessions_kind_sso", "Single sign-on"};
  if (kind == "wallet") return Text{"sessions_kind_wallet", "Wallet"};
  if (kind == "seedphrase") return Text{"sessions_kind_seedphrase", "Recovery phrase"};
  if (kind == "signup") return Text{"sessions_kind_signup", "New account"};
  if (kind == "auth_code") return Text{"sessions_kind_auth_code", "Auth code"};
  if (kind == "device_adopt") return Text{"sessions_kind_device_adopt", "Device pairing"};
  if (kind == "api_key_client") return Text{"sessions_kind_api_key_client", "API key"};
  return std::nullopt;
}

// The colour this app already gives a place whose country is unknown
// (ProviderLocationsSheet.cpp and ProviderGlobe.cpp kUnknownCountry, the web
// globe's).
inline constexpr const char* kUnknownCountryColorHex = "#0099FF";

// §3: the circle's colour, "#RRGGBB": the sdk's colour for the country of the
// last use (getColorHex takes the lower-case code and answers "rrggbb"), or
// the unknown-country colour when no country is known or the answer is not a
// colour.
inline std::string CountryColorHex(
    const std::optional<LastUse>& lastUse,
    const std::function<std::string(const std::string& countryCode)>& colorHexFor) {
  if (!lastUse || lastUse->countryCode.empty() || !colorHexFor) return kUnknownCountryColorHex;
  std::string code = lastUse->countryCode;
  std::transform(code.begin(), code.end(), code.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string hex = colorHexFor(code);
  if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
  if (hex.size() != 6) return kUnknownCountryColorHex;
  for (char& c : hex) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return kUnknownCountryColorHex;
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return "#" + hex;
}

// §3 line 2: the known parts of the place, "Chicago, Illinois, United States";
// "" when none is known. The names come from outside the app (GeoLite2, by
// way of the server), so they are filtered for display like every other
// place name (DisplayText.hpp), and a part with nothing left is left out.
inline std::string PlaceOf(const LastUse& lastUse) {
  std::string place;
  for (const std::string* raw : {&lastUse.city, &lastUse.region, &lastUse.country}) {
    const std::string part = SanitizeExternalDisplayText(*raw);
    if (part.empty()) continue;
    if (!place.empty()) place += ", ";
    place += part;
  }
  return place;
}

// The app version a use reported, filtered for display: any client can report
// any version (the metadata is advisory), so it is external text too.
inline std::string VersionOf(const LastUse& lastUse) {
  return SanitizeExternalDisplayText(lastUse.appVersion);
}

// §1.3: the first 8 characters of the session ID (all of a shorter one). The
// full ID is what Copy puts on the clipboard.
inline std::string ShortId(const std::string& sessionId) { return sessionId.substr(0, 8); }

// This app's own session.
inline bool IsThisSession(const Snapshot& snapshot, const Session& session) {
  return session.current ||
         (!snapshot.currentSessionId.empty() && session.sessionId == snapshot.currentSessionId);
}

// ---- a row (§3) ---------------------------------------------------------------------

struct Row {
  std::string sessionId;     // the full ID
  std::string title;         // line 1: "Android · 2026.10.8-1067"
  bool thisSession = false;  // the "This session" tag shows
  std::string lastUse;       // line 2: "Chicago, Illinois · Last used 5m ago"
  std::string lastUseFull;   // line 2's full date and time; "" with no use observed
  std::string signedIn;      // line 3: "Signed in 10/03/26 · Google · ID 01a1f3c2"
  std::string signedInFull;  // line 3's full date and time; "" with no time
  std::string device;        // the device label, which a confirmation names
  std::string place;         // the place of the last use; "" when unknown
  std::string signOutName;   // "Sign out Android": the row's action, by name
  MdiGlyph glyph = MdiGlyph::DeviceUnknown;
  std::string colorHex;  // the circle
};

inline Row RowFor(const Snapshot& snapshot, const Session& session, const Words& words,
                  int64_t nowUnixSeconds,
                  const std::function<std::string(const std::string&)>& colorHexFor) {
  Row row;
  row.sessionId = session.sessionId;
  row.thisSession = IsThisSession(snapshot, session);
  const Device device = DeviceOf(session);
  row.device = words.lookup(device.label);
  row.glyph = device.glyph;
  row.colorHex = CountryColorHex(session.lastUse, colorHexFor);

  row.title = row.device;
  if (session.lastUse) {
    const std::string version = VersionOf(*session.lastUse);
    if (!version.empty()) row.title += kSeparator + version;
  }

  if (session.lastUse) {
    const int64_t secondsAgo = std::max<int64_t>(0, nowUnixSeconds - session.lastUse->unixSeconds);
    const std::string used =
        words.format(words.lookup(kLastUsedText), {words.relativeTime(secondsAgo)});
    row.place = PlaceOf(*session.lastUse);
    row.lastUse = row.place.empty() ? used : row.place + kSeparator + used;
    row.lastUseFull = words.dateTime(session.lastUse->unixSeconds);
  } else {
    row.lastUse = words.lookup(kLastUseUnavailableText);
  }

  std::vector<std::string> parts;
  if (session.createUnixSeconds > 0) {
    const std::string date = words.date(session.createUnixSeconds);
    parts.push_back(words.format(words.lookup(kSignedInText), {date}));
    row.signedInFull = words.dateTime(session.createUnixSeconds);
  }
  if (const std::optional<Text> method = MethodFor(session.kind)) {
    parts.push_back(words.lookup(*method));
  }
  parts.push_back(words.format(words.lookup(kIdText), {ShortId(session.sessionId)}));
  for (const std::string& part : parts) {
    if (!row.signedIn.empty()) row.signedIn += kSeparator;
    row.signedIn += part;
  }

  row.signOutName = words.format(words.lookup(kSignOutNamedText), {row.device});
  return row;
}

// ---- actions (§4) --------------------------------------------------------------------

// A sign-out's state, on its row or on the bulk button.
enum class ActionView {
  Idle,
  Busy,    // Loading or Pending: progress and "Signing out…", the control off
  Failed,  // the error on its row; the control on again for another try
};

inline ActionView ActionViewOf(const std::optional<Action>& action) {
  if (!action) return ActionView::Idle;
  // a retry in flight is busy, whatever the last attempt said
  if (action->loading || action->pending) return ActionView::Busy;
  return action->error ? ActionView::Failed : ActionView::Idle;
}

// The line under a sign-out's control: "Signing out…" while it runs, then its
// failure, under a row (`others` false) or under Sign out all other sessions;
// nothing while idle. A failed control stays on: pressing it again asks the
// controller again (SignOutFlow).
inline std::optional<Text> ActionStatusText(ActionView view, bool others) {
  switch (view) {
    case ActionView::Idle:
      return std::nullopt;
    case ActionView::Busy:
      return kSigningOutText;
    case ActionView::Failed:
      return others ? kOthersFailedText : kActionFailedText;
  }
  return std::nullopt;
}

// The sign-out of one session, if the controller has one for it.
inline std::optional<Action> ActionFor(const Snapshot& snapshot, const std::string& sessionId) {
  for (const Action& action : snapshot.actions) {
    if (!sessionId.empty() && action.sessionId == sessionId) return action;
  }
  return std::nullopt;
}

// ---- the screen (§5) -----------------------------------------------------------------

enum class Body {
  NoSession,          // signed out: nothing to list and nothing asked
  Progress,           // never loaded
  Rows,               // the list
  Empty,              // loaded, and nothing to list
  LoadFailed,         // the first load failed: Try again
  Unsupported,        // the server has no sessions yet
  SignInRequired,     // the credential was refused: sign in again, no cause named
  SignedOutRemotely,  // ...with a trustworthy session-revoked cause
};

struct Screen {
  Body body = Body::Progress;
  bool refreshing = false;     // the header's progress over the list kept
  bool refreshFailed = false;  // the list kept, with the non-blocking notice
  bool signOutOthers = false;  // the bottom button shows
  ActionView signOutOthersView = ActionView::Idle;
  bool lastUsedHelp = false;  // what Last used means
  bool legacyNote = false;    // older sign-ins appear once they renew
};

inline Screen ScreenFor(const Snapshot& snapshot, bool signedIn) {
  Screen screen;
  if (!signedIn) {
    screen.body = Body::NoSession;
    return screen;
  }
  const std::optional<Error>& error = snapshot.error;
  if (error && error->signInRequired) {
    screen.body = error->sessionRevoked ? Body::SignedOutRemotely : Body::SignInRequired;
    return screen;
  }
  if (!snapshot.supported || (error && error->unsupported)) {
    screen.body = Body::Unsupported;
    return screen;
  }
  if (!snapshot.loaded) {
    // a load in flight shows progress, even over the last attempt's failure
    screen.body = !snapshot.loading && error ? Body::LoadFailed : Body::Progress;
    return screen;
  }
  screen.body = snapshot.sessions.empty() ? Body::Empty : Body::Rows;
  screen.refreshing = snapshot.refreshing;
  screen.refreshFailed = error.has_value();
  screen.lastUsedHelp = screen.body == Body::Rows;
  screen.legacyNote = snapshot.legacyCoverage == "partial";
  // §4: a current session and at least one other row
  bool current = false;
  size_t others = 0;
  for (const Session& session : snapshot.sessions) {
    if (IsThisSession(snapshot, session)) {
      current = true;
    } else {
      ++others;
    }
  }
  screen.signOutOthers = current && others > 0;
  screen.signOutOthersView = ActionViewOf(snapshot.bulkAction);
  return screen;
}

// The line a body without rows shows; nothing for the rows.
inline std::optional<Text> BodyText(Body body) {
  switch (body) {
    case Body::NoSession:
      return Text{"please_login_to_urnetwork", "Please login to URnetwork"};
    case Body::SignInRequired:
      return Text{"sessions_sign_in_required", "Sign in again to manage sessions."};
    case Body::Progress:
      return kLoadingText;
    case Body::Rows:
      return std::nullopt;
    case Body::Empty:
      return Text{"sessions_empty", "No active sessions"};
    case Body::LoadFailed:
      return Text{"sessions_load_failed", "Couldn't load sessions."};
    case Body::Unsupported:
      return Text{"sessions_unsupported", "Sessions aren't available yet."};
    case Body::SignedOutRemotely:
      return Text{"sessions_signed_out_remotely",
                  "This session was signed out from another device."};
  }
  return std::nullopt;
}

// ---- the confirmation (§4) --------------------------------------------------------------

// What a sign-out signs out: one session, or every session but this one.
struct Target {
  bool others = false;
  std::string sessionId;  // the one session; "" for the others
};

// Every sign-out is confirmed first; Cancel is the default.
struct Confirmation {
  Target target;
  Text title;
  Text body;
  std::vector<std::string> bodyArgs;  // {device, place}, {device} or none
  bool self = false;                  // this app's session: the app signs out
};

inline Confirmation ConfirmationFor(const Snapshot& snapshot, const Session& session,
                                    const Words& words) {
  Confirmation confirmation;
  confirmation.target = Target{false, session.sessionId};
  confirmation.title = Text{"sessions_confirm_title", "Sign out this session?"};
  if (IsThisSession(snapshot, session)) {
    confirmation.self = true;
    confirmation.body = Text{"sessions_confirm_self_body",
                             "This is the session you're using. This app will be signed out."};
    return confirmation;
  }
  const std::string device = words.lookup(DeviceOf(session).label);
  const std::string place = session.lastUse ? PlaceOf(*session.lastUse) : std::string();
  if (place.empty()) {
    confirmation.body = Text{"sessions_confirm_body_no_place", "{} will be signed out."};
    confirmation.bodyArgs = {device};
  } else {
    confirmation.body = Text{"sessions_confirm_body", "{0} in {1} will be signed out."};
    confirmation.bodyArgs = {device, place};
  }
  return confirmation;
}

inline Confirmation OthersConfirmation() {
  Confirmation confirmation;
  confirmation.target = Target{true, std::string()};
  confirmation.title = Text{"sessions_confirm_others_title", "Sign out all other sessions?"};
  confirmation.body = Text{
      "sessions_confirm_others_body",
      "Every other session in this list will be signed out. This session stays signed in. Anyone "
      "who knows your sign-in details can still sign in again."};
  return confirmation;
}

inline std::string ConfirmationBody(const Confirmation& confirmation, const Words& words) {
  return words.format(words.lookup(confirmation.body), confirmation.bodyArgs);
}

// The sign-out gestures between a press and the controller: a press opens one
// confirmation (never a second, never for a row already signing out or gone,
// never for the bulk action unless it is offered), Cancel drops it, and only
// the confirmation's Sign out hands back the target to send, once.
class SignOutFlow {
 public:
  std::optional<Confirmation> Press(const Snapshot& snapshot, const Target& target,
                                    const Words& words) {
    if (open_) return std::nullopt;
    std::optional<Confirmation> confirmation;
    if (target.others) {
      const Screen screen = ScreenFor(snapshot, true);
      if (screen.body != Body::Rows || !screen.signOutOthers ||
          screen.signOutOthersView == ActionView::Busy) {
        return std::nullopt;
      }
      confirmation = OthersConfirmation();
    } else {
      const auto session =
          std::find_if(snapshot.sessions.begin(), snapshot.sessions.end(),
                       [&](const Session& s) { return s.sessionId == target.sessionId; });
      if (target.sessionId.empty() || session == snapshot.sessions.end()) return std::nullopt;
      if (ActionViewOf(ActionFor(snapshot, target.sessionId)) == ActionView::Busy) {
        return std::nullopt;
      }
      confirmation = ConfirmationFor(snapshot, *session, words);
    }
    open_ = confirmation->target;
    return confirmation;
  }

  // Cancel, Escape or the window closed: nothing is sent.
  void Cancel() { open_.reset(); }

  // The confirmation's Sign out: the target, once.
  std::optional<Target> Confirm() {
    std::optional<Target> target = std::move(open_);
    open_.reset();
    return target;
  }

  bool Open() const { return open_.has_value(); }

 private:
  std::optional<Target> open_;
};

}  // namespace urnw::sessions
