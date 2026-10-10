// Account -> Sessions decided from controller snapshots (SessionsPresentation.hpp,
// REVOKE-UI-FINAL.md §3-§5, §10): device labels and logos for every type,
// sign-in methods for every kind, the country colour and its fallback, the
// three lines of a row with and without a last use, the screen for every
// controller state, and the confirmation each sign-out asks before anything
// is sent. The words are the English sources (the catalog with no translation
// loaded), pinned against po/en.po at the end; the clock and the dates are
// fixed strings, so nothing here depends on time or locale.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "RelativeTimeSpan.hpp"
#include "SessionsPresentation.hpp"

using urnw::MdiGlyph;
namespace s = urnw::sessions;

#define UR_EXPECT_TEXT(expected, actual)                                                    \
  do {                                                                                      \
    const std::string ur_e_ = (expected);                                                   \
    const std::string ur_a_ = (actual);                                                     \
    if (ur_e_ != ur_a_)                                                                     \
      UR_FAIL(std::string(#actual) + ": expected \"" + ur_e_ + "\", got \"" + ur_a_ + "\""); \
  } while (0)

namespace {

constexpr int64_t kNow = 1790000000;  // a fixed "now", UTC Unix seconds

// The store's {} substitution (I18n.hpp FormatArgs): "{}" takes the next
// argument, "{0}" "{1}" a given one.
std::string FormatLike(const std::string& pattern, const std::vector<std::string>& args) {
  std::string out;
  size_t next = 0;
  for (size_t i = 0; i < pattern.size();) {
    if (pattern[i] == '{') {
      const size_t close = pattern.find('}', i);
      if (close != std::string::npos) {
        const std::string spec = pattern.substr(i + 1, close - i - 1);
        size_t index = next;
        if (!spec.empty()) index = static_cast<size_t>(std::stoul(spec));
        if (index < args.size()) {
          out += args[index];
          if (spec.empty()) ++next;
          i = close + 1;
          continue;
        }
      }
    }
    out += pattern[i++];
  }
  return out;
}

// Formatters.cpp RelativeTime with the English words and a fixed date.
std::string RelativeEnglish(int64_t secondsAgo) {
  const urnw::relative_time::Span span = urnw::relative_time::SpanFor(secondsAgo);
  switch (span.unit) {
    case urnw::relative_time::Unit::Now: return "now";
    case urnw::relative_time::Unit::Seconds: return std::to_string(span.count) + "s ago";
    case urnw::relative_time::Unit::Minutes: return std::to_string(span.count) + "m ago";
    case urnw::relative_time::Unit::Hours: return std::to_string(span.count) + "h ago";
    case urnw::relative_time::Unit::Days: return std::to_string(span.count) + "d ago";
    case urnw::relative_time::Unit::Date: return "D" + std::to_string(kNow - secondsAgo);
  }
  return "";
}

s::Words EnglishWords() {
  s::Words words;
  words.lookup = [](const s::Text& text) { return std::string(text.english); };
  words.format = &FormatLike;
  words.relativeTime = &RelativeEnglish;
  words.date = [](int64_t t) { return "D" + std::to_string(t); };
  words.dateTime = [](int64_t t) { return "DT" + std::to_string(t); };
  return words;
}

// The sdk's palette, faked: "us" is blue, anything else unknown to it is "".
std::string FakeColorHex(const std::string& code) { return code == "us" ? "3a7bd5" : ""; }

s::LastUse Chicago(int64_t secondsAgo) {
  s::LastUse use;
  use.unixSeconds = kNow - secondsAgo;
  use.city = "Chicago";
  use.region = "Illinois";
  use.country = "United States";
  use.countryCode = "us";
  use.deviceType = "android";
  use.appVersion = "2026.10.8-1067";
  return use;
}

s::Session SessionOf(const std::string& id, bool current, const std::string& kind,
                     std::optional<s::LastUse> lastUse, int64_t createUnixSeconds = kNow - 86400 * 6) {
  s::Session session;
  session.sessionId = id;
  session.current = current;
  session.kind = kind;
  session.createUnixSeconds = createUnixSeconds;
  session.lastUse = std::move(lastUse);
  return session;
}

// A loaded list: this session (linux) and one other (android, Chicago).
s::Snapshot TwoSessions() {
  s::Snapshot snapshot;
  snapshot.loaded = true;
  snapshot.supported = true;
  snapshot.legacyCoverage = "complete";
  s::LastUse here = Chicago(3);
  here.deviceType = "linux";
  here.appVersion = "2026.10.9";
  snapshot.sessions.push_back(SessionOf("aaaaaaaa-0000-0000-0000-000000000001", true, "password", here));
  snapshot.sessions.push_back(
      SessionOf("01a1f3c2-1111-2222-3333-444455556666", false, "google", Chicago(5 * 60)));
  snapshot.currentSessionId = "aaaaaaaa-0000-0000-0000-000000000001";
  return snapshot;
}

s::Row RowOf(const s::Snapshot& snapshot, size_t index) {
  return s::RowFor(snapshot, snapshot.sessions[index], EnglishWords(), kNow, &FakeColorHex);
}

}  // namespace

// ---- labels, logos, colours ------------------------------------------------------

UR_TEST(Sessions_EveryDeviceTypeHasItsLabelAndLogo) {
  const struct {
    const char* type;
    const char* label;
    MdiGlyph glyph;
  } cases[] = {
      {"android", "Android", MdiGlyph::DeviceAndroid},
      {"ios", "iOS", MdiGlyph::DeviceApple},
      {"macos", "macOS", MdiGlyph::DeviceApple},
      {"windows", "Windows", MdiGlyph::DeviceWindows},
      {"linux", "Linux", MdiGlyph::DeviceLinux},
      {"web", "Web", MdiGlyph::DeviceWeb},
      {"cli", "Command line", MdiGlyph::DeviceCli},
      {"server", "Server", MdiGlyph::DeviceServer},
      {"unknown", "Unknown device", MdiGlyph::DeviceUnknown},
      {"", "Unknown device", MdiGlyph::DeviceUnknown},
      {"tv", "Unknown device", MdiGlyph::DeviceUnknown},
      {"Linux", "Unknown device", MdiGlyph::DeviceUnknown},  // the server's enum is lower case
  };
  for (const auto& c : cases) {
    const s::Device device = s::DeviceFor(c.type);
    UR_EXPECT_TEXT(c.label, device.label.english);
    UR_EXPECT_TRUE_MSG(c.type, device.glyph == c.glyph);
  }
  // a session with no use observed has no device to name
  UR_EXPECT_TEXT("Unknown device", s::DeviceOf(SessionOf("x", false, "", std::nullopt)).label.english);
}

UR_TEST(Sessions_EveryKindHasItsMethodAndLegacyKindsNone) {
  const std::pair<const char*, const char*> cases[] = {
      {"password", "Password"},         {"verify", "Verification code"},
      {"apple", "Apple"},               {"google", "Google"},
      {"sso", "Single sign-on"},        {"wallet", "Wallet"},
      {"seedphrase", "Recovery phrase"}, {"signup", "New account"},
      {"auth_code", "Auth code"},       {"device_adopt", "Device pairing"},
      {"api_key_client", "API key"},
  };
  for (const auto& [kind, label] : cases) {
    const std::optional<s::Text> method = s::MethodFor(kind);
    UR_EXPECT_TRUE_MSG(kind, method.has_value());
    if (method) UR_EXPECT_TEXT(label, method->english);
  }
  for (const char* omitted : {"legacy", "legacy_proxy", "", "something_new"}) {
    UR_EXPECT_TRUE_MSG(omitted, !s::MethodFor(omitted).has_value());
  }
}

UR_TEST(Sessions_TheCircleIsTheCountrysColourOrTheUnknownOne) {
  std::string asked;
  const auto palette = [&](const std::string& code) {
    asked = code;
    return code == "us" ? std::string("3a7bd5") : code == "de" ? std::string("#AbCdEf") : "";
  };
  s::LastUse use = Chicago(0);
  UR_EXPECT_TEXT("#3A7BD5", s::CountryColorHex(use, palette));
  use.countryCode = "US";  // looked up lower case, as the sdk keys it
  UR_EXPECT_TEXT("#3A7BD5", s::CountryColorHex(use, palette));
  UR_EXPECT_TEXT("us", asked);
  use.countryCode = "de";
  UR_EXPECT_TEXT("#ABCDEF", s::CountryColorHex(use, palette));
  // empty code, no use observed, and an answer that is not a colour
  use.countryCode = "";
  UR_EXPECT_TEXT("#0099FF", s::CountryColorHex(use, palette));
  UR_EXPECT_TEXT("#0099FF", s::CountryColorHex(std::nullopt, palette));
  use.countryCode = "zz";
  UR_EXPECT_TEXT("#0099FF", s::CountryColorHex(use, palette));
  UR_EXPECT_TEXT("#0099FF", s::CountryColorHex(use, [](const std::string&) { return "nothex"; }));
  UR_EXPECT_TEXT("#0099FF", s::kUnknownCountryColorHex);
}

// ---- a row's lines (§3) ------------------------------------------------------------

UR_TEST(Sessions_ARowWithAFullLastUse) {
  const s::Snapshot snapshot = TwoSessions();
  const s::Row row = RowOf(snapshot, 1);
  UR_EXPECT_TEXT("Android · 2026.10.8-1067", row.title);
  UR_EXPECT_FALSE(row.thisSession);
  UR_EXPECT_TEXT("Chicago, Illinois, United States · Last used 5m ago", row.lastUse);
  UR_EXPECT_TEXT("DT" + std::to_string(kNow - 300), row.lastUseFull);
  UR_EXPECT_TEXT("Signed in D" + std::to_string(kNow - 86400 * 6) + " · Google · ID 01a1f3c2",
                 row.signedIn);
  UR_EXPECT_TEXT("DT" + std::to_string(kNow - 86400 * 6), row.signedInFull);
  UR_EXPECT_TEXT("Android", row.device);
  UR_EXPECT_TEXT("Chicago, Illinois, United States", row.place);
  UR_EXPECT_TEXT("Sign out Android", row.signOutName);
  UR_EXPECT_TRUE(row.glyph == MdiGlyph::DeviceAndroid);
  UR_EXPECT_TEXT("#3A7BD5", row.colorHex);
  // the full ID is kept for Copy; the line shows the first 8 characters
  UR_EXPECT_TEXT("01a1f3c2-1111-2222-3333-444455556666", row.sessionId);
}

UR_TEST(Sessions_ThisSessionIsTaggedByFlagOrById) {
  s::Snapshot snapshot = TwoSessions();
  UR_EXPECT_TRUE(RowOf(snapshot, 0).thisSession);
  UR_EXPECT_TEXT("Linux · 2026.10.9", RowOf(snapshot, 0).title);
  UR_EXPECT_TEXT("Chicago, Illinois, United States · Last used now", RowOf(snapshot, 0).lastUse);
  // the id alone marks it too
  snapshot.sessions[0].current = false;
  UR_EXPECT_TRUE(RowOf(snapshot, 0).thisSession);
  snapshot.currentSessionId.clear();
  UR_EXPECT_FALSE(RowOf(snapshot, 0).thisSession);
}

UR_TEST(Sessions_ARowWithNoLastUse) {
  s::Snapshot snapshot = TwoSessions();
  snapshot.sessions[1].lastUse.reset();
  const s::Row row = RowOf(snapshot, 1);
  UR_EXPECT_TEXT("Unknown device", row.title);  // no version to add
  UR_EXPECT_TEXT("Last use unavailable", row.lastUse);
  UR_EXPECT_TEXT("", row.lastUseFull);
  UR_EXPECT_TEXT("", row.place);
  UR_EXPECT_TRUE(row.glyph == MdiGlyph::DeviceUnknown);
  UR_EXPECT_TEXT("#0099FF", row.colorHex);
  UR_EXPECT_TEXT("Sign out Unknown device", row.signOutName);
}

UR_TEST(Sessions_ARowLeavesOutWhatIsNotKnown) {
  s::Snapshot snapshot = TwoSessions();
  s::Session& session = snapshot.sessions[1];
  session.lastUse->appVersion.clear();
  session.lastUse->city.clear();
  session.kind = "legacy";
  session.createUnixSeconds = 0;
  s::Row row = RowOf(snapshot, 1);
  UR_EXPECT_TEXT("Android", row.title);
  UR_EXPECT_TEXT("Illinois, United States · Last used 5m ago", row.lastUse);
  // no sign-in time and a legacy kind: the ID alone
  UR_EXPECT_TEXT("ID 01a1f3c2", row.signedIn);
  UR_EXPECT_TEXT("", row.signedInFull);
  // no place at all
  session.lastUse->region.clear();
  session.lastUse->country.clear();
  row = RowOf(snapshot, 1);
  UR_EXPECT_TEXT("Last used 5m ago", row.lastUse);
  UR_EXPECT_TEXT("", row.place);
}

// LastUsed.UnixTime is seconds (§3.1): three days ago reads "3d ago", and from
// seven days on the date of the use.
UR_TEST(Sessions_LastUseIsInSecondsAndBecomesADateAfterAWeek) {
  s::Snapshot snapshot = TwoSessions();
  snapshot.sessions[1].lastUse = Chicago(3 * 86400);
  UR_EXPECT_TEXT("Chicago, Illinois, United States · Last used 3d ago", RowOf(snapshot, 1).lastUse);
  snapshot.sessions[1].lastUse = Chicago(2 * 3600 + 5);
  UR_EXPECT_TEXT("Chicago, Illinois, United States · Last used 2h ago", RowOf(snapshot, 1).lastUse);
  snapshot.sessions[1].lastUse = Chicago(7 * 86400);
  UR_EXPECT_TEXT("Chicago, Illinois, United States · Last used D" + std::to_string(kNow - 7 * 86400),
                 RowOf(snapshot, 1).lastUse);
  // a use the server dated after this clock's now is "now", not negative
  snapshot.sessions[1].lastUse = Chicago(-120);
  UR_EXPECT_TEXT("Chicago, Illinois, United States · Last used now", RowOf(snapshot, 1).lastUse);
}

UR_TEST(Sessions_TheShortIdIsTheFirstEightCharacters) {
  UR_EXPECT_TEXT("01a1f3c2", s::ShortId("01a1f3c2-1111-2222-3333-444455556666"));
  UR_EXPECT_TEXT("abc", s::ShortId("abc"));
  UR_EXPECT_TEXT("", s::ShortId(""));
}

// Long places and versions are kept whole (the row wraps them); the names
// from outside the app are filtered like every other place name.
UR_TEST(Sessions_LongAndExternalTextIsShownWholeButFiltered) {
  s::Snapshot snapshot = TwoSessions();
  const std::string longCity(120, 'x');
  const std::string longVersion = "2026.10.8-1067+build." + std::string(40, '7');
  snapshot.sessions[1].lastUse->city = longCity;
  snapshot.sessions[1].lastUse->appVersion = longVersion;
  s::Row row = RowOf(snapshot, 1);
  UR_EXPECT_TEXT("Android · " + longVersion, row.title);
  UR_EXPECT_TRUE(row.lastUse.find(longCity + ", Illinois") == 0);
  // a right-to-left override in a version and a place is dropped
  snapshot.sessions[1].lastUse->appVersion = "1.0‮9.9";
  snapshot.sessions[1].lastUse->city = "Chi‮cago";
  row = RowOf(snapshot, 1);
  UR_EXPECT_TEXT("Android · 1.09.9", row.title);
  UR_EXPECT_TRUE(row.lastUse.find("Chicago, Illinois") == 0);
  // a part with nothing left is left out
  snapshot.sessions[1].lastUse->city = "​";
  UR_EXPECT_TRUE(RowOf(snapshot, 1).lastUse.find("Illinois, United States") == 0);
}

// ---- the screen (§5) ------------------------------------------------------------------

UR_TEST(Sessions_SignedOutListsNothingAndAsksNothing) {
  const s::Screen screen = s::ScreenFor(TwoSessions(), /*signedIn=*/false);
  UR_EXPECT_TRUE(screen.body == s::Body::NoSession);
  UR_EXPECT_FALSE(screen.signOutOthers);
  UR_EXPECT_TEXT("Please login to URnetwork", s::BodyText(screen.body)->english);
}

UR_TEST(Sessions_NeverLoadedShowsProgress) {
  s::Snapshot snapshot;  // the controller's first snapshot: nothing loaded
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).body == s::Body::Progress);
  snapshot.loading = true;
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).body == s::Body::Progress);
  UR_EXPECT_TEXT("Loading...", s::BodyText(s::Body::Progress)->english);
}

