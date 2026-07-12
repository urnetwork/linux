// Gettext bindings for the app (GNOME/gtkmm convention: glib's i18n, bound to
// the "urnetwork" domain in main.cpp).
//
// The catalogs in po/ are generated from the canonical localization store
// (localizations/keys/*.yaml) and are never hand-edited. In them:
//
//   * msgid   = the English source text
//   * msgctxt = the store key id
//
// The key id is what disambiguates keys that legitimately share English text
// ("Name" is both name_label and device_name_label), so every lookup here is a
// context (pgettext) lookup, never a bare gettext:
//
//   T_("connect", "Connect")                          -> pgettext
//   TN_("host_count", "{} host", "{} hosts", n)       -> npgettext. The CLDR
//                                                        plural forms come from
//                                                        the catalog; never
//                                                        inflect in C++.
//   Format(T_("of_total", "of {}"), total)            -> placeholder substitution
//
// The English text passed here must match the store's `en` byte for byte: it is
// the msgid the catalog is keyed on, and it is what the user sees when no
// catalog is loaded (untranslated locale, or running uninstalled).
// SPDX-License-Identifier: MPL-2.0
#pragma once

// The gettext domain. meson passes -DGETTEXT_PACKAGE="urnetwork" (see
// meson.build); the fallback keeps the header self-contained.
#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "urnetwork"
#endif

#include <glib.h>
#include <glib/gi18n.h>

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace urnw {

// glib has no context-aware ngettext, so look the plural up under the
// "msgctxt\004msgid" key gettext stores it as (the same thing GNU gettext's
// npgettext_aux does). On a miss gettext hands back the pointer it was given,
// which is why TN_ glues the literals at compile time.
inline const char* NpgettextAux(const char* msgCtxtId, const char* msgid,
                                const char* msgidPlural, unsigned long n) {
  const char* translated = g_dngettext(GETTEXT_PACKAGE, msgCtxtId, msgidPlural, n);
  if (translated == msgCtxtId || translated == msgidPlural) {
    return n == 1 ? msgid : msgidPlural;  // no catalog entry: the English source
  }
  return translated;
}

// ---- placeholder substitution ----------------------------------------------
// The store lowers its named placeholders to the C++ forms `{}` (a single
// argument) and `{0}`, `{1}` (two or more, so a translation may reorder them) —
// see localizations/gen/store.mjs lowerCpp. This is only that substitution, not
// std::format: the app is C++17 and the store never carries format specs.
// Braces that are not a placeholder ({link}, {terms_start}) pass through
// untouched, the way the store's own markers do.
inline std::string FormatArgs(const char* fmt, const std::vector<std::string>& args) {
  std::string out;
  size_t next = 0;  // each `{}` consumes the next argument in order
  for (const char* p = fmt; *p != '\0';) {
    if (*p == '{') {
      const char* close = p + 1;
      while (*close != '\0' && *close != '}') ++close;
      if (*close == '}') {
        const std::string spec(p + 1, static_cast<size_t>(close - p - 1));
        size_t index = next;
        const bool positional = !spec.empty();
        if (positional) {
          index = 0;
          for (const char c : spec) {
            if (c < '0' || '9' < c) {
              index = args.size();  // not a placeholder: copy it through
              break;
            }
            index = index * 10 + static_cast<size_t>(c - '0');
          }
        }
        if (index < args.size()) {
          out += args[index];
          if (!positional) ++next;
          p = close + 1;
          continue;
        }
      }
    }
    out += *p++;
  }
  return out;
}

inline std::string ToText(const std::string& value) { return value; }
inline std::string ToText(const char* value) { return value ? std::string(value) : std::string(); }
template <typename T, typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0>
inline std::string ToText(T value) {
  return std::to_string(value);
}

// Format(T_("of_total", "of {}"), FormatByteCountCompact(total))
template <typename... Args>
inline std::string Format(const char* fmt, Args&&... args) {
  return FormatArgs(fmt, {ToText(std::forward<Args>(args))...});
}

}  // namespace urnw

// pgettext: msgctxt is the store key id, msgid the English source text.
#define T_(key_id, english) g_dpgettext2(GETTEXT_PACKAGE, key_id, english)

// npgettext: same key id, with the plural forms the catalog carries for the
// target language (2 for English, 1 for Japanese, 6 for Arabic, ...).
#define TN_(key_id, english_one, english_other, n) \
  ::urnw::NpgettextAux(key_id "\004" english_one, english_one, english_other, (n))
