// The owner's sharing limits on the connect page (feature 10, owner decision
// 2026-11-28): the "Sharing limits" section after the provide group, its two
// pickers and the line under them, decided in pure steps the page only draws
// (ConnectPage::ApplyShareLimits, ApplyShareStatus). The sdk keeps and applies
// the limits; 0 is no limit.
//
//   * Max sharing speed: No limit, then 1, 2, 5, 10, 20, 50 and 100 Mbps, a
//     megabit a second being 125,000 bytes a second. A speed the list does not
//     have (one the CLI set) is an option of its own, in its place by size,
//     labelled with its whole Mbps.
//   * Monthly data limit: No limit, then 5, 10, 25, 50, 100, 250, 500 and 1024
//     GiB, labelled by the app's byte formatter; a value the list does not have
//     is an option of its own the same way.
//   * The line under them: the month's budget reached says when sharing with
//     others resumes; with a monthly limit, the month's data of it; otherwise
//     the month's data.
//
// Free of GTK, gettext and the SDK so tests/ProvideSharePresentationTest.cpp
// runs anywhere; the page hands in the store's words and the formatter.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace urnw::provide_share {

inline constexpr int64_t kBytesPerSecondPerMbps = 125000;
inline constexpr int64_t kBytesPerGib = 1024LL * 1024LL * 1024LL;

// The choices after No limit, smallest first.
inline const std::vector<int64_t>& SpeedChoicesMbps() {
  static const std::vector<int64_t> kChoices = {1, 2, 5, 10, 20, 50, 100};
  return kChoices;
}
inline const std::vector<int64_t>& MonthlyChoicesGib() {
  static const std::vector<int64_t> kChoices = {5, 10, 25, 50, 100, 250, 500, 1024};
  return kChoices;
}

// One choice: the limit it writes (bytes a second, or bytes a month; 0 is no
// limit) and its label.
struct Option {
  int64_t value = 0;
  std::string label;

  bool operator==(const Option& other) const {
    return value == other.value && label == other.label;
  }
  bool operator!=(const Option& other) const { return !(*this == other); }
};

// A picker as it is drawn: its options in order and the one selected.
struct Picker {
  std::vector<Option> options;
  size_t selected = 0;

  bool operator==(const Picker& other) const {
    return options == other.options && selected == other.selected;
  }
  bool operator!=(const Picker& other) const { return !(*this == other); }
};

// The words the labels are built from, supplied by the page (the tests pass
// tagged stand-ins).
struct Words {
  std::string noLimit;                                  // provide_share_no_limit
  std::function<std::string(int64_t mbps)> mbps;        // provide_share_speed_mbps
  std::function<std::string(int64_t byteCount)> bytes;  // the app's byte formatter
};

// No limit, then `values` (smallest first) labelled by `label`, with `current`
// in its place by size when the list does not have it, and `current` selected.
// A negative limit is no limit, as the sdk keeps it.
inline Picker PickerFor(int64_t current, const std::vector<int64_t>& values,
                        const std::string& noLimit,
                        const std::function<std::string(int64_t value)>& label) {
  Picker picker;
  picker.options.push_back(Option{0, noLimit});
  const int64_t wanted = current > 0 ? current : 0;
  bool placed = wanted == 0;
  for (const int64_t value : values) {
    if (!placed && wanted < value) {
      picker.selected = picker.options.size();
      picker.options.push_back(Option{wanted, label(wanted)});
      placed = true;
    }
    if (value == wanted) {
      picker.selected = picker.options.size();
      placed = true;
    }
    picker.options.push_back(Option{value, label(value)});
  }
  if (!placed) {
    picker.selected = picker.options.size();
    picker.options.push_back(Option{wanted, label(wanted)});
  }
  return picker;
}

// The whole megabits a second of a speed in bytes a second, rounded.
inline int64_t MbpsOf(int64_t bytesPerSecond) {
  return static_cast<int64_t>(
      std::llround(static_cast<double>(bytesPerSecond) / static_cast<double>(kBytesPerSecondPerMbps)));
}

// The Max sharing speed picker for the limits' max bytes per second.
inline Picker SpeedPickerFor(int64_t maxBytesPerSecond, const Words& words) {
  std::vector<int64_t> values;
  for (const int64_t mbps : SpeedChoicesMbps()) values.push_back(mbps * kBytesPerSecondPerMbps);
  return PickerFor(maxBytesPerSecond, values, words.noLimit,
                   [&words](int64_t bytesPerSecond) { return words.mbps(MbpsOf(bytesPerSecond)); });
}

// The Monthly data limit picker for the limits' monthly byte count.
inline Picker MonthlyPickerFor(int64_t monthlyByteCount, const Words& words) {
  std::vector<int64_t> values;
  for (const int64_t gib : MonthlyChoicesGib()) values.push_back(gib * kBytesPerGib);
  return PickerFor(monthlyByteCount, values, words.noLimit,
                   [&words](int64_t byteCount) { return words.bytes(byteCount); });
}

// The line under the pickers.
enum class Line {
  Hidden,       // no status: no device takes the limits
  LimitReached, // provide_share_limit_reached, with the month's end as a date
  UsedOfLimit,  // provide_share_used_of_limit, the month's data and the limit
  Used,         // provide_share_used, the month's data
};

inline Line LineFor(bool hasStatus, bool budgetReached, int64_t monthlyByteCount) {
  if (!hasStatus) return Line::Hidden;
  if (budgetReached) return Line::LimitReached;
  if (monthlyByteCount > 0) return Line::UsedOfLimit;
  return Line::Used;
}

}  // namespace urnw::provide_share