UR_TEST(Sessions_LoadedEmptyIsDistinctFromNeverLoaded) {
  s::Snapshot snapshot;
  snapshot.loaded = true;
  snapshot.legacyCoverage = "partial";
  const s::Screen screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TRUE(screen.body == s::Body::Empty);
  UR_EXPECT_TEXT("No active sessions", s::BodyText(screen.body)->english);
  UR_EXPECT_FALSE(screen.signOutOthers);
  UR_EXPECT_FALSE(screen.lastUsedHelp);
  // an empty list under partial coverage says why older sign-ins may be missing
  UR_EXPECT_TRUE(screen.legacyNote);
}

UR_TEST(Sessions_RowsCarryTheFooterNotes) {
  s::Snapshot snapshot = TwoSessions();
  s::Screen screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TRUE(screen.body == s::Body::Rows);
  UR_EXPECT_FALSE(s::BodyText(screen.body).has_value());
  UR_EXPECT_TRUE(screen.lastUsedHelp);
  UR_EXPECT_FALSE(screen.legacyNote);  // complete coverage
  snapshot.legacyCoverage = "partial";
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).legacyNote);
}

UR_TEST(Sessions_ARefreshKeepsTheRows) {
  s::Snapshot snapshot = TwoSessions();
  snapshot.refreshing = true;
  const s::Screen screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TRUE(screen.body == s::Body::Rows);
  UR_EXPECT_TRUE(screen.refreshing);
  UR_EXPECT_FALSE(screen.refreshFailed);
}

