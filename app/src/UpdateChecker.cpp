// SPDX-License-Identifier: MPL-2.0
#include "UpdateChecker.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>
#include <libsoup/soup.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <optional>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "AppPrefs.hpp"
#include "Ui.hpp"
#include "UpdateSchedule.hpp"
#include "UpdateSignature.hpp"

// The build stamp: meson passes -DUR_APP_VERSION into the GUI (see
// meson.build); the fallback keeps this TU self-contained, the same idiom
// SdkHost.cpp uses. "0.0.0" parses to code 0 -- a dev build, never offered an
// install (ReleaseSelection.hpp).
#ifndef UR_APP_VERSION
#define UR_APP_VERSION "0.0.0"
#endif

extern char** environ;

namespace urnw {
namespace {

using Clock = std::chrono::steady_clock;

// The launch check waits out the startup rush (SDK init, daemon reconnect,
// the tray settling) rather than adding an HTTP request to it.
constexpr std::int64_t kLaunchDelaySeconds = 30;

// Response caps. The release LIST is JSON of a few hundred KB (every
// platform's assets ride along); the AppImage is ~100 MB today. A cap is not
// a guess about the future, it is the refusal to stream an unbounded body
// into a file because a server said so.
constexpr std::uint64_t kMaxJsonBytes = 8ull * 1024 * 1024;
constexpr std::uint64_t kMaxImageBytes = 1ull * 1024 * 1024 * 1024;

constexpr const char* kAutoCheckPrefKey = "check_updates_automatically";
constexpr const char* kLastCheckPrefKey = "update_last_check_at";  // unix seconds
// Unix seconds of the last check that reached GitHub; the six-hour throttle
// above counts every completed check, failed ones too, so it cannot say this.
constexpr const char* kLastSuccessPrefKey = "update_last_check_success";

const std::uint64_t kOwnCode = update::ParseReleaseCode(UR_APP_VERSION);

std::int64_t NowUnix() { return g_get_real_time() / G_USEC_PER_SEC; }

// Whole seconds from now until `deadline` on the steady clock, rounded up; 0
// once it has come.
std::int64_t SecondsUntil(Clock::time_point deadline) {
  const Clock::duration left = deadline - Clock::now();
  if (left <= Clock::duration::zero()) return 0;
  return std::chrono::ceil<std::chrono::seconds>(left).count();
}

std::string EnvOr(const char* name) {
  const char* v = g_getenv(name);
  return v ? std::string(v) : std::string();
}

// A Unix second in this machine's zone, in a g_date_time_format pattern; ""
// when it cannot be represented.
std::string FormatLocal(std::int64_t unixSeconds, const char* pattern) {
  GDateTime* moment = g_date_time_new_from_unix_local(unixSeconds);
  if (!moment) return {};
  gchar* text = g_date_time_format(moment, pattern);
  g_date_time_unref(moment);
  std::string out = text ? text : "";
  g_free(text);
  return out;
}

// ---- the fetch ---------------------------------------------------------------

// A redirect that steps down to http is refused: libsoup follows redirects
// itself (the asset's browser_download_url 302s to a storage host), and
// "restarted" fires once per hop with the new URI already on the message.
void OnRestarted(SoupMessage* msg, gpointer data) {
  GUri* uri = soup_message_get_uri(msg);
  const char* scheme = uri ? g_uri_get_scheme(uri) : nullptr;
  if (!scheme || std::strcmp(scheme, "https") != 0) {
    g_warning("update: refusing a redirect off https");
    g_cancellable_cancel(G_CANCELLABLE(data));
  }
}

// What a GET said beside its body.
struct FetchHeaders {
  // The Date header in Unix seconds, 0 when it had none.
  std::int64_t serverUnixSeconds = 0;
  // What a refused request said about asking again (UpdateSchedule.hpp).
  std::int64_t retryAfterSeconds = 0;
  std::int64_t rateLimitResetUnixSeconds = 0;
  bool rateLimitExhausted = false;
};

// The response's Date header in Unix seconds, or 0 when it has none or it
// does not parse.
std::int64_t ResponseDateUnixSeconds(SoupMessage* msg) {
  const char* date = soup_message_headers_get_one(soup_message_get_response_headers(msg), "Date");
  if (!date) return 0;
  GDateTime* parsed = soup_date_time_new_from_http_string(date);
  if (!parsed) return 0;
  const std::int64_t seconds = g_date_time_to_unix(parsed);
  g_date_time_unref(parsed);
  return seconds;
}

// One GET, streamed into `sink` chunk by chunk. GitHub requires a User-Agent
// on every request. Any status but 200 fails the fetch, and `error` names it.
// With `followRedirects` false a redirect comes back as its own status and
// fails the fetch: the release list's URL names the repository by its id, and
// nothing may move it. The AppImage download, a browser_download_url that
// 302s to a storage host, follows them over https. `headers`, when given,
// receives what the response said beside its body.
bool FetchUrl(const std::string& url, const char* accept, std::uint64_t maxBytes,
              bool followRedirects, GCancellable* cancellable,
              const std::function<bool(const char*, gsize)>& sink, FetchHeaders* headers,
              std::string& error) {
  {
    GError* err = nullptr;
    GUri* uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, &err);
    if (!uri) {
      error = std::string("not a url: ") + (err ? err->message : "?");
      if (err) g_error_free(err);
      return false;
    }
    const bool https = std::strcmp(g_uri_get_scheme(uri), "https") == 0;
    g_uri_unref(uri);
    if (!https) {
      error = "not an https url";
      return false;
    }
  }

