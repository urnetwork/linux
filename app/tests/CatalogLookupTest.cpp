// Every string the GUI looks up must be an entry of the catalog it reads.
//
// gettext finds an entry by its context (the store's key id) and its msgid (the
// English), so a key the localization store lacks, or English that differs from
// the store's by a byte, reads in English in every locale and nothing else
// notices: the lookup falls back to the English it was given. po/urnetwork.pot
// is the store's template, which carries every live key, so this reads the
// sources and checks each lookup against it:
//   - every T_/TN_ call whose arguments are string literals, and
//   - every key/English pair a table hands to a T_ call at run time
//     ({"key_id", "English"} rows, returns and arguments), which xgettext
//     cannot see, including DeveloperPage.cpp's <key>_detail explanations,
//     rows that name their key through a constant ({kErrorLinkInvalid,
//     "This is not a valid VLESS link."}), and keys assigned apart from their
//     English (row.textKey = "id"; row.textEnglish = "English";), as
//     Health.hpp's status and ExtenderProvidePresentation.hpp's row carry them.
// The daemon's sources are left out: urnetworkd links no gettext and its
// ("code", "message") pairs are error records, not lookups.
//
// Every key looked up must also be tagged linux in the store. The catalog
// carries every live key, so an untagged lookup reads translated today, and
// reads English the day the platforms the key is tagged for retire it: the
// key goes dead and leaves the catalog (net_points_earned, enter_wallet_address
// and network_earnings left the Earnings page that way). po/linux-keys.txt,
// generated next to the catalogs, lists the keys tagged linux. The other way
// round, every key on that list must be looked up: a tag the app no longer
// needs keeps the key alive in every desktop catalog and in front of the
// translators for nothing.
//
// SPDX-License-Identifier: MPL-2.0
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

// The whole file, or "" when it cannot be read.
std::string ReadCatalogLookupText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// A character of an identifier or a number.
bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// A prefix a string or character literal may carry (u8"", L'x').
bool IsLiteralPrefix(const std::string& word) {
  return word == "u8" || word == "u" || word == "U" || word == "L";
}

// Appends a code point as UTF-8, the encoding the compiler gives a \u escape.
void AppendUtf8(std::string& out, unsigned long code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code >> 18));
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

// A word (identifier or number), a string literal (its value, escapes
// decoded) or one punctuation character, with the line it starts on.
struct Token {
  enum class Kind { Word, String, Punct };
  Kind kind;
  std::string text;
  int line;
};