UR_TEST(Sessions_AFailedFirstLoadOffersTryAgain) {
  s::Snapshot snapshot;
  snapshot.error = s::Error{/*retryable=*/true, false, false, false};
  s::Screen screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TRUE(screen.body == s::Body::LoadFailed);
  UR_EXPECT_TEXT("Couldn't load sessions.", s::BodyText(screen.body)->english);
  // Try again: the retry in flight shows progress over the last failure
  snapshot.loading = true;
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).body == s::Body::Progress);
}

UR_TEST(Sessions_AFailedRefreshKeepsTheListWithANotice) {
  s::Snapshot snapshot = TwoSessions();
  snapshot.error = s::Error{true, false, false, false};
  const s::Screen screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TRUE(screen.body == s::Body::Rows);
  UR_EXPECT_TRUE(screen.refreshFailed);
  UR_EXPECT_TEXT("Couldn't refresh. Showing the last list.", s::kRefreshFailedText.english);
}

UR_TEST(Sessions_UnsupportedSaysSo) {
  s::Snapshot snapshot;
  snapshot.supported = false;
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).body == s::Body::Unsupported);
  snapshot = TwoSessions();
  snapshot.error = s::Error{false, false, /*unsupported=*/true, false};
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).body == s::Body::Unsupported);
  UR_EXPECT_TEXT("Sessions aren't available yet.", s::BodyText(s::Body::Unsupported)->english);
}

