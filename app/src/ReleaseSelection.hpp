// Which published release the in-app updater offers, decided pure.
//
// The update source is one repository: urnetwork/linux (kUpdateRepo). Every
// stable Linux release is published by hand to that repo's GitHub releases,
// carrying the same asset names build/all/run.sh mints for the nightly
// urnetwork/build release (require_linux_artifacts), so this file speaks the
// build names and polls the stable repo. The release list is asked for by the
// repository's numeric id (kUpdateRepoId), so a rename, or someone
// registering the owner's old name, cannot move it; the owner and repo stay
// only for the URLs GitHub spells by name. A release's download URL must be
// exactly the one its tag and asset name have on that repo (IsFeedAssetUrl):
// the nightly repo, personal forks, every other host and any other path are
// refused -- an API answer that points anywhere else is treated as hostile,
// never followed.
//
// The checker (UpdateChecker.cpp) turns the releases/latest JSON into the
// plain structs below and asks SelectRelease; the decision itself -- the tag
// grammar, the own-arch/own-kind asset name, the digest, the install-kind
// probe, the launch throttle -- touches no GTK or libsoup header, so
// tests/ReleaseSelectionTest.cpp runs it on any host against the names the
// release pipeline actually publishes. Same arrangement as the windows app's
// ReleaseSelection.h.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace urnw::update {

// ---- the one place the repository is named ---------------------------------

// Stable Linux releases. NOT urnetwork/build: that repo holds the nightly
// builds (and android-only prereleases); the team copies a nightly's Linux
// assets to a release here when it is declared stable.
inline constexpr const char* kUpdateRepo = "urnetwork/linux";

// The id GitHub assigned urnetwork/linux, the path of the release list. An
// owner/name path follows a rename with a redirect and goes to whoever
// registers the old name next; an id names this repository for good. The
// checker refuses redirects on that request, so nothing can move the feed.
inline constexpr std::uint64_t kUpdateRepoId = 1297137671;

// The release list, not /releases/latest: the stable repo has no release at
// all until the first one is published (a 404 there), and the newest release
// is not always the one this build can verify (SelectRelease falls back to an
// older one that is). 15 is the windows checker's page size: enough history
// to find a verifiable release behind a broken newest one, small enough to
// stay well under the body cap.
inline std::string ReleasesApiUrl() {
  return "https://api.github.com/repositories/" + std::to_string(kUpdateRepoId) +
         "/releases?per_page=15";
}

// The human page for a tag (the "Release page" button and the notice for the
// package-managed installs).
inline std::string ReleasePageUrl(std::string_view tag) {
  std::string url = std::string("https://github.com/") + kUpdateRepo + "/releases";
  if (!tag.empty()) {
    url += "/tag/";
    url += tag;
  }
  return url;
}

// The download URL the official repo gives `asset` of the release `tag`:
// https://github.com/urnetwork/linux/releases/download/<tag>/<asset>. The tag
// and the asset name come from the grammar, which has nothing to
// percent-encode.
inline std::string FeedAssetUrl(std::string_view tag, std::string_view asset) {
  std::string url = std::string("https://github.com/") + kUpdateRepo + "/releases/download/";
  url.append(tag);
  url.push_back('/');
  url.append(asset);
  return url;
}

// Whether a release names exactly that URL for its asset. Matched whole: a
// URL that only starts with the repo's download path can name another tag's
// file, another file, or a path a server resolves elsewhere. The storage
// host's redirect is libsoup's business and happens after this gate, over
// https.
inline bool IsFeedAssetUrl(std::string_view tag, std::string_view asset, std::string_view url) {
  return !tag.empty() && !asset.empty() && url == FeedAssetUrl(tag, asset);
}

// ---- the tag grammar: v<YYYY.M.D>-<code>[-beta] -----------------------------
// The same wire format every platform agrees on (windows VersionGrammar.h):
// the pipeline mints the tag, meson stamps the v-less form into UR_APP_VERSION,
// and this ranks them by code. 0 on no match -- which is also what a dev
// build's "0.0.0" sentinel parses to, so "not a release tag" and "never newer
// than anything" are deliberately the same answer.