// The source's code as tokens. Comments, character literals and preprocessor
// lines are dropped, so the documentation in I18n.hpp and its #define of T_ are
// no call sites. Knows raw strings (Ui.cpp's stylesheet) and digit separators
// (21'600LL).
std::vector<Token> Tokenize(const std::string& text) {
  std::vector<Token> tokens;
  const size_t n = text.size();
  size_t i = 0;
  int line = 1;
  bool lineStart = true;
  auto at = [&](size_t index) { return index < n ? text[index] : '\0'; };
  // a prefix word just lexed (u8, L) belongs to the literal that follows it
  auto dropLiteralPrefix = [&] {
    if (!tokens.empty() && tokens.back().kind == Token::Kind::Word &&
        IsLiteralPrefix(tokens.back().text) && i > 0 && IsWordChar(text[i - 1])) {
      tokens.pop_back();
    }
  };
  while (i < n) {
    const char c = text[i];
    if (c == '\n') {
      ++line;
      lineStart = true;
      ++i;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (lineStart && c == '#') {
      // a preprocessor line, with its backslash continuations
      while (i < n && text[i] != '\n') {
        if (text[i] == '\\' && at(i + 1) == '\n') {
          ++line;
          ++i;
        }
        ++i;
      }
      continue;
    }
    lineStart = false;
    if (c == '/' && at(i + 1) == '/') {
      while (i < n && text[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && at(i + 1) == '*') {
      const size_t end = text.find("*/", i + 2);
      const size_t stop = end == std::string::npos ? n : end + 2;
      for (size_t k = i; k < stop; ++k) {
        if (text[k] == '\n') ++line;
      }
      i = stop;
      continue;
    }
    if (c == '"' || c == '\'') {
      if (c == '"') dropLiteralPrefix();
      std::string value;
      const int startLine = line;
      size_t k = i + 1;
      while (k < n && text[k] != c && text[k] != '\n') {
        if (text[k] != '\\') {
          value += text[k++];
          continue;
        }
        const char e = at(k + 1);
        k += 2;
        switch (e) {
          case 'n': value += '\n'; break;
          case 't': value += '\t'; break;
          case 'r': value += '\r'; break;
          case '0': value += '\0'; break;
          case 'x': {
            const size_t start = k;
            while (std::isxdigit(static_cast<unsigned char>(at(k)))) ++k;
            value += static_cast<char>(std::stoul(text.substr(start, k - start), nullptr, 16));
            break;
          }
          case 'u':
          case 'U': {
            const size_t digits = e == 'u' ? 4 : 8;
            AppendUtf8(value, std::stoul(text.substr(k, digits), nullptr, 16));
            k += digits;
            break;
          }
          default: value += e; break;  // \" \' \\ \?
        }
      }
      i = k + 1;
      // character literals are dropped
      if (c == '"') tokens.push_back(Token{Token::Kind::String, value, startLine});
      continue;
    }
    if (c == 'R' && at(i + 1) == '"') {
      // R"delim( ... )delim"
      dropLiteralPrefix();
      const size_t open = text.find('(', i + 2);
      if (open == std::string::npos) break;
      const std::string close = ")" + text.substr(i + 2, open - (i + 2)) + "\"";
      const size_t end = text.find(close, open + 1);
      const size_t stop = end == std::string::npos ? n : end;
      tokens.push_back(Token{Token::Kind::String, text.substr(open + 1, stop - open - 1), line});
      for (size_t k = i; k < stop; ++k) {
        if (text[k] == '\n') ++line;
      }
      i = end == std::string::npos ? n : end + close.size();
      continue;
    }
    if (IsWordChar(c)) {
      size_t k = i;
      // a number keeps its digit separators
      const bool number = std::isdigit(static_cast<unsigned char>(c)) != 0;
      while (k < n && (IsWordChar(text[k]) || (number && text[k] == '\'' && IsWordChar(at(k + 1))))) {
        ++k;
      }
      tokens.push_back(Token{Token::Kind::Word, text.substr(i, k - i), line});
      i = k;
      continue;
    }
    tokens.push_back(Token{Token::Kind::Punct, std::string(1, c), line});
    ++i;
  }
  return tokens;
}

// Whether the token at `index` is the punctuation character `c`.
bool IsPunct(const std::vector<Token>& tokens, size_t index, char c) {
  return index < tokens.size() && tokens[index].kind == Token::Kind::Punct &&
         tokens[index].text[0] == c;
}

// The value of the adjacent string literals starting at `index` ("a" "b" is
// "ab"), with `index` moved past them; nullopt when no literal starts there.
std::optional<std::string> LiteralRun(const std::vector<Token>& tokens, size_t& index) {
  if (index >= tokens.size() || tokens[index].kind != Token::Kind::String) return std::nullopt;
  std::string value;
  while (index < tokens.size() && tokens[index].kind == Token::Kind::String) {
    value += tokens[index++].text;
  }
  return value;
}

// One lookup a source makes: a T_ call, a TN_ call (with its plural English)
// or a key/English pair a table hands to T_.
struct Lookup {
  std::string file;
  int line;
  std::string key;
  std::string english;
  std::optional<std::string> plural;
};

// The lookups of one or more sources. Literal calls and table pairs are kept
// apart; a call with a non-literal argument is only counted, since its pair
// comes from a table the pair scan reads.
struct Scan {
  std::vector<Lookup> calls;
  std::vector<Lookup> pairs;
  int dynamicCalls = 0;
};

// Spelled like a store key id: lowercase letters, digits and underscores,
// starting with a letter.
bool IsKeyShaped(const std::string& value) {
  if (value.empty() || !std::islower(static_cast<unsigned char>(value[0]))) return false;
  for (const char c : value) {
    if (!(std::islower(static_cast<unsigned char>(c)) ||
          std::isdigit(static_cast<unsigned char>(c)) || c == '_')) {
      return false;
    }
  }
  return true;
}

// UI text rather than a code or an identifier: a space, a capital, a digit or
// a non-ASCII character.
bool ReadsAsText(const std::string& value) {
  for (const char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == ' ' || std::isupper(u) || std::isdigit(u) || u >= 0x80) return true;
  }
  return false;
}

// The lookups in one source. A pair is a key id with an underscore followed by
// a whole UI-text literal; a one-word key ({"none", "None"}) counts only when
// `catalogKeys` has it and its English is capitalized. The key may also be a
// constant this source defines as a key id (kName = "key_id";), or be assigned
// apart from its English: a <prefix>English = "..."; assignment pairs with the
// last <prefix>Key = "..."; before it. With `derivesDetailKeys`, a {key,
// label, detail} row is also looked up as <key>_detail (DeveloperPage.cpp's
// DetailKey).
Scan ScanSource(const std::string& file, const std::string& text,
                const std::set<std::string>& catalogKeys, bool derivesDetailKeys) {
  Scan scan;
  const std::vector<Token> tokens = Tokenize(text);
  // keeps a pair that reads as a lookup, and says whether it did
  const auto addPair = [&](const std::string& key, const std::string& english, int line) {
    const bool oneWord = key.find('_') == std::string::npos;
    if (!ReadsAsText(english) ||
        (oneWord && (catalogKeys.count(key) == 0 ||
                     !std::isupper(static_cast<unsigned char>(english[0]))))) {
      return false;
    }
    scan.pairs.push_back(Lookup{file, line, key, english, std::nullopt});
    return true;
  };
  std::vector<bool> insideCall(tokens.size(), false);
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.kind != Token::Kind::Word || (token.text != "T_" && token.text != "TN_") ||
        !IsPunct(tokens, i + 1, '(')) {
      continue;
    }
    // the argument token ranges, split at the call's own commas
    std::vector<std::vector<size_t>> args(1);
    int depth = 0;
    size_t k = i + 1;
    for (; k < tokens.size(); ++k) {
      const bool opens = IsPunct(tokens, k, '(') || IsPunct(tokens, k, '[') || IsPunct(tokens, k, '{');
      const bool closes = IsPunct(tokens, k, ')') || IsPunct(tokens, k, ']') || IsPunct(tokens, k, '}');
      if (opens && depth++ == 0) continue;
      if (closes && --depth == 0) break;
      if (depth == 1 && IsPunct(tokens, k, ',')) {
        args.emplace_back();
        continue;
      }
      args.back().push_back(k);
    }
    for (size_t m = i; m <= k && m < tokens.size(); ++m) insideCall[m] = true;
    // an argument is a literal when it is string literals and nothing else
    std::vector<std::optional<std::string>> values;
    for (const auto& arg : args) {
      size_t next = arg.empty() ? tokens.size() : arg.front();
      const std::optional<std::string> value = LiteralRun(tokens, next);
      values.push_back(value && next == arg.back() + 1 ? value : std::nullopt);
    }
    const bool plural = token.text == "TN_";
    const size_t literalCount = plural ? 3 : 2;
    bool literal = values.size() >= literalCount;
    for (size_t a = 0; literal && a < literalCount; ++a) literal = values[a].has_value();
    if (!literal) {
      ++scan.dynamicCalls;
      continue;
    }
    scan.calls.push_back(Lookup{file, token.line, *values[0], *values[1],
                                plural ? values[2] : std::nullopt});
  }

  for (size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].kind != Token::Kind::String || insideCall[i]) continue;
    size_t k = i;
    const std::string key = *LiteralRun(tokens, k);
    const int line = tokens[i].line;
    i = k - 1;  // past the run, whatever follows
    if (!IsKeyShaped(key) || !IsPunct(tokens, k, ',')) continue;
    ++k;
    const std::optional<std::string> english = LiteralRun(tokens, k);
    // a whole literal, not the head of a message built with +
    if (!english || !(IsPunct(tokens, k, ',') || IsPunct(tokens, k, ')') || IsPunct(tokens, k, '}'))) {
      continue;
    }
    if (!addPair(key, *english, line)) continue;
    if (derivesDetailKeys && IsPunct(tokens, k, ',')) {
      ++k;
      const std::optional<std::string> detail = LiteralRun(tokens, k);
      if (detail && (IsPunct(tokens, k, ',') || IsPunct(tokens, k, '}'))) {
        scan.pairs.push_back(Lookup{file, line, key + "_detail", *detail, std::nullopt});
      }
    }
  }

  // name = "literal"; assignments: the constants that name a key id, and the
  // keys and English assigned apart, paired by their name's prefix
  std::map<std::string, std::string> constantKeys;
  std::map<std::string, std::pair<std::string, int>> assignedKeys;
  const auto prefixOf = [](const std::string& name, const std::string& suffix) {
    const bool ends = name.size() > suffix.size() &&
                      name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    return ends ? std::optional<std::string>(name.substr(0, name.size() - suffix.size()))
                : std::nullopt;
  };
  for (size_t i = 0; i + 1 < tokens.size(); ++i) {
    const Token& name = tokens[i];
    if (name.kind != Token::Kind::Word || !IsPunct(tokens, i + 1, '=')) continue;
    size_t k = i + 2;
    const std::optional<std::string> value = LiteralRun(tokens, k);
    if (!value || !IsPunct(tokens, k, ';')) continue;
    const bool constant = name.text.size() > 1 && name.text[0] == 'k' &&
                          std::isupper(static_cast<unsigned char>(name.text[1])) != 0;
    if (constant && IsKeyShaped(*value)) constantKeys[name.text] = *value;
    const std::optional<std::string> keyPrefix = prefixOf(name.text, "Key");
    const std::optional<std::string> englishPrefix = prefixOf(name.text, "English");
    if (keyPrefix && IsKeyShaped(*value)) {
      assignedKeys[*keyPrefix] = {*value, name.line};
    } else if (keyPrefix) {
      assignedKeys.erase(*keyPrefix);
    } else if (englishPrefix) {
      const auto assigned = assignedKeys.find(*englishPrefix);
      if (assigned == assignedKeys.end()) continue;
      addPair(assigned->second.first, *value, assigned->second.second);
      assignedKeys.erase(assigned);
    }
  }
  // {kName, "English"}: a constant key followed by a whole UI-text literal
  for (size_t i = 0; i + 2 < tokens.size(); ++i) {
    if (tokens[i].kind != Token::Kind::Word || insideCall[i] || !IsPunct(tokens, i + 1, ',')) {
      continue;
    }
    const auto constant = constantKeys.find(tokens[i].text);
    if (constant == constantKeys.end()) continue;
    size_t k = i + 2;
    const std::optional<std::string> english = LiteralRun(tokens, k);
    if (english && (IsPunct(tokens, k, ',') || IsPunct(tokens, k, ')') || IsPunct(tokens, k, '}'))) {
      addPair(constant->second, *english, tokens[i].line);
    }
  }
  return scan;
}