// A refused credential asks to sign in again, naming no cause (the screen's
// own line, not the signed-out pages' "Please login to URnetwork"); only the
// sdk's trusted session-revoked cause says the session was signed out from
// another device.
UR_TEST(Sessions_SignInRequiredAsksToSignInAgainUnlessTheCauseIsKnown) {
  s::Snapshot snapshot = TwoSessions();
  snapshot.error = s::Error{false, /*signInRequired=*/true, false, false};
  s::Screen screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TRUE(screen.body == s::Body::SignInRequired);
  UR_EXPECT_TEXT("sessions_sign_in_required", s::BodyText(screen.body)->key);
  UR_EXPECT_TEXT("Sign in again to manage sessions.", s::BodyText(screen.body)->english);
  UR_EXPECT_FALSE(screen.signOutOthers);
  UR_EXPECT_FALSE(screen.lastUsedHelp);
  // the same before anything loaded (a refused first load), and over a
  // retryable flag
  s::Snapshot first;
  first.error = s::Error{/*retryable=*/true, /*signInRequired=*/true, false, false};
  UR_EXPECT_TRUE(s::ScreenFor(first, true).body == s::Body::SignInRequired);
  // the trusted cause
  snapshot.error->sessionRevoked = true;
  screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TRUE(screen.body == s::Body::SignedOutRemotely);
  UR_EXPECT_TEXT("sessions_signed_out_remotely", s::BodyText(screen.body)->key);
  UR_EXPECT_TEXT("This session was signed out from another device.",
                 s::BodyText(screen.body)->english);
  // signed out, the page keeps the signed-out pages' line
  UR_EXPECT_TEXT("please_login_to_urnetwork", s::BodyText(s::Body::NoSession)->key);
}

