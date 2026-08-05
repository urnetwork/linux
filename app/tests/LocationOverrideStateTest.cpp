// Port of the android MockLocationStateTest.kt (13 cases), with the android
// gates mapped onto the linux ones: developer options -> GeoClue installed,
// mock app selection -> static source enabled, location master switch ->
// privileged writer available. Plus the /etc/geolocation file contract shared
// with the packaging (the marker line purge/uninstall grep for).
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <clocale>
#include <string>
#include <vector>

#include "LocationOverrideState.hpp"

using urnw::GeolocationContentsAreOurs;
using urnw::kGeolocationMarker;
using urnw::LocationOverrideState;
using urnw::LocationOverrideStatus;
using urnw::LocationOverrideTarget;
using urnw::RenderGeolocationFileContents;
using urnw::ResolveLocationOverrideStatus;

namespace {

// Case-insensitive substring, mirroring the packaging's `grep -qi urnetwork`.
bool ContainsNoCase(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  auto lower = [](std::string s) {
    for (char& c : s) {
      if ('A' <= c && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
  };
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

size_t CountLines(const std::string& s) {
  size_t n = 0;
  for (const char c : s) {
    if (c == '\n') ++n;
  }
  return n;
}

// The value lines GeoClue actually parses: everything that is not a comment.
// (The comment header legitimately contains commas -- "latitude, longitude,
// ..." -- so the C-locale assertion must apply to these lines alone.)
std::vector<std::string> ValueLines(const std::string& contents) {
  std::vector<std::string> lines;
  std::string current;
  for (const char c : contents) {
    if (c == '\n') {
      if (!current.empty() && current[0] != '#') lines.push_back(current);
      current.clear();
      continue;
    }
    current += c;
  }
  if (!current.empty() && current[0] != '#') lines.push_back(current);
  return lines;
}

// The android test's named-default helper, spelled out as a struct so each case
// overrides exactly the signals it cares about.
struct ResolveArgs {
  bool enabled = true;
  bool geoclueInstalled = true;
  bool staticSourceEnabled = true;
  bool writerAvailable = true;
  bool tunnelUp = true;
  bool hasTarget = true;  // "tokyo"
  bool orphaned = false;
};

LocationOverrideStatus Resolve(const ResolveArgs& args) {
  return ResolveLocationOverrideStatus(args.enabled, args.geoclueInstalled,
                                       args.staticSourceEnabled, args.writerAvailable,
                                       args.tunnelUp, args.hasTarget, args.orphaned);
}

LocationOverrideState State(bool geoclueInstalled = true, bool staticSourceEnabled = true,
                            bool writerAvailable = true) {
  LocationOverrideState state;
  state.status = LocationOverrideStatus::Disabled;
  state.enabled = false;
  state.hasTarget = false;
  state.geoclueInstalled = geoclueInstalled;
  state.staticSourceEnabled = staticSourceEnabled;
  state.writerAvailable = writerAvailable;
  return state;
}

}  // namespace

UR_TEST(disabledWinsOverEverySignalWhenNotOrphaned) {
  ResolveArgs off;
  off.enabled = false;
  UR_EXPECT_TRUE(Resolve(off) == LocationOverrideStatus::Disabled);

  // gates are not reported while the toggle is off
  ResolveArgs nothingSetUp;
  nothingSetUp.enabled = false;
  nothingSetUp.geoclueInstalled = false;
  nothingSetUp.staticSourceEnabled = false;
  nothingSetUp.writerAvailable = false;
  nothingSetUp.tunnelUp = false;
  nothingSetUp.hasTarget = false;
  UR_EXPECT_TRUE(Resolve(nothingSetUp) == LocationOverrideStatus::Disabled);
}

UR_TEST(orphanedWinsEvenWhenDisabled) {
  // the flag only clears on a successful removal; until then the user must see
  // the recovery instructions regardless of the toggle
  ResolveArgs args;
  args.enabled = false;
  args.orphaned = true;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::Orphaned);
}

UR_TEST(orphanedWinsOverActiveConditions) {
  ResolveArgs args;
  args.orphaned = true;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::Orphaned);
}

UR_TEST(orphanedWinsOverGates) {
  ResolveArgs args;
  args.geoclueInstalled = false;
  args.staticSourceEnabled = false;
  args.writerAvailable = false;
  args.orphaned = true;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::Orphaned);
}

UR_TEST(geoclueGateComesFirst) {
  ResolveArgs args;
  args.geoclueInstalled = false;
  args.staticSourceEnabled = false;
  args.writerAvailable = false;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::NeedsGeoClue);
}

UR_TEST(staticSourceGateComesSecond) {
  ResolveArgs args;
  args.staticSourceEnabled = false;
  args.writerAvailable = false;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::NeedsStaticSource);
}

UR_TEST(privilegeGateComesThird) {
  ResolveArgs args;
  args.writerAvailable = false;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::NeedsPrivilege);
}

UR_TEST(eligibleWhenNoTunnelAndNoTarget) {
  ResolveArgs args;
  args.tunnelUp = false;
  args.hasTarget = false;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::Eligible);
}

UR_TEST(targetPresentButTunnelDownIsEligible) {
  ResolveArgs args;
  args.tunnelUp = false;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::Eligible);
}

