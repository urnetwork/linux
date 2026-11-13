// External names filtered for display (DisplayText.hpp): a Zalgo stack keeps
// one mark per base, the invisible format characters go, and everything a
// real name is made of passes byte for byte. The display sites need GTK and
// the SDK, so the wiring cases read their sources.
// SPDX-License-Identifier: MPL-2.0
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "DisplayText.hpp"
#include "PeerLocation.hpp"
#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::SanitizeExternalDisplayText;

bool Same(const std::string& expected, const std::string& actual) {
  if (expected == actual) return true;
  std::string hex;
  for (unsigned char c : actual) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02x", c);
    hex += buf;
  }
  UR_FAIL("got bytes " + hex);
  return false;
}

// The fields of urnet::NetworkPeer PeerDisplayName reads, by the generated
// urnetwork_sdk.hpp's names and types.
struct DisplayPeer {
  std::optional<std::string> ClientId;
  std::string DeviceSpec{};
  std::string DeviceName{};
};

std::string ReadDisplaySource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// From the definition that starts with `signature` to the closing brace in
// column 0 that ends it; empty when it is gone.
std::string DisplayBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace

// What real names are made of passes unchanged: ASCII, precomposed Latin,
// CJK, Cyrillic, Arabic, Devanagari with its own marks, and emoji.
UR_TEST(DisplayText_RealNamesPassByteForByte) {
  for (const char* name :
       {"Office laptop", "", "caf\xC3\xA9 r\xC3\xA9seau",  // café réseau, é precomposed
        "\xE6\x9D\xB1\xE4\xBA\xAC",                         // 東京
        "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0",  // Москва
        "\xD8\xB4\xD8\xA8\xD9\x83\xD8\xA9",                 // شبكة
        "\xE0\xA4\xA8\xE0\xA5\x87\xE0\xA4\x9F",             // नेट, a Devanagari sign
        "\xF0\x9F\x9A\x80 rocket"}) {                        // 🚀
    Same(name, SanitizeExternalDisplayText(name));
  }
}

// One combining mark on a base is a legitimate accent and stays.
UR_TEST(DisplayText_OneMarkStays) {
  Same("e\xCC\x81", SanitizeExternalDisplayText("e\xCC\x81"));  // e + U+0301
  Same("ae\xCC\x81i\xCC\x88", SanitizeExternalDisplayText("ae\xCC\x81i\xCC\x88"));
}

// A stack keeps its first mark, on every base, in every range.
UR_TEST(DisplayText_AStackKeepsItsFirstMark) {
  // e + U+0300 U+0301 U+0302
  Same("e\xCC\x80", SanitizeExternalDisplayText("e\xCC\x80\xCC\x81\xCC\x82"));
  // Z + U+0351 U+1AB0 U+1DC0 U+20D0, a + U+0315 U+0315
  Same("Z\xCD\x91"
       "a\xCC\x95",
       SanitizeExternalDisplayText("Z\xCD\x91\xE1\xAA\xB0\xE1\xB7\x80\xE2\x83\x90"
                                   "a\xCC\x95\xCC\x95"));
}

// A mark with no base before it has nothing to attach to.
UR_TEST(DisplayText_ALeadingMarkIsDropped) {
  Same("x", SanitizeExternalDisplayText("\xCC\x81x"));
  Same("", SanitizeExternalDisplayText("\xCC\x81\xCC\x82"));
}

// The zero-width space, the directional marks, the bidi embeddings, overrides
// and isolates, the word joiner and the byte order mark are dropped.
UR_TEST(DisplayText_HiddenFormatCharactersAreDropped) {
  Same("abc", SanitizeExternalDisplayText("a\xE2\x80\x8B"
                                          "b\xE2\x80\x8E"
                                          "c\xE2\x80\x8F"));  // U+200B U+200E U+200F
  Same("evil.exe", SanitizeExternalDisplayText("evil\xE2\x80\xAE.exe"));     // U+202E
  Same("name", SanitizeExternalDisplayText("\xE2\x81\xA6name\xE2\x81\xA9"));  // U+2066 U+2069
  Same("ab", SanitizeExternalDisplayText("a\xE2\x81\xA0"
                                         "b"));  // U+2060
  Same("name", SanitizeExternalDisplayText("\xEF\xBB\xBFname"));  // U+FEFF
  // a format character is no base: the mark after it is still the base's second
  Same("e\xCC\x81", SanitizeExternalDisplayText("e\xCC\x81\xE2\x80\x8B\xCC\x82"));
}

// ZWJ and ZWNJ are kept, so emoji sequences and joining scripts survive, but
// they cannot restart a stack.
UR_TEST(DisplayText_JoinControlsAreKeptButStartNoStack) {
  // 👨‍👩‍👧: U+1F468 ZWJ U+1F469 ZWJ U+1F467
  const std::string family =
      "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
  Same(family, SanitizeExternalDisplayText(family));
  // می‌خواهم with its ZWNJ
  const std::string persian = "\xD9\x85\xDB\x8C\xE2\x80\x8C\xD8\xAE\xD9\x88\xD8\xA7\xD9\x87\xD9\x85";
  Same(persian, SanitizeExternalDisplayText(persian));
  // e U+0301 ZWJ U+0302 ZWJ U+0303: the ZWJs stay, the second and third marks go
  Same("e\xCC\x81\xE2\x80\x8D\xE2\x80\x8D",
       SanitizeExternalDisplayText("e\xCC\x81\xE2\x80\x8D\xCC\x82\xE2\x80\x8D\xCC\x83"));
}