  SoupSession* session = soup_session_new();
  soup_session_set_user_agent(session, ("URnetwork-Linux/" UR_APP_VERSION));
  soup_session_set_timeout(session, 30);
  SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
  if (!msg) {
    g_object_unref(session);
    error = "could not build the request";
    return false;
  }
  if (!followRedirects) soup_message_add_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
  if (accept) soup_message_headers_append(soup_message_get_request_headers(msg), "Accept", accept);
  soup_message_headers_append(soup_message_get_request_headers(msg), "X-GitHub-Api-Version",
                              "2022-11-28");
  g_signal_connect(msg, "restarted", G_CALLBACK(OnRestarted), cancellable);

  GError* err = nullptr;
  GInputStream* in = soup_session_send(session, msg, cancellable, &err);
  bool ok = false;
  if (!in) {
    error = std::string("request failed: ") + (err ? err->message : "?");
    if (err) g_error_free(err);
  } else {
    const unsigned status = soup_message_get_status(msg);
    if (headers) headers->serverUnixSeconds = ResponseDateUnixSeconds(msg);
    if (status != 200) {
      if (headers) {
        // GitHub says when to ask again: Retry-After for a secondary limit,
        // the reset time once the hour's requests are spent.
        SoupMessageHeaders* response = soup_message_get_response_headers(msg);
        headers->retryAfterSeconds = update::ParseDecimalHeader(
            soup_message_headers_get_one(response, "Retry-After"), 0);
        headers->rateLimitResetUnixSeconds = update::ParseDecimalHeader(
            soup_message_headers_get_one(response, "X-RateLimit-Reset"), 0);
        headers->rateLimitExhausted =
            update::ParseDecimalHeader(
                soup_message_headers_get_one(response, "X-RateLimit-Remaining"), -1) == 0;
      }
      error = "http status " + std::to_string(status);
    } else {
      std::vector<char> chunk(64 * 1024);
      std::uint64_t total = 0;
      for (;;) {
        const gssize n = g_input_stream_read(in, chunk.data(), chunk.size(), cancellable, &err);
        if (n < 0) {
          error = std::string("read failed: ") + (err ? err->message : "?");
          if (err) g_error_free(err);
          break;
        }
        if (n == 0) {
          ok = true;  // the body is complete
          break;
        }
        total += static_cast<std::uint64_t>(n);
        if (total > maxBytes) {
          error = "response larger than the " + std::to_string(maxBytes) + " byte cap";
          break;
        }
        if (!sink(chunk.data(), static_cast<gsize>(n))) {
          error = "write failed";
          break;
        }
      }
    }
    g_input_stream_close(in, nullptr, nullptr);
    g_object_unref(in);
  }
  g_object_unref(msg);
  g_object_unref(session);
  return ok;
}

// ---- SHA-256 (GChecksum) -----------------------------------------------------

// The file's hash as lowercase hex -- the canonical form
// DigestHexFromAssetDigest returns, so the comparison is bytewise. Empty on
// any failure: an unreadable file must fail verification, not pass it.
std::string Sha256File(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  GChecksum* sum = g_checksum_new(G_CHECKSUM_SHA256);
  std::vector<char> buf(64 * 1024);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const std::streamsize n = in.gcount();
    if (n <= 0) break;
    g_checksum_update(sum, reinterpret_cast<const guchar*>(buf.data()), static_cast<gssize>(n));
  }
  std::string hex;
  if (!in.bad()) hex = g_checksum_get_string(sum);
  g_checksum_free(sum);
  return hex;
}