UR_TEST(tunnelUpButNoTargetIsEligible) {
  ResolveArgs args;
  args.hasTarget = false;
  UR_EXPECT_TRUE(Resolve(args) == LocationOverrideStatus::Eligible);
}

UR_TEST(activeOnlyWhenTunnelUpAndTargetPresent) {
  UR_EXPECT_TRUE(Resolve(ResolveArgs{}) == LocationOverrideStatus::Active);
}

// The toggle opens the setup guide only when setup is incomplete, and the guide
// marks its steps from these signals. Both must stay readable while the feature
// is off, when `status` is Disabled no matter how the machine is configured.
UR_TEST(setupCompleteIsReportedWhileTheFeatureIsOff) {
  const LocationOverrideState complete = State();
  UR_EXPECT_TRUE(complete.status == LocationOverrideStatus::Disabled);
  UR_EXPECT_TRUE(complete.setupComplete());
}

UR_TEST(setupIsIncompleteWhenAnySignalIsMissing) {
  UR_EXPECT_FALSE(State(/*geoclueInstalled=*/false).setupComplete());
  UR_EXPECT_FALSE(State(true, /*staticSourceEnabled=*/false).setupComplete());
  UR_EXPECT_FALSE(State(true, true, /*writerAvailable=*/false).setupComplete());
}

// ---- /etc/geolocation file contract (shared with the packaging) -------------

// THE UNINSTALL CONTRACT. `postrm purge` and `uninstall.sh` delete
// /etc/geolocation only when it carries the URnetwork marker, so a file an
// admin wrote is never destroyed. If a write ever stops emitting it, purge
// silently stops cleaning up and an uninstalling user keeps a spoofed system
// location PERMANENTLY -- nothing else on the system reverts that file. This
// is the test that makes that failure loud.
UR_TEST(geolocationFileCarriesTheUninstallMarker) {
  const std::string contents = RenderGeolocationFileContents(35.6762, 139.6503, 0.0, 5000.0);
  // the marker is present, and is the FIRST line (what the daemon's startup
  // cleaner keys on)
  UR_EXPECT_TRUE(contents.find(kGeolocationMarker) == 0);
  UR_EXPECT_TRUE(GeolocationContentsAreOurs(contents));
  // and it satisfies the packaging's actual test: `grep -qi 'urnetwork'`
  UR_EXPECT_TRUE(ContainsNoCase(contents, "urnetwork"));
}

UR_TEST(geolocationContentsAreOursRejectsForeignFiles) {
  // a hand-written admin file must never be treated as ours (the daemon's
  // startup cleaner would otherwise delete it)
  UR_EXPECT_FALSE(GeolocationContentsAreOurs("51.5\n-0.12\n0\n100\n"));
  UR_EXPECT_FALSE(GeolocationContentsAreOurs("# set by the site admin\n51.5\n"));
  UR_EXPECT_FALSE(GeolocationContentsAreOurs(""));
  // the marker must be at the START, not merely somewhere in the file
  UR_EXPECT_FALSE(
      GeolocationContentsAreOurs(std::string("# admin header\n") + kGeolocationMarker));
}

// GeoClue's static source parses exactly four values, in order, and rejects a
// comma decimal separator -- which is what a locale-sensitive formatter would
// emit under fr_FR/de_DE.
UR_TEST(geolocationFileIsFourCLocaleValuesAfterTheHeader) {
  const std::string contents = RenderGeolocationFileContents(-33.8688, 151.2093, 0.0, 5000.0);
  // exactly four value lines, in GeoClue's order: lat, lon, altitude, accuracy
  const std::vector<std::string> values = ValueLines(contents);
  UR_EXPECT_EQ(4u, values.size());
  if (values.size() == 4) {
    UR_EXPECT_TRUE(values[0] == "-33.868800");
    UR_EXPECT_TRUE(values[1] == "151.209300");
    UR_EXPECT_TRUE(values[2] == "0.0");
    UR_EXPECT_TRUE(values[3] == "5000.0");
  }
  // no comma decimal separator in the parsed lines (the comment header may
  // and does contain prose commas)
  for (const auto& value : values) {
    UR_EXPECT_TRUE_MSG(value, value.find(',') == std::string::npos);
  }
  // 3 comment lines + the 4 values, each newline-terminated
  UR_EXPECT_EQ(7u, CountLines(contents));
  UR_EXPECT_TRUE(!contents.empty() && contents.back() == '\n');
}

UR_TEST(geolocationFileStaysCLocaleUnderACommaLocale) {
  // Only asserts something when the host actually has a comma-separator
  // locale; where it does, this is the regression that would otherwise ship
  // (GeoClue rejects every line of a de_DE-formatted file).
  const char* previous = std::setlocale(LC_NUMERIC, nullptr);
  const std::string saved = previous != nullptr ? previous : "C";
  if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr ||
      std::setlocale(LC_NUMERIC, "fr_FR.UTF-8") != nullptr) {
    const std::string contents = RenderGeolocationFileContents(48.8566, 2.3522, 0.0, 5000.0);
    for (const auto& value : ValueLines(contents)) {
      UR_EXPECT_TRUE_MSG(value, value.find(',') == std::string::npos);
    }
    UR_EXPECT_TRUE(contents.find("48.856600") != std::string::npos);
  }
  std::setlocale(LC_NUMERIC, saved.c_str());
}