// A catalog entry's English: its msgid, and msgid_plural for a plural key.
struct CatalogEntry {
  std::string msgid;
  std::optional<std::string> msgidPlural;
};

using Catalog = std::map<std::string, CatalogEntry>;

// A po string's value: \\, \", \n and \t decoded.
std::string PoUnescape(const std::string& quoted) {
  std::string out;
  for (size_t i = 0; i < quoted.size(); ++i) {
    if (quoted[i] != '\\' || i + 1 == quoted.size()) {
      out += quoted[i];
      continue;
    }
    const char e = quoted[++i];
    out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
  }
  return out;
}

// A po file's entries by msgctxt (the header and context-less entries are
// skipped).
Catalog ReadCatalog(const std::string& text) {
  Catalog entries;
  std::string ctxt, msgid;
  std::optional<std::string> plural;
  std::string* field = nullptr;
  auto flush = [&] {
    if (!ctxt.empty()) entries[ctxt] = CatalogEntry{msgid, plural};
    ctxt.clear();
    msgid.clear();
    plural.reset();
    field = nullptr;
  };
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (line.empty()) {
      flush();
      continue;
    }
    if (line[0] == '#') continue;
    const size_t open = line.find('"');
    const size_t close = line.rfind('"');
    if (open == std::string::npos || close <= open) continue;
    const std::string value = PoUnescape(line.substr(open + 1, close - open - 1));
    const std::string head = line.substr(0, open);
    if (head.rfind("msgctxt", 0) == 0) {
      field = &ctxt;
    } else if (head.rfind("msgid_plural", 0) == 0) {
      plural = std::string();
      field = &*plural;
    } else if (head.rfind("msgid", 0) == 0) {
      field = &msgid;
    } else if (head.rfind("msgstr", 0) == 0) {
      field = nullptr;
    } else {
      // a continuation line of the field above (msgstr's are not kept)
      if (field != nullptr) *field += value;
      continue;
    }
    if (field != nullptr) *field = value;
  }
  flush();
  return entries;
}