UR_TEST(Sessions_SignOutAllOthersNeedsThisSessionAndAnother) {
  s::Snapshot snapshot = TwoSessions();
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).signOutOthers);
  // this session alone
  s::Snapshot alone = snapshot;
  alone.sessions.pop_back();
  UR_EXPECT_FALSE(s::ScreenFor(alone, true).signOutOthers);
  // others, but none of them this one
  s::Snapshot noCurrent = snapshot;
  noCurrent.sessions.erase(noCurrent.sessions.begin());
  noCurrent.currentSessionId = "not-listed";
  UR_EXPECT_FALSE(s::ScreenFor(noCurrent, true).signOutOthers);
  // the bulk action's own state
  snapshot.bulkAction = s::Action{"", /*loading=*/false, /*pending=*/true, std::nullopt};
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).signOutOthersView == s::ActionView::Busy);
  snapshot.bulkAction = s::Action{"", false, false, s::Error{true, false, false, false}};
  UR_EXPECT_TRUE(s::ScreenFor(snapshot, true).signOutOthersView == s::ActionView::Failed);
}

// A failed Sign out all other sessions says so under the button, in its own
// words (the row's line says "this session"), and leaves the button on: the
// controller is asked again from it (SessionsBindingTest).
UR_TEST(Sessions_AFailedSignOutOfTheOthersSaysSoUnderTheButton) {
  s::Snapshot snapshot = TwoSessions();
  snapshot.bulkAction = s::Action{"", false, false, s::Error{/*retryable=*/true, false, false, false}};
  s::Screen screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TRUE(screen.signOutOthers);
  UR_EXPECT_TRUE(screen.signOutOthersView == s::ActionView::Failed);
  std::optional<s::Text> line = s::ActionStatusText(screen.signOutOthersView, /*others=*/true);
  UR_EXPECT_TRUE(line.has_value());
  UR_EXPECT_TEXT("sessions_sign_out_others_failed", line->key);
  UR_EXPECT_TEXT("Couldn't sign out the other sessions. Try again.", line->english);
  // whatever refused it: a refused credential, an unsupported server
  for (const s::Error& error : {s::Error{false, /*signInRequired=*/true, false, false},
                                s::Error{false, false, /*unsupported=*/true, false},
                                s::Error{false, false, false, false}}) {
    snapshot.bulkAction->error = error;
    screen = s::ScreenFor(snapshot, true);
    UR_EXPECT_TRUE(screen.signOutOthersView == s::ActionView::Failed);
    UR_EXPECT_TEXT("sessions_sign_out_others_failed",
                   s::ActionStatusText(screen.signOutOthersView, true)->key);
  }
  // in flight again: "Signing out…" instead; settled: nothing
  snapshot.bulkAction->loading = true;
  screen = s::ScreenFor(snapshot, true);
  UR_EXPECT_TEXT("Signing out…", s::ActionStatusText(screen.signOutOthersView, true)->english);
  snapshot.bulkAction.reset();
  UR_EXPECT_FALSE(s::ActionStatusText(s::ScreenFor(snapshot, true).signOutOthersView, true).has_value());
  // a row's own failure keeps the row's words
  UR_EXPECT_TEXT("sessions_action_failed", s::ActionStatusText(s::ActionView::Failed, false)->key);
  UR_EXPECT_TEXT("Signing out…", s::ActionStatusText(s::ActionView::Busy, false)->english);
  UR_EXPECT_FALSE(s::ActionStatusText(s::ActionView::Idle, false).has_value());
}