// ---- files -------------------------------------------------------------------

bool DirWritable(const std::string& dir) { return ::access(dir.c_str(), W_OK | X_OK) == 0; }

std::string DirOf(const std::string& path) {
  char* dir = g_path_get_dirname(path.c_str());
  std::string out(dir);
  g_free(dir);
  return out;
}

std::string DownloadsDir() {
  const char* dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  std::string out = dir ? dir : std::string(g_get_home_dir()) + "/Downloads";
  g_mkdir_with_parents(out.c_str(), 0755);
  return out;
}

// The new image takes the running one's mode bits plus execute, so a
// deliberately group-readable or private install stays what it was.
mode_t ExecutableMode(const std::string& like) {
  struct stat st {};
  mode_t mode = 0755;
  if (::stat(like.c_str(), &st) == 0) mode = (st.st_mode & 07777) | S_IXUSR | S_IXGRP | S_IXOTH;
  return mode;
}

void RemoveIfPresent(const std::string& path, const char* what) {
  if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) return;
  if (::unlink(path.c_str()) == 0) {
    g_message("update: removed %s %s", what, path.c_str());
  } else {
    g_warning("update: could not remove %s %s: %s", what, path.c_str(), std::strerror(errno));
  }
}

// /etc/os-release, the two keys the package-family decision reads.
void ReadOsRelease(std::string* id, std::string* idLike) {
  std::ifstream in("/etc/os-release");
  std::string line;
  while (std::getline(in, line)) {
    const auto take = [&line](const char* key) -> std::string {
      const std::string prefix = std::string(key) + "=";
      if (line.compare(0, prefix.size(), prefix) != 0) return {};
      std::string value = line.substr(prefix.size());
      if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
        value = value.substr(1, value.size() - 2);
      }
      return value;
    };
    if (std::string v = take("ID"); !v.empty()) *id = v;
    if (std::string v = take("ID_LIKE"); !v.empty()) *idLike = v;
  }
}

}  // namespace

// ---- lifecycle ---------------------------------------------------------------

UpdateChecker::UpdateChecker() : cancellable_(g_cancellable_new()) {
  snapshot_.kind = DetectInstallKind();
}

UpdateChecker::~UpdateChecker() {
  Stop();
  g_object_unref(cancellable_);
}

update::InstallKind UpdateChecker::DetectInstallKind() {
  update::InstallProbe probe;
  probe.appimageEnv = EnvOr("APPIMAGE");
  probe.appdirEnv = EnvOr("APPDIR");
  probe.flatpakInfo = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
  if (char* exe = g_file_read_link("/proc/self/exe", nullptr)) {
    probe.exePath = exe;
    g_free(exe);
  }
  ReadOsRelease(&probe.osReleaseId, &probe.osReleaseIdLike);
  return update::DetectInstallKind(probe);
}

std::string UpdateChecker::LocalDate(std::int64_t unixSeconds) {
  return FormatLocal(unixSeconds, "%x");
}

std::string UpdateChecker::LocalDateTime(std::int64_t unixSeconds) {
  return FormatLocal(unixSeconds, "%x, %R");
}