// app/src, as meson passes it.
const std::filesystem::path& CatalogSrcDir() {
  static const std::filesystem::path src(UR_SRC_DIR);
  return src;
}

// po/urnetwork.pot, the template the store generates with every live key.
const Catalog& TemplateCatalog() {
  static const Catalog catalog =
      ReadCatalog(ReadCatalogLookupText(CatalogSrcDir() / ".." / "po" / "urnetwork.pot"));
  return catalog;
}

// The ids of a generated key list: one per line, # comment lines skipped.
std::set<std::string> ReadTaggedKeys(const std::string& text) {
  std::set<std::string> keys;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    keys.insert(line);
  }
  return keys;
}

// po/linux-keys.txt, the keys the store tags linux.
const std::set<std::string>& LinuxTaggedKeys() {
  static const std::set<std::string> keys =
      ReadTaggedKeys(ReadCatalogLookupText(CatalogSrcDir() / ".." / "po" / "linux-keys.txt"));
  return keys;
}

// The crypto payment row's labels (mmm/BTCPAY.md §17.6), which the SDK hands
// out with each tab and coin as a store key with its English
// (urnet::cryptoPaymentTabs' label_key and label, urnet::cryptoAssetLabelKey and
// urnet::cryptoAssetLabel), as the SDK's crypto_pay.go spells them.
// PaymentMethodRow.cpp LabelText looks each up with T_(key, English), so the
// English must be the catalog's msgid byte for byte.
const std::vector<std::pair<std::string, std::string>>& SdkCryptoLabels() {
  static const std::vector<std::pair<std::string, std::string>> labels = {
      {"pay_method_usdc_solana", "USDC on Solana"},
      {"pay_method_zcash", "Zcash"},
      {"pay_method_monero", "Monero"},
      {"pay_method_more_coins", "More coins"},
      {"pay_method_tao", "TAO"},
      {"pay_method_bitcoin", "Bitcoin"},
      {"pay_method_eth", "ETH"},
      {"pay_method_sol", "SOL"},
  };
  return labels;
}

