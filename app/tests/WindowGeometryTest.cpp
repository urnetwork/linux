// The window's size across runs (WindowGeometry.hpp): a saved size opens again
// when the window could have had it, and anything else (never saved, zero,
// negative, under the minimum, beyond any monitor) opens at the default. The
// window and its close and quit need GTK, so the wiring cases read
// MainWindow.cpp and main.cpp with the comments blanked; the one-write merge
// the save goes through runs on a temporary directory.
// SPDX-License-Identifier: MPL-2.0
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "PrefsFile.hpp"
#include "TestHarness.hpp"
#include "WindowGeometry.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::window_geometry::SizeToOpenAt;
namespace geometry = urnw::window_geometry;

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadGeometrySource(const std::string& relative) {
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

std::string GeometryBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The text from `from` to the first `to` after it, or nothing.
std::string GeometrySpan(const std::string& text, const std::string& from, const std::string& to) {
  const size_t start = text.find(from);
  if (start == std::string::npos) return std::string();
  const size_t end = text.find(to, start + from.size());
  return end == std::string::npos ? std::string() : text.substr(start, end - start);
}

// Each needle occurs after the one before it.
bool GeometryInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

bool IsDefault(geometry::Size size) {
  return size.width == geometry::kDefaultWidth && size.height == geometry::kDefaultHeight;
}

}  // namespace

UR_TEST(WindowGeometry_ASavedSizeOpensAgain) {
  const geometry::Size size = SizeToOpenAt(900, 700);
  UR_EXPECT_EQ(900, size.width);
  UR_EXPECT_EQ(700, size.height);
  const geometry::Size smallest = SizeToOpenAt(geometry::kMinWidth, geometry::kMinHeight);
  UR_EXPECT_EQ(geometry::kMinWidth, smallest.width);
  UR_EXPECT_EQ(geometry::kMinHeight, smallest.height);
  UR_EXPECT_EQ(geometry::kMaxSide, SizeToOpenAt(geometry::kMaxSide, 900).width);
}

UR_TEST(WindowGeometry_ASizeTheWindowCouldNotHaveOpensAtTheDefault) {
  UR_EXPECT_EQ(1120, geometry::kDefaultWidth);
  UR_EXPECT_EQ(820, geometry::kDefaultHeight);
  // never saved
  UR_EXPECT_TRUE(IsDefault(SizeToOpenAt(0, 0)));
  UR_EXPECT_TRUE(IsDefault(SizeToOpenAt(900, 0)));
  UR_EXPECT_TRUE(IsDefault(SizeToOpenAt(-900, 700)));
  UR_EXPECT_TRUE(IsDefault(SizeToOpenAt(geometry::kMinWidth - 1, 700)));
  UR_EXPECT_TRUE(IsDefault(SizeToOpenAt(900, geometry::kMinHeight - 1)));
  UR_EXPECT_TRUE(IsDefault(SizeToOpenAt(geometry::kMaxSide + 1, 700)));
  // a value no int can hold is no size either
  UR_EXPECT_TRUE(IsDefault(SizeToOpenAt(int64_t{1} << 40, 700)));
}