void UpdateChecker::Start() {
  autoCheck_ = AutoCheckEnabled();
  // When a check last reached GitHub; before any has, this first launch is
  // when checks began, so 72 hours of failures from now are reported too.
  // Before the worker exists, so no lock is needed.
  snapshot_.lastSuccessUnix = prefs::Get<std::int64_t>(kLastSuccessPrefKey, 0);
  if (snapshot_.lastSuccessUnix <= 0) {
    snapshot_.lastSuccessUnix = NowUnix();
    prefs::Set(kLastSuccessPrefKey, snapshot_.lastSuccessUnix);
  }
  if (kOwnCode == 0) {
    g_message("update: dev build (%s) -- automatic checking disabled; the developer "
              "screen's manual check still runs and reports", UR_APP_VERSION);
  }
  g_message("update: install kind %s, arch %s, repo %s", update::InstallKindName(snapshot_.kind),
            update::OwnArch(), update::kUpdateRepo);
  worker_ = std::thread([this] { WorkerLoop(); });
}

void UpdateChecker::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  if (cancellable_) g_cancellable_cancel(cancellable_);
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

UpdateChecker::Snapshot UpdateChecker::Current() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

void UpdateChecker::SetHandler(Handler h) {
  std::lock_guard<std::mutex> lock(handlerMutex_);
  handler_ = std::move(h);
}

void UpdateChecker::CheckNow() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    checkRequested_ = true;
  }
  cv_.notify_all();
}

void UpdateChecker::BeginInstall() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    applyRequested_ = true;
  }
  cv_.notify_all();
}

bool UpdateChecker::AutoCheckEnabled() { return prefs::Get<bool>(kAutoCheckPrefKey, true); }

void UpdateChecker::SetAutoCheckEnabled(bool on) {
  prefs::Set(kAutoCheckPrefKey, on);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    autoCheck_ = on;
    // The user just asked for updates; answer now, not in six hours, but
    // never before GitHub said it may be asked again.
    if (on) nextAutoUnix_ = NowUnix() + SecondsUntil(holdUntil_);
  }
  // "The app keeps trying" is said only while it does.
  const std::int64_t now = NowUnix();
  Mutate([now, on](Snapshot& s) {
    s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, on && kOwnCode != 0);
  });
  cv_.notify_all();
  g_message("update: automatic checking %s", on ? "enabled" : "disabled");
}

void UpdateChecker::Mutate(const std::function<void(Snapshot&)>& fn) {
  Snapshot copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    fn(snapshot_);
    copy = snapshot_;
  }
  Publish(copy);
}

void UpdateChecker::CheckFailed() {
  const std::int64_t now = NowUnix();
  Mutate([this, now](Snapshot& s) {
    s.lastCheck = CheckOutcome::Failed;
    s.checkStale = update::CheckIsStale(now, s.lastSuccessUnix, autoCheck_ && kOwnCode != 0);
  });
}

void UpdateChecker::Publish(const Snapshot& copy) {
  Handler handler;
  {
    std::lock_guard<std::mutex> lock(handlerMutex_);
    handler = handler_;
  }
  if (!handler) return;
  PostToMain([handler, copy] { handler(copy); });
}

// ---- the worker --------------------------------------------------------------