namespace grammar_detail {

inline constexpr bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

struct DigitRun {
  std::uint64_t value = 0;
  std::size_t length = 0;  // 0 == no acceptable run at this position
};

// The leading decimal run, refused (length 0) when empty or longer than
// maxDigits. A too-long run is a REFUSAL, not a truncation. 18 digits sits
// below uint64_t's range, so the accumulation cannot wrap.
inline constexpr DigitRun TakeDigits(std::string_view s, std::size_t maxDigits) noexcept {
  std::size_t run = 0;
  while (run < s.size() && IsDigit(s[run])) ++run;
  if (run == 0 || run > maxDigits) return {};
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < run; ++i) value = value * 10 + static_cast<std::uint64_t>(s[i] - '0');
  return {value, run};
}

}  // namespace grammar_detail

inline constexpr std::uint64_t ParseReleaseCode(std::string_view tag) noexcept {
  using grammar_detail::TakeDigits;
  if (!tag.empty() && tag.front() == 'v') tag.remove_prefix(1);

  const auto year = TakeDigits(tag, 4);
  if (year.length != 4) return 0;
  tag.remove_prefix(year.length);
  if (tag.empty() || tag.front() != '.') return 0;
  tag.remove_prefix(1);

  const auto month = TakeDigits(tag, 2);
  if (month.length == 0 || month.value < 1 || month.value > 12) return 0;
  tag.remove_prefix(month.length);
  if (tag.empty() || tag.front() != '.') return 0;
  tag.remove_prefix(1);

  const auto day = TakeDigits(tag, 2);
  if (day.length == 0 || day.value < 1 || day.value > 31) return 0;
  tag.remove_prefix(day.length);

  if (tag.empty() || tag.front() != '-') return 0;
  tag.remove_prefix(1);

  const auto code = TakeDigits(tag, 18);
  if (code.length == 0) return 0;
  tag.remove_prefix(code.length);

  if (!tag.empty() && tag != "-beta") return 0;
  return code.value;
}

// The v-less version a tag names ("v2026.3.23-895075980" -> "2026.3.23-895075980").
inline std::string VersionFromTag(std::string_view tag) {
  if (!tag.empty() && tag.front() == 'v') tag.remove_prefix(1);
  return std::string(tag);
}

// The instant every release code counts from, 2023-05-23T00:00:00Z, in Unix
// seconds. A code is tenths of a second after it.
inline constexpr std::int64_t kCodeEpochUnixSeconds = 1684800000;

// How far after the release list's Date header a release code may point. A
// code is minted from the clock of the machine that built the release, and
// two days covers any honest skew between that clock and GitHub's.
inline constexpr std::int64_t kFutureCodeLimitSeconds = 48 * 60 * 60;

// The Unix second a release code names. Codes are at most 18 digits (the
// grammar above), so this cannot overflow.
inline constexpr std::int64_t CodeUnixSeconds(std::uint64_t code) {
  return kCodeEpochUnixSeconds + static_cast<std::int64_t>(code / 10);
}

// ---- the digest ------------------------------------------------------------
// GitHub's per-asset `digest` is `sha256:<64 lowercase hex>`. Anything that
// only ALMOST matches -- another algorithm, truncated hex, uppercase mixed in
// -- comes back empty: the caller compares it with a hash it computed itself,
// and empty can never accidentally equal anything.
inline std::string DigestHexFromAssetDigest(std::string_view digest) {
  constexpr std::string_view kPrefix = "sha256:";
  if (digest.size() != kPrefix.size() + 64) return {};
  if (digest.compare(0, kPrefix.size(), kPrefix) != 0) return {};
  const std::string_view hex = digest.substr(kPrefix.size());
  for (const char c : hex) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!ok) return {};
  }
  return std::string(hex);
}

// ---- what is installed, and which asset would replace it --------------------

// How this GUI got onto the machine. Only the AppImage replaces itself; the
// rest are told which file the release carries for them and how their package
// manager installs it. Never elevated by the app.
enum class InstallKind { Unknown, AppImage, Flatpak, Deb, Rpm, Arch, Tarball };

