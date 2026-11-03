// The provider extender row's reading of ExtenderProvideStatus, and the
// visibility rule of the two statistics groups on the earnings page
// (connect/EXTENDER.md N3, N7, O4, O5, O8), computed as pure steps so every
// display rule is deterministic and testable, and so the widgets only draw
// what these decided.
//
// The row is one dot (grey off and not providing, yellow setting up, green
// active, red error), one line of state text built from the store keys of N5,
// chosen by the SDK's State and, in the error state, by its ErrorCase -- the
// app never re-derives the rule of N3 and never names a case the SDK did not
// pick -- and the toggle's position, which is the setting read beside the
// status. It is visible exactly while the device reports the role supported:
// hidden, never disabled, so an app talking to an older daemon shows nothing
// rather than a dead toggle (N1).
//
// Header-only and free of GTK, glib and the SDK so the unit tests need no
// vendored headers (tests/ExtenderProvidePresentationTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstring>
#include <optional>
#include <string>

namespace urnw::extender {

// Mirrors of the SDK's vocabulary (URNET_EXTENDER_PROVIDE_STATE_* and
// URNET_EXTENDER_PROVIDE_ERROR_*), kept here so this header stays SDK-free.
inline constexpr const char* kProvideStateOff = "off";
inline constexpr const char* kProvideStateNotProviding = "not_providing";
inline constexpr const char* kProvideStateSettingUp = "setting_up";
inline constexpr const char* kProvideStateActive = "active";
inline constexpr const char* kProvideStateError = "error";

inline constexpr const char* kProvideErrorRevoked = "revoked";
inline constexpr const char* kProvideErrorStart = "start";
// tcp 443, the one port the role requires, could not be bound (usually another
// program holds it): the role stays off and the SDK binds again every few minutes
inline constexpr const char* kProvideErrorTcpUnavailable = "tcp_unavailable";
inline constexpr const char* kProvideErrorListen = "listen";
inline constexpr const char* kProvideErrorActivationFailed = "activation_failed";
inline constexpr const char* kProvideErrorActivationRefused = "activation_refused";
inline constexpr const char* kProvideErrorActivationUnreachable = "activation_unreachable";

// What joins the active text and the other family's failure on one line (N7).
inline constexpr const char* kProvideTextSeparator = " · ";

// The dot's four colors, named by role rather than by pixel so this header
// carries no palette (the widgets paint kUrTextMuted, kUrGreen, kUrAmber and
// kUrCoral).
enum class ProvideDot { Grey, Green, Yellow, Red };

struct ProvideRow {
  // The row shows (and on the connect page its description with it): a status
  // is in hand and the device reports the role supported.
  bool visible = false;
  ProvideDot dot = ProvideDot::Grey;
  // The state text's store key and the English source it carries (the msgctxt
  // and msgid of po/). Empty for a case this build does not know, whose whole
  // text is `argument`: the SDK's Reason, bare.
  const char* textKey = "";
  const char* textEnglish = "";
  // What the key's {} takes (the Reason of an error case), or the whole text
  // when there is no key.
  std::string argument;
  // Active only: the families label that fills extender_active's {}.
  const char* familiesKey = "";
  const char* familiesEnglish = "";
  // Active only, while the other family's last attempt failed (Reason is not
  // empty; ErrorCase is empty in this state): the text after the separator,
  // extender_activation_refused when LastActivationRefused says the Reason is
  // the operator's refusal, else extender_activation_failed, the Reason as {}.
  const char* detailKey = "";
  const char* detailEnglish = "";
  std::string detailArgument;
  // The state line takes the error color: the error state, in every case.
  bool errorText = false;
  // The toggle's position: the setting, read through the device beside the
  // status (GetProvideExtender), never derived from the state.
  bool on = false;