void UpdateChecker::WorkerLoop() {
  // Every dispatch is wrapped: an exception escaping a std::thread is
  // std::terminate, so one surprise (a nlohmann type_error on API drift, a
  // bad_alloc mid-download) must not take the whole app down -- and recur on
  // the next six-hour check.
  try {
    CleanupStaleFiles();
  } catch (const std::exception& e) {
    g_warning("update: startup cleanup threw: %s", e.what());
  }

  std::unique_lock<std::mutex> lock(mutex_);
  {
    // The persisted throttle: a launch within six hours of the last completed
    // check asks nothing; one past it asks thirty seconds in.
    const std::int64_t last = prefs::Get<std::int64_t>(kLastCheckPrefKey, 0);
    const std::int64_t now = NowUnix();
    if (update::ShouldCheckNow(autoCheck_, kOwnCode, last, now)) {
      nextAutoUnix_ = now + kLaunchDelaySeconds;
    } else {
      nextAutoUnix_ = last + update::kCheckIntervalSeconds;
    }
  }
  for (;;) {
    if (stop_) break;
    if (applyRequested_) {
      applyRequested_ = false;
      lock.unlock();
      try {
        RunApply();
      } catch (const std::exception& e) {
        g_warning("update: apply threw: %s", e.what());
        Mutate([](Snapshot& s) {
          s.phase = Phase::Failed;
          s.failure = Failure::Download;
        });
      }
      lock.lock();
      continue;
    }
    // A dev build never schedules its own checks; only CheckNow lands here.
    const bool timed = autoCheck_ && kOwnCode != 0;
    if (checkRequested_ || (timed && NowUnix() >= nextAutoUnix_)) {
      checkRequested_ = false;
      lock.unlock();
      try {
        RunCheck();
      } catch (const std::exception& e) {
        g_warning("update: check threw: %s", e.what());
        CheckFailed();
      }
      // Any completed check -- manual or automatic -- restarts the cadence
      // and the persisted throttle; two checks 30 seconds apart cannot say
      // different things. Never before GitHub's own Retry-After or rate-limit
      // reset.
      const std::int64_t now = NowUnix();
      prefs::Set(kLastCheckPrefKey, now);
      lock.lock();
      nextAutoUnix_ = now + std::max(update::kCheckIntervalSeconds, SecondsUntil(holdUntil_));
      continue;
    }
    if (timed) {
      const std::int64_t wait = nextAutoUnix_ - NowUnix();
      cv_.wait_for(lock, std::chrono::seconds(wait > 0 ? wait : 0));
    } else {
      cv_.wait(lock);
    }
  }
}

void UpdateChecker::CleanupStaleFiles() {
  if (snapshot_.kind != update::InstallKind::AppImage) return;
  const std::string appimage = EnvOr("APPIMAGE");
  if (appimage.empty()) return;
  // We are running, so the image that replaced the previous one starts: the
  // .bak it kept has done its job. A half-written .part from an interrupted
  // download is never resumed.
  RemoveIfPresent(update::BackupPath(appimage), "the previous image");
  RemoveIfPresent(update::PartPath(appimage), "a partial download");
}

// ---- the check ---------------------------------------------------------------