// Keys tagged linux that the app looks up only where the scan cannot see, each
// with where: the crypto payment row's labels (SdkCryptoLabels). Every other
// lookup is a T_/TN_ call or a table pair the scan reads.
const std::map<std::string, std::string>& UnscannedTaggedKeys() {
  static const std::map<std::string, std::string> keys = [] {
    std::map<std::string, std::string> out;
    for (const auto& label : SdkCryptoLabels()) {
      out.emplace(label.first,
                  "a crypto payment label the SDK hands out with its English, looked up by "
                  "PaymentMethodRow.cpp LabelText");
    }
    return out;
  }();
  return keys;
}

// The sources only urnetworkd builds (meson.build), which link no gettext.
bool IsDaemonSource(const std::string& relative) {
  return relative.rfind("daemon/", 0) == 0 || relative == "Tunnel.cpp";
}

// Every GUI source under src/, scanned against the template's keys.
Scan ScanGui() {
  std::set<std::string> catalogKeys;
  for (const auto& entry : TemplateCatalog()) catalogKeys.insert(entry.first);
  Scan all;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(CatalogSrcDir())) {
    if (!entry.is_regular_file()) continue;
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".hpp" && ext != ".h") continue;
    const std::string relative =
        std::filesystem::relative(entry.path(), CatalogSrcDir()).generic_string();
    if (IsDaemonSource(relative)) continue;
    const Scan scan = ScanSource("src/" + relative, ReadCatalogLookupText(entry.path()),
                                 catalogKeys, relative == "DeveloperPage.cpp");
    all.calls.insert(all.calls.end(), scan.calls.begin(), scan.calls.end());
    all.pairs.insert(all.pairs.end(), scan.pairs.begin(), scan.pairs.end());
    all.dynamicCalls += scan.dynamicCalls;
  }
  return all;
}

// Why `catalog` does not answer `lookup`, or "" when it does.
std::string CatalogMiss(const Catalog& catalog, const Lookup& lookup) {
  const std::string where =
      lookup.file + ":" + std::to_string(lookup.line) + ": \"" + lookup.key + "\" ";
  const auto found = catalog.find(lookup.key);
  if (found == catalog.end()) {
    return where + "is not in the catalog: add it to the localization store and regenerate po/";
  }
  const CatalogEntry& entry = found->second;
  if (lookup.plural.has_value() != entry.msgidPlural.has_value()) {
    return where + (lookup.plural ? "is looked up with TN_ but its entry is not a plural"
                                  : "is a plural entry looked up without TN_");
  }
  if (entry.msgid != lookup.english) {
    return where + "reads \"" + lookup.english + "\" but the catalog's English is \"" +
           entry.msgid + "\"";
  }
  if (lookup.plural && *entry.msgidPlural != *lookup.plural) {
    return where + "reads the plural \"" + *lookup.plural + "\" but the catalog's is \"" +
           *entry.msgidPlural + "\"";
  }
  return "";
}

// Why `key`, tagged linux, is a stale tag, or "" when `lookedUp` (the keys the
// scan saw looked up) or `unscanned` has it.
std::string StaleTagMiss(const std::set<std::string>& lookedUp,
                         const std::map<std::string, std::string>& unscanned,
                         const std::string& key) {
  if (lookedUp.count(key) == 1 || unscanned.count(key) == 1) return "";
  return "\"" + key +
         "\" is tagged linux but nothing looks it up: in localizations/keys/" + key +
         ".yaml move linux from platforms to deprecated and regenerate po/, or list it in "
         "UnscannedTaggedKeys with where it is looked up";
}