  // By value, so a widget can drop a push that changes nothing.
  friend bool operator==(const ProvideRow& a, const ProvideRow& b) {
    return a.visible == b.visible && a.dot == b.dot &&
           std::strcmp(a.textKey, b.textKey) == 0 &&
           std::strcmp(a.textEnglish, b.textEnglish) == 0 && a.argument == b.argument &&
           std::strcmp(a.familiesKey, b.familiesKey) == 0 &&
           std::strcmp(a.familiesEnglish, b.familiesEnglish) == 0 &&
           std::strcmp(a.detailKey, b.detailKey) == 0 &&
           std::strcmp(a.detailEnglish, b.detailEnglish) == 0 &&
           a.detailArgument == b.detailArgument && a.errorText == b.errorText && a.on == b.on;
  }
  friend bool operator!=(const ProvideRow& a, const ProvideRow& b) { return !(a == b); }
};

// The row's whole reading, from the fields of one ExtenderProvideStatus
// (`haveStatus` false when there is none: no device) and the setting read
// beside it. Reads Supported, State, ErrorCase, Reason, ActivatedV4,
// ActivatedV6 and LastActivationRefused, and nothing else of the status.
inline ProvideRow ProvideRowFor(bool haveStatus, bool supported, const std::string& state,
                                const std::string& errorCase, const std::string& reason,
                                bool activatedV4, bool activatedV6, bool refused,
                                bool provideExtender) {
  ProvideRow row;
  // No status and an unsupported role read the same: hidden, never disabled.
  // A hidden row reads nothing else, so every hidden reading compares equal.
  if (!haveStatus || !supported) return row;
  row.visible = true;
  row.on = provideExtender;
  if (state == kProvideStateOff) {
    row.textKey = "off";
    row.textEnglish = "Off";
  } else if (state == kProvideStateNotProviding) {
    row.textKey = "extender_not_providing";
    row.textEnglish = "Not providing";
  } else if (state == kProvideStateSettingUp) {
    row.dot = ProvideDot::Yellow;
    row.textKey = "extender_setting_up";
    row.textEnglish = "Setting up";
  } else if (state == kProvideStateActive) {
    row.dot = ProvideDot::Green;
    row.textKey = "extender_active";
    row.textEnglish = "Active · {}";
    // Active means at least one family is activated (N3); a reading with
    // neither flag names IPv4 rather than inventing a label.
    if (activatedV4 && activatedV6) {
      row.familiesKey = "ipv4_and_ipv6";
      row.familiesEnglish = "IPv4 and IPv6";
    } else if (activatedV6) {
      row.familiesKey = "ipv6";
      row.familiesEnglish = "IPv6";
    } else {
      row.familiesKey = "ipv4";
      row.familiesEnglish = "IPv4";
    }
    if (!reason.empty()) {
      if (refused) {
        row.detailKey = "extender_activation_refused";
        row.detailEnglish = "Activation refused: {}";
      } else {
        row.detailKey = "extender_activation_failed";
        row.detailEnglish = "Activation failed: {}";
      }
      row.detailArgument = reason;
    }
  } else if (state == kProvideStateError) {
    row.dot = ProvideDot::Red;
    row.errorText = true;
    if (errorCase == kProvideErrorRevoked) {
      row.textKey = "extender_revoked";
      row.textEnglish = "Revoked by the operator";
    } else if (errorCase == kProvideErrorStart) {
      row.textKey = "extender_start_failed";
      row.textEnglish = "Could not start: {}";
      row.argument = reason;
    } else if (errorCase == kProvideErrorTcpUnavailable) {
      // the Reason is the tcp 443 bind error
      row.textKey = "extender_tcp_unavailable";
      row.textEnglish =
          "TCP port 443 is in use by another program. Trying again every few minutes: {}";
      row.argument = reason;
    } else if (errorCase == kProvideErrorListen) {
      row.textKey = "extender_listen_failed";
      row.textEnglish = "Could not listen: {}";
      row.argument = reason;
    } else if (errorCase == kProvideErrorActivationRefused) {
      row.textKey = "extender_activation_refused";
      row.textEnglish = "Activation refused: {}";
      row.argument = reason;
    } else if (errorCase == kProvideErrorActivationUnreachable) {
      // the operator's required tcp test found no answer: say what it needs
      row.textKey = "extender_activation_unreachable";
      row.textEnglish =
          "The internet can't reach this device on TCP port 443. Allow or forward TCP 443 (and UDP 443 and 4053), or turn Extender off: {}";
      row.argument = reason;
    } else if (errorCase == kProvideErrorActivationFailed) {
      row.textKey = "extender_activation_failed";
      row.textEnglish = "Activation failed: {}";
      row.argument = reason;
    } else {
      // an error case this build does not know (a newer device process): the
      // Reason bare, in the error color
      row.argument = reason;
    }
  } else {
    // A state this build does not know (a newer device process). No color may
    // claim a meaning and no label may name a case the SDK did not pick: grey,
    // with the Reason bare in the muted note.
    row.argument = reason;
  }
  return row;
}

// The row's reading of the status as the widgets hold it: the SDK's optional
// status, and the setting read beside it only when the row will show (N7).
// Generic so the tests can hand it every field of ExtenderProvideStatus.
template <typename Status, typename ReadSetting>
ProvideRow ProvideRowOf(const std::optional<Status>& status, const ReadSetting& readSetting) {
  if (!status || !status->Supported) return ProvideRow();
  return ProvideRowFor(true, true, status->State, status->ErrorCase, status->Reason,
                       status->ActivatedV4, status->ActivatedV6,
                       status->LastActivationRefused, readSetting());
}

// The toggle's local repaint (N7), written before the listener answers and
// replaced by the next status: off gives grey Off; on gives yellow Setting up
// while the device is providing and grey Not providing while it is not. Only
// ever drawn over a visible row, since the toggle hides with the row.
inline ProvideRow ProvideRowGuess(bool on, bool providing) {
  const char* state = !on ? kProvideStateOff
                          : (providing ? kProvideStateSettingUp : kProvideStateNotProviding);
  return ProvideRowFor(true, true, state, std::string(), std::string(), false, false, false, on);
}

// ---- the lag.dev speed tests switch (lag.dev/PLAN.md C4, decision 18) ------
// The switch under the Extender row: whether this device's extender role
// serves lag.dev's measurement API, so people testing their connection there
// can measure to it. On by default. It rides on the role, so it shows exactly
// while the Extender row does and its own write has a taker
// (provide::MeasurementsSwitchSourceFor): hidden, never disabled (N1).
struct SpeedTestsRow {
  bool visible = false;
  // the setting, read beside the Extender row only when this row will show
  bool on = true;

