// The provider extender row's reading of ExtenderProvideStatus (EXTENDER.md
// N3, N7) and the earnings page's statistics sections (O8). The texts are
// composed from the English sources, which ExtenderProvide_KeysAreTheCatalogs
// pins against po/en.po.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ExtenderProvidePresentation.hpp"

using urnw::extender::ProvideDot;
using urnw::extender::ProvideRow;
using urnw::extender::ProvideRowFor;
using urnw::extender::ProvideRowGuess;
using urnw::extender::ProvideRowOf;
using urnw::extender::SpeedTestsRow;
using urnw::extender::SpeedTestsRowOf;
using urnw::extender::StatsSections;
using urnw::extender::StatsSectionsFor;

#define UR_EXPECT_TEXT(expected, actual)                                                    \
  do {                                                                                      \
    const std::string ur_e_ = (expected);                                                   \
    const std::string ur_a_ = (actual);                                                     \
    if (ur_e_ != ur_a_)                                                                     \
      UR_FAIL(std::string(#actual) + ": expected \"" + ur_e_ + "\", got \"" + ur_a_ + "\""); \
  } while (0)

namespace {

// The catalog with no translation loaded: every key reads its English source.
std::string English(const char* /*key*/, const char* english) { return english; }

// The store's single {} substitution (I18n.hpp's Format with one argument).
std::string FormatOne(const std::string& pattern, const std::string& argument) {
  const size_t at = pattern.find("{}");
  if (at == std::string::npos) return pattern;
  return pattern.substr(0, at) + argument + pattern.substr(at + 2);
}

std::string TextOf(const ProvideRow& row) {
  return urnw::extender::StateTextFor(row, &English, &FormatOne);
}

// A supported status as the SDK sends it, with the setting read beside it.
ProvideRow Row(const std::string& state, const std::string& errorCase = std::string(),
               const std::string& reason = std::string(), bool activatedV4 = false,
               bool activatedV6 = false, bool refused = false, bool provideExtender = true) {
  return ProvideRowFor(true, true, state, errorCase, reason, activatedV4, activatedV6, refused,
                       provideExtender);
}

// Every field of urnet::ExtenderProvideStatus, by the SDK's names, so the
// reading is tested with the whole status a widget holds.
struct FullStatus {
  bool Supported{};
  std::string State{};
  std::string ErrorCase{};
  std::string Reason{};
  bool Enabled{};
  std::string StartError{};
  std::string TcpUnavailableError{};
  bool Listening{};
  std::string ListenError{};
  bool ActivatedV4{};
  bool ActivatedV6{};
  std::string Ipv4{};
  std::string Ipv6{};
  int64_t LastActivationTime{};
  std::string LastActivationError{};
  bool LastActivationRefused{};
  int64_t RevokedTime{};
  std::string DnsPorts{};
  int64_t PeerPingCount{};
  int64_t PeerPingCosignedCount{};
  int64_t PeerPingRejectedCount{};
  int64_t PeerPingUnknownCount{};
  int64_t LastPeerPingTime{};
  int64_t LimitedBySubnetsCount{};
  int64_t LimitedBySourceCount{};
  int64_t ConnectionCount{};
};

// A supported status with the fields the reading takes; the other nineteen stay
// at their zero values.
FullStatus SupportedStatus(const std::string& state, const std::string& errorCase = std::string(),
                           const std::string& reason = std::string(), bool activatedV4 = false,
                           bool activatedV6 = false, bool refused = false) {
  FullStatus status;
  status.Supported = true;
  status.State = state;
  status.ErrorCase = errorCase;
  status.Reason = reason;
  status.ActivatedV4 = activatedV4;
  status.ActivatedV6 = activatedV6;
  status.LastActivationRefused = refused;
  return status;
}

ProvideRow RowOf(const FullStatus& status, bool setting) {
  return ProvideRowOf(std::optional<FullStatus>(status), [setting] { return setting; });
}

}  // namespace

// No status (no device) is not "off": the row is hidden, not drawn grey.
UR_TEST(ExtenderProvide_NoStatusIsHidden) {
  const ProvideRow row = ProvideRowFor(false, true, "active", "", "", true, true, false, true);
  UR_EXPECT_FALSE(row.visible);
  UR_EXPECT_TRUE(TextOf(row).empty());
}

// An unsupported role is hidden, never disabled, whatever else the status
// says -- and a hidden row reads nothing else, so every hidden push compares
// equal and the widget draws nothing for it.
UR_TEST(ExtenderProvide_UnsupportedIsHiddenInEveryState) {
  for (const char* state : {"off", "not_providing", "setting_up", "active", "error", ""}) {
    const ProvideRow row = ProvideRowFor(true, false, state, "listen", "tcp: bind: permission denied",
                                         true, true, true, true);
    UR_EXPECT_TRUE_MSG(state, !row.visible);
    UR_EXPECT_TRUE_MSG(state, TextOf(row).empty());
    UR_EXPECT_TRUE_MSG(state, row == ProvideRow());
  }
}

UR_TEST(ExtenderProvide_OffIsGreyOff) {
  const ProvideRow row = Row("off");
  UR_EXPECT_TRUE(row.visible);
  UR_EXPECT_TRUE(row.dot == ProvideDot::Grey);
  UR_EXPECT_TEXT("off", row.textKey);
  UR_EXPECT_TEXT("Off", TextOf(row));
  UR_EXPECT_FALSE(row.errorText);
}

UR_TEST(ExtenderProvide_NotProvidingIsGrey) {
  const ProvideRow row = Row("not_providing");
  UR_EXPECT_TRUE(row.dot == ProvideDot::Grey);
  UR_EXPECT_TEXT("extender_not_providing", row.textKey);
  UR_EXPECT_TEXT("Not providing", TextOf(row));
  UR_EXPECT_FALSE(row.errorText);
}

UR_TEST(ExtenderProvide_SettingUpIsYellow) {
  const ProvideRow row = Row("setting_up");
  UR_EXPECT_TRUE(row.dot == ProvideDot::Yellow);
  UR_EXPECT_TEXT("extender_setting_up", row.textKey);
  UR_EXPECT_TEXT("Setting up", TextOf(row));
  UR_EXPECT_FALSE(row.errorText);
}

// The three family texts, from the two activation flags.
UR_TEST(ExtenderProvide_ActiveNamesTheFamilies) {
  const ProvideRow both = Row("active", "", "", true, true);
  UR_EXPECT_TRUE(both.dot == ProvideDot::Green);
  UR_EXPECT_FALSE(both.errorText);
  UR_EXPECT_TEXT("extender_active", both.textKey);
  UR_EXPECT_TEXT("ipv4_and_ipv6", both.familiesKey);
  UR_EXPECT_TEXT("Active · IPv4 and IPv6", TextOf(both));

  const ProvideRow v4 = Row("active", "", "", true, false);
  UR_EXPECT_TEXT("ipv4", v4.familiesKey);
  UR_EXPECT_TEXT("Active · IPv4", TextOf(v4));

  const ProvideRow v6 = Row("active", "", "", false, true);
  UR_EXPECT_TEXT("ipv6", v6.familiesKey);
  UR_EXPECT_TEXT("Active · IPv6", TextOf(v6));
}

// Active with the other family's last attempt refused: the refusal follows on
// the same line, the dot stays green and the line stays muted.
UR_TEST(ExtenderProvide_ActiveCarriesTheOtherFamilysRefusal) {
  const ProvideRow row =
      Row("active", "", "the operator refused the activation", true, false, true);
  UR_EXPECT_TRUE(row.dot == ProvideDot::Green);
  UR_EXPECT_FALSE(row.errorText);
  UR_EXPECT_TEXT("extender_activation_refused", row.detailKey);
  UR_EXPECT_TEXT("Active · IPv4 · Activation refused: the operator refused the activation",
                 TextOf(row));
}

UR_TEST(ExtenderProvide_ActiveCarriesTheOtherFamilysFailure) {
  const ProvideRow row = Row("active", "",
                             "dial tcp6 [2001:db8::1]:443: connect: network is unreachable", false,
                             true, false);
  UR_EXPECT_TRUE(row.dot == ProvideDot::Green);
  UR_EXPECT_FALSE(row.errorText);
  UR_EXPECT_TEXT("extender_activation_failed", row.detailKey);
  UR_EXPECT_TEXT(
      "Active · IPv6 · Activation failed: dial tcp6 [2001:db8::1]:443: connect: network is unreachable",
      TextOf(row));
}

// An active reading with no Reason carries no second clause.
UR_TEST(ExtenderProvide_ActiveWithoutAReasonHasNoDetail) {
  const ProvideRow row = Row("active", "", "", true, true, true);
  UR_EXPECT_TEXT("", row.detailKey);
  UR_EXPECT_TEXT("Active · IPv4 and IPv6", TextOf(row));
}

// Every error case is red, in the error color, and labeled by ErrorCase alone:
// neither the refused flag nor the activation flags move the label.
UR_TEST(ExtenderProvide_EachErrorCaseByErrorCaseAlone) {
  struct Case {
    const char* errorCase;
    const char* reason;
    const char* key;
    const char* text;
  };
  const std::vector<Case> cases = {
      {"revoked", "", "extender_revoked", "Revoked by the operator"},
      {"start", "no extender directory in this network space", "extender_start_failed",
       "Could not start: no extender directory in this network space"},
      {"tcp_unavailable", "listen tcp :443: bind: address already in use",
       "extender_tcp_unavailable",
       "TCP port 443 is in use by another program. Trying again every few minutes: listen tcp "
       ":443: bind: address already in use"},
      {"listen", "tcp: bind: permission denied; quic: bind: permission denied",
       "extender_listen_failed",
       "Could not listen: tcp: bind: permission denied; quic: bind: permission denied"},
      {"activation_refused", "not reachable from the operator", "extender_activation_refused",
       "Activation refused: not reachable from the operator"},
      {"activation_unreachable", "the tcp carrier failed after 0.05 s: no route to host",
       "extender_activation_unreachable",
       "The internet can't reach this device on TCP port 443. Allow or forward TCP 443 (and UDP 443 and 4053), or turn Extender off: the tcp carrier failed after 0.05 s: no route to host"},
      {"activation_failed", "context deadline exceeded", "extender_activation_failed",
       "Activation failed: context deadline exceeded"},
  };
  for (const auto& c : cases) {
    for (bool refused : {false, true}) {
      for (bool families : {false, true}) {
        const ProvideRow row = Row("error", c.errorCase, c.reason, families, families, refused);
        UR_EXPECT_TRUE_MSG(c.errorCase, row.visible);
        UR_EXPECT_TRUE_MSG(c.errorCase, row.dot == ProvideDot::Red);
        UR_EXPECT_TRUE_MSG(c.errorCase, row.errorText);
        UR_EXPECT_TEXT(c.key, row.textKey);
        UR_EXPECT_TEXT(c.text, TextOf(row));
        UR_EXPECT_TEXT("", row.familiesKey);
        UR_EXPECT_TEXT("", row.detailKey);
      }
    }
  }
}

// A refusal and a request error with the same text are two readings.
UR_TEST(ExtenderProvide_RefusalAndFailureAreDistinct) {
  const ProvideRow refused = Row("error", "activation_refused", "probe failed");
  const ProvideRow failed = Row("error", "activation_failed", "probe failed");
  UR_EXPECT_TRUE(refused != failed);
  UR_EXPECT_TEXT("Activation refused: probe failed", TextOf(refused));
  UR_EXPECT_TEXT("Activation failed: probe failed", TextOf(failed));
}

// An error case this build does not know renders the Reason bare, in the error
// color; so does an error that names no case.
UR_TEST(ExtenderProvide_UnknownErrorCaseRendersTheReasonBare) {
  const ProvideRow unknown = Row("error", "quota", "the operator is over its quota");
  UR_EXPECT_TRUE(unknown.visible);
  UR_EXPECT_TRUE(unknown.dot == ProvideDot::Red);
  UR_EXPECT_TRUE(unknown.errorText);
  UR_EXPECT_TEXT("", unknown.textKey);
  UR_EXPECT_TEXT("the operator is over its quota", TextOf(unknown));

  const ProvideRow noCase = Row("error", "", "something the device said");
  UR_EXPECT_TRUE(noCase.dot == ProvideDot::Red);
  UR_EXPECT_TRUE(noCase.errorText);
  UR_EXPECT_TEXT("something the device said", TextOf(noCase));
}

// A state this build does not know claims no color and names no case: grey,
// the Reason bare, muted.
UR_TEST(ExtenderProvide_UnknownStateIsGreyWithTheReasonBare) {
  const ProvideRow row = Row("paused", "", "held by the operator");
  UR_EXPECT_TRUE(row.visible);
  UR_EXPECT_TRUE(row.dot == ProvideDot::Grey);
  UR_EXPECT_FALSE(row.errorText);
  UR_EXPECT_TEXT("", row.textKey);
  UR_EXPECT_TEXT("held by the operator", TextOf(row));
}

// The toggle shows the setting, not the state: a write racing the status reads
// on over an "off" status until the status catches up.
UR_TEST(ExtenderProvide_ToggleIsTheSettingNotTheState) {
  UR_EXPECT_TRUE(Row("off", "", "", false, false, false, true).on);
  UR_EXPECT_FALSE(Row("active", "", "", true, true, false, false).on);
  UR_EXPECT_FALSE(Row("setting_up", "", "", false, false, false, false).on);
}

// The local repaint after a toggle, before the listener answers.
UR_TEST(ExtenderProvide_ToggleGuess) {
  const ProvideRow onProviding = ProvideRowGuess(true, true);
  UR_EXPECT_TRUE(onProviding.visible);
  UR_EXPECT_TRUE(onProviding.dot == ProvideDot::Yellow);
  UR_EXPECT_TEXT("Setting up", TextOf(onProviding));
  UR_EXPECT_TRUE(onProviding.on);

  const ProvideRow onIdle = ProvideRowGuess(true, false);
  UR_EXPECT_TRUE(onIdle.dot == ProvideDot::Grey);
  UR_EXPECT_TEXT("Not providing", TextOf(onIdle));
  UR_EXPECT_TRUE(onIdle.on);

  for (bool providing : {false, true}) {
    const ProvideRow off = ProvideRowGuess(false, providing);
    UR_EXPECT_TRUE(off.dot == ProvideDot::Grey);
    UR_EXPECT_TEXT("Off", TextOf(off));
    UR_EXPECT_FALSE(off.on);
    // the guess is the reading the SDK's own "off" produces, so a status that
    // confirms it draws nothing new
    UR_EXPECT_TRUE(off == Row("off", "", "", false, false, false, false));
  }
}

// By value: an unchanged push is dropped, a changed Reason or setting is not.
UR_TEST(ExtenderProvide_EqualReadingsCompareEqual) {
  UR_EXPECT_TRUE(Row("error", "listen", "a") == Row("error", "listen", "a"));
  UR_EXPECT_TRUE(Row("error", "listen", "a") != Row("error", "listen", "b"));
  UR_EXPECT_TRUE(Row("active", "", "", true, true, false, true) !=
                 Row("active", "", "", true, true, false, false));
  UR_EXPECT_TRUE(Row("active", "", "x", true, false, true) !=
                 Row("active", "", "x", true, false, false));
}

// The reading takes Supported, State, ErrorCase, Reason, ActivatedV4,
// ActivatedV6 and LastActivationRefused and nothing else: the other nineteen
// fields, set to noise, leave every reading as it was. The tcp 443 bind error
// reaches the row as the Reason of its case, never as TcpUnavailableError.
UR_TEST(ExtenderProvide_ReadsOnlyTheContractFields) {
  FullStatus unsupported = SupportedStatus("active", "", "", true, true);
  unsupported.Supported = false;
  const std::vector<FullStatus> readings = {
      SupportedStatus("off"),
      SupportedStatus("not_providing"),
      SupportedStatus("setting_up"),
      SupportedStatus("active", "", "", true, true),
      SupportedStatus("active", "", "the operator refused the activation", true, false, true),
      SupportedStatus("error", "revoked"),
      SupportedStatus("error", "listen", "tcp: bind: permission denied"),
      SupportedStatus("error", "tcp_unavailable", "listen tcp :443: bind: address already in use"),
      unsupported,
  };
  for (const FullStatus& reading : readings) {
    FullStatus noisy = reading;
    noisy.Enabled = !reading.Enabled;
    noisy.StartError = "no extender directory in this network space";
    noisy.TcpUnavailableError = "listen tcp 0.0.0.0:443: bind: permission denied";
    noisy.Listening = !reading.Listening;
    noisy.ListenError = "udp: bind: address already in use";
    noisy.Ipv4 = "192.0.2.10";
    noisy.Ipv6 = "2001:db8::10";
    noisy.LastActivationTime = 1757800000000;
    noisy.LastActivationError = "context deadline exceeded";
    noisy.RevokedTime = 1757800000001;
    noisy.DnsPorts = "53,4053";
    noisy.PeerPingCount = 9;
    noisy.PeerPingCosignedCount = 5;
    noisy.PeerPingRejectedCount = 2;
    noisy.PeerPingUnknownCount = 2;
    noisy.LastPeerPingTime = 1757800000002;
    noisy.LimitedBySubnetsCount = 3;
    noisy.LimitedBySourceCount = 4;
    noisy.ConnectionCount = 42;
    const std::string label = reading.State + "/" + reading.ErrorCase;
    UR_EXPECT_TRUE_MSG(label, RowOf(reading, true) == RowOf(noisy, true));
    UR_EXPECT_TRUE_MSG(label, RowOf(reading, false) == RowOf(noisy, false));
  }
}

// Each field the reading takes, and the setting, moves the row on its own, so
// a swap of the two family flags, or of the refusal and the setting (adjacent
// bools in the positional call), cannot pass.
UR_TEST(ExtenderProvide_EachReadFieldMovesTheRow) {
  FullStatus unsupported = SupportedStatus("active", "", "", true, true);
  unsupported.Supported = false;
  UR_EXPECT_TRUE(RowOf(SupportedStatus("active", "", "", true, true), true).visible);
  UR_EXPECT_FALSE(RowOf(unsupported, true).visible);

  UR_EXPECT_TEXT("off", RowOf(SupportedStatus("off"), true).textKey);
  UR_EXPECT_TEXT("extender_setting_up", RowOf(SupportedStatus("setting_up"), true).textKey);

  UR_EXPECT_TEXT("extender_start_failed",
                 RowOf(SupportedStatus("error", "start", "x"), true).textKey);
  UR_EXPECT_TEXT("extender_listen_failed",
                 RowOf(SupportedStatus("error", "listen", "x"), true).textKey);

  UR_EXPECT_TEXT(
      "tcp: bind: permission denied",
      RowOf(SupportedStatus("error", "listen", "tcp: bind: permission denied"), true).argument);

  UR_EXPECT_TEXT("ipv4", RowOf(SupportedStatus("active", "", "", true, false), true).familiesKey);
  UR_EXPECT_TEXT("ipv6", RowOf(SupportedStatus("active", "", "", false, true), true).familiesKey);

  const ProvideRow refused = RowOf(
      SupportedStatus("active", "", "the operator refused the activation", true, false, true),
      false);
  UR_EXPECT_TEXT("extender_activation_refused", refused.detailKey);
  UR_EXPECT_FALSE(refused.on);
  const ProvideRow failed =
      RowOf(SupportedStatus("active", "", "context deadline exceeded", true, false, false), true);
  UR_EXPECT_TEXT("extender_activation_failed", failed.detailKey);
  UR_EXPECT_TRUE(failed.on);

  UR_EXPECT_TRUE(RowOf(SupportedStatus("off"), true).on);
  UR_EXPECT_FALSE(RowOf(SupportedStatus("off"), false).on);
}

// A row that will not show reads nothing else: the setting, an rpc, is read
// once beside a status that shows and never for a hidden one (N7).
UR_TEST(ExtenderProvide_HiddenRowReadsNoSetting) {
  int reads = 0;
  const auto reader = [&reads] {
    ++reads;
    return true;
  };
  const ProvideRow none = ProvideRowOf(std::optional<FullStatus>(), reader);
  UR_EXPECT_FALSE(none.visible);
  UR_EXPECT_EQ(0, reads);

  FullStatus unsupported = SupportedStatus("active", "", "", true, true);
  unsupported.Supported = false;
  const ProvideRow hidden = ProvideRowOf(std::optional<FullStatus>(unsupported), reader);
  UR_EXPECT_FALSE(hidden.visible);
  UR_EXPECT_EQ(0, reads);

  const ProvideRow shown = ProvideRowOf(std::optional<FullStatus>(SupportedStatus("off")), reader);
  UR_EXPECT_TRUE(shown.visible);
  UR_EXPECT_TRUE(shown.on);
  UR_EXPECT_EQ(1, reads);
}

// The lag.dev speed tests switch (lag.dev/PLAN.md C4): shown only beside a
// visible Extender row and where its write has a taker, hidden otherwise, and
// its setting read once, only for a row that shows.
UR_TEST(SpeedTests_ShowsOnlyBesideTheExtenderRowWithAWriter) {
  int reads = 0;
  const auto off = [&reads] {
    ++reads;
    return false;
  };
  const ProvideRow extenderShown =
      ProvideRowOf(std::optional<FullStatus>(SupportedStatus("active", "", "", true)),
                   [] { return true; });
  const ProvideRow extenderHidden = ProvideRowOf(std::optional<FullStatus>(), [] { return true; });
  UR_EXPECT_TRUE(extenderShown.visible);
  UR_EXPECT_FALSE(extenderHidden.visible);

  UR_EXPECT_FALSE(SpeedTestsRowOf(extenderHidden, true, off).visible);
  UR_EXPECT_FALSE(SpeedTestsRowOf(extenderShown, false, off).visible);
  UR_EXPECT_FALSE(SpeedTestsRowOf(extenderHidden, false, off).visible);
  UR_EXPECT_EQ(0, reads);

  const SpeedTestsRow shown = SpeedTestsRowOf(extenderShown, true, off);
  UR_EXPECT_TRUE(shown.visible);
  UR_EXPECT_FALSE(shown.on);
  UR_EXPECT_EQ(1, reads);
}

// It shows whatever the Extender switch's position: the setting still applies
// when the role is off, and is on by default.
UR_TEST(SpeedTests_IndependentOfTheExtenderSwitchAndOnByDefault) {
  for (const bool extenderOn : {false, true}) {
    const ProvideRow extender = ProvideRowOf(
        std::optional<FullStatus>(SupportedStatus(extenderOn ? "active" : "off", "", "", true)),
        [extenderOn] { return extenderOn; });
    const SpeedTestsRow row = SpeedTestsRowOf(extender, true, [] { return true; });
    UR_EXPECT_TRUE(row.visible);
    UR_EXPECT_TRUE(row.on);
  }
  UR_EXPECT_TRUE(SpeedTestsRow().on);
  UR_EXPECT_FALSE(SpeedTestsRow().visible);
  // a hidden row compares equal to every other hidden row, so a push that
  // changes nothing is dropped
  UR_EXPECT_TRUE(SpeedTestsRowOf(ProvideRow(), true, [] { return false; }) == SpeedTestsRow());
}

// Every key and English source the row can emit is the catalog's, byte for
// byte (I18n.hpp: the English text is the msgid the catalog is keyed on).
UR_TEST(ExtenderProvide_KeysAreTheCatalogs) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/../po/en.po", std::ios::binary);
  UR_EXPECT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string catalog = buffer.str();

  const std::vector<ProvideRow> rows = {
      Row("off"),
      Row("not_providing"),
      Row("setting_up"),
      Row("active", "", "", true, true),
      Row("active", "", "", true, false),
      Row("active", "", "", false, true),
      Row("active", "", "x", true, false, true),
      Row("active", "", "x", true, false, false),
      Row("error", "revoked"),
      Row("error", "start", "x"),
      Row("error", "tcp_unavailable", "x"),
      Row("error", "listen", "x"),
      Row("error", "activation_refused", "x"),
      Row("error", "activation_failed", "x"),
  };
  std::set<std::string> keys;
  auto expectInCatalog = [&](const char* key, const char* english) {
    if (key[0] == '\0') return;
    keys.insert(key);
    const std::string entry =
        std::string("msgctxt \"") + key + "\"\nmsgid \"" + english + "\"\n";
    UR_EXPECT_TRUE_MSG(entry, catalog.find(entry) != std::string::npos);
  };
  for (const auto& row : rows) {
    expectInCatalog(row.textKey, row.textEnglish);
    expectInCatalog(row.familiesKey, row.familiesEnglish);
    expectInCatalog(row.detailKey, row.detailEnglish);
  }
  // off, the three plain states, active and its three families, and the six
  // error cases, two of which also serve as the active line's detail
  UR_EXPECT_EQ(13, static_cast<int>(keys.size()));
}

// The O8 rule over every combination of its three inputs, as a literal table
// rather than the expressions it is made of.
UR_TEST(ExtenderStats_SectionsForEveryCombination) {
  struct Case {
    bool providing;
    bool hasStats;
    bool running;
    bool providerVisible;
    bool extenderVisible;
    bool disabledMeta;
  };
  const Case cases[] = {
      // providing, stats, running -> provider rows, extender group, disabled meta
      {false, false, false, false, false, true},
      {false, false, true, false, false, true},
      {false, true, false, false, false, true},
      {false, true, true, false, false, true},
      {true, false, false, false, false, true},
      {true, false, true, false, false, true},
      {true, true, false, true, false, false},
      {true, true, true, true, true, false},
  };
  for (const Case& c : cases) {
    const StatsSections sections = StatsSectionsFor(c.providing, c.hasStats, c.running);
    const std::string label = std::string("providing=") + (c.providing ? "1" : "0") +
                              " stats=" + (c.hasStats ? "1" : "0") +
                              " running=" + (c.running ? "1" : "0");
    UR_EXPECT_TRUE_MSG(label, sections.providerVisible == c.providerVisible);
    UR_EXPECT_TRUE_MSG(label, sections.extenderVisible == c.extenderVisible);
    UR_EXPECT_TRUE_MSG(label, sections.disabledMeta == c.disabledMeta);
  }
  UR_EXPECT_TRUE(StatsSectionsFor(true, true, true) == StatsSectionsFor(true, true, true));
  UR_EXPECT_TRUE(StatsSectionsFor(true, true, true) != StatsSectionsFor(true, true, false));
}