// ---- actions (§4) --------------------------------------------------------------------

UR_TEST(Sessions_AnActionIsBusyWhileLoadingOrPending) {
  UR_EXPECT_TRUE(s::ActionViewOf(std::nullopt) == s::ActionView::Idle);
  UR_EXPECT_TRUE(s::ActionViewOf(s::Action{"a", true, false, std::nullopt}) == s::ActionView::Busy);
  // 202: accepted, not confirmed enforced, still busy
  UR_EXPECT_TRUE(s::ActionViewOf(s::Action{"a", false, true, std::nullopt}) == s::ActionView::Busy);
  const s::Error failure{true, false, false, false};
  UR_EXPECT_TRUE(s::ActionViewOf(s::Action{"a", false, false, failure}) == s::ActionView::Failed);
  // a retry in flight is busy whatever the last attempt said
  UR_EXPECT_TRUE(s::ActionViewOf(s::Action{"a", true, false, failure}) == s::ActionView::Busy);
  UR_EXPECT_TRUE(s::ActionViewOf(s::Action{"a", false, false, std::nullopt}) == s::ActionView::Idle);

  s::Snapshot snapshot = TwoSessions();
  snapshot.actions.push_back(s::Action{"01a1f3c2-1111-2222-3333-444455556666", false, true, std::nullopt});
  UR_EXPECT_TRUE(s::ActionFor(snapshot, "01a1f3c2-1111-2222-3333-444455556666").has_value());
  UR_EXPECT_FALSE(s::ActionFor(snapshot, "aaaaaaaa-0000-0000-0000-000000000001").has_value());
  UR_EXPECT_FALSE(s::ActionFor(snapshot, "").has_value());
  UR_EXPECT_TEXT("Signing out…", s::kSigningOutText.english);
  UR_EXPECT_TEXT("Couldn't sign out this session. Try again.", s::kActionFailedText.english);
}

