// The window's status strip (HomeShell) says what the Connect page says, on
// every destination, as Windows does: its state field is the page's status row
// (the localized word and the dot of the same health::Render, "Disconnecting…"
// included) and its provider field is the page's provider row (the selected
// country, city or peer, or "Best available provider"). Before, the window
// wrote the raw SDK token ("CONNECTED") into the state field and a constant
// "Best available provider" into the provider field. MainWindow and ConnectPage
// need gtkmm, so this reads their sources with the comments blanked.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadStripSource(const std::string& relative) {
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

// The definition that starts with `signature`, up to the closing brace at the
// start of a line.
std::string StripBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool StripHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool StripInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

// The page raises its status row's word and dot at the end of the one render,
// and its provider row's text where it sets it.
UR_TEST(StatusStripWiring_ThePageRaisesWhatItRendered) {
  const std::string page = ReadStripSource("ConnectPage.cpp");
  const std::string status = StripBody(page, "void ConnectPage::ApplyConnectStatus() {");
  UR_EXPECT_TRUE(!status.empty());
  // the text is the state's words, or the SDK's no-post-quantum-exits notice
  // in their place
  UR_EXPECT_TRUE(
      StripInOrder(status, {"Glib::ustring text = T_(view.textKey, view.textEnglish);",
                            "text = Format(T_(\"post_quantum_no_exits\",",
                            "statusText_->set_text(text);", "on_status_rendered(text, dot);"}));
  const std::string location = StripBody(page, "void ConnectPage::ApplyLocationRow() {");
  UR_EXPECT_TRUE(!location.empty());
  UR_EXPECT_TRUE(
      StripInOrder(location, {"locationText_->set_text(text);", "on_location_rendered(text);"}));
  const std::string republish = StripBody(page, "void ConnectPage::RepublishStatus() {");
  UR_EXPECT_TRUE(StripInOrder(republish, {"ApplyConnectStatus();", "ApplyLocationRow();"}));
}

// The window forwards both to the strip, and replays the page's first render,
// which ran before the hooks were wired.
UR_TEST(StatusStripWiring_TheWindowForwardsThePageRender) {
  const std::string window = ReadStripSource("MainWindow.cpp");
  const std::string home = StripBody(window, "void MainWindow::BuildHome() {");
  UR_EXPECT_TRUE(!home.empty());
  UR_EXPECT_TRUE(StripInOrder(home, {"connectPage_->on_status_rendered =",
                                     "shell_->SetStatusState(text, dot);",
                                     "connectPage_->on_location_rendered =",
                                     "shell_->SetStatusProvider(text);",
                                     "connectPage_->RepublishStatus();"}));
}

// Nothing else writes the two fields: not the raw SDK token, and not a
// constant provider on every stats push.
UR_TEST(StatusStripWiring_NoOtherWriterOfStateOrProvider) {
  const std::string window = ReadStripSource("MainWindow.cpp");
  const std::string reading = StripBody(window, "void MainWindow::ApplyConnectReading(");
  UR_EXPECT_TRUE(!reading.empty());
  UR_EXPECT_FALSE(StripHas(reading, "SetStatusState"));
  UR_EXPECT_FALSE(StripHas(reading, "rawStatus"));
  const std::string stats = StripBody(window, "void MainWindow::ApplyStats(");
  UR_EXPECT_TRUE(!stats.empty());
  UR_EXPECT_FALSE(StripHas(stats, "SetStatusProvider"));
  UR_EXPECT_FALSE(StripHas(stats, "best_available_provider"));
  size_t writers = 0;
  for (size_t at = window.find("SetStatusState("); at != std::string::npos;
       at = window.find("SetStatusState(", at + 1)) {
    ++writers;
  }
  UR_EXPECT_EQ(1u, writers);
  writers = 0;
  for (size_t at = window.find("SetStatusProvider("); at != std::string::npos;
       at = window.find("SetStatusProvider(", at + 1)) {
    ++writers;
  }
  UR_EXPECT_EQ(1u, writers);
}