inline const char* InstallKindName(InstallKind kind) {
  switch (kind) {
    case InstallKind::AppImage: return "appimage";
    case InstallKind::Flatpak:  return "flatpak";
    case InstallKind::Deb:      return "deb";
    case InstallKind::Rpm:      return "rpm";
    case InstallKind::Arch:     return "arch";
    case InstallKind::Tarball:  return "tarball";
    case InstallKind::Unknown:  break;
  }
  return "unknown";
}

// What the running process can see, gathered by the checker and decided here.
struct InstallProbe {
  std::string appimageEnv;      // $APPIMAGE (the type2 runtime sets it)
  std::string appdirEnv;        // $APPDIR
  bool flatpakInfo = false;     // /.flatpak-info exists
  std::string exePath;          // readlink /proc/self/exe
  std::string osReleaseId;      // /etc/os-release ID
  std::string osReleaseIdLike;  // /etc/os-release ID_LIKE
};

inline bool ContainsWord(std::string_view haystack, std::string_view word) {
  std::size_t at = 0;
  while ((at = haystack.find(word, at)) != std::string_view::npos) {
    const bool startOk = at == 0 || haystack[at - 1] == ' ' || haystack[at - 1] == '"';
    const std::size_t end = at + word.size();
    const bool endOk = end == haystack.size() || haystack[end] == ' ' || haystack[end] == '"';
    if (startOk && endOk) return true;
    at = end;
  }
  return false;
}

// The package family a distro installs from, read off ID / ID_LIKE. Unknown
// when neither names a family we package for.
inline InstallKind PackageKindForDistro(std::string_view id, std::string_view idLike) {
  const auto any = [&](std::string_view word) {
    return ContainsWord(id, word) || ContainsWord(idLike, word);
  };
  if (any("debian") || any("ubuntu")) return InstallKind::Deb;
  if (any("arch")) return InstallKind::Arch;
  if (any("fedora") || any("rhel") || any("centos") || any("suse") || any("opensuse"))
    return InstallKind::Rpm;
  return InstallKind::Unknown;
}

// Flatpak first: a sandboxed GUI can carry an APPIMAGE variable it inherited
// and must never try to rewrite a file it cannot see. Then the AppImage
// runtime's own variables. A packaged binary (/usr/bin, /usr/lib, /opt) is
// told its distro's package; anywhere else (/usr/local, a build tree, ~) is
// the install tarball's audience.
inline InstallKind DetectInstallKind(const InstallProbe& probe) {
  if (probe.flatpakInfo) return InstallKind::Flatpak;
  if (!probe.appimageEnv.empty() && !probe.appdirEnv.empty()) return InstallKind::AppImage;
  const auto under = [&](std::string_view prefix) {
    return probe.exePath.size() > prefix.size() &&
           probe.exePath.compare(0, prefix.size(), prefix) == 0;
  };
  if (under("/usr/bin/") || under("/usr/lib/") || under("/usr/lib64/") ||
      under("/usr/libexec/") || under("/opt/")) {
    const InstallKind kind = PackageKindForDistro(probe.osReleaseId, probe.osReleaseIdLike);
    return kind == InstallKind::Unknown ? InstallKind::Tarball : kind;
  }
  if (probe.exePath.empty()) return InstallKind::Unknown;
  return InstallKind::Tarball;
}

// "amd64" / "arm64": the arch half of every Linux asset name, decided at
// compile time because a binary only ever updates itself to its own
// architecture.
inline constexpr const char* OwnArch() {
#if defined(__aarch64__)
  return "arm64";
#elif defined(__x86_64__)
  return "amd64";
#else
  return "";
#endif
}