// Why `tagged` (the keys the store tags linux) does not cover `lookup`, or ""
// when it does.
std::string TagMiss(const std::set<std::string>& tagged, const Lookup& lookup) {
  if (tagged.count(lookup.key) == 1) return "";
  return lookup.file + ":" + std::to_string(lookup.line) + ": \"" + lookup.key +
         "\" is looked up but the store does not tag it linux: add linux to the platforms of "
         "localizations/keys/" +
         lookup.key + ".yaml and regenerate po/";
}

}  // namespace

// The scanner's rules on synthetic sources: what counts as a literal lookup and
// as a table pair, and what does not; and how a miss is reported.
UR_TEST(catalogLookupScannerRules) {
  const Scan calls = ScanSource("calls.cpp", R"src(
label.set_text(T_("sample_key", "One " "two"));
auto s = TN_("sample_count", "{} item", "{} items", n);
note = T_("sample_quote", "Say \"hi\" …");
row = T_(spec.key, spec.label);
// T_("comment_key", "Comment")
/* TN_("block_key", "A", "B", n) */
#define T_(key_id, english) g_dpgettext2(GETTEXT_PACKAGE, key_id, english)
const char* css = R"css(a { T_("raw_key", "Raw") })css";
const char* text = "T_(\"string_key\", \"String\")";
int ms = 21'600; label.set_text(T_("after_separator", "After"));
)src", {}, false);
  UR_EXPECT_TRUE(calls.calls.size() == 4);
  UR_EXPECT_TRUE(calls.dynamicCalls == 1);
  if (calls.calls.size() == 4) {
    UR_EXPECT_TRUE(calls.calls[0].key == "sample_key" && calls.calls[0].english == "One two" &&
                   !calls.calls[0].plural && calls.calls[0].line == 2);
    UR_EXPECT_TRUE(calls.calls[1].key == "sample_count" && calls.calls[1].english == "{} item" &&
                   calls.calls[1].plural && *calls.calls[1].plural == "{} items");
    UR_EXPECT_TRUE(calls.calls[2].english == "Say \"hi\" \xE2\x80\xA6");
    UR_EXPECT_TRUE(calls.calls[3].key == "after_separator" && calls.calls[3].line == 11);
  }
  UR_EXPECT_TRUE(calls.pairs.empty());

  const Scan pairs = ScanSource("pairs.cpp", R"src(
const Copy kCopy{"sample_label", "Sample label"};
return {"sample_error", "Something failed."};
add("sample_button", "Drop", Fault::Drop);
{Section::Detection, 1, "sample_row", "Row label", "Row detail", &Settings::Field},
{"embedded", "never", true};
{"secret_backend", "secret-service"};
return fail("sample_code", "invalid name '" + name + "'");
{"none", "None"};
{"off", "Off"};
)src", {"none"}, true);
  std::string seen;
  for (const Lookup& pair : pairs.pairs) seen += pair.key + "=" + pair.english + "; ";
  UR_EXPECT_TRUE_MSG(seen, seen == "sample_label=Sample label; sample_error=Something failed.; "
                               "sample_button=Drop; sample_row=Row label; "
                               "sample_row_detail=Row detail; none=None; ");
  UR_EXPECT_TRUE(pairs.calls.empty());

  const Scan assigned = ScanSource("assigned.cpp", R"src(
struct Reading {
  const char* textKey = "sample_idle";
  const char* textEnglish = "Idle";
  const char* detailKey = "";
  const char* detailEnglish = "";
};
inline constexpr const char* kSampleCode = "sample_code_invalid";
inline constexpr const char* kSamplePrefKey = "sample_pref";
inline constexpr const char* kSampleTag = "tls";
r.textKey = "sample_state";
r.familiesKey = "off";
r.familiesEnglish = "Off";
r.textEnglish = "State";
if (r.textKey == other.textKey) r.textEnglish = "Not paired";
static const Text kErrors[] = {{kSampleCode, "The code is invalid."}, {kSampleTag, "TLS"}};
Use(kSamplePrefKey, "sample_value");
)src", {"off"}, false);
  seen.clear();
  for (const Lookup& pair : assigned.pairs) {
    seen += pair.key + "=" + pair.english + "@" + std::to_string(pair.line) + "; ";
  }
  UR_EXPECT_TRUE_MSG(seen, seen == "sample_idle=Idle@3; off=Off@12; sample_state=State@11; "
                               "sample_code_invalid=The code is invalid.@16; ");
  UR_EXPECT_TRUE(assigned.calls.empty());

  const Catalog catalog = ReadCatalog(R"po(msgid ""
msgstr ""
"Plural-Forms: nplurals=2; plural=(n != 1);\n"

#. a comment
msgctxt "sample_present"
msgid "Present \"quoted\""
msgstr ""

msgctxt "sample_count"
msgid "{} item"
msgid_plural "{} items"
msgstr[0] ""
msgstr[1] ""
)po");
  UR_EXPECT_TRUE(catalog.size() == 2);
  const auto miss = [&](const std::string& key, const std::string& english,
                        std::optional<std::string> plural) {
    return CatalogMiss(catalog, Lookup{"f.cpp", 7, key, english, plural});
  };
  UR_EXPECT_TRUE(miss("sample_present", "Present \"quoted\"", std::nullopt).empty());
  UR_EXPECT_TRUE(miss("sample_count", "{} item", std::string("{} items")).empty());
  UR_EXPECT_TRUE(miss("sample_present", "Present", std::nullopt)
                     .find("but the catalog's English is \"Present \"quoted\"\"") !=
                 std::string::npos);
  UR_EXPECT_TRUE(miss("sample_absent", "Absent", std::nullopt) ==
                 "f.cpp:7: \"sample_absent\" is not in the catalog: add it to the localization "
                 "store and regenerate po/");
  UR_EXPECT_TRUE(miss("sample_count", "{} item", std::nullopt).find("without TN_") !=
                 std::string::npos);
  UR_EXPECT_TRUE(miss("sample_count", "{} item", std::string("{} things")).find("plural") !=
                 std::string::npos);

  const std::set<std::string> tagged = ReadTaggedKeys(
      "# Generated by @urnetwork/localizations (gen/generate.mjs). DO NOT EDIT.\n"
      "# The store keys tagged linux: exactly the keys the app looks up.\n"
      "sample_count\r\n"
      "sample_present\n");
  UR_EXPECT_TRUE(tagged == std::set<std::string>({"sample_count", "sample_present"}));
  UR_EXPECT_TRUE(TagMiss(tagged, Lookup{"f.cpp", 7, "sample_present", "Present", std::nullopt}).empty());
  UR_EXPECT_TRUE(TagMiss(tagged, Lookup{"f.cpp", 7, "sample_absent", "Absent", std::nullopt}) ==
                 "f.cpp:7: \"sample_absent\" is looked up but the store does not tag it linux: add "
                 "linux to the platforms of localizations/keys/sample_absent.yaml and regenerate po/");
  const std::map<std::string, std::string> unscanned = {{"sample_unscanned", "a reason"}};
  UR_EXPECT_TRUE(StaleTagMiss({"sample_present"}, unscanned, "sample_present").empty());
  UR_EXPECT_TRUE(StaleTagMiss({"sample_present"}, unscanned, "sample_unscanned").empty());
  UR_EXPECT_TRUE(StaleTagMiss({"sample_present"}, unscanned, "sample_count") ==
                 "\"sample_count\" is tagged linux but nothing looks it up: in "
                 "localizations/keys/sample_count.yaml move linux from platforms to deprecated and "
                 "regenerate po/, or list it in UnscannedTaggedKeys with where it is looked up");
}

