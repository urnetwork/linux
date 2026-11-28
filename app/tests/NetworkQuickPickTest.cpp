// The Network page's best available detail (NetworkQuickPick.hpp and
// NetworkPage::RenderDetail): two notes say what Best available provider
// means and how to leave it, then the first five countries ride the pane as
// the list's own rows. The page needs GTK and the SDK, so the wiring cases
// read its source with the comments blanked.
// SPDX-License-Identifier: MPL-2.0
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#include "NetworkQuickPick.hpp"
#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadQuickPickSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  bool inString = false;
  for (size_t at = 0; at < text.size(); ++at) {
    const char c = text[at];
    if (inString) {
      if (c == '\\') {
        ++at;
      } else if (c == '"' || c == '\n') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
      while (at < text.size() && text[at] != '\n') text[at++] = ' ';
    }
  }
  return text;
}

// From the definition that starts with `signature` to the closing brace in
// column 0 that ends it; empty when it is gone.
std::string QuickPickBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Every needle occurs, in this order.
bool QuickPickInOrder(const std::string& text, std::initializer_list<const char*> needles) {
  size_t at = 0;
  for (const char* needle : needles) {
    at = text.find(needle, at);
    if (at == std::string::npos) return false;
    at += 1;
  }
  return true;
}

}  // namespace

// No countries hide the group; fewer than five show them all; more show five.
UR_TEST(NetworkQuickPick_ShowsAtMostFiveCountries) {
  UR_EXPECT_EQ(size_t{0}, urnw::QuickPickCount(0));
  UR_EXPECT_EQ(size_t{1}, urnw::QuickPickCount(1));
  UR_EXPECT_EQ(size_t{3}, urnw::QuickPickCount(3));
  UR_EXPECT_EQ(size_t{5}, urnw::QuickPickCount(5));
  UR_EXPECT_EQ(size_t{5}, urnw::QuickPickCount(9));
  UR_EXPECT_EQ(size_t{5}, urnw::QuickPickCount(250));
}

// Under Best available provider the pane says what it means and how to leave
// it, then offers the first countries as the list's own rows, under a header
// with no count (Available providers counts them all); a concrete selection
// shows none of it.
UR_TEST(NetworkQuickPick_TheBestAvailableDetailExplainsAndOffers) {
  const std::string page = ReadQuickPickSource("NetworkPage.cpp");
  UR_EXPECT_TRUE(!page.empty());
  const std::string detail = QuickPickBody(page, "void NetworkPage::RenderDetail()");
  const size_t concrete = detail.find("} else {");
  UR_EXPECT_TRUE(concrete != std::string::npos);
  const std::string best = detail.substr(0, concrete);
  UR_EXPECT_TRUE(QuickPickInOrder(
      best, {"if (best) {", "MakeDetailNote(T_(", "\"adv_best_available_note\",",
             "\"URnetwork picks the fastest healthy providers for you, with no location constraint, \"",
             "\"and re-picks as the network changes.\"", "MakeDetailNote(",
             "T_(\"adv_pick_location_note\",",
             "\"Pick a country, region, city or device in the list to connect there instead.\"",
             "QuickPickCount(locations_->Countries->size())", "if (shown > 0) {",
             "kit::MakePaneGroupHeader(T_(\"countries\", \"Countries\")).root",
             "for (size_t i = 0; i < shown; ++i) {",
             "MakeLocationRow((*locations_->Countries)[i], /*selected=*/false)"}));
  UR_EXPECT_TRUE(best.find("std::to_string(shown)") == std::string::npos);
  const std::string other = detail.substr(concrete);
  UR_EXPECT_TRUE(other.find("adv_best_available_note") == std::string::npos);
  UR_EXPECT_TRUE(other.find("MakeLocationRow(") == std::string::npos);
}

// A quick-pick row is the list's row: one builder, whose click connects
// through the row coalescer as the list's does.
UR_TEST(NetworkQuickPick_TheListAndTheQuickPickShareOneRow) {
  const std::string page = ReadQuickPickSource("NetworkPage.cpp");
  UR_EXPECT_TRUE(QuickPickInOrder(QuickPickBody(page, "void NetworkPage::AppendLocationSection("),
                                  {"for (const auto& location : *items) {",
                                   "listHost_->append(*MakeLocationRow(location, "
                                   "IsLocationSelected(selected, location)));"}));
  UR_EXPECT_TRUE(QuickPickInOrder(
      QuickPickBody(page, "Gtk::Button* NetworkPage::MakeLocationRow("),
      {"TN_(\"provider_count\", \"{} provider\", \"{} providers\", providerCount)",
       // the row's name: a country in the app's language (LocationDisplayName)
       "MakeRow(SanitizeExternalDisplayText(LocationDisplayName(location)), meta,",
       "LocationRowColor(location), selected, !location.stable,",
       "row->signal_clicked().connect([this, copy] {", "host_.ConnectFromRow(copy);", "Render();",
       "return row;"}));
}