void UpdateChecker::RunCheck() {
  bool held = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    held = Clock::now() < holdUntil_;
  }
  if (held) {
    // GitHub asked for no request before then; a manual check waits too.
    g_warning("update: GitHub asked for no request yet; the check is not sent");
    CheckFailed();
    return;
  }
  Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });

  std::string body;
  std::string error;
  FetchHeaders headers;
  const bool fetched = FetchUrl(
      update::ReleasesApiUrl(), "application/vnd.github+json", kMaxJsonBytes,
      /*followRedirects=*/false, cancellable_,
      [&body](const char* data, gsize n) {
        body.append(data, n);
        return true;
      },
      &headers, error);
  {
    // A list that came back ends a hold; a refusal that says when to ask
    // again starts one, on the steady clock.
    const update::RateLimit limit{headers.retryAfterSeconds, headers.rateLimitResetUnixSeconds,
                                  headers.rateLimitExhausted, headers.serverUnixSeconds};
    const std::int64_t wait = fetched ? 0 : update::NextCheckDelaySeconds(0, limit);
    if (wait > 0) {
      g_warning("update: GitHub asked for no request for %lld s", static_cast<long long>(wait));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    holdUntil_ = wait > 0 ? Clock::now() + std::chrono::seconds(wait) : Clock::time_point{};
    snapshot_.holdUntil = holdUntil_;
    snapshot_.holdUntilUnix = wait > 0 ? NowUnix() + wait : 0;
  }
  if (!fetched) {
    // A repository with no release yet answers an empty list, which is "no
    // update"; a 404 means the id no longer names a repository this app can
    // read, and fails like any other status.
    g_warning("update: release check failed: %s", error.c_str());
    CheckFailed();
    return;
  }
  // parse(..., false): a malformed body comes back as `discarded`, not a throw.
  const nlohmann::json releases = nlohmann::json::parse(body, nullptr, false);
  if (!releases.is_array()) {
    g_warning("update: release list was not a JSON array");
    CheckFailed();
    return;
  }
  const std::vector<update::Release> parsed = update::ParseReleases(releases);
  // GitHub was reached: the list came back.
  const std::int64_t succeeded = NowUnix();
  prefs::Set(kLastSuccessPrefKey, succeeded);

  // Codes are judged against GitHub's clock, not this machine's. A list
  // without a Date header is judged against this machine's.
  std::int64_t serverUnixSeconds = headers.serverUnixSeconds;
  if (serverUnixSeconds == 0) {
    g_warning("update: the release list had no Date header; judging by this clock");
    serverUnixSeconds = NowUnix();
  }
  const update::Selection sel = update::SelectRelease(parsed, kOwnCode, snapshot_.kind,
                                                      update::OwnArch(), serverUnixSeconds);
  for (const auto& skip : sel.skipped) {
    g_warning("update: release %s %s -- skipped", skip.tag.c_str(), skip.reason.c_str());
  }
  g_message("update: check complete -- own %s (code %llu), newest stable release %s (code %llu)",
            UR_APP_VERSION, static_cast<unsigned long long>(kOwnCode),
            sel.newestVersion.empty() ? "none" : sel.newestVersion.c_str(),
            static_cast<unsigned long long>(sel.newestCode));

  Snapshot copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.newestVersion = sel.newestVersion;
    snapshot_.newestCode = sel.newestCode;
    if (kOwnCode == 0) {
      snapshot_.lastCheck = sel.newestCode ? CheckOutcome::DevBuild : CheckOutcome::NoUpdate;
    } else if (sel.updateAvailable) {
      offer_ = Offer{sel.code,      sel.version,     sel.assetName, sel.assetUrl,
                     sel.digestHex, sel.sigAttached, sel.sumsUrl, sel.sigUrl};
      snapshot_.lastCheck = CheckOutcome::UpdateFound;
      // A different (newer) release replaces whatever the notice said about
      // an older one; the SAME release keeps its standing Ready/Downloaded/
      // Failed state -- a periodic check must not wipe the outcome of a click.
      if (snapshot_.phase == Phase::None || snapshot_.version != sel.version) {
        snapshot_.phase = Phase::Available;
        snapshot_.failure = Failure::None;
        snapshot_.version = sel.version;
        snapshot_.releasePage = sel.releasePage;
        snapshot_.assetName = sel.assetName;
        snapshot_.command = update::PackageManagerCommand(snapshot_.kind, sel.assetName);
        snapshot_.installedPath.clear();
      }
    } else {
      snapshot_.lastCheck = CheckOutcome::NoUpdate;
      // A notice for a release that stopped outranking us (it was deleted, or
      // this build updated by hand) closes; a click outcome for it is moot.
      if (snapshot_.phase != Phase::None && snapshot_.phase != Phase::Ready) {
        const update::InstallKind kind = snapshot_.kind;
        snapshot_ = Snapshot{};
        snapshot_.kind = kind;
        snapshot_.lastCheck = CheckOutcome::NoUpdate;
        snapshot_.newestVersion = sel.newestVersion;
        snapshot_.newestCode = sel.newestCode;
        offer_ = Offer{};
      }
    }
    snapshot_.lastSuccessUnix = succeeded;
    snapshot_.checkStale = false;
    copy = snapshot_;
  }
  Publish(copy);
}

// ---- the apply ---------------------------------------------------------------