// Every T_ and TN_ call with literal arguments names an entry of the catalog,
// with the catalog's English.
UR_TEST(catalogLookupEveryLiteralCallIsInTheCatalog) {
  UR_EXPECT_TRUE_MSG("po/urnetwork.pot was read", TemplateCatalog().size() > 1000);
  const Scan scan = ScanGui();
  UR_EXPECT_TRUE_MSG("the scan saw the literal calls", scan.calls.size() > 1000);
  for (const Lookup& call : scan.calls) {
    const std::string miss = CatalogMiss(TemplateCatalog(), call);
    if (!miss.empty()) UR_FAIL(miss);
  }
}

// Every key/English pair a table, a return or an argument hands to a T_ call
// at run time names an entry of the catalog, with the catalog's English.
UR_TEST(catalogLookupEveryTablePairIsInTheCatalog) {
  const Scan scan = ScanGui();
  UR_EXPECT_TRUE_MSG("the scan saw the table pairs", scan.pairs.size() > 50);
  UR_EXPECT_TRUE_MSG("the scan saw the table-fed calls", scan.dynamicCalls > 10);
  for (const Lookup& pair : scan.pairs) {
    const std::string miss = CatalogMiss(TemplateCatalog(), pair);
    if (!miss.empty()) UR_FAIL(miss);
  }
}

