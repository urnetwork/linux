// The bundled release note, read and chosen without GTK so the rules are
// unit-tested (tests/ReleaseNotesTest.cpp). The App version row in Settings >
// About opens it (ReleaseNotesSheet.hpp).
//
// The file is assets/release-notes.json, installed as
// <pkgdatadir>/release-notes.json. The localization store generates it from its
// release-notes/latest.yaml (`npm run gen`, gen/release-notes.mjs) and it is
// never hand-edited. One file, the latest release only:
//
//   { "version": "2026.11", "date": "2026-11-19",
//     "locales": { "en": { "title": "What's new", "intro": "...",
//                          "sections": [{ "heading": "...", "items": ["..."] }] },
//                  "de": { ... }, ... } }
//
// `en` is the one locale the store requires. The intro and a section's heading
// are optional, and the sections may be empty.
//
// Every app shows it by one rule (the localization store's README, "Release
// notes"). THE NOTE IS IN THE LANGUAGE OF THE UI AROUND IT. The catalog gettext
// loaded for the app's domain names its language in its header ("Language:
// pt_BR", po/*.po), and that is the po name of a store locale: pt_BR is pt-BR,
// zh_CN is zh (gen/store.mjs LINUX_LOCALE maps them the other way). The note is
// that exact locale's; else the same language's in another form, the bare
// language first (pt-BR reads pt) and then another region of it (pt-PT reads
// pt-BR), never across Chinese scripts (the store's zh is Simplified, zh-HK
// Traditional); else en's. With no catalog loaded the UI is English, and so is
// the note. The note is laid out in its own language's direction
// (IsRightToLeft), so the English fallback reads left to right in an Arabic or
// Hebrew UI.
//
// A file that does not parse, or that lacks the version, the date or a usable
// en note, is no release note at all: Parse refuses it and the version row
// stays a plain value. Another locale's malformed note is dropped, and that
// locale reads en. Every read is a find plus a type check, never a throwing
// get<>() on a value of the wrong type.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cctype>
#include <cstddef>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace urnw::release_notes {

struct Section {
  std::string heading;             // "" = a section with no heading
  std::vector<std::string> items;  // never empty
};

struct Note {
  std::string title;
  std::string intro;              // "" = no intro
  std::vector<Section> sections;  // may be empty
};

struct ReleaseNotes {
  std::string version;  // the release as people see it ("2026.11")
  std::string date;     // YYYY-MM-DD
  // store locale code -> its note; always carries kFallbackLocale
  std::map<std::string, Note> locales;
};

// The locale every note falls back to, and the one the store requires.
inline constexpr const char* kFallbackLocale = "en";

struct Date {
  int year = 0;
  int month = 0;  // 1..12
  int day = 0;    // 1..the month's length
};

namespace detail {

inline std::string Trim(const std::string& text) {
  static const char* kSpace = " \t\r\n";
  const size_t begin = text.find_first_not_of(kSpace);
  if (begin == std::string::npos) return std::string();
  const size_t end = text.find_last_not_of(kSpace);
  return text.substr(begin, end - begin + 1);
}

inline std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// The language subtag of a locale code, lowercased: "pt" of "pt-BR".
inline std::string LanguageOf(const std::string& code) {
  return Lower(code.substr(0, code.find('-')));
}

// What a locale falls back within: its language, except that Chinese splits by
// script, which a note never crosses. The store's zh is Simplified and zh-HK
// Traditional, so zh-Hant and the Traditional regions (HK, TW, MO) are
// "zh-hant" and every other zh is "zh-hans"; an explicit script subtag wins
// over the region (zh-Hans-HK is Simplified).
inline std::string FamilyOf(const std::string& code) {
  const std::string lower = Lower(code);
  const std::string language = LanguageOf(lower);
  if (language != "zh") return language;
  std::string traditionalRegion;
  size_t start = lower.find('-');
  while (start != std::string::npos) {
    const size_t end = lower.find('-', start + 1);
    const std::string subtag =
        lower.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
    if (subtag == "hans") return "zh-hans";
    if (subtag == "hant") return "zh-hant";
    if (subtag == "hk" || subtag == "tw" || subtag == "mo") traditionalRegion = subtag;
    start = end;
  }
  return traditionalRegion.empty() ? "zh-hans" : "zh-hant";
}

// An optional string field: absent or null is "", a string is its trimmed
// value, and any other type is a malformed field (nullopt).
inline std::optional<std::string> OptionalString(const nlohmann::json& object, const char* key) {
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) return std::string();
  if (!it->is_string()) return std::nullopt;
  return Trim(it->get<std::string>());
}

