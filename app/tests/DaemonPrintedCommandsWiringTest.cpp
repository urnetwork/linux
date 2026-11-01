// Every command urnetworkd prints names the binary by its path.
//
// No package puts urnetworkd on $PATH: the deb, rpm and Arch packages install
// it at /usr/lib/urnetwork/urnetworkd, and the install tarball moves it to
// /usr/local/lib/urnetwork/urnetworkd on an immutable host. The egress
// refusal told people to run `urnetworkd --diagnose`, which is "command not
// found" as printed. The daemon's sources need glib, the SDK and Linux
// headers, so they are read as source here; the path and the refusal's text
// are tested in DaemonSelfPathTest.cpp.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "WiringSource.hpp"

namespace fs = std::filesystem;
using namespace urnw::testing::wiring;

namespace {

bool HexDigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// The string literals of `code` (comments already blanked), with adjacent
// literals joined the way the compiler joins them and the common escapes
// turned back into their characters.
std::vector<std::string> StringLiterals(const std::string& code) {
  std::vector<std::string> out;
  std::string current;
  bool open = false;  // `current` can still be extended by an adjacent literal
  const size_t n = code.size();
  size_t i = 0;
  while (i < n) {
    const char c = code[i];
    if (c == '\'') {
      // a digit separator (21'600LL) is not a character literal
      if (i > 0 && i + 1 < n && HexDigit(code[i - 1]) && HexDigit(code[i + 1])) {
        ++i;
        continue;
      }
      size_t k = i + 1;
      while (k < n && code[k] != '\'' && code[k] != '\n') {
        if (code[k] == '\\') ++k;
        ++k;
      }
      i = k + 1;
      if (open) {
        out.push_back(current);
        current.clear();
        open = false;
      }
      continue;
    }
    if (c == '"') {
      if (i > 0 && code[i - 1] == 'R') {  // R"delim( ... )delim" carries no escapes
        const size_t paren = code.find('(', i);
        if (paren == std::string::npos) break;
        const std::string close = ")" + code.substr(i + 1, paren - i - 1) + "\"";
        const size_t end = code.find(close, paren);
        if (end == std::string::npos) break;
        current += code.substr(paren + 1, end - paren - 1);
        i = end + close.size();
      } else {
        size_t k = i + 1;
        while (k < n && code[k] != '"' && code[k] != '\n') {
          if (code[k] == '\\' && k + 1 < n) {
            const char e = code[k + 1];
            current += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            k += 2;
            continue;
          }
          current += code[k++];
        }
        i = k + 1;
      }
      open = true;
      continue;
    }
    if (open && !std::isspace(static_cast<unsigned char>(c))) {
      out.push_back(current);
      current.clear();
      open = false;
    }
    ++i;
  }
  if (open) out.push_back(current);
  return out;
}

bool NamePart(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '/' || c == '_' || c == '-' ||
         c == '.';
}

// Each command in `text` that runs urnetworkd by its bare name: the name, not
// part of a path or a longer word, then a flag ("urnetworkd --diagnose") or a
// usage line's "urnetworkd [". Not a command: a "urnetworkd --mode: ..."
// message prefix, and the unit's name after systemctl or journalctl.
std::vector<std::string> BareCommands(const std::string& text) {
  static const std::string kName = "urnetworkd";
  std::vector<std::string> found;
  for (size_t at = text.find(kName); at != std::string::npos; at = text.find(kName, at + 1)) {
    if (at > 0 && NamePart(text[at - 1])) continue;
    const size_t newline = text.rfind('\n', at);
    const size_t lineStart = newline == std::string::npos ? 0 : newline + 1;
    const std::string before = text.substr(lineStart, at - lineStart);
    if (before.find("systemctl ") != std::string::npos ||
        before.find("journalctl ") != std::string::npos) {
      continue;
    }
    size_t k = at + kName.size();
    if (k >= text.size() || text[k] != ' ') continue;
    while (k < text.size() && text[k] == ' ') ++k;
    if (k < text.size() && text[k] == '[') {
      found.push_back(text.substr(at, k + 1 - at));
      continue;
    }
    if (k >= text.size() || text[k] != '-') continue;
    size_t end = k;
    while (end < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '-')) {
      ++end;
    }
    if (end < text.size() && text[end] == ':') continue;
    found.push_back(text.substr(at, end - at));
  }
  return found;
}

}  // namespace