// Every key a literal call or a table pair looks up is tagged linux, so it
// stays in the catalog for as long as this app looks it up.
UR_TEST(catalogLookupEveryKeyIsTaggedLinux) {
  UR_EXPECT_TRUE_MSG("po/linux-keys.txt was read", LinuxTaggedKeys().size() > 500);
  const Scan scan = ScanGui();
  for (const std::vector<Lookup>* lookups : {&scan.calls, &scan.pairs}) {
    for (const Lookup& lookup : *lookups) {
      const std::string miss = TagMiss(LinuxTaggedKeys(), lookup);
      if (!miss.empty()) UR_FAIL(miss);
    }
  }
}

// Every key tagged linux is looked up, by a call or a table pair the scan sees
// or where UnscannedTaggedKeys says, so no tag outlives the app's use of it.
UR_TEST(catalogLookupEveryTaggedKeyIsLookedUp) {
  UR_EXPECT_TRUE_MSG("po/linux-keys.txt was read", LinuxTaggedKeys().size() > 500);
  const Scan scan = ScanGui();
  std::set<std::string> lookedUp;
  for (const std::vector<Lookup>* lookups : {&scan.calls, &scan.pairs}) {
    for (const Lookup& lookup : *lookups) lookedUp.insert(lookup.key);
  }
  for (const std::string& key : LinuxTaggedKeys()) {
    const std::string miss = StaleTagMiss(lookedUp, UnscannedTaggedKeys(), key);
    if (!miss.empty()) UR_FAIL(miss);
  }
  // the exceptions shrink with the code
  for (const auto& entry : UnscannedTaggedKeys()) {
    UR_EXPECT_TRUE_MSG(entry.first + " (" + entry.second + ") is no longer tagged linux, or the "
                       "scan now sees it: drop it from UnscannedTaggedKeys",
                       LinuxTaggedKeys().count(entry.first) == 1 && lookedUp.count(entry.first) == 0);
  }
}

// The labels the SDK hands out are entries of the catalog under the SDK's
// English and tagged linux, so the row's runtime T_(key, English) finds each.
UR_TEST(catalogLookupSdkCryptoLabelsAreInTheCatalog) {
  UR_EXPECT_TRUE_MSG("po/urnetwork.pot was read", TemplateCatalog().size() > 1000);
  for (const auto& label : SdkCryptoLabels()) {
    const std::string miss = CatalogMiss(
        TemplateCatalog(), Lookup{"the SDK's crypto_pay.go", 0, label.first, label.second, std::nullopt});
    if (!miss.empty()) UR_FAIL(miss);
    UR_EXPECT_TRUE_MSG(label.first + " is tagged linux", LinuxTaggedKeys().count(label.first) == 1);
  }
}

// The daemon's sources stay out of the scan only while they translate nothing,
// and DeveloperPage.cpp's detail keys are scanned only while it derives them.
UR_TEST(catalogLookupScanBoundariesStillHold) {
  int daemonSources = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(CatalogSrcDir())) {
    if (!entry.is_regular_file()) continue;
    const std::string relative =
        std::filesystem::relative(entry.path(), CatalogSrcDir()).generic_string();
    if (!IsDaemonSource(relative)) continue;
    ++daemonSources;
    const Scan scan = ScanSource(relative, ReadCatalogLookupText(entry.path()), {}, false);
    UR_EXPECT_TRUE_MSG("src/" + relative + " calls T_ or TN_: scan it (IsDaemonSource)",
                       scan.calls.empty() && scan.dynamicCalls == 0);
  }
  UR_EXPECT_TRUE_MSG("the daemon's sources were found", daemonSources > 3);
  const std::string developer = ReadCatalogLookupText(CatalogSrcDir() / "DeveloperPage.cpp");
  UR_EXPECT_TRUE(developer.find("std::string DetailKey(const char* key) { return std::string(key) "
                                "+ \"_detail\"; }") != std::string::npos);
  // the status line's keys are assigned apart from their English, and the
  // VLESS errors are keyed by constants: both shapes stay visible to the scan
  std::set<std::string> pairKeys;
  for (const Lookup& pair : ScanGui().pairs) pairKeys.insert(pair.file + " " + pair.key);
  UR_EXPECT_TRUE(pairKeys.count("src/Health.hpp conn_finding_providers") == 1);
  UR_EXPECT_TRUE(pairKeys.count("src/ExtenderProvidePresentation.hpp ipv4_and_ipv6") == 1);
  UR_EXPECT_TRUE(pairKeys.count("src/VlessPresentation.hpp vless_error_link_invalid") == 1);
  UR_EXPECT_TRUE(pairKeys.count("src/ExtenderSharePresentation.hpp control_doh_error_too_many") == 1);
}