// The save is one write of the three keys, beside the other preferences, and
// a close that changed nothing writes nothing.
UR_TEST(WindowGeometry_TheSaveIsOneMergeThatKeepsTheRest) {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "urnw-geometry-XXXXXX").string();
  UR_EXPECT_TRUE(::mkdtemp(pattern.data()) != nullptr);
  const std::string path = pattern + "/app_prefs.json";
  UR_EXPECT_EQ(0, urnw::prefs::SetAt(path, "advanced_mode", true));
  const nlohmann::json values = {{geometry::kWidthKey, 900},
                                 {geometry::kHeightKey, 700},
                                 {geometry::kMaximizedKey, true}};
  UR_EXPECT_EQ(0, urnw::prefs::MergeAt(path, values));
  const nlohmann::json all = urnw::prefs::ReadAllAt(path);
  UR_EXPECT_TRUE(all.value("advanced_mode", false));
  UR_EXPECT_EQ(900, all.value(geometry::kWidthKey, 0));
  UR_EXPECT_EQ(700, all.value(geometry::kHeightKey, 0));
  UR_EXPECT_TRUE(all.value(geometry::kMaximizedKey, false));
  struct stat before {};
  ::stat(path.c_str(), &before);
  UR_EXPECT_EQ(0, urnw::prefs::MergeAt(path, values));
  struct stat after {};
  ::stat(path.c_str(), &after);
  // unchanged: the same file, not a replacement
  UR_EXPECT_TRUE(before.st_ino == after.st_ino);
  std::error_code error;
  std::filesystem::remove_all(pattern, error);
}

// The window opens at the restored size, maximized again if it was, and the
// preview harness neither restores nor saves, nor does a run that never showed
// the window; the close (before it hides or minimizes), the tray's Quit
// (before the teardown) and a settled resize or maximize save.
UR_TEST(WindowGeometryWiring_TheWindowRestoresAndEveryWayOutSaves) {
  const std::string window = ReadGeometrySource("MainWindow.cpp");
  const std::string ctor = GeometrySpan(window, "MainWindow::MainWindow(SdkHost& host)", "BuildChrome();");
  UR_EXPECT_TRUE(GeometryInOrder(
      ctor, {"g_getenv(\"URNETWORK_PREVIEW_UI\") == nullptr;", "if (restore) {",
             "window_geometry::SizeToOpenAt(prefs::Get<int64_t>(window_geometry::kWidthKey, 0),",
             "prefs::Get<int64_t>(window_geometry::kHeightKey, 0));",
             "set_default_size(size.width, size.height);",
             "set_size_request(window_geometry::kMinWidth, window_geometry::kMinHeight);",
             "if (restore && prefs::Get<bool>(window_geometry::kMaximizedKey, false)) maximize();"}));
  // and once a resize or a maximize has settled, so an app a logout ends keeps it
  UR_EXPECT_TRUE(geometry::kSaveDebounceMillis == 700);
  UR_EXPECT_TRUE(GeometryInOrder(
      ctor, {"maximize();", "if (restore) {", "geometrySave_.disconnect();",
             "geometrySave_ = Glib::signal_timeout().connect(", "SaveGeometry();",
             "window_geometry::kSaveDebounceMillis);",
             "property_default_width().signal_changed().connect(saveSoon);",
             "property_default_height().signal_changed().connect(saveSoon);",
             "property_maximized().signal_changed().connect(saveSoon);"}));
  UR_EXPECT_TRUE(GeometryInOrder(GeometryBody(window, "MainWindow::~MainWindow() {"),
                                 {"geometrySave_.disconnect();"}));
  const std::string save = GeometryBody(window, "void MainWindow::SaveGeometry() {");
  UR_EXPECT_TRUE(GeometryInOrder(
      save, {"if (g_getenv(\"URNETWORK_PREVIEW_UI\")) return;", "if (!get_realized()) return;",
             "get_default_size(width, height);",
             "is_maximized()", "if (window_geometry::Plausible(width, height)) {",
             "prefs::SetAll(values);"}));
  const std::string main = ReadGeometrySource("main.cpp");
  const std::string close =
      GeometrySpan(main, "window->signal_close_request().connect(", "return true;");
  UR_EXPECT_TRUE(GeometryInOrder(close, {"window->SaveGeometry();", "switch (",
                                         "window->set_visible(false);", "window->minimize();"}));
  const std::string quit = GeometrySpan(main, "const auto endApp = [&](bool teardown) {", "\n  };\n");
  UR_EXPECT_TRUE(GeometryInOrder(quit, {"window->SaveGeometry();", "host->WhenCommandsDrained("}));
}