UR_TEST(Sessions_ConfirmationsNameTheSession) {
  const s::Snapshot snapshot = TwoSessions();
  const s::Words words = EnglishWords();
  s::Confirmation other = s::ConfirmationFor(snapshot, snapshot.sessions[1], words);
  UR_EXPECT_FALSE(other.self);
  UR_EXPECT_FALSE(other.target.others);
  UR_EXPECT_TEXT("01a1f3c2-1111-2222-3333-444455556666", other.target.sessionId);
  UR_EXPECT_TEXT("Sign out this session?", other.title.english);
  UR_EXPECT_TEXT("Android in Chicago, Illinois, United States will be signed out.",
                 s::ConfirmationBody(other, words));
  // no place known
  s::Session nowhere = snapshot.sessions[1];
  nowhere.lastUse->city.clear();
  nowhere.lastUse->region.clear();
  nowhere.lastUse->country.clear();
  UR_EXPECT_TEXT("Android will be signed out.",
                 s::ConfirmationBody(s::ConfirmationFor(snapshot, nowhere, words), words));
  // no use at all: the unknown device
  nowhere.lastUse.reset();
  UR_EXPECT_TEXT("Unknown device will be signed out.",
                 s::ConfirmationBody(s::ConfirmationFor(snapshot, nowhere, words), words));
  // this session warns that this app signs out
  const s::Confirmation self = s::ConfirmationFor(snapshot, snapshot.sessions[0], words);
  UR_EXPECT_TRUE(self.self);
  UR_EXPECT_TEXT("This is the session you're using. This app will be signed out.",
                 s::ConfirmationBody(self, words));
  // every other session, never "all devices"
  const s::Confirmation others = s::OthersConfirmation();
  UR_EXPECT_TRUE(others.target.others);
  UR_EXPECT_TEXT("Sign out all other sessions?", others.title.english);
  const std::string body = s::ConfirmationBody(others, words);
  UR_EXPECT_TRUE(body.find("Every other session in this list will be signed out.") == 0);
  UR_EXPECT_TRUE(body.find("all devices") == std::string::npos);
}

// The sign-out gestures: a press opens one confirmation, Cancel sends nothing,
// the confirmation's Sign out sends once.
UR_TEST(Sessions_APressConfirmsBeforeAnythingIsSent) {
  const s::Snapshot snapshot = TwoSessions();
  const s::Words words = EnglishWords();
  s::SignOutFlow flow;
  const s::Target other{false, "01a1f3c2-1111-2222-3333-444455556666"};
  std::optional<s::Confirmation> confirmation = flow.Press(snapshot, other, words);
  UR_EXPECT_TRUE(confirmation.has_value() && flow.Open());
  // Cancel: nothing to send
  flow.Cancel();
  UR_EXPECT_FALSE(flow.Open());
  UR_EXPECT_FALSE(flow.Confirm().has_value());
  // pressed again and confirmed: the target, once
  confirmation = flow.Press(snapshot, other, words);
  UR_EXPECT_TRUE(confirmation.has_value());
  const std::optional<s::Target> sent = flow.Confirm();
  UR_EXPECT_TRUE(sent.has_value() && !sent->others && sent->sessionId == other.sessionId);
  UR_EXPECT_FALSE(flow.Confirm().has_value());  // a second press of Sign out sends nothing
}

UR_TEST(Sessions_DuplicateActivationIsSuppressed) {
  s::Snapshot snapshot = TwoSessions();
  const s::Words words = EnglishWords();
  s::SignOutFlow flow;
  const s::Target other{false, "01a1f3c2-1111-2222-3333-444455556666"};
  const s::Target self{false, "aaaaaaaa-0000-0000-0000-000000000001"};
  // one confirmation at a time: a second press, on any row, opens nothing
  UR_EXPECT_TRUE(flow.Press(snapshot, other, words).has_value());
  UR_EXPECT_FALSE(flow.Press(snapshot, other, words).has_value());
  UR_EXPECT_FALSE(flow.Press(snapshot, self, words).has_value());
  flow.Cancel();
  // a row already signing out (loading, or 202 pending) opens nothing
  snapshot.actions.push_back(s::Action{other.sessionId, false, true, std::nullopt});
  UR_EXPECT_FALSE(flow.Press(snapshot, other, words).has_value());
  UR_EXPECT_FALSE(flow.Open());
  // ...but a failed one can be tried again
  snapshot.actions.back() = s::Action{other.sessionId, false, false, s::Error{true, false, false, false}};
  UR_EXPECT_TRUE(flow.Press(snapshot, other, words).has_value());
  flow.Cancel();
  // a session no longer listed (signed out elsewhere) opens nothing
  UR_EXPECT_FALSE(flow.Press(snapshot, s::Target{false, "gone"}, words).has_value());
  UR_EXPECT_FALSE(flow.Press(snapshot, s::Target{false, ""}, words).has_value());
}