// The release asset that would replace THIS install, in the names
// build/all/run.sh require_linux_artifacts pins (linux/MIGRATION.md):
//   URnetwork-<v>-<amd64|arm64>.AppImage          the GUI
//   URnetwork-<v>-<amd64|arm64>.flatpak           the GUI
//   urnetwork-daemon_<v>_<amd64|arm64>.deb        the service (GUI via the launcher)
//   urnetwork-daemon-<v>.<x86_64|aarch64>.rpm
//   urnetwork-daemon-<v>-<x86_64|aarch64>.pkg.tar.zst
//   urnetwork-daemon-<v>-<amd64|arm64>.install.tar.gz
// Empty when the kind or arch is unknown: no name, no offer.
inline std::string AssetNameFor(InstallKind kind, std::string_view version, std::string_view arch) {
  if (version.empty() || arch.empty()) return {};
  std::string pkgArch;
  if (arch == "amd64") pkgArch = "x86_64";
  else if (arch == "arm64") pkgArch = "aarch64";
  else return {};
  std::string v(version);
  std::string a(arch);
  switch (kind) {
    case InstallKind::AppImage: return "URnetwork-" + v + "-" + a + ".AppImage";
    case InstallKind::Flatpak:  return "URnetwork-" + v + "-" + a + ".flatpak";
    case InstallKind::Deb:      return "urnetwork-daemon_" + v + "_" + a + ".deb";
    case InstallKind::Rpm:      return "urnetwork-daemon-" + v + "." + pkgArch + ".rpm";
    case InstallKind::Arch:     return "urnetwork-daemon-" + v + "-" + pkgArch + ".pkg.tar.zst";
    case InstallKind::Tarball:  return "urnetwork-daemon-" + v + "-" + a + ".install.tar.gz";
    case InstallKind::Unknown:  break;
  }
  return {};
}

// The command the user runs after downloading `assetName` from the release
// page. Shown, never executed: the app does not elevate.
inline std::string PackageManagerCommand(InstallKind kind, std::string_view assetName) {
  const std::string name(assetName);
  switch (kind) {
    case InstallKind::Deb:     return "sudo apt install ./" + name;
    case InstallKind::Rpm:     return "sudo dnf install ./" + name;
    case InstallKind::Arch:    return "sudo pacman -U ./" + name;
    case InstallKind::Flatpak: return "flatpak install ./" + name;
    case InstallKind::Tarball: return "tar xzf " + name + " && sudo urnetwork-daemon/install.sh";
    case InstallKind::AppImage:
    case InstallKind::Unknown:
      break;
  }
  return {};
}

// ---- the release list ------------------------------------------------------

struct ReleaseAsset {
  std::string name;
  std::string url;     // browser_download_url
  std::string digest;  // the API's `sha256:<hex>`, verbatim
};

struct Release {
  std::string tag;  // tag_name, with its v
  std::string htmlUrl;
  bool draft = false;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;
};

// nlohmann's value() only substitutes the default for a MISSING key; a key
// present with the wrong type ("tag_name": null) throws out of get<>(). The
// release list is another service's JSON -- its shape is what this code must
// survive, not assume -- so every read is a find + type check.
inline bool JsonFlag(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_boolean() && it->get<bool>();
}

