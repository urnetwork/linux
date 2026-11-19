// The bundled release note (src/ReleaseNotes.hpp): the file the store
// generates parses, a file that is not a note is refused without throwing, a
// locale's malformed note falls back to en, and the note is chosen for the
// UI's catalog: its locale, then its language, then en, with the po names
// (pt_BR, zh_CN) read as the store's codes (pt-BR, zh).
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ReleaseNotes.hpp"

// meson passes the source-tree assets and src dirs (as for WorldTopologyTest)
#ifndef UR_ASSETS_DIR
#define UR_ASSETS_DIR ""
#endif
#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace urnw {
namespace {

using release_notes::Note;
using release_notes::ReleaseNotes;

std::string ReadNotesFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// A note with every field, in en, de and pt-BR.
constexpr const char* kFullNote = R"json({
  "version": "2026.11",
  "date": "2026-11-19",
  "locales": {
    "en": {
      "title": "What's new",
      "intro": "A faster connect.",
      "sections": [
        { "heading": "New", "items": ["Release notes in Settings", "A second item"] },
        { "items": ["No heading here"] }
      ]
    },
    "de": { "title": "Neuigkeiten", "sections": [] },
    "pt-BR": { "title": "Novidades", "intro": "Mais rápido.", "sections": [] }
  }
})json";

// The version, date and locales around one en note.
std::string WithEn(const std::string& enNote) {
  return R"({"version": "2026.11", "date": "2026-11-19", "locales": {"en": )" + enNote + "}}";
}

ReleaseNotes ParsedFull() { return release_notes::Parse(kFullNote).value_or(ReleaseNotes{}); }

std::string TitleFor(const ReleaseNotes& notes, const std::string& uiLocale) {
  const Note* note = release_notes::PickNote(notes, uiLocale);
  return note ? note->title : std::string("(none)");
}

UR_TEST(ReleaseNotes_TheBundledFileIsANote) {
  const std::vector<std::string> candidates{
      std::string(UR_ASSETS_DIR) + "/release-notes.json",
      "assets/release-notes.json",
      "../assets/release-notes.json",
      "linux/app/assets/release-notes.json",
  };
  std::string text;
  for (const auto& candidate : candidates) {
    text = ReadNotesFile(candidate);
    if (!text.empty()) break;
  }
  UR_EXPECT_TRUE_MSG("assets/release-notes.json not found", !text.empty());
  const auto notes = release_notes::Parse(text);
  UR_EXPECT_TRUE_MSG("assets/release-notes.json is not a valid release note", notes.has_value());
  if (!notes) return;
  UR_EXPECT_FALSE(notes->version.empty());
  UR_EXPECT_TRUE(release_notes::ParseDate(notes->date).has_value());
  const Note* en = release_notes::PickNote(*notes, "en");
  UR_EXPECT_TRUE(en != nullptr && !en->title.empty());
}

UR_TEST(ReleaseNotes_EveryLocaleOfTheBundledNoteHasACatalog) {
  // A note in a locale no catalog's language maps to is never shown: the
  // store's code and the po name must meet through StoreLocale.
  const std::string po = std::string(UR_SRC_DIR) + "/../po/";
  std::set<std::string> reachable;
  std::istringstream linguas(ReadNotesFile(po + "LINGUAS"));
  for (std::string name; std::getline(linguas, name);) {
    if (name.empty()) continue;
    // the header gettext returns at run time is this line of the catalog
    const std::string catalog = ReadNotesFile(po + name + ".po");
    UR_EXPECT_TRUE_MSG(name + ".po does not name its Language",
                       catalog.find("\"Language: " + name + "\\n\"") != std::string::npos);
    reachable.insert(release_notes::StoreLocale(name));
  }
  UR_EXPECT_TRUE_MSG("no catalogs listed in po/LINGUAS", reachable.size() > 1);
  const auto notes =
      release_notes::Parse(ReadNotesFile(std::string(UR_ASSETS_DIR) + "/release-notes.json"));
  if (!notes) return;  // ReleaseNotes_TheBundledFileIsANote says why
  for (const auto& entry : notes->locales) {
    UR_EXPECT_TRUE_MSG("the note's locale " + entry.first + " matches no catalog",
                       reachable.count(entry.first) == 1);
  }
}