// The scanner below finds what it is meant to find, and nothing else.
UR_TEST(DaemonCommands_TheScannerTellsACommandFromAName) {
  UR_EXPECT_EQ(1, static_cast<int>(BareCommands("run `urnetworkd --diagnose` now").size()));
  UR_EXPECT_EQ(1, static_cast<int>(BareCommands("#     sudo urnetworkd --revert\n").size()));
  UR_EXPECT_EQ(1, static_cast<int>(BareCommands("usage: urnetworkd [--foreground]").size()));
  UR_EXPECT_EQ(0, static_cast<int>(BareCommands("sudo /usr/lib/urnetwork/urnetworkd --revert").size()));
  UR_EXPECT_EQ(0, static_cast<int>(BareCommands("urnetworkd --selftest-egress: aborted").size()));
  UR_EXPECT_EQ(0, static_cast<int>(BareCommands("journalctl -u urnetworkd -f").size()));
  UR_EXPECT_EQ(0, static_cast<int>(BareCommands("    sudo systemctl stop urnetworkd\n").size()));
  UR_EXPECT_EQ(0, static_cast<int>(BareCommands("urnetworkd %s (control protocol %d)").size()));
  UR_EXPECT_EQ(0, static_cast<int>(BareCommands("system.slice/urnetworkd.service").size()));
  // Adjacent literals are one string; a comment (blanked first) and a
  // character literal are none.
  const std::vector<std::string> joined = StringLiterals(
      BlankComments("f(\"a `urnetworkd \"\n  \"--diagnose` b\", '\"'); // \"urnetworkd --no\""));
  UR_EXPECT_EQ(1, static_cast<int>(joined.size()));
  UR_EXPECT_TRUE(joined.size() == 1 && joined[0] == "a `urnetworkd --diagnose` b");
  UR_EXPECT_TRUE(joined.size() == 1 && BareCommands(joined[0]).size() == 1);
}

// THE DEFECT: nothing the daemon or the app prints, in the journal, the
// refusal the app shows, --help, the recovery lines or the files it writes,
// tells a person to type a bare `urnetworkd`.
UR_TEST(DaemonCommands_NoPrintedTextNamesABareUrnetworkd) {
  int sources = 0;
  std::error_code error;
  for (const auto& entry : fs::recursive_directory_iterator(UR_SRC_DIR, error)) {
    if (!entry.is_regular_file()) continue;
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".hpp") continue;
    const std::string relative = fs::relative(entry.path(), UR_SRC_DIR).generic_string();
    ++sources;
    for (const auto& literal : StringLiterals(ReadCode(relative))) {
      for (const auto& command : BareCommands(literal)) {
        UR_FAIL("src/" + relative + " prints `" + command +
                "`, a name on no one's $PATH, in: " + literal.substr(0, 160));
      }
    }
  }
  UR_EXPECT_TRUE_MSG("src/ is listed", !error && sources > 100);
}

// The refusal names --diagnose by the running binary's path.
UR_TEST(DaemonCommands_TheEgressRefusalNamesTheRunningBinary) {
  const std::string tunnel = ReadCode("Tunnel.cpp");
  const std::string witness = FunctionBody(tunnel, "bool Tunnel::VerifyEgressWitness(");
  UR_EXPECT_TRUE_MSG("Tunnel::VerifyEgressWitness is read", !witness.empty());
  UR_EXPECT_TRUE(Contains(witness, "err->message = EgressRefusalMessage(message, SelfPath());"));
  UR_EXPECT_TRUE(Contains(tunnel, "#include \"daemon/SelfPath.hpp\""));
  UR_EXPECT_TRUE(Contains(ReadCode("TunnelPolicy.hpp"),
                          "inline std::string EgressRefusalMessage(const std::string& reason, "
                          "const std::string& self) {"));
}

// One resolver for the daemon's path, and its fallback is where the unit
// starts the daemon.
UR_TEST(DaemonCommands_OneResolverForTheDaemonsPath) {
  const std::string main = ReadCode("daemon/main.cpp");
  UR_EXPECT_TRUE(Contains(main, "#include \"daemon/SelfPath.hpp\""));
  UR_EXPECT_TRUE(Contains(main, "using urnw::SelfPath;"));
  UR_EXPECT_FALSE(Contains(main, "readlink(\"/proc/self/exe\""));
  UR_EXPECT_FALSE(Contains(main, "kInstalledPath"));
  const std::string self = ReadCode("daemon/SelfPath.hpp");
  UR_EXPECT_TRUE(Contains(self, "readlink(\"/proc/self/exe\""));
  UR_EXPECT_TRUE(Contains(self,
                          "kInstalledDaemonPath = \"/usr/lib/urnetwork/urnetworkd\";"));
  UR_EXPECT_TRUE(Contains(ReadRaw("../packaging/urnetworkd.service"),
                          "\nExecStart=/usr/lib/urnetwork/urnetworkd\n"));
}
