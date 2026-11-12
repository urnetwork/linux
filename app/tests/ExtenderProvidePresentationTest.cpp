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

using urnw::extender::PortMappingLine;
using urnw::extender::PortMappingLineFor;
using urnw::extender::PortMappingLineOf;
using urnw::extender::PortMappingRow;
using urnw::extender::PortMappingRowOf;
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
  std::string PortMapping{};
  std::string PortMappingMethod{};
  std::string PortMappingExternalIp{};
  int64_t PortMappingTcpPort{};
  int64_t PortMappingUdpPort{};
  int64_t PortMappingDnsPort{};
  std::string PortMappingError{};
  std::string PortMappingMethodName{};
  std::string PortMappingPorts{};
  int64_t ConnectionCount{};
};

// A supported status with the fields the reading takes; the other twenty-eight
// stay at their zero values.
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

// The store's {0} {1} substitution (I18n.hpp's Format with two arguments):
// "{0}" and "{1}" take that argument, "{}" the next one.
std::string FormatTwo(const std::string& pattern, const std::string& first,
                      const std::string& second) {
  const std::string args[] = {first, second};
  std::string out;
  size_t next = 0;
  for (size_t i = 0; i < pattern.size();) {
    const size_t close = pattern[i] == '{' ? pattern.find('}', i) : std::string::npos;
    if (close != std::string::npos) {
      const std::string spec = pattern.substr(i + 1, close - i - 1);
      size_t index = next;
      if (spec == "0") {
        index = 0;
      } else if (spec == "1") {
        index = 1;
      } else if (!spec.empty()) {
        index = 2;  // not a placeholder: copied through
      }
      if (index < 2) {
        out += args[index];
        if (spec.empty()) ++next;
        i = close + 1;
        continue;
      }
    }
    out += pattern[i++];
  }
  return out;
}

std::string LineTextOf(const PortMappingLine& line) {
  return urnw::extender::PortMappingLineTextFor(line, &English, &FormatTwo);
}

// The router port mapping switch's row as drawn: shown and on unless said
// otherwise.
PortMappingRow SwitchRow(bool visible = true, bool on = true) {
  PortMappingRow row;
  row.visible = visible;
  row.on = on;
  return row;
}

// An active role whose port mapping reports `state`, with the sdk's method
// name and ports text.
FullStatus PortMappingStatus(const std::string& state,
                             const std::string& methodName = std::string(),
                             const std::string& ports = std::string()) {
  FullStatus status = SupportedStatus("active", "", "", true);
  status.PortMapping = state;
  status.PortMappingMethodName = methodName;
  status.PortMappingPorts = ports;
  return status;
}