UR_TEST(ReleaseNotes_ParseReadsEveryField) {
  const auto notes = release_notes::Parse(kFullNote);
  UR_EXPECT_TRUE(notes.has_value());
  if (!notes) return;
  UR_EXPECT_TRUE(notes->version == "2026.11");
  UR_EXPECT_TRUE(notes->date == "2026-11-19");
  UR_EXPECT_EQ(size_t{3}, notes->locales.size());
  const Note& en = notes->locales.at("en");
  UR_EXPECT_TRUE(en.title == "What's new");
  UR_EXPECT_TRUE(en.intro == "A faster connect.");
  UR_EXPECT_EQ(size_t{2}, en.sections.size());
  if (en.sections.size() != 2) return;
  UR_EXPECT_TRUE(en.sections[0].heading == "New");
  UR_EXPECT_EQ(size_t{2}, en.sections[0].items.size());
  UR_EXPECT_TRUE(en.sections[0].items[0] == "Release notes in Settings");
  UR_EXPECT_TRUE(en.sections[1].heading.empty());
  UR_EXPECT_EQ(size_t{1}, en.sections[1].items.size());
  // the optional fields of the other locales
  UR_EXPECT_TRUE(notes->locales.at("de").intro.empty());
  UR_EXPECT_TRUE(notes->locales.at("de").sections.empty());
}