void UpdateChecker::RunApply() {
  Offer offer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool actionable = snapshot_.phase == Phase::Available ||
                            snapshot_.phase == Phase::Failed ||
                            snapshot_.phase == Phase::Downloaded;
    if (!actionable || offer_.code == 0) return;
    if (snapshot_.kind != update::InstallKind::AppImage) return;  // never installs
    offer = offer_;
  }
  const std::string appimage = EnvOr("APPIMAGE");
  if (appimage.empty()) return;

  const auto fail = [this](Failure f) {
    Mutate([f](Snapshot& s) {
      s.phase = Phase::Failed;
      s.failure = f;
    });
  };
  Mutate([&offer](Snapshot& s) {
    s.phase = Phase::Downloading;
    s.failure = Failure::None;
    s.version = offer.version;
    s.installedPath.clear();
  });
  g_message("update: applying v%s (code %llu)", offer.version.c_str(),
            static_cast<unsigned long long>(offer.code));

  // ---- where the file goes ----------------------------------------------------
  // Beside $APPIMAGE when its directory can take a rename; otherwise the
  // Downloads folder, and the user finishes by hand (a root-owned
  // /usr/lib/urnetwork copy is dpkg's to replace, not ours).
  const std::string dir = DirOf(appimage);
  const bool replace = DirWritable(dir);
  const std::string finalPath = replace ? appimage : DownloadsDir() + "/" + offer.assetName;
  const std::string part = update::PartPath(finalPath);
  if (!replace) g_message("update: %s is not writable -- saving to %s", dir.c_str(), finalPath.c_str());

  // ---- (a) download -----------------------------------------------------------
  {
    ::unlink(part.c_str());
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
      g_warning("update: could not open %s for writing", part.c_str());
      fail(Failure::Download);
      return;
    }
    std::string error;
    const bool ok = FetchUrl(
        offer.assetUrl, nullptr, kMaxImageBytes, /*followRedirects=*/true, cancellable_,
        [&out](const char* data, gsize n) {
          out.write(data, static_cast<std::streamsize>(n));
          return out.good();
        },
        nullptr, error);
    out.close();
    if (!ok || !out.good()) {
      g_warning("update: download failed: %s", ok ? "file write failed" : error.c_str());
      ::unlink(part.c_str());
      fail(Failure::Download);
      return;
    }
  }

  // ---- (b) verify against the asset's digest ----------------------------------
  // The expected hash travelled inside the Offer since check time -- GitHub's
  // per-asset SHA-256 from the very JSON object whose URL was just
  // downloaded -- so verification is purely local: hash the file, compare.
  Mutate([](Snapshot& s) { s.phase = Phase::Verifying; });
  const std::string actual = Sha256File(part);
  if (offer.digestHex.empty() || actual.empty() || actual != offer.digestHex) {
    // The unverifiable download does not stay on disk.
    g_warning("update: checksum mismatch for %s -- expected '%s', got '%s'",
              offer.assetName.c_str(), offer.digestHex.c_str(), actual.c_str());
    ::unlink(part.c_str());
    fail(Failure::Checksum);
    return;
  }
  g_message("update: verified %s (%s)", offer.assetName.c_str(), actual.c_str());

  // ---- (b2) the signed SHA256SUMS (UpdateSignature.hpp) -------------------------
  // Staged: an attached SHA256SUMS.sig must verify and vouch for this file;
  // an unsigned release proceeds on the digest alone, with a warning, until
  // update::kRequireUpdateSignature is true.
  {
    const auto fetchSmall = [this](const std::string& url,
                                   std::uint64_t cap) -> std::optional<std::string> {
      if (url.empty()) return std::nullopt;
      std::string bytes;
      std::string error;
      const bool ok = FetchUrl(
          url, nullptr, cap, /*followRedirects=*/true, cancellable_,
          [&bytes](const char* data, gsize n) {
            bytes.append(data, n);
            return true;
          },
          nullptr, error);
      if (!ok) {
        g_warning("update: fetching %s failed: %s", url.c_str(), error.c_str());
        return std::nullopt;
      }
      return bytes;
    };
    std::optional<std::string> sig;
    std::optional<std::string> sums;
    if (offer.sigAttached) {
      sig = fetchSmall(offer.sigUrl, update::kMaxSigBytes);
      sums = fetchSmall(offer.sumsUrl, update::kMaxSumsBytes);
    }
    const update::SignatureCheck signature = update::CheckReleaseSignature(
        offer.sigAttached, sums, sig, offer.assetName, actual, update::kUpdateSigningPublicKey,
        update::kRequireUpdateSignature);
    if (signature.verdict == update::SignatureVerdict::Refused) {
      g_warning("update: signature check refused %s: %s", offer.assetName.c_str(),
                signature.reason.c_str());
      ::unlink(part.c_str());
      fail(Failure::Checksum);
      return;
    }
    if (signature.verdict == update::SignatureVerdict::Unsigned) {
      g_warning("update: %s -- proceeding on the asset digest alone", signature.reason.c_str());
    } else {
      g_message("update: SHA256SUMS.sig verified for %s", offer.assetName.c_str());
    }
  }

  // ---- (c) make it the image ----------------------------------------------------
  Mutate([](Snapshot& s) { s.phase = Phase::Installing; });
  if (::chmod(part.c_str(), ExecutableMode(appimage)) != 0) {
    g_warning("update: chmod %s: %s", part.c_str(), std::strerror(errno));
    ::unlink(part.c_str());
    fail(Failure::Install);
    return;
  }
  if (!replace) {
    if (::rename(part.c_str(), finalPath.c_str()) != 0) {
      g_warning("update: rename to %s: %s", finalPath.c_str(), std::strerror(errno));
      ::unlink(part.c_str());
      fail(Failure::Install);
      return;
    }
    g_message("update: saved the verified image to %s", finalPath.c_str());
    Mutate([&finalPath](Snapshot& s) {
      s.phase = Phase::Downloaded;
      s.installedPath = finalPath;
    });
    return;
  }

  // Keep the running image as .bak: a hard link leaves the path in place
  // until the single atomic rename below; a filesystem that refuses links
  // gets the two-step rename instead (a window of one syscall).
  const std::string bak = update::BackupPath(appimage);
  ::unlink(bak.c_str());
  bool movedAside = false;
  if (::link(appimage.c_str(), bak.c_str()) != 0) {
    if (::rename(appimage.c_str(), bak.c_str()) != 0) {
      g_warning("update: could not keep %s as %s: %s", appimage.c_str(), bak.c_str(),
                std::strerror(errno));
      ::unlink(part.c_str());
      fail(Failure::Install);
      return;
    }
    movedAside = true;
  }
  if (::rename(part.c_str(), appimage.c_str()) != 0) {
    g_warning("update: rename %s over %s: %s", part.c_str(), appimage.c_str(),
              std::strerror(errno));
    if (movedAside) ::rename(bak.c_str(), appimage.c_str());  // put the old image back
    ::unlink(part.c_str());
    fail(Failure::Install);
    return;
  }
  g_message("update: %s is now v%s (previous image kept as %s until the next launch)",
            appimage.c_str(), offer.version.c_str(), bak.c_str());
  Mutate([&appimage](Snapshot& s) {
    s.phase = Phase::Ready;
    s.installedPath = appimage;
  });
}