PortMappingLine LineOf(const PortMappingRow& row, const FullStatus& status) {
  return PortMappingLineOf(row, std::optional<FullStatus>(status));
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
// ActivatedV6 and LastActivationRefused and nothing else: the other
// twenty-eight fields, set to noise, leave every reading as it was. The tcp 443
// bind error reaches the row as the Reason of its case, never as
// TcpUnavailableError, and the router port mapping's state and texts never move
// the row.
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
    noisy.PortMapping = "failed";
    noisy.PortMappingMethod = "upnp";
    noisy.PortMappingExternalIp = "198.51.100.7";
    noisy.PortMappingTcpPort = 8443;
    noisy.PortMappingUdpPort = 8443;
    noisy.PortMappingDnsPort = 14053;
    noisy.PortMappingError = "no gateway answered";
    noisy.PortMappingMethodName = "UPnP";
    noisy.PortMappingPorts = "TCP 8443→443, UDP 8443→443, UDP 14053→4053";
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

// The router port mapping switch under it (connect EXTENDER.md S8): shown only
// beside a visible Extender row and where its write has a taker, hidden
// otherwise, and its setting read once, only for a row that shows.
UR_TEST(PortMapping_ShowsOnlyBesideTheExtenderRowWithAWriter) {
  int reads = 0;
  const auto on = [&reads] {
    ++reads;
    return true;
  };
  const ProvideRow extenderShown =
      ProvideRowOf(std::optional<FullStatus>(SupportedStatus("active", "", "", true)),
                   [] { return true; });
  const ProvideRow extenderHidden = ProvideRowOf(std::optional<FullStatus>(), [] { return true; });
  const ProvideRow extenderUnsupported =
      ProvideRowOf(std::optional<FullStatus>(FullStatus()), [] { return true; });
  UR_EXPECT_TRUE(extenderShown.visible);
  UR_EXPECT_FALSE(extenderHidden.visible);
  UR_EXPECT_FALSE(extenderUnsupported.visible);

  UR_EXPECT_FALSE(PortMappingRowOf(extenderHidden, true, on).visible);
  UR_EXPECT_FALSE(PortMappingRowOf(extenderUnsupported, true, on).visible);
  UR_EXPECT_FALSE(PortMappingRowOf(extenderShown, false, on).visible);
  UR_EXPECT_FALSE(PortMappingRowOf(extenderHidden, false, on).visible);
  UR_EXPECT_EQ(0, reads);
  // a hidden row never reads as on, whatever the setting
  UR_EXPECT_FALSE(PortMappingRowOf(extenderHidden, true, on).on);

  const PortMappingRow shown = PortMappingRowOf(extenderShown, true, on);
  UR_EXPECT_TRUE(shown.visible);
  UR_EXPECT_TRUE(shown.on);
  UR_EXPECT_EQ(1, reads);
}

// It shows whatever the Extender switch's position, and is off by default on
// every app (owner, 2026-11-11): the switch starts off until a reading says
// otherwise.
UR_TEST(PortMapping_IndependentOfTheExtenderSwitchAndOffByDefault) {
  for (const bool extenderOn : {false, true}) {
    const ProvideRow extender = ProvideRowOf(
        std::optional<FullStatus>(SupportedStatus(extenderOn ? "active" : "off", "", "", true)),
        [extenderOn] { return extenderOn; });
    const PortMappingRow row = PortMappingRowOf(extender, true, [] { return false; });
    UR_EXPECT_TRUE(row.visible);
    UR_EXPECT_FALSE(row.on);
  }
  UR_EXPECT_FALSE(PortMappingRow().on);
  UR_EXPECT_FALSE(PortMappingRow().visible);
  // a hidden row compares equal to every other hidden row, so a push that
  // changes nothing is dropped
  UR_EXPECT_TRUE(PortMappingRowOf(ProvideRow(), true, [] { return true; }) == PortMappingRow());
  // and a shown row that changes only the setting is a change
  const ProvideRow extender =
      ProvideRowOf(std::optional<FullStatus>(SupportedStatus("active", "", "", true)),
                   [] { return true; });
  UR_EXPECT_TRUE(PortMappingRowOf(extender, true, [] { return true; }) !=
                 PortMappingRowOf(extender, true, [] { return false; }));
}

// ---- the router port mapping's status line (connect EXTENDER.md S10) --------

// Under a shown switch that is on, each state the role reports has its own
// line, named by the catalog's key; the mapped line carries the sdk's ports
// text and method name as they are, the arrow of a moved port included.
UR_TEST(PortMappingLine_EachStateHasItsLine) {
  const PortMappingLine notNeeded = PortMappingLineFor(SwitchRow(), "not_needed", "", "");
  UR_EXPECT_TRUE(notNeeded.visible);
  UR_EXPECT_TEXT("extender_port_mapping_not_needed", notNeeded.textKey);
  UR_EXPECT_TEXT("Not needed: this device has a public address.", LineTextOf(notNeeded));

  const PortMappingLine asking = PortMappingLineFor(SwitchRow(), "mapping", "", "");
  UR_EXPECT_TRUE(asking.visible);
  UR_EXPECT_TEXT("extender_port_mapping_mapping", asking.textKey);
  UR_EXPECT_TEXT("Asking your router…", LineTextOf(asking));

  const PortMappingLine mapped =
      PortMappingLineFor(SwitchRow(), "mapped", "UPnP", "TCP 443, UDP 443, UDP 4053");
  UR_EXPECT_TRUE(mapped.visible);
  UR_EXPECT_TEXT("extender_port_mapping_mapped", mapped.textKey);
  UR_EXPECT_TEXT("Your router forwards TCP 443, UDP 443, UDP 4053 (UPnP).", LineTextOf(mapped));

  const PortMappingLine moved =
      PortMappingLineFor(SwitchRow(), "mapped", "NAT-PMP", "TCP 40443→443, UDP 4053");
  UR_EXPECT_TEXT("Your router forwards TCP 40443→443, UDP 4053 (NAT-PMP).", LineTextOf(moved));

  const PortMappingLine failed = PortMappingLineFor(SwitchRow(), "failed", "", "");
  UR_EXPECT_TRUE(failed.visible);
  UR_EXPECT_TEXT("extender_port_mapping_failed", failed.textKey);
  UR_EXPECT_TEXT("Your router didn't open the ports. Trying again later.", LineTextOf(failed));
  // only the mapped line takes the texts, whatever a failed role carries
  UR_EXPECT_TRUE(PortMappingLineFor(SwitchRow(), "failed", "UPnP", "TCP 443") == failed);
}

// Off, no role (an empty state) and a state this build does not know have no
// line: hidden, never a guess, and equal to every other hidden line. No status
// at all reads the same.
UR_TEST(PortMappingLine_NothingForOffNoRoleOrAnUnknownState) {
  for (const char* state : {urnw::extender::kPortMappingOff, "", "paused"}) {
    const PortMappingLine line = PortMappingLineFor(SwitchRow(), state, "UPnP", "TCP 443");
    UR_EXPECT_TRUE_MSG(state, !line.visible);
    UR_EXPECT_TRUE_MSG(state, LineTextOf(line).empty());
    UR_EXPECT_TRUE_MSG(state, line == PortMappingLine());
  }
  UR_EXPECT_TRUE(PortMappingLineOf(SwitchRow(), std::optional<FullStatus>()) == PortMappingLine());
}

// The line speaks for the switch: none while the switch is hidden or off,
// whatever the role reports, so turning the switch off takes the line with it.
UR_TEST(PortMappingLine_OnlyUnderAShownSwitchThatIsOn) {
  for (const char* state : {"not_needed", "mapping", "mapped", "failed"}) {
    UR_EXPECT_TRUE_MSG(state, PortMappingLineFor(SwitchRow(), state, "PCP", "TCP 443").visible);
    for (const PortMappingRow& row : {SwitchRow(true, false), SwitchRow(false, true),
                                      SwitchRow(false, false), PortMappingRow()}) {
      const PortMappingLine line = PortMappingLineFor(row, state, "PCP", "TCP 443");
      UR_EXPECT_TRUE_MSG(state, line == PortMappingLine());
      UR_EXPECT_TRUE_MSG(state, LineTextOf(line).empty());
    }
  }
}

// A mapped role without both of the sdk's texts (a device process from before
// them) has no line rather than a sentence with empty parentheses.
UR_TEST(PortMappingLine_MappedWithoutItsTextsHasNoLine) {
  struct Texts {
    const char* method;
    const char* ports;
  };
  for (const Texts& texts :
       {Texts{"", ""}, Texts{"UPnP", ""}, Texts{"", "TCP 443, UDP 443, UDP 4053"}}) {
    const PortMappingLine line =
        PortMappingLineFor(SwitchRow(), "mapped", texts.method, texts.ports);
    const std::string label = std::string(texts.method) + "/" + texts.ports;
    UR_EXPECT_TRUE_MSG(label, line == PortMappingLine());
    UR_EXPECT_TRUE_MSG(label, LineTextOf(line).empty());
  }
}

// Through the status as the widgets hold it, the method name and the ports text
// land in their own places (two strings side by side, so a swap cannot pass),
// and a switch that is off reads no line from any status.
UR_TEST(PortMappingLine_ReadsTheStatusAsTheWidgetsHoldIt) {
  UR_EXPECT_TEXT("Your router forwards TCP 443, UDP 4053 (PCP).",
                 LineTextOf(LineOf(SwitchRow(), PortMappingStatus("mapped", "PCP",
                                                                  "TCP 443, UDP 4053"))));
  UR_EXPECT_TEXT("extender_port_mapping_not_needed",
                 LineOf(SwitchRow(), PortMappingStatus("not_needed")).textKey);
  UR_EXPECT_TEXT("extender_port_mapping_mapping",
                 LineOf(SwitchRow(), PortMappingStatus("mapping")).textKey);
  UR_EXPECT_TEXT("extender_port_mapping_failed",
                 LineOf(SwitchRow(), PortMappingStatus("failed")).textKey);
  UR_EXPECT_TRUE(LineOf(SwitchRow(), PortMappingStatus("off")) == PortMappingLine());
  UR_EXPECT_TRUE(LineOf(SwitchRow(true, false),
                        PortMappingStatus("mapped", "PCP", "TCP 443, UDP 4053")) ==
                 PortMappingLine());
}

// The line reads PortMapping, PortMappingMethodName and PortMappingPorts and
// nothing else: the router's own words (PortMappingError), the address it
// answered from, its raw method and ports, and every other field, set to noise,
// leave each line as it was, and no line's text carries the router's words or
// an address.
UR_TEST(PortMappingLine_NeverCarriesTheRoutersWords) {
  const std::string routerWords =
      "AddPortMapping: 192.168.1.1 answered 718 ConflictInMappingEntry for 198.51.100.7";
  const std::vector<FullStatus> readings = {
      PortMappingStatus("not_needed"),
      PortMappingStatus("mapping"),
      PortMappingStatus("mapped", "UPnP", "TCP 443, UDP 443, UDP 4053"),
      PortMappingStatus("mapped", "NAT-PMP", "TCP 40443→443, UDP 4053"),
      PortMappingStatus("mapped"),
      PortMappingStatus("failed"),
      PortMappingStatus("off"),
      PortMappingStatus(""),
  };
  for (const FullStatus& reading : readings) {
    FullStatus noisy = reading;
    noisy.Supported = !reading.Supported;
    noisy.State = "error";
    noisy.ErrorCase = "listen";
    noisy.Reason = "tcp: bind: permission denied";
    noisy.Enabled = !reading.Enabled;
    noisy.StartError = "no extender directory in this network space";
    noisy.TcpUnavailableError = "listen tcp 0.0.0.0:443: bind: permission denied";
    noisy.ListenError = "udp: bind: address already in use";
    noisy.Ipv4 = "192.0.2.10";
    noisy.Ipv6 = "2001:db8::10";
    noisy.LastActivationError = "context deadline exceeded";
    noisy.DnsPorts = "53,4053";
    noisy.PortMappingMethod = "upnp";
    noisy.PortMappingExternalIp = "198.51.100.7";
    noisy.PortMappingTcpPort = 40443;
    noisy.PortMappingUdpPort = 40444;
    noisy.PortMappingDnsPort = 40053;
    noisy.PortMappingError = routerWords;
    noisy.ConnectionCount = 42;
    const std::string label = reading.PortMapping + "/" + reading.PortMappingMethodName;
    const PortMappingLine line = LineOf(SwitchRow(), noisy);
    UR_EXPECT_TRUE_MSG(label, line == LineOf(SwitchRow(), reading));
    const std::string text = LineTextOf(line);
    for (const char* never : {"AddPortMapping", "ConflictInMappingEntry", "192.168.1.1",
                              "198.51.100.7", "192.0.2.10", "2001:db8::10", "upnp"}) {
      UR_EXPECT_TRUE_MSG(label + " carries " + never, text.find(never) == std::string::npos);
    }
  }
}

// By value: an unchanged reading is dropped, another method, a moved port or
// another state is not.
UR_TEST(PortMappingLine_EqualReadingsCompareEqual) {
  const auto mapped = [](const char* method, const char* ports) {
    return PortMappingLineFor(SwitchRow(), "mapped", method, ports);
  };
  UR_EXPECT_TRUE(mapped("UPnP", "TCP 443") == mapped("UPnP", "TCP 443"));
  UR_EXPECT_TRUE(mapped("UPnP", "TCP 443") != mapped("PCP", "TCP 443"));
  UR_EXPECT_TRUE(mapped("UPnP", "TCP 443") != mapped("UPnP", "TCP 40443→443"));
  UR_EXPECT_TRUE(PortMappingLineFor(SwitchRow(), "mapping", "", "") !=
                 PortMappingLineFor(SwitchRow(), "failed", "", ""));
}

// Every key and English source the line can emit is the catalog's, byte for
// byte, and the mapped line's sentence takes both of its arguments.
UR_TEST(PortMappingLine_KeysAreTheCatalogs) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/../po/en.po", std::ios::binary);
  UR_EXPECT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string catalog = buffer.str();

  const PortMappingLine mapped = PortMappingLineFor(SwitchRow(), "mapped", "UPnP", "TCP 443");
  std::set<std::string> keys;
  for (const PortMappingLine& line : {PortMappingLineFor(SwitchRow(), "not_needed", "", ""),
                                      PortMappingLineFor(SwitchRow(), "mapping", "", ""), mapped,
                                      PortMappingLineFor(SwitchRow(), "failed", "", "")}) {
    keys.insert(line.textKey);
    const std::string entry = std::string("msgctxt \"") + line.textKey + "\"\nmsgid \"" +
                              line.textEnglish + "\"\n";
    UR_EXPECT_TRUE_MSG(entry, catalog.find(entry) != std::string::npos);
  }
  UR_EXPECT_EQ(4, static_cast<int>(keys.size()));
  UR_EXPECT_TRUE(std::string(mapped.textEnglish).find("{0}") != std::string::npos);
  UR_EXPECT_TRUE(std::string(mapped.textEnglish).find("{1}") != std::string::npos);
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