UR_TEST(ReleaseNotes_ANoteNeedsOnlyItsTitle) {
  // the store's placeholder shape: an intro, no sections
  auto notes = release_notes::Parse(
      WithEn(R"({"title": "What's new", "intro": "On the way.", "sections": []})"));
  UR_EXPECT_TRUE(notes.has_value() && notes->locales.at("en").sections.empty());
  // no intro and no sections key at all, or nulls for them
  notes = release_notes::Parse(WithEn(R"({"title": "What's new"})"));
  UR_EXPECT_TRUE(notes.has_value() && notes->locales.at("en").intro.empty());
  notes = release_notes::Parse(
      WithEn(R"({"title": "What's new", "intro": null, "sections": null})"));
  UR_EXPECT_TRUE(notes.has_value());
}

UR_TEST(ReleaseNotes_WhatIsNotANoteIsRefused) {
  const std::vector<std::string> refused{
      "",
      "not json",
      "{\"version\": \"2026.11\"",  // truncated
      "[]",
      "\"text\"",
      // the version and the date
      R"({"date": "2026-11-19", "locales": {"en": {"title": "t"}}})",
      R"({"version": " ", "date": "2026-11-19", "locales": {"en": {"title": "t"}}})",
      R"({"version": 2026, "date": "2026-11-19", "locales": {"en": {"title": "t"}}})",
      R"({"version": "2026.11", "locales": {"en": {"title": "t"}}})",
      R"({"version": "2026.11", "date": "19-11-2026", "locales": {"en": {"title": "t"}}})",
      R"({"version": "2026.11", "date": 20261119, "locales": {"en": {"title": "t"}}})",
      R"({"version": "2026.11", "date": "2026-13-01", "locales": {"en": {"title": "t"}}})",
      // the locales and en
      R"({"version": "2026.11", "date": "2026-11-19"})",
      R"({"version": "2026.11", "date": "2026-11-19", "locales": []})",
      R"({"version": "2026.11", "date": "2026-11-19", "locales": {}})",
      R"({"version": "2026.11", "date": "2026-11-19", "locales": {"de": {"title": "t"}}})",
      WithEn("\"What's new\""),
      WithEn("{}"),
      WithEn(R"({"title": ""})"),
      WithEn(R"({"title": "  "})"),
      WithEn(R"({"title": 7})"),
      WithEn(R"({"title": "t", "intro": 7})"),
      WithEn(R"({"title": "t", "sections": {}})"),
      WithEn(R"({"title": "t", "sections": ["item"]})"),
      WithEn(R"({"title": "t", "sections": [{"heading": 7, "items": ["a"]}]})"),
      WithEn(R"({"title": "t", "sections": [{"items": "a"}]})"),
      WithEn(R"({"title": "t", "sections": [{"items": ["a", 7]}]})"),
  };
  for (const std::string& text : refused) {
    UR_EXPECT_TRUE_MSG("accepted: " + text, !release_notes::Parse(text).has_value());
  }
}

UR_TEST(ReleaseNotes_AMalformedLocaleReadsEn) {
  const auto notes = release_notes::Parse(R"({
    "version": "2026.11", "date": "2026-11-19",
    "locales": {
      "en": {"title": "What's new"},
      "de": {"title": 7},
      "fr": {"title": "Nouveautés", "sections": [{"items": [null]}]}
    }
  })");
  UR_EXPECT_TRUE(notes.has_value());
  if (!notes) return;
  UR_EXPECT_EQ(size_t{1}, notes->locales.size());
  UR_EXPECT_TRUE(TitleFor(*notes, "de") == "What's new");
  UR_EXPECT_TRUE(TitleFor(*notes, "fr") == "What's new");
}

UR_TEST(ReleaseNotes_EmptySectionsAndBlankItemsAreDropped) {
  const auto notes = release_notes::Parse(WithEn(R"({"title": "t", "sections": [
      {"heading": "Nothing under it", "items": []},
      {"heading": "Only blanks", "items": ["", "  "]},
      {"heading": "No items key"},
      {"heading": "Kept", "items": [" padded ", ""]}
  ]})"));
  UR_EXPECT_TRUE(notes.has_value());
  if (!notes) return;
  const Note& en = notes->locales.at("en");
  UR_EXPECT_EQ(size_t{1}, en.sections.size());
  if (en.sections.size() != 1) return;
  UR_EXPECT_TRUE(en.sections[0].heading == "Kept");
  UR_EXPECT_EQ(size_t{1}, en.sections[0].items.size());
  UR_EXPECT_TRUE(en.sections[0].items[0] == "padded");
}

UR_TEST(ReleaseNotes_TheDateIsACalendarDate) {
  const auto date = release_notes::ParseDate("2026-11-19");
  UR_EXPECT_TRUE(date.has_value());
  if (date) {
    UR_EXPECT_EQ(2026, date->year);
    UR_EXPECT_EQ(11, date->month);
    UR_EXPECT_EQ(19, date->day);
  }
  for (const char* good : {"2024-02-29", "2000-02-29", "2026-12-31", "2026-01-01"}) {
    UR_EXPECT_TRUE_MSG(good, release_notes::ParseDate(good).has_value());
  }
  for (const char* bad : {"2026-02-29", "1900-02-29", "2026-04-31", "2026-00-10", "2026-11-00",
                          "0000-01-01", "2026-1-19", "2026/11/19", "2026-11-19T00:00", "",
                          "20261119xx", "2026-1a-19"}) {
    UR_EXPECT_TRUE_MSG(bad, !release_notes::ParseDate(bad).has_value());
  }
}

UR_TEST(ReleaseNotes_GettextNamesReadAsStoreCodes) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"pt_BR", "pt-BR"},      {"pt_BR.UTF-8", "pt-BR"}, {"pt_PT", "pt-PT"},
      {"zh_CN", "zh"},         {"zh_CN.UTF-8", "zh"},    {"zh_HK", "zh-HK"},
      {"es_419", "es-419"},    {"es_MX", "es-MX"},       {"de_DE.UTF-8@euro", "de-DE"},
      {"sr@latin", "sr"},      {"en", "en"},             {"pt-BR", "pt-BR"},
      {"zh", "zh"},            {" ja ", "ja"},           {"C", ""},
      {"C.UTF-8", ""},         {"POSIX", ""},            {"", ""},
  };
  for (const auto& [name, code] : cases) {
    const std::string got = release_notes::StoreLocale(name);
    UR_EXPECT_TRUE_MSG("StoreLocale(\"" + name + "\") is \"" + got + "\", not \"" + code + "\"",
                       got == code);
  }
}

UR_TEST(ReleaseNotes_TheCatalogHeaderNamesTheLanguage) {
  // what gettext answers msgid "" with, for the generated pt_BR catalog
  const std::string header =
      "Project-Id-Version: urnetwork\n"
      "MIME-Version: 1.0\n"
      "Content-Type: text/plain; charset=UTF-8\n"
      "Content-Transfer-Encoding: 8bit\n"
      "Last-Translator: URnetwork <support@ur.io>\n"
      "Language-Team: URnetwork\n"
      "Language: pt_BR\n"
      "Plural-Forms: nplurals=2; plural=(n > 1);\n";
  UR_EXPECT_TRUE(release_notes::CatalogLanguage(header) == "pt_BR");
  UR_EXPECT_TRUE(release_notes::CatalogLanguage("Language: zh_CN") == "zh_CN");  // no newline
  UR_EXPECT_TRUE(release_notes::CatalogLanguage("Language:  ja \n") == "ja");
  // no catalog (gettext hands "" back), or a header without the field
  UR_EXPECT_TRUE(release_notes::CatalogLanguage("").empty());
  UR_EXPECT_TRUE(release_notes::CatalogLanguage("Language-Team: URnetwork\n").empty());
  UR_EXPECT_TRUE(release_notes::CatalogLanguage("X-Language: de\n").empty());
  UR_EXPECT_TRUE(release_notes::CatalogLanguage("Language:\n").empty());
}

UR_TEST(ReleaseNotes_TheNoteIsTheLocalesThenTheLanguagesThenEn) {
  const ReleaseNotes notes = ParsedFull();
  // exact, from the catalog's po name or a store code, in any case
  UR_EXPECT_TRUE(TitleFor(notes, "pt_BR") == "Novidades");
  UR_EXPECT_TRUE(TitleFor(notes, "pt-BR") == "Novidades");
  UR_EXPECT_TRUE(TitleFor(notes, "pt-br") == "Novidades");
  UR_EXPECT_TRUE(TitleFor(notes, "de") == "Neuigkeiten");
  // the language's note: de_AT is de; pt_PT is not pt-BR, and there is no pt
  UR_EXPECT_TRUE(TitleFor(notes, "de_AT") == "Neuigkeiten");
  UR_EXPECT_TRUE(TitleFor(notes, "pt_PT") == "What's new");
  UR_EXPECT_TRUE(TitleFor(notes, "pt") == "What's new");
  // en, for every other language and for none
  UR_EXPECT_TRUE(TitleFor(notes, "fr") == "What's new");
  UR_EXPECT_TRUE(TitleFor(notes, "zh_CN") == "What's new");
  UR_EXPECT_TRUE(TitleFor(notes, "") == "What's new");
  UR_EXPECT_TRUE(TitleFor(notes, "C") == "What's new");
  // Simplified Chinese is the store's zh, which zh_CN's catalog reads; zh_HK
  // has its own note, or zh's
  const auto chinese = release_notes::Parse(R"json({
    "version": "2026.11", "date": "2026-11-19",
    "locales": {"en": {"title": "What's new"}, "zh": {"title": "新功能"},
                "zh-HK": {"title": "新功能 (HK)"}}
  })json");
  UR_EXPECT_TRUE(chinese.has_value());
  if (!chinese) return;
  UR_EXPECT_TRUE(TitleFor(*chinese, "zh_CN") == "新功能");
  UR_EXPECT_TRUE(TitleFor(*chinese, "zh_HK") == "新功能 (HK)");
  UR_EXPECT_TRUE(TitleFor(*chinese, "zh_TW") == "新功能");
  // no en: nothing to fall back to (Parse never returns such notes)
  UR_EXPECT_TRUE(release_notes::PickNote(ReleaseNotes{}, "en") == nullptr);
}

UR_TEST(ReleaseNotes_TheMetaLineJoinsVersionAndDate) {
  UR_EXPECT_TRUE(release_notes::MetaLine("2026.11.18-1101907820", "11/19/2026") ==
                 "2026.11.18-1101907820 · 11/19/2026");
  UR_EXPECT_TRUE(release_notes::MetaLine("0.0.0", "") == "0.0.0");
  UR_EXPECT_TRUE(release_notes::MetaLine("", "11/19/2026") == "11/19/2026");
}

}  // namespace
}  // namespace urnw
