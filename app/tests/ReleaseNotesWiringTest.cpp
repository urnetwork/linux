// The bundled release note reaches the user (read as source: the sheet and the
// Settings page need GTK). The build installs the file where the sheet reads
// it, through the shared runtime ladder; the App version row opens the sheet
// only when the file gave a note, and is the plain value row otherwise; the row
// names its version and carries the store's hint; the sheet shows the note of
// the UI's catalog under the running version, and its text is never markup.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "WiringSource.hpp"

namespace urnw {
namespace {

using namespace testing::wiring;

// Each needle occurs after the one before it.
bool NotesInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) {
      UR_FAIL("not found in order: " + needle);
      return false;
    }
    from = at + needle.size();
  }
  return true;
}

UR_TEST(ReleaseNotesWiring_TheBuildShipsTheSheetAndTheFile) {
  // read as it is: meson's comments are not C++'s
  const std::string build = ReadRaw("../meson.build");
  UR_EXPECT_TRUE(Before(build, "executable('urnetwork-gui',", "'src/ReleaseNotesSheet.cpp',"));
  UR_EXPECT_TRUE(Before(build, "'src/ReleaseNotesSheet.cpp',", "executable('urnetworkd',"));
  NotesInOrder(build, {"install_data('assets/release-notes.json',",
                       "install_dir : get_option('datadir') / 'urnetwork')"});
  NotesInOrder(build, {"urnetwork_tests = executable('urnetwork-tests',",
                       "'tests/ReleaseNotesTest.cpp',", "'tests/ReleaseNotesWiringTest.cpp',"});
  UR_EXPECT_TRUE(Contains(ReadRaw("../po/POTFILES.in"), "\nsrc/ReleaseNotesSheet.cpp\n"));
}

UR_TEST(ReleaseNotesWiring_TheFileIsReadOnceThroughTheRuntimeLadder) {
  const std::string sheet = ReadCode("ReleaseNotesSheet.cpp");
  // the installed path ($APPDIR first inside an AppImage), then the build tree
  NotesInOrder(FunctionBody(sheet, "ReadBundledReleaseNotes() {"),
               {"ResolveRuntimePath(UR_PKGDATADIR \"/release-notes.json\"",
                "\"assets/release-notes.json\"", "g_file_get_contents(",
                "release_notes::Parse(text)", "if (!notes) {", "return nullptr;"});
  // (the type in front, which "ReadBundledReleaseNotes() {" does not carry)
  NotesInOrder(FunctionBody(sheet, "release_notes::ReleaseNotes> BundledReleaseNotes() {"),
               {"static const std::shared_ptr<const release_notes::ReleaseNotes> notes =",
                "ReadBundledReleaseNotes();", "return notes;"});
}

UR_TEST(ReleaseNotesWiring_TheVersionRowOpensTheNoteOnlyWhenThereIsOne) {
  const std::string page = ReadCode("SettingsPage.cpp");
  const std::string version =
      FunctionBody(page, "void SettingsPage::BuildVersionSection(Gtk::Box& host) {");
  UR_EXPECT_TRUE_MSG("could not read BuildVersionSection", !version.empty());
  // no note: the plain value row, as before; a note: the row button
  NotesInOrder(version, {"if (!BundledReleaseNotes()) {",
                         "AddValueRow(host, T_(\"app_version\", \"App version\"));",
                         "UR_APP_VERSION);", "return;",
                         "kit::MakePaneTwoLineRowButton(T_(\"app_version\", \"App version\")",
                         "ApplyFieldState(*appRow.value, SettingsFieldState::Loaded, "
                         "UR_APP_VERSION);",
                         "const Glib::ustring hint = T_(\"release_notes\", \"Release notes\");",
                         "UR_APP_VERSION);", "GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, hint.c_str()",
                         "appRow.root->set_tooltip_text(hint);",
                         "appRow.root->signal_clicked().connect("
                         "[this] { ShowReleaseNotesSheet(); });",
                         "host.append(*appRow.root);"});
  // one sheet for both copies of the row (pane C and pane A's About fold)
  NotesInOrder(FunctionBody(page, "void SettingsPage::ShowReleaseNotesSheet() {"),
               {"if (!releaseNotesSheet_) {", "BundledReleaseNotes();", "if (!notes) return;",
                "std::make_unique<ReleaseNotesSheet>(*root, std::move(notes));",
                "releaseNotesSheet_->Open();"});
  UR_EXPECT_EQ(size_t{1}, CountOf(page, "std::make_unique<ReleaseNotesSheet>("));
}

UR_TEST(ReleaseNotesWiring_TheSheetShowsTheUisNoteUnderTheRunningVersion) {
  const std::string sheet = ReadCode("ReleaseNotesSheet.cpp");
  // the catalog gettext loaded names the UI's language; the same lookup T_ makes
  NotesInOrder(FunctionBody(sheet, "std::string UiCatalogLanguage() {"),
               {"g_dgettext(GETTEXT_PACKAGE, \"\")", "release_notes::CatalogLanguage("});
  const std::string ctor = FunctionBody(sheet, "ReleaseNotesSheet::ReleaseNotesSheet(");
  NotesInOrder(ctor, {"set_title(T_(\"release_notes\", \"Release notes\"));",
                      "release_notes::PickNote(*notes_, UiCatalogLanguage())",
                      "AppendNote(*column, *note);", "T_(\"close\", \"Close\")"});
  NotesInOrder(FunctionBody(sheet, "void ReleaseNotesSheet::AppendNote("),
               {"note.title",
                "release_notes::MetaLine(UR_APP_VERSION, LocalNoteDate(notes_->date))",
                "note.intro", "kit::MakePaneGroupHeader(section.heading)", "section.items",
                "kit::MarkDecorative(*bullet);"});
  // the note's text is data from a generated file: set, never parsed as markup
  UR_EXPECT_FALSE(Contains(sheet, "set_markup("));
}

}  // namespace
}  // namespace urnw
