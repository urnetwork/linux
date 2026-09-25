// What the Licenses sheet (LicensesSheet.cpp) shows, decided without GTK or the
// SDK so the rules are unit-tested (tests/LicensesPresentationTest.cpp).
//
// The list is the SDK's embedded license.yml (sdk.GetLicenses("linux")): the
// open source software and data attributions this app includes. The SDK hands
// the entries over in DISPLAY ORDER -- data attributions first (GeoLite2 by
// MaxMind leads, and its notice is the attribution its license requires), then
// software and fonts by name -- and that order is kept verbatim inside each of
// the sheet's two sections. The only decision taken here is which section an
// entry belongs to: kind "data" is "Data attributions", every other kind
// (software, font, and any kind a newer SDK adds) is "Open source software",
// so an unknown kind is still shown rather than dropped.
//
// A NOTICE, when present, is shown verbatim and never trimmed: it is text a
// license requires be published, so it is rendered in full on the row and on
// the detail, not ellipsized.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace urnw {

// The SDK's LicenseInfo, as plain strings (urnet::LicenseInfo carries exactly
// these; the sheet copies them over so this header needs no SDK include).
struct LicenseEntry {
  std::string name;
  std::string version;
  std::string kind;  // "data" | "software" | "font"
  std::string url;
  std::string spdx;
  std::string copyright;  // newline separated
  std::string notice;     // shown verbatim when non-empty; usually empty
  std::string text;       // the full license text
};

inline constexpr const char* kLicenseKindData = "data";

// The row subtitle: "version · spdx", either half omitted when empty, "" when
// both are (the row then collapses its note line).
inline std::string LicenseSubtitle(const LicenseEntry& entry) {
  if (entry.version.empty()) return entry.spdx;
  if (entry.spdx.empty()) return entry.version;
  return entry.version + " · " + entry.spdx;
}

// The copyright block split into its lines, blank lines dropped (the SDK joins
// the lines it found with '\n'; a trailing newline must not add an empty line).
inline std::vector<std::string> LicenseCopyrightLines(const std::string& copyright) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= copyright.size()) {
    size_t end = copyright.find('\n', start);
    if (end == std::string::npos) end = copyright.size();
    std::string line = copyright.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") != std::string::npos) lines.push_back(std::move(line));
    start = end + 1;
  }
  return lines;
}

// Only an http(s) URL becomes the "Project page" link: the value is data from
// a generated file, and a link label must never launch anything else.
inline bool LicenseHasProjectPage(const LicenseEntry& entry) {
  return entry.url.rfind("https://", 0) == 0 || entry.url.rfind("http://", 0) == 0;
}

struct LicenseSections {
  std::vector<LicenseEntry> data;      // "Data attributions"
  std::vector<LicenseEntry> software;  // "Open source software" (everything else)
};

// Splits the SDK's list into the two sections, preserving the SDK's order in
// each. An entry with no name is skipped: the JSON list may carry a null slot
// (LicenseInfoList is `LicenseInfo | null[]`), which parses to an empty entry
// and has nothing to show.
inline LicenseSections PartitionLicenses(std::vector<LicenseEntry> entries) {
  LicenseSections sections;
  for (auto& entry : entries) {
    if (entry.name.empty()) continue;
    if (entry.kind == kLicenseKindData) {
      sections.data.push_back(std::move(entry));
    } else {
      sections.software.push_back(std::move(entry));
    }
  }
  return sections;
}

}  // namespace urnw