// ---- the relaunch ------------------------------------------------------------

void UpdateChecker::Relaunch() {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.phase != Phase::Ready) return;
    path = snapshot_.installedPath;
  }
  g_message("update: relaunching %s", path.c_str());

  // Nothing of this process may leak into the next: every descriptor above
  // the standard three closes on exec (the daemon's control socket, the SDK's
  // sockets, the log files).
  if (DIR* fds = ::opendir("/proc/self/fd")) {
    const int self = ::dirfd(fds);
    while (const dirent* entry = ::readdir(fds)) {
      const int fd = std::atoi(entry->d_name);
      if (fd > 2 && fd != self) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    ::closedir(fds);
  }

  update::EnvList env;
  for (char** e = environ; e && *e; ++e) {
    const char* eq = std::strchr(*e, '=');
    if (!eq) continue;
    env.emplace_back(std::string(*e, static_cast<std::size_t>(eq - *e)), std::string(eq + 1));
  }
  std::vector<std::string> scrubbed;
  for (const auto& [name, value] : update::ScrubRelaunchEnv(env, EnvOr("APPDIR"))) {
    scrubbed.push_back(name + "=" + value);
  }
  std::vector<char*> envp;
  for (auto& kv : scrubbed) envp.push_back(kv.data());
  envp.push_back(nullptr);
  char* argv[] = {path.data(), nullptr};
  ::execve(path.c_str(), argv, envp.data());

  g_warning("update: exec %s failed: %s", path.c_str(), std::strerror(errno));
  Mutate([](Snapshot& s) {
    s.phase = Phase::Failed;
    s.failure = Failure::Install;
  });
}

}  // namespace urnw