// One locale's note, or nullopt when it is malformed. A section left with no
// items is dropped: the store refuses one, and a heading over nothing reads as
// a failed load.
inline std::optional<Note> ParseNote(const nlohmann::json& value) {
  if (!value.is_object()) return std::nullopt;
  const auto title = OptionalString(value, "title");
  const auto intro = OptionalString(value, "intro");
  if (!title || title->empty() || !intro) return std::nullopt;
  Note note;
  note.title = *title;
  note.intro = *intro;
  const auto sections = value.find("sections");
  if (sections == value.end() || sections->is_null()) return note;
  if (!sections->is_array()) return std::nullopt;
  for (const auto& entry : *sections) {
    if (!entry.is_object()) return std::nullopt;
    const auto heading = OptionalString(entry, "heading");
    if (!heading) return std::nullopt;
    Section section;
    section.heading = *heading;
    const auto items = entry.find("items");
    if (items != entry.end() && !items->is_null()) {
      if (!items->is_array()) return std::nullopt;
      for (const auto& item : *items) {
        if (!item.is_string()) return std::nullopt;
        std::string text = Trim(item.get<std::string>());
        if (!text.empty()) section.items.push_back(std::move(text));
      }
    }
    if (!section.items.empty()) note.sections.push_back(std::move(section));
  }
  return note;
}

}  // namespace detail

// "YYYY-MM-DD" as a calendar date (the store's own check is the same shape),
// or nullopt when it is not one: February 30 is refused here rather than left
// for the formatter to stumble on.
inline std::optional<Date> ParseDate(const std::string& text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') return std::nullopt;
  const auto number = [&text](size_t from, size_t length, int& out) {
    out = 0;
    for (size_t i = from; i < from + length; ++i) {
      if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
      out = out * 10 + (text[i] - '0');
    }
    return true;
  };
  Date date;
  if (!number(0, 4, date.year) || !number(5, 2, date.month) || !number(8, 2, date.day)) {
    return std::nullopt;
  }
  if (date.year < 1 || date.month < 1 || date.month > 12 || date.day < 1) return std::nullopt;
  static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (date.year % 4 == 0 && date.year % 100 != 0) || date.year % 400 == 0;
  const int length = date.month == 2 && leap ? 29 : kDays[date.month - 1];
  if (date.day > length) return std::nullopt;
  return date;
}

// The bundled file's text as a release note, or nullopt when it is none (see
// the top of this file).
inline std::optional<ReleaseNotes> Parse(const std::string& json) {
  try {
    const nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return std::nullopt;
    const auto version = detail::OptionalString(root, "version");
    const auto date = detail::OptionalString(root, "date");
    if (!version || version->empty() || !date || !ParseDate(*date)) return std::nullopt;
    const auto locales = root.find("locales");
    if (locales == root.end() || !locales->is_object()) return std::nullopt;
    ReleaseNotes notes;
    notes.version = *version;
    notes.date = *date;
    for (auto it = locales->begin(); it != locales->end(); ++it) {
      auto note = detail::ParseNote(it.value());
      if (note) notes.locales.emplace(it.key(), std::move(*note));
    }
    if (notes.locales.find(kFallbackLocale) == notes.locales.end()) return std::nullopt;
    return notes;
  } catch (const std::exception&) {
    return std::nullopt;  // nothing in a bundled file may take the window down
  }
}