inline std::string JsonString(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

// /releases/latest answers ONE release object; /releases answers an array.
// Both are accepted so a future switch between them cannot reach the
// decision below. Anything else parses to no releases.
inline std::vector<Release> ParseReleases(const nlohmann::json& body) {
  std::vector<Release> out;
  const auto one = [&out](const nlohmann::json& rel) {
    if (!rel.is_object()) return;
    Release r;
    r.tag = JsonString(rel, "tag_name");
    r.htmlUrl = JsonString(rel, "html_url");
    r.draft = JsonFlag(rel, "draft");
    r.prerelease = JsonFlag(rel, "prerelease");
    if (auto assets = rel.find("assets"); assets != rel.end() && assets->is_array()) {
      for (const auto& asset : *assets) {
        if (!asset.is_object()) continue;
        r.assets.push_back({JsonString(asset, "name"), JsonString(asset, "browser_download_url"),
                            JsonString(asset, "digest")});
      }
    }
    out.push_back(std::move(r));
  };
  if (body.is_array()) {
    for (const auto& rel : body) one(rel);
  } else {
    one(body);
  }
  return out;
}

struct Selection {
  // The newest non-draft, non-prerelease tag that parses and names no future
  // code, offerable or not -- the developer line names it either way.
  std::uint64_t newestCode = 0;
  std::string newestVersion;  // v-less

  // The newest release this install can actually be pointed at and, for the
  // AppImage, verify: own-kind/own-arch asset attached, hosted by the official
  // repo, carrying a usable sha256 digest. code == 0 means none.
  std::uint64_t code = 0;
  std::string version;      // v-less
  std::string tag;          // as minted, with the v
  std::string releasePage;  // html_url, or ReleasePageUrl(tag) when absent
  std::string assetName;
  std::string assetUrl;
  std::string digestHex;  // lowercase
  // The signed checksum pair (UpdateSignature.hpp), from the SAME release.
  // sigAttached says a SHA256SUMS.sig is listed at all; its URL (and the
  // SHA256SUMS URL) stay empty unless hosted by the official repo, and an
  // attached signature with no usable URL is refused at apply time.
  bool sigAttached = false;
  std::string sumsUrl;
  std::string sigUrl;

  // code outranks the running build. A dev build (own code 0) is never
  // offered anything: nothing is "newer" than unversioned in a way that is
  // safe to install over it.
  bool updateAvailable = false;
  // updateAvailable AND this install replaces itself (the AppImage): the
  // apply may download, verify and swap. Every other kind is told the file
  // and the command instead.
  bool installable = false;

  struct Skip {
    std::string tag;
    std::string reason;
  };
  std::vector<Skip> skipped;  // releases that parsed but could not be offered, for the log
};

// The release to offer from `releases` (the API's order), judged against
// `serverUnixSeconds`, the release list's Date header.
inline Selection SelectRelease(const std::vector<Release>& releases, std::uint64_t ownCode,
                               InstallKind kind, std::string_view arch,
                               std::int64_t serverUnixSeconds) {
  Selection s;
  for (const auto& rel : releases) {
    // Drafts and prereleases are skipped outright rather than merely failing
    // the asset match: a prerelease can outrank the real release by code and
    // the developer line must not name it as "the newest release".
    if (rel.draft || rel.prerelease) continue;
    const std::uint64_t code = ParseReleaseCode(rel.tag);
    if (code == 0) continue;
    // Before the release can count as the newest: one mistyped or hostile
    // far-future code would otherwise outrank every real release for good.
    if (CodeUnixSeconds(code) > serverUnixSeconds + kFutureCodeLimitSeconds) {
      s.skipped.push_back({rel.tag, "has a future code (more than 48 h after the server's date)"});
      continue;
    }
    const std::string ver = VersionFromTag(rel.tag);
    if (code > s.newestCode) {
      s.newestCode = code;
      s.newestVersion = ver;
    }
    if (code <= s.code) continue;  // an older release than one already offerable

    const std::string name = AssetNameFor(kind, ver, arch);
    if (name.empty()) {
      s.skipped.push_back({rel.tag, std::string("has no asset for this install (") +
                                        InstallKindName(kind) + ")"});
      continue;
    }
    const ReleaseAsset* match = nullptr;
    for (const auto& asset : rel.assets) {
      if (asset.name == name) match = &asset;
    }
    if (!match || match->url.empty()) {
      s.skipped.push_back({rel.tag, "lacks " + name});
      continue;
    }
    if (!IsFeedAssetUrl(rel.tag, name, match->url)) {
      // Named right, hosted wrong: refused, and the next older release is
      // considered instead.
      s.skipped.push_back({rel.tag, name + " is not hosted by " + kUpdateRepo});
      continue;
    }
    // URL and expected hash from the SAME asset object: the digest is GitHub's
    // own upload-time SHA-256 for exactly the bytes this URL serves.
    std::string digestHex = DigestHexFromAssetDigest(match->digest);
    if (digestHex.empty()) {
      s.skipped.push_back({rel.tag, "lacks a usable digest for " + name});
      continue;
    }
    s.sigAttached = false;
    s.sumsUrl.clear();
    s.sigUrl.clear();
    for (const auto& asset : rel.assets) {
      if (asset.name == "SHA256SUMS.sig") {
        s.sigAttached = true;
        if (IsFeedAssetUrl(rel.tag, asset.name, asset.url)) s.sigUrl = asset.url;
      } else if (asset.name == "SHA256SUMS" && IsFeedAssetUrl(rel.tag, asset.name, asset.url)) {
        s.sumsUrl = asset.url;
      }
    }
    s.code = code;
    s.version = ver;
    s.tag = rel.tag;
    s.releasePage = rel.htmlUrl.empty() ? ReleasePageUrl(rel.tag) : rel.htmlUrl;
    s.assetName = name;
    s.assetUrl = match->url;
    s.digestHex = std::move(digestHex);
  }
  s.updateAvailable = ownCode != 0 && s.code > ownCode;
  s.installable = s.updateAvailable && kind == InstallKind::AppImage;
  return s;
}

// ---- the launch throttle ---------------------------------------------------

inline constexpr std::int64_t kCheckIntervalSeconds = 6 * 60 * 60;

// Whether a launch (or a timer tick) should hit the network now. The last
// check time is persisted, so relaunching every hour does not re-ask every
// hour; a clock that went backwards reads as "due" rather than as a wait of
// years. A dev build (own code 0) never checks on its own -- the developer
// screen's manual check still runs.
inline bool ShouldCheckNow(bool enabled, std::uint64_t ownCode, std::int64_t lastCheckUnix,
                           std::int64_t nowUnix) {
  if (!enabled || ownCode == 0) return false;
  if (lastCheckUnix <= 0) return true;
  if (nowUnix < lastCheckUnix) return true;
  return nowUnix - lastCheckUnix >= kCheckIntervalSeconds;
}

// ---- the AppImage swap paths -----------------------------------------------
// The download lands NEXT TO the running AppImage (same filesystem, so the
// final rename is atomic), under a dot-name no launcher glob matches
// (~/Applications/URnetwork*.AppImage must not pick up a half-written file).
inline std::string PartPath(std::string_view appimage) {
  const std::size_t slash = appimage.rfind('/');
  const std::string_view dir = slash == std::string_view::npos ? "" : appimage.substr(0, slash + 1);
  const std::string_view base =
      slash == std::string_view::npos ? appimage : appimage.substr(slash + 1);
  return std::string(dir) + "." + std::string(base) + ".part";
}

// The running image survives as <appimage>.bak until the new one has launched
// (the next start deletes it).
inline std::string BackupPath(std::string_view appimage) { return std::string(appimage) + ".bak"; }

// ---- the relaunch environment ----------------------------------------------
// AppRun exports paths INTO the old mount, and the type2 runtime exports its
// own four. The new runtime re-exports its four and the new AppRun re-exports
// its paths, but an inherited $APPDIR-prefixed LD_LIBRARY_PATH would still be
// searched, so every value that names the old mount is dropped and the
// list-valued ones keep only their foreign entries.
using EnvList = std::vector<std::pair<std::string, std::string>>;

inline EnvList ScrubRelaunchEnv(const EnvList& env, std::string_view appdir) {
  static const char* const kRuntimeVars[] = {"APPDIR", "APPIMAGE", "OWD", "ARGV0"};
  static const char* const kListVars[] = {"LD_LIBRARY_PATH", "XDG_DATA_DIRS"};
  EnvList out;
  for (const auto& [name, value] : env) {
    bool runtime = false;
    for (const char* v : kRuntimeVars) runtime = runtime || name == v;
    if (runtime) continue;
    bool list = false;
    for (const char* v : kListVars) list = list || name == v;
    if (list && !appdir.empty()) {
      std::string kept;
      std::size_t start = 0;
      while (start <= value.size()) {
        const std::size_t end = value.find(':', start);
        const std::string entry = value.substr(start, end == std::string::npos ? std::string::npos
                                                                               : end - start);
        const bool inMount = entry.size() >= appdir.size() &&
                             entry.compare(0, appdir.size(), appdir) == 0;
        if (!entry.empty() && !inMount) {
          if (!kept.empty()) kept += ':';
          kept += entry;
        }
        if (end == std::string::npos) break;
        start = end + 1;
      }
      if (!kept.empty()) out.emplace_back(name, kept);
      continue;
    }
    if (!appdir.empty() && value.size() >= appdir.size() &&
        value.compare(0, appdir.size(), appdir) == 0) {
      continue;  // GSETTINGS_SCHEMA_DIR, GIO_MODULE_DIR: a path into the old mount
    }
    if (name == "GDK_PIXBUF_MODULE_FILE") continue;  // regenerated by the new AppRun
    out.emplace_back(name, value);
  }
  return out;
}

}  // namespace urnw::update