UR_TEST(Sessions_SelfSignOutConfirmsWithTheSelfBody) {
  const s::Snapshot snapshot = TwoSessions();
  s::SignOutFlow flow;
  const std::optional<s::Confirmation> confirmation =
      flow.Press(snapshot, s::Target{false, "aaaaaaaa-0000-0000-0000-000000000001"}, EnglishWords());
  UR_EXPECT_TRUE(confirmation.has_value() && confirmation->self);
  UR_EXPECT_TEXT("sessions_confirm_self_body", confirmation->body.key);
}

UR_TEST(Sessions_SignOutAllOthersOnlyWhenOffered) {
  s::Snapshot snapshot = TwoSessions();
  const s::Words words = EnglishWords();
  s::SignOutFlow flow;
  const s::Target others{true, ""};
  const std::optional<s::Confirmation> confirmation = flow.Press(snapshot, others, words);
  UR_EXPECT_TRUE(confirmation.has_value() && confirmation->target.others);
  const std::optional<s::Target> sent = flow.Confirm();
  UR_EXPECT_TRUE(sent.has_value() && sent->others);
  // not offered: this session alone, or the bulk action already running
  s::Snapshot alone = snapshot;
  alone.sessions.pop_back();
  UR_EXPECT_FALSE(flow.Press(alone, others, words).has_value());
  snapshot.bulkAction = s::Action{"", true, false, std::nullopt};
  UR_EXPECT_FALSE(flow.Press(snapshot, others, words).has_value());
  UR_EXPECT_FALSE(flow.Open());
}

// ---- every word is the catalog's ----------------------------------------------------

UR_TEST(Sessions_EveryKeyAndEnglishIsTheCatalogs) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/../po/en.po", std::ios::binary);
  UR_EXPECT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string catalog = buffer.str();
  std::vector<s::Text> texts = {
      s::kTitleText,        s::kThisSessionText,   s::kLastUsedText,      s::kLastUseUnavailableText,
      s::kSignedInText,     s::kIdText,            s::kCopyIdText,        s::kCopiedText,
      s::kSignOutText,      s::kSignOutNamedText,  s::kSignOutOthersText, s::kCancelText,
      s::kSigningOutText,   s::kActionFailedText,  s::kOthersFailedText,  s::kRefreshText,
      s::kTryAgainText,     s::kRefreshFailedText, s::kLastUsedHelpText,  s::kLegacyNoteText,
      s::kAccountText,      s::kLoadingText,
  };
  for (const char* type : {"android", "ios", "macos", "windows", "linux", "web", "cli", "server", ""}) {
    texts.push_back(s::DeviceFor(type).label);
  }
  for (const char* kind : {"password", "verify", "apple", "google", "sso", "wallet", "seedphrase",
                           "signup", "auth_code", "device_adopt", "api_key_client"}) {
    texts.push_back(*s::MethodFor(kind));
  }
  for (s::Body body : {s::Body::NoSession, s::Body::Progress, s::Body::Empty, s::Body::LoadFailed,
                       s::Body::Unsupported, s::Body::SignInRequired, s::Body::SignedOutRemotely}) {
    texts.push_back(*s::BodyText(body));
  }
  for (s::ActionView view : {s::ActionView::Busy, s::ActionView::Failed}) {
    for (bool others : {false, true}) texts.push_back(*s::ActionStatusText(view, others));
  }
  const s::Snapshot snapshot = TwoSessions();
  for (const s::Confirmation& confirmation :
       {s::ConfirmationFor(snapshot, snapshot.sessions[0], EnglishWords()),
        s::ConfirmationFor(snapshot, snapshot.sessions[1], EnglishWords()),
        s::OthersConfirmation()}) {
    texts.push_back(confirmation.title);
    texts.push_back(confirmation.body);
  }
  s::Session nowhere = snapshot.sessions[1];
  nowhere.lastUse.reset();
  texts.push_back(s::ConfirmationFor(snapshot, nowhere, EnglishWords()).body);
  for (const s::Text& text : texts) {
    const std::string entry =
        std::string("msgctxt \"") + text.key + "\"\nmsgid \"" + text.english + "\"\n";
    UR_EXPECT_TRUE_MSG(entry, catalog.find(entry) != std::string::npos);
  }
}