// A gettext locale name ("pt_BR.UTF-8", "sr@latin", a catalog header's
// "zh_CN") as the store's locale code: the codeset and modifier dropped, '_'
// as '-', and zh_CN as zh, the store's code for Simplified Chinese. "C" and
// "POSIX" name no language: "". A store code passes through unchanged.
inline std::string StoreLocale(const std::string& name) {
  std::string code = detail::Trim(name.substr(0, name.find_first_of(".@")));
  for (char& c : code) {
    if (c == '_') c = '-';
  }
  if (code == "C" || code == "POSIX") return std::string();
  if (detail::Lower(code) == "zh-cn") return "zh";
  return code;
}

// The Language field of a catalog header (what gettext gives for msgid "": the
// "Language: pt_BR" line po/*.po carry), or "" when the header has none. Only a
// line that starts with the field counts, so Language-Team is never read.
inline std::string CatalogLanguage(const std::string& header) {
  static const std::string kField = "Language:";
  size_t start = 0;
  while (start < header.size()) {
    size_t end = header.find('\n', start);
    if (end == std::string::npos) end = header.size();
    if (header.compare(start, kField.size(), kField) == 0) {
      return detail::Trim(header.substr(start + kField.size(), end - start - kField.size()));
    }
    start = end + 1;
  }
  return std::string();
}

// The store locale whose note to show for the UI's locale (a gettext name or a
// store code), as the file spells it; codes match without regard to case. That
// exact locale; else the same language in another form, the bare language
// first (pt-BR reads pt) and then another region of it, the first by code
// (pt-PT reads pt-BR, es reads es-419), never across Chinese scripts (zh_CN
// reads only zh, zh_HK and zh_TW only zh-HK); else en. "" only for notes that
// carry no en, which Parse never returns.
inline std::string PickLocale(const ReleaseNotes& notes, const std::string& uiLocale) {
  const auto find = [&notes](const std::string& lowerCode) -> std::string {
    for (const auto& entry : notes.locales) {
      if (detail::Lower(entry.first) == lowerCode) return entry.first;
    }
    return std::string();
  };
  const std::string want = detail::Lower(StoreLocale(uiLocale));
  if (!want.empty()) {
    if (std::string exact = find(want); !exact.empty()) return exact;
    const std::string family = detail::FamilyOf(want);
    // the bare language, unless it is the other script (zh for zh-TW)
    const std::string bare = detail::LanguageOf(want);
    if (detail::FamilyOf(bare) == family) {
      if (std::string language = find(bare); !language.empty()) return language;
    }
    for (const auto& entry : notes.locales) {  // a std::map: by code
      if (detail::FamilyOf(entry.first) == family) return entry.first;
    }
  }
  return find(kFallbackLocale);
}

// The note PickLocale chooses; nullptr only for notes that carry no en.
inline const Note* PickNote(const ReleaseNotes& notes, const std::string& uiLocale) {
  const auto it = notes.locales.find(PickLocale(notes, uiLocale));
  return it == notes.locales.end() ? nullptr : &it->second;
}

// Whether a note in this locale is laid out right to left: the store's
// right-to-left languages, Arabic and Hebrew. A note keeps its own language's
// direction whatever the UI's, so the English fallback reads left to right
// (its alignment and its punctuation) in an Arabic or Hebrew UI.
inline bool IsRightToLeft(const std::string& locale) {
  const std::string language = detail::LanguageOf(StoreLocale(locale));
  return language == "ar" || language == "he";
}

// The line under the note's title: the note's own version and its date,
// "2026.11 · 19.11.2026", either half omitted when empty (the Licenses sheet's
// "version · spdx" form). Never the running build's version: a later build
// carries the same note, and that version beside this date would read wrong.
inline std::string MetaLine(const std::string& version, const std::string& date) {
  if (version.empty()) return date;
  if (date.empty()) return version;
  return version + " · " + date;
}

}  // namespace urnw::release_notes