// Bytes that are not UTF-8 pass through rather than being guessed at.
UR_TEST(DisplayText_IllFormedBytesPassThrough) {
  Same("a\xC3", SanitizeExternalDisplayText("a\xC3"));                // a lone lead byte
  Same("a\xE2\x80", SanitizeExternalDisplayText("a\xE2\x80"));        // a truncated sequence
  Same("\xC3(x", SanitizeExternalDisplayText("\xC3(x"));              // a bad continuation
  Same("\xFF\xFE", SanitizeExternalDisplayText("\xFF\xFE"));
}

// A peer's name is another device's, so the name every peer row and the
// Connect page's location row show is filtered.
UR_TEST(DisplayText_APeerNameIsFiltered) {
  DisplayPeer peer;
  peer.ClientId = "018f2c3e-7a10-7b44-9c1d-5e2a3f4b5c6d";
  peer.DeviceName = "Lap\xE2\x80\xAEtop e\xCC\x81\xCC\x81\xCC\x81";
  Same("Laptop e\xCC\x81", urnw::PeerDisplayName(peer));
  peer.DeviceName.clear();
  peer.DeviceSpec = "\xE2\x80\x8BUbuntu";
  Same("Ubuntu", urnw::PeerDisplayName(peer));
}

// Every place an external name is shown filters it: the location rows, the
// Network page's detail, the Connect page's location row and peers, the
// status strip's network, the Account page's network name row, the
// Earnings boards' names, and the referral network's name on the Referrals
// page's row and in its sheet.
UR_TEST(DisplayText_TheDisplaySitesFilterExternalNames) {
  struct Site {
    const char* file;
    const char* needle;
  };
  const Site sites[] = {
      {"LocationsSheet.cpp", "SanitizeExternalDisplayText(location.name.value_or(std::string()))"},
      {"LocationsSheet.cpp", "SanitizeExternalDisplayText(peer.DeviceSpec)"},
      {"NetworkPage.cpp", "SanitizeExternalDisplayText(location.name.value_or(std::string()))"},
      {"NetworkPage.cpp", "SanitizeExternalDisplayText(peer.DeviceSpec)"},
      {"NetworkPage.cpp", "SanitizeExternalDisplayText(loc.name.value_or(std::string()))"},
      {"NetworkPage.cpp", "SanitizeExternalDisplayText(loc.country.value_or(std::string()))"},
      {"NetworkPage.cpp", "SanitizeExternalDisplayText(loc.region.value_or(std::string()))"},
      {"NetworkPage.cpp", "SanitizeExternalDisplayText(loc.city.value_or(std::string()))"},
      {"ConnectPage.cpp", "SanitizeExternalDisplayText(location->name.value_or(std::string()))"},
      {"ConnectPage.cpp", "SanitizeExternalDisplayText(peer.DeviceSpec)"},
      {"MainWindow.cpp", "SanitizeExternalDisplayText(byJwt->NetworkName)"},
      {"EarningsPage.cpp", "SanitizeExternalDisplayText(earner.network_name)"},
      {"EarningsPage.cpp", "SanitizeExternalDisplayText(r.displayName)"},
      {"EarningsPage.cpp", "SanitizeExternalDisplayText(pointsMe_->displayName)"},
      {"EarningsPage.cpp", "SanitizeExternalDisplayText(jwt->NetworkName)"},
      {"ReferralsPage.cpp",
       "ApplyFieldState(*referralNetworkRow_.value, state, SanitizeExternalDisplayText(name));"},
      {"ReferralsPage.cpp", "name_ = SanitizeExternalDisplayText(name);\n"
                            "    ApplyFieldState(*current_, state, name_);"},
  };
  for (const Site& site : sites) {
    const std::string source = ReadDisplaySource(site.file);
    if (source.find(site.needle) == std::string::npos) {
      UR_FAIL(std::string(site.file) + " shows an unfiltered name: " + site.needle);
    }
  }
}

// The filter is for display only: the network name editor is seeded with the
// name the server acknowledged, and the name typed at sign-up is sent as
// typed.
UR_TEST(DisplayText_TypedAndSeededTextIsNeverFiltered) {
  const std::string account = ReadDisplaySource("AccountPage.cpp");
  const std::string apply = DisplayBody(account, "void AccountPage::ApplyNetworkName(");
  UR_EXPECT_TRUE(apply.find("acknowledgedName_ = name;") != std::string::npos);
  UR_EXPECT_TRUE(apply.find("const std::string shown = SanitizeExternalDisplayText(name);") !=
                 std::string::npos);
  UR_EXPECT_TRUE(apply.find("kit::SetTextOrCollapse(*nameRow_.value, shown);") !=
                 std::string::npos);
  const std::string plan = DisplayBody(account, "void AccountPage::ApplyPlanAccount(");
  UR_EXPECT_TRUE(plan.find("urnet::planAccountName(SanitizeExternalDisplayText(acknowledgedName_), balance_.guest)") !=
                 std::string::npos);
  const std::string editor = DisplayBody(account, "void AccountPage::OpenNameEditor(");
  UR_EXPECT_TRUE(editor.find("nameBox_->set_text(acknowledgedName_);") != std::string::npos);
  UR_EXPECT_TRUE(editor.find("SanitizeExternalDisplayText") == std::string::npos);
  const std::string save = DisplayBody(account, "void AccountPage::OnSaveNetworkName(");
  UR_EXPECT_TRUE(save.find("const std::string name = TrimSpace(nameBox_->get_text().raw());") !=
                 std::string::npos);
  UR_EXPECT_TRUE(save.find("args.new_name = name;") != std::string::npos);
  UR_EXPECT_TRUE(save.find("SanitizeExternalDisplayText") == std::string::npos);
  UR_EXPECT_TRUE(ReadDisplaySource("AuthViews.cpp").find("SanitizeExternalDisplayText") ==
                 std::string::npos);
}
