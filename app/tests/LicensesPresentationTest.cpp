// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "LicensesPresentation.hpp"

namespace urnw {
namespace {

LicenseEntry Entry(const std::string& name, const std::string& kind,
                   const std::string& version = {}, const std::string& spdx = {}) {
  LicenseEntry entry;
  entry.name = name;
  entry.kind = kind;
  entry.version = version;
  entry.spdx = spdx;
  return entry;
}

UR_TEST(LicenseSubtitleJoinsVersionAndSpdx) {
  UR_EXPECT_TRUE(LicenseSubtitle(Entry("a", "software", "v1.2.3", "MIT")) ==
                 "v1.2.3 · MIT");
}

UR_TEST(LicenseSubtitleOmitsEmptyHalves) {
  UR_EXPECT_TRUE(LicenseSubtitle(Entry("a", "software", "v1.2.3", "")) == "v1.2.3");
  UR_EXPECT_TRUE(LicenseSubtitle(Entry("a", "data", "", "CC-BY-4.0")) == "CC-BY-4.0");
  UR_EXPECT_TRUE(LicenseSubtitle(Entry("a", "data")).empty());
}

UR_TEST(PartitionKeepsTheSdkOrderInEachSection) {
  const auto sections = PartitionLicenses({
      Entry("GeoLite2 by MaxMind", "data"),
      Entry("GeoNames", "data"),
      Entry("zeta", "software"),
      Entry("alpha", "software"),
  });
  UR_EXPECT_EQ(2, sections.data.size());
  UR_EXPECT_EQ(2, sections.software.size());
  UR_EXPECT_TRUE(sections.data[0].name == "GeoLite2 by MaxMind");
  UR_EXPECT_TRUE(sections.data[1].name == "GeoNames");
  // never re-sorted: the SDK's order is the display order
  UR_EXPECT_TRUE(sections.software[0].name == "zeta");
  UR_EXPECT_TRUE(sections.software[1].name == "alpha");
}

UR_TEST(PartitionShowsFontsAndUnknownKindsAsSoftware) {
  const auto sections = PartitionLicenses({
      Entry("a font", "font"),
      Entry("from a newer sdk", "firmware"),
      Entry("no kind", ""),
  });
  UR_EXPECT_EQ(0, sections.data.size());
  UR_EXPECT_EQ(3, sections.software.size());
}

UR_TEST(PartitionSkipsNullSlots) {
  // a `null` in the SDK's JSON list parses to an all-empty entry
  const auto sections = PartitionLicenses({LicenseEntry{}, Entry("x", "software")});
  UR_EXPECT_EQ(1, sections.software.size());
  UR_EXPECT_EQ(0, sections.data.size());
}

UR_TEST(CopyrightLinesDropBlanksAndTrailingNewline) {
  const auto lines = LicenseCopyrightLines("Copyright (c) 2020 A\n\nCopyright 2021 B\r\n");
  UR_EXPECT_EQ(2, lines.size());
  UR_EXPECT_TRUE(lines[0] == "Copyright (c) 2020 A");
  UR_EXPECT_TRUE(lines[1] == "Copyright 2021 B");
  UR_EXPECT_EQ(0, LicenseCopyrightLines("").size());
  UR_EXPECT_EQ(0, LicenseCopyrightLines("\n  \n").size());
}

UR_TEST(ProjectPageIsOnlyAnHttpUrl) {
  LicenseEntry entry = Entry("a", "software");
  entry.url = "https://www.maxmind.com";
  UR_EXPECT_TRUE(LicenseHasProjectPage(entry));
  entry.url = "http://example.org";
  UR_EXPECT_TRUE(LicenseHasProjectPage(entry));
  entry.url = "";
  UR_EXPECT_FALSE(LicenseHasProjectPage(entry));
  entry.url = "file:///etc/passwd";
  UR_EXPECT_FALSE(LicenseHasProjectPage(entry));
  entry.url = "javascript:alert(1)";
  UR_EXPECT_FALSE(LicenseHasProjectPage(entry));
}

}  // namespace
}  // namespace urnw