  friend bool operator==(const SpeedTestsRow& a, const SpeedTestsRow& b) {
    return a.visible == b.visible && a.on == b.on;
  }
  friend bool operator!=(const SpeedTestsRow& a, const SpeedTestsRow& b) { return !(a == b); }
};

// `extenderRow` is the Extender row as drawn, `writable` whether the switch's
// write has a taker, and `readSetting` the measurements setting, asked only of
// a row that will show.
template <typename ReadSetting>
SpeedTestsRow SpeedTestsRowOf(const ProvideRow& extenderRow, bool writable,
                              const ReadSetting& readSetting) {
  SpeedTestsRow row;
  if (!extenderRow.visible || !writable) return row;
  row.visible = true;
  row.on = readSetting();
  return row;
}

// The row's one line of state text. `lookup(key, english)` is the catalog --
// T_ in the widgets, the English source in the tests -- and
// `format(pattern, argument)` the store's {} substitution (I18n.hpp's Format).
// Every key goes through `format`, the ones without a placeholder included,
// which it leaves as they are; an empty Reason fills a {} with nothing rather
// than leaving the braces on screen.
template <typename Lookup, typename FormatOne>
std::string StateTextFor(const ProvideRow& row, const Lookup& lookup, const FormatOne& format) {
  if (!row.visible) return std::string();
  if (row.textKey[0] == '\0') return row.argument;
  const std::string pattern = lookup(row.textKey, row.textEnglish);
  std::string text =
      row.familiesKey[0] != '\0'
          ? format(pattern, std::string(lookup(row.familiesKey, row.familiesEnglish)))
          : format(pattern, row.argument);
  if (row.detailKey[0] != '\0') {
    text += kProvideTextSeparator;
    text += format(std::string(lookup(row.detailKey, row.detailEnglish)), row.detailArgument);
  }
  return text;
}

// The visibility of the earnings page's two statistics groups (O4, O5, O8).
struct StatsSections {
  // The provider statistics' chart rows (Local, the transport bar, Blocked):
  // the provide control mode is not never AND the device reports provider
  // packet stats, the gate macOS applies. The group's title, the provide mode
  // row and the extender row stay on screen either way.
  bool providerVisible = false;
  // The extender statistics group, header and chart row: the provider
  // statistics are visible AND the role is running (ExtenderProvideStatus's
  // Enabled, from the pushed status, never the throughput tick or the points).
  bool extenderVisible = false;
  // providing_disabled as the provider group header's meta label: exactly
  // while the provider statistics are not visible.
  bool disabledMeta = true;

  friend bool operator==(const StatsSections& a, const StatsSections& b) {
    return a.providerVisible == b.providerVisible && a.extenderVisible == b.extenderVisible &&
           a.disabledMeta == b.disabledMeta;
  }
  friend bool operator!=(const StatsSections& a, const StatsSections& b) { return !(a == b); }
};

inline StatsSections StatsSectionsFor(bool providingEnabled, bool hasProviderStats,
                                      bool extenderRunning) {
  StatsSections sections;
  sections.providerVisible = providingEnabled && hasProviderStats;
  sections.extenderVisible = sections.providerVisible && extenderRunning;
  sections.disabledMeta = !sections.providerVisible;
  return sections;
}

}  // namespace urnw::extender
