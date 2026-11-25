// The updater exists AND something calls it.
//
// ReleaseSelectionTest proves the decisions; this proves the call sites, the
// same way TunnelPolicyTest reads Tunnel.cpp and EarningsPageTeardownTest
// reads EarningsPage.cpp: a checker that nothing starts, a button that
// reports "no UpdateChecker in this build", an AppImage whose embedded update
// info points at a host that does not resolve -- each is a shipped defect
// this file fails on. UpdateChecker needs GTK and libsoup, so the sources are
// read as text.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include "ReleaseSelection.hpp"

#include <fstream>
#include <sstream>
#include <string>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The body of the function whose definition starts with `signature`, or ""
// (braces balanced from the first '{' after the signature).
std::string FunctionBody(const std::string& source, const std::string& signature) {
  const size_t at = source.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = source.find('{', at);
  if (open == std::string::npos) return std::string();
  int depth = 0;
  for (size_t i = open; i < source.size(); ++i) {
    if (source[i] == '{') ++depth;
    if (source[i] == '}' && --depth == 0) return source.substr(open, i - open + 1);
  }
  return std::string();
}

bool Has(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

UR_TEST(theWindowBuildsBindsAndStartsTheChecker) {
  const std::string source = ReadSource("MainWindow.cpp");
  if (source.empty()) {
    UR_FAIL("could not read MainWindow.cpp to check the updater is wired");
    return;
  }
  UR_EXPECT_TRUE_MSG("MainWindow does not construct an UpdateChecker",
                     Has(source, "updates_ = std::make_unique<UpdateChecker>()"));
  UR_EXPECT_TRUE_MSG("the developer page is not handed the checker",
                     Has(source, "developerPage_->SetUpdateChecker(updates_.get())"));
  UR_EXPECT_TRUE_MSG("the settings page is not handed the checker",
                     Has(source, "settingsPage_->SetUpdateChecker(updates_.get())"));
  UR_EXPECT_TRUE_MSG("no handler replays snapshots into Settings",
                     Has(source, "settingsPage_->ApplyUpdate(snap)"));
  UR_EXPECT_TRUE_MSG("no handler replays snapshots into Developer",
                     Has(source, "developerPage_->ApplyUpdateCheck(snap)"));
  // bind-then-replay-then-start: a launch check that fires before the
  // handler is bound is a check nobody sees
  const size_t handler = source.find("updates_->SetHandler(");
  const size_t replay = source.find("settingsPage_->ApplyUpdate(updates_->Current())");
  const size_t start = source.find("updates_->Start()");
  UR_EXPECT_TRUE_MSG("the checker is never started", start != std::string::npos);
  UR_EXPECT_TRUE_MSG("the handler is bound after Start()",
                     handler != std::string::npos && handler < start);
  UR_EXPECT_TRUE_MSG("the current snapshot is not replayed before Start()",
                     replay != std::string::npos && replay < start);
}

UR_TEST(theDeveloperButtonRunsARealCheck) {
  const std::string source = ReadSource("DeveloperPage.cpp");
  if (source.empty()) {
    UR_FAIL("could not read DeveloperPage.cpp to check the button");
    return;
  }
  const std::string intro = FunctionBody(source, "void DeveloperPage::BuildIntroCard()");
  if (intro.empty()) {
    UR_FAIL("could not find DeveloperPage::BuildIntroCard");
    return;
  }
  UR_EXPECT_TRUE_MSG("Check for updates does not call UpdateChecker::CheckNow",
                     Has(intro, "updates_->CheckNow()"));
  UR_EXPECT_TRUE_MSG("the 'no UpdateChecker in this build' stub is still the whole handler",
                     !Has(intro, "no UpdateChecker in this build"));
  const std::string apply =
      FunctionBody(source, "void DeveloperPage::ApplyUpdateCheck(");
  UR_EXPECT_TRUE_MSG("ApplyUpdateCheck does not render the outcome", Has(apply, "updateCheckText_"));
  UR_EXPECT_TRUE_MSG("ApplyUpdateCheck does not name the newest release",
                     Has(apply, "snap.newestVersion"));
}

UR_TEST(settingsDrivesTheCheckerAndRendersItsStates) {
  const std::string source = ReadSource("SettingsPage.cpp");
  if (source.empty()) {
    UR_FAIL("could not read SettingsPage.cpp to check the notice");
    return;
  }
  const std::string general = FunctionBody(source, "void SettingsPage::BuildGeneralSection(");
  UR_EXPECT_TRUE_MSG("the auto-check toggle no longer reaches the checker",
                     Has(general, "updates_->SetAutoCheckEnabled(on)"));
  UR_EXPECT_TRUE_MSG("the toggle still carries the 'does not exist in this tree' TODO",
                     !Has(general, "TODO(sdk-wiring): urnw::UpdateChecker"));
  // Check now runs a real check, from a row between the startup toggle and
  // the notice, and the line under it is written before the notice's early
  // return, so "up to date" and "the check failed" reach the page too
  const size_t startup = general.find("launchAtStartup_ = AddToggleRow(");
  const size_t checkNow = general.find(
      "checkNow_ = AddButtonRow(host, T_(\"dev_check_updates\", \"Check for updates\"), {},");
  const size_t notice = general.find("updateRow_ = row.root;");
  UR_EXPECT_TRUE_MSG("Settings has no Check now row between the startup toggle and the notice",
                     startup != std::string::npos && checkNow != std::string::npos &&
                         notice != std::string::npos && startup < checkNow && checkNow < notice);
  UR_EXPECT_TRUE_MSG("Check now does not call UpdateChecker::CheckNow",
                     Has(general, "if (updates_) updates_->CheckNow();"));
  const std::string state = FunctionBody(source, "void SettingsPage::ApplyUpdateState(");
  for (const char* input :
       {"in.outcome = snap.lastCheck;", "in.newestKnown = !snap.newestVersion.empty();",
        "in.newestOutranksOwn = snap.newestCode > update::ParseReleaseCode(UR_APP_VERSION);",
        "in.stale = snap.checkStale;",
        "in.held = holdLeft > std::chrono::steady_clock::duration::zero();",
        "update::UpdateStateLineFor(in)"}) {
    UR_EXPECT_TRUE_MSG(std::string("the state line does not read ") + input,
                       Has(state, input));
  }
  // the checker publishes the newest release's code beside its version, so
  // "up to date" is not said of a newer release this install is not offered
  const std::string check =
      FunctionBody(ReadSource("UpdateChecker.cpp"), "void UpdateChecker::RunCheck()");
  const size_t newest = check.find("snapshot_.newestCode = sel.newestCode;");
  UR_EXPECT_TRUE_MSG("RunCheck does not publish the newest release's code",
                     newest != std::string::npos &&
                         check.find("snapshot_.newestCode = sel.newestCode;", newest + 1) !=
                             std::string::npos);
  UR_EXPECT_TRUE_MSG("the stale line does not name the last success's date",
                     Has(state, "UpdateChecker::LocalDate(snap.lastSuccessUnix)"));
  UR_EXPECT_TRUE_MSG("the held line does not name when GitHub may be asked again",
                     Has(state, "UpdateChecker::LocalDateTime(snap.holdUntilUnix)"));
  UR_EXPECT_TRUE_MSG("the stale line's second sentence is not looked up",
                     Has(state, "T_(line.detailKey, line.detailEnglish)"));
  // whether the hold lasts is read off the steady clock the checker keeps it
  // on, never off the time the line names, so a system clock set back does
  // not keep Check now held
  UR_EXPECT_TRUE_MSG("the checker does not publish its steady hold beside the time it names",
                     Has(check, "snapshot_.holdUntil = holdUntil_;") &&
                         Has(check, "snapshot_.holdUntilUnix = wait > 0 ? NowUnix() + wait : 0;"));
  UR_EXPECT_TRUE_MSG("Settings reads the hold off the wall clock",
                     Has(state, "snap.holdUntil - std::chrono::steady_clock::now()") &&
                         !Has(state, "snap.holdUntilUnix >") && !Has(state, "snap.holdUntilUnix -"));
  // the hold ends with no snapshot: the line is read again then, and the
  // timer dies with the page
  UR_EXPECT_TRUE_MSG("the line is not read again when GitHub's hold ends",
                     Has(state, "holdEnd_ = Glib::signal_timeout().connect_seconds(") &&
                         Has(state, "ApplyUpdateState(updateSnapshot_);\n          return false;"));
  UR_EXPECT_TRUE_MSG("the hold's timer outlives the page",
                     Has(FunctionBody(source, "SettingsPage::~SettingsPage()"),
                         "holdEnd_.disconnect();"));
  UR_EXPECT_TRUE_MSG("the state line's key is not looked up",
                     Has(state, "T_(line.textKey, line.textEnglish)"));
  UR_EXPECT_TRUE_MSG("Check now is not held while a check is in flight",
                     Has(state, "checkNow_->set_sensitive(updates_ != nullptr && line.canCheck);"));
  const std::string button = FunctionBody(source, "void SettingsPage::OnUpdateButton()");
  UR_EXPECT_TRUE_MSG("Install does not begin the apply", Has(button, "updates_->BeginInstall()"));
  UR_EXPECT_TRUE_MSG("Relaunch does not relaunch", Has(button, "updates_->Relaunch()"));
  UR_EXPECT_TRUE_MSG("the packaged installs are not sent to the release page",
                     Has(button, "snap.releasePage"));
  // the install path is the AppImage's alone
  UR_EXPECT_TRUE_MSG("BeginInstall is not gated on the AppImage kind",
                     Has(button, "snap.kind == update::InstallKind::AppImage && updates_"));
  const std::string apply = FunctionBody(source, "void SettingsPage::ApplyUpdate(");
  for (const char* phase : {"Phase::Available", "Phase::Downloading", "Phase::Verifying",
                            "Phase::Installing", "Phase::Ready", "Phase::Downloaded",
                            "Phase::Failed"}) {
    UR_EXPECT_TRUE_MSG(std::string("ApplyUpdate does not render ") + phase, Has(apply, phase));
  }
  UR_EXPECT_TRUE_MSG("the package-manager command is not shown", Has(apply, "snap.command"));
  const size_t written = apply.find("ApplyUpdateState(snap);");
  const size_t none = apply.find("if (snap.phase == Phase::None) {");
  UR_EXPECT_TRUE_MSG("the check's outcome is not written before the notice's early return",
                     written != std::string::npos && none != std::string::npos && written < none);
}

UR_TEST(theCheckerVerifiesBeforeItSwapsAndReadsOnlyTheOfficialList) {
  const std::string source = ReadSource("UpdateChecker.cpp");
  if (source.empty()) {
    UR_FAIL("could not read UpdateChecker.cpp");
    return;
  }
  // the URL comes from the one constant, never a second literal
  UR_EXPECT_TRUE_MSG("the check does not use update::ReleasesApiUrl()",
                     Has(source, "update::ReleasesApiUrl()"));
  UR_EXPECT_TRUE_MSG("UpdateChecker.cpp names a GitHub host of its own",
                     !Has(source, "api.github.com") && !Has(source, "github.com/"));
  UR_EXPECT_TRUE_MSG("UpdateChecker.cpp names the nightly repo", !Has(source, "urnetwork/build"));
  // the list is asked for by the repository id, and a redirect fails it; the
  // AppImage download follows the storage host's
  const std::string fetch = FunctionBody(source, "bool FetchUrl(");
  UR_EXPECT_TRUE_MSG("FetchUrl does not refuse redirects when asked to",
                     Has(fetch, "if (!followRedirects) soup_message_add_flags(msg, "
                                "SOUP_MESSAGE_NO_REDIRECT)"));
  const std::string check = FunctionBody(source, "void UpdateChecker::RunCheck()");
  UR_EXPECT_TRUE_MSG("the release list request follows redirects",
                     Has(check, "kMaxJsonBytes,\n      /*followRedirects=*/false"));
  // an empty stable repo answers an empty list, which is "no update"; a 404
  // from a list asked for by id means the id no longer resolves, and fails
  // the check like any other status
  UR_EXPECT_TRUE_MSG("a 404 from the release list reads as 'no releases yet'",
                     !Has(check, "== 404") && !Has(check, "404 ==") &&
                         Has(check, "if (!fetched) {"));
  // verify, THEN swap -- and a mismatch unlinks the download
  const std::string apply = FunctionBody(source, "void UpdateChecker::RunApply()");
  if (apply.empty()) {
    UR_FAIL("could not find UpdateChecker::RunApply");
    return;
  }
  const size_t verify = apply.find("actual != offer.digestHex");
  const size_t swap = apply.find("::rename(part.c_str(), appimage.c_str())");
  const size_t save = apply.find("::rename(part.c_str(), finalPath.c_str())");
  UR_EXPECT_TRUE_MSG("the apply never compares the digest", verify != std::string::npos);
  UR_EXPECT_TRUE_MSG("the apply never swaps the image", swap != std::string::npos);
  UR_EXPECT_TRUE_MSG("the digest is compared AFTER the swap", verify < swap);
  UR_EXPECT_TRUE_MSG("the digest is compared AFTER the save", verify < save);
  // the signed SHA256SUMS is judged after the digest, before either rename,
  // and a refusal unlinks the download
  const size_t signature = apply.find("update::CheckReleaseSignature(");
  const size_t refused = apply.find("signature.verdict == update::SignatureVerdict::Refused");
  UR_EXPECT_TRUE_MSG("the apply never checks the signed SHA256SUMS",
                     signature != std::string::npos && refused != std::string::npos);
  UR_EXPECT_TRUE_MSG("the signature is checked before the digest", verify < signature);
  UR_EXPECT_TRUE_MSG("the signature is checked AFTER the swap", signature < swap);
  UR_EXPECT_TRUE_MSG("the signature is checked AFTER the save", signature < save);
  UR_EXPECT_TRUE_MSG("the release key or the staged flag is not what the apply uses",
                     Has(apply, "update::kUpdateSigningPublicKey") &&
                         Has(apply, "update::kRequireUpdateSignature"));
  UR_EXPECT_TRUE_MSG("the AppImage download does not follow the storage host's redirect",
                     Has(apply, "kMaxImageBytes, /*followRedirects=*/true"));
  UR_EXPECT_TRUE_MSG("the apply never runs a package manager or elevates",
                     !Has(apply, "sudo") && !Has(apply, "pkexec") && !Has(apply, "g_spawn"));
  // the running image is kept
  UR_EXPECT_TRUE_MSG("the running image is not kept as .bak",
                     Has(apply, "update::BackupPath(appimage)"));
  // the next launch is what removes it
  const std::string cleanup = FunctionBody(source, "void UpdateChecker::CleanupStaleFiles()");
  UR_EXPECT_TRUE_MSG("the next launch does not remove the .bak",
                     Has(cleanup, "update::BackupPath(appimage)"));
}

UR_TEST(theCheckJudgesCodesByTheReleaseListsDate) {
  const std::string source = ReadSource("UpdateChecker.cpp");
  const std::string fetch = FunctionBody(source, "bool FetchUrl(");
  UR_EXPECT_TRUE_MSG("FetchUrl does not read the response's Date header",
                     Has(fetch, "headers->serverUnixSeconds = ResponseDateUnixSeconds(msg)"));
  UR_EXPECT_TRUE_MSG("the Date header is not parsed as an HTTP date",
                     Has(FunctionBody(source, "std::int64_t ResponseDateUnixSeconds("),
                         "soup_date_time_new_from_http_string(date)"));
  const std::string check = FunctionBody(source, "void UpdateChecker::RunCheck()");
  const size_t date = check.find("std::int64_t serverUnixSeconds = headers.serverUnixSeconds;");
  const size_t fallback = check.find("serverUnixSeconds = NowUnix();");
  const size_t select = check.find("update::OwnArch(), serverUnixSeconds)");
  UR_EXPECT_TRUE_MSG("RunCheck does not take the server's date from the list's headers",
                     date != std::string::npos);
  UR_EXPECT_TRUE_MSG("a list without a Date header has no fallback to this clock",
                     fallback != std::string::npos && date < fallback);
  UR_EXPECT_TRUE_MSG("SelectRelease is not judged against the server's date",
                     select != std::string::npos && fallback < select);
}

UR_TEST(noCheckIsSentWhileGitHubAsksTheNetworkToWait) {
  const std::string source = ReadSource("UpdateChecker.cpp");
  // a refused list's Retry-After and rate-limit reset are read off the
  // response, digits only
  const std::string fetch = FunctionBody(source, "bool FetchUrl(");
  for (const char* header : {"\"Retry-After\"", "\"X-RateLimit-Reset\"",
                             "\"X-RateLimit-Remaining\"), -1) == 0"}) {
    UR_EXPECT_TRUE_MSG(std::string("FetchUrl does not read ") + header,
                       Has(fetch, header));
  }
  UR_EXPECT_TRUE_MSG("the headers are not read as decimal counts",
                     Has(fetch, "update::ParseDecimalHeader("));
  // the check returns before any request while the hold lasts, and a refusal
  // that says when to ask again starts one, capped by the pure schedule
  const std::string check = FunctionBody(source, "void UpdateChecker::RunCheck()");
  const size_t held = check.find("held = Clock::now() < holdUntil_;");
  const size_t heldReturn = check.find("return;", held);
  const size_t fetchCall = check.find("FetchUrl(");
  UR_EXPECT_TRUE_MSG("RunCheck does not test the hold",
                     held != std::string::npos);
  UR_EXPECT_TRUE_MSG("RunCheck sends the request while GitHub holds it",
                     held != std::string::npos && heldReturn < fetchCall);
  const size_t delay = check.find("update::NextCheckDelaySeconds(0, limit)");
  const size_t start = check.find(
      "holdUntil_ = wait > 0 ? Clock::now() + std::chrono::seconds(wait) : Clock::time_point{};");
  UR_EXPECT_TRUE_MSG("a refusal's headers do not set the hold",
                     delay != std::string::npos && start != std::string::npos &&
                         fetchCall < delay && delay < start);
  UR_EXPECT_TRUE_MSG("a list that came back does not end the hold",
                     Has(check, "const std::int64_t wait = fetched ? 0 : "));
  // the hold is kept on the steady clock, so setting the system clock back
  // cannot stretch it past the day the schedule allows
  UR_EXPECT_TRUE_MSG("the hold is kept on the wall clock",
                     Has(ReadSource("UpdateChecker.hpp"),
                         "std::chrono::steady_clock::time_point holdUntil_{};") &&
                         !Has(source, "holdUntilUnix_"));
  // and nothing schedules a check before it
  const std::string loop = FunctionBody(source, "void UpdateChecker::WorkerLoop()");
  UR_EXPECT_TRUE_MSG("the cadence schedules a check before the hold ends",
                     Has(loop, "nextAutoUnix_ = now + std::max(update::kCheckIntervalSeconds, "
                               "SecondsUntil(holdUntil_));"));
  const std::string autoCheck = FunctionBody(source, "void UpdateChecker::SetAutoCheckEnabled(");
  UR_EXPECT_TRUE_MSG("turning automatic checks on schedules a check before the hold ends",
                     Has(autoCheck, "nextAutoUnix_ = NowUnix() + SecondsUntil(holdUntil_);"));
}

UR_TEST(aCheckThatHasNotReachedGitHubForThreeDaysIsSaid) {
  const std::string source = ReadSource("UpdateChecker.cpp");
  // the first launch that tries seeds the baseline, before the worker runs
  const std::string start = FunctionBody(source, "void UpdateChecker::Start()");
  const size_t read = start.find("prefs::Get<std::int64_t>(kLastSuccessPrefKey, 0)");
  const size_t seed = start.find("prefs::Set(kLastSuccessPrefKey, snapshot_.lastSuccessUnix);");
  const size_t worker = start.find("worker_ = std::thread(");
  UR_EXPECT_TRUE_MSG("Start does not seed update_last_check_success before the worker",
                     read != std::string::npos && seed != std::string::npos && read < seed &&
                         seed < worker);
  UR_EXPECT_TRUE_MSG("the success baseline is not its own pref",
                     Has(source, "kLastSuccessPrefKey = \"update_last_check_success\";"));
  // only a check that reached GitHub writes it, and clears the warning;
  // every check still restarts the throttle
  const std::string check = FunctionBody(source, "void UpdateChecker::RunCheck()");
  const size_t failed = check.find("g_warning(\"update: release check failed: %s\"");
  const std::string persist = "prefs::Set(kLastSuccessPrefKey, succeeded);";
  const size_t succeeded = check.find(persist);
  UR_EXPECT_TRUE_MSG("the success is persisted before the fetch's failure path returns",
                     failed != std::string::npos && succeeded != std::string::npos &&
                         failed < succeeded);
  // only once the list came back: a 404 or any other status returns first
  const size_t notFetched = check.find("if (!fetched) {");
  const size_t notFetchedReturn = check.find("return;", notFetched);
  UR_EXPECT_TRUE_MSG("the success is persisted on a path where the list did not come back",
                     notFetched != std::string::npos && notFetched < failed &&
                         notFetchedReturn < succeeded);
  UR_EXPECT_TRUE_MSG("a success does not clear the warning",
                     Has(check, "snapshot_.lastSuccessUnix = succeeded;\n"
                                "    snapshot_.checkStale = false;"));
  UR_EXPECT_TRUE_MSG("RunCheck writes the success baseline twice",
                     succeeded == std::string::npos ||
                         check.find("kLastSuccessPrefKey", succeeded + persist.size()) ==
                             std::string::npos);
  UR_EXPECT_TRUE_MSG("the worker no longer restarts the throttle after every check",
                     Has(FunctionBody(source, "void UpdateChecker::WorkerLoop()"),
                         "prefs::Set(kLastCheckPrefKey, now);"));
  // every failure goes through CheckFailed, which asks the pure rule
  UR_EXPECT_TRUE_MSG("a failure path sets Failed without the stale rule",
                     !Has(check, "s.lastCheck = CheckOutcome::Failed"));
  size_t calls = 0;
  for (size_t at = check.find("CheckFailed();"); at != std::string::npos;
       at = check.find("CheckFailed();", at + 1)) {
    ++calls;
  }
  UR_EXPECT_EQ(size_t{3}, calls);  // held, not fetched, not a JSON array
  const std::string failure = FunctionBody(source, "void UpdateChecker::CheckFailed()");
  UR_EXPECT_TRUE_MSG("CheckFailed does not ask CheckIsStale with automatic checks",
                     Has(failure, "update::CheckIsStale(now, s.lastSuccessUnix, autoCheck_ && "
                                  "kOwnCode != 0)"));
  // turning automatic checks off takes the warning down, on puts it back
  UR_EXPECT_TRUE_MSG("SetAutoCheckEnabled does not recompute the warning",
                     Has(FunctionBody(source, "void UpdateChecker::SetAutoCheckEnabled("),
                         "update::CheckIsStale(now, s.lastSuccessUnix, on && kOwnCode != 0)"));
  // and the developer line says it too
  const std::string developer =
      FunctionBody(ReadSource("DeveloperPage.cpp"), "void DeveloperPage::ApplyUpdateCheck(");
  UR_EXPECT_TRUE_MSG("the developer line does not say since when checks have failed",
                     Has(developer, "if (snap.checkStale) {") &&
                         Has(developer, "UpdateChecker::LocalDate(snap.lastSuccessUnix)"));
}

UR_TEST(theTarballUpdateAsksTheSameRepositoryIdWithoutRedirects) {
  // install.sh --update is the daemon tarball's half of the same rule: the
  // API by kUpdateRepoId, its answer never followed elsewhere
  const std::string script = ReadSource("../../packaging/tarball/install.sh");
  if (script.empty()) {
    UR_FAIL("could not read packaging/tarball/install.sh");
    return;
  }
  const std::string id = std::to_string(urnw::update::kUpdateRepoId);
  UR_EXPECT_TRUE_MSG("install.sh does not pin the repository id ReleaseSelection.hpp pins",
                     Has(script, ("UPDATE_REPO_ID='" + id + "'").c_str()));
  UR_EXPECT_TRUE_MSG("install.sh does not ask the API by the repository id",
                     Has(script, "UPDATE_API_URL=\"https://api.github.com/repositories/"
                                 "${UPDATE_REPO_ID}/releases/latest\""));
  const size_t api = script.find("-o \"${UPDATE_JSON}\"");
  const size_t call = script.rfind("curl ", api);
  UR_EXPECT_TRUE_MSG("install.sh's API request is not found", api != std::string::npos &&
                                                               call != std::string::npos);
  if (api != std::string::npos && call != std::string::npos) {
    UR_EXPECT_TRUE_MSG("install.sh's API request follows redirects (-L)",
                       script.compare(call, 9, "curl -sS ") == 0);
  }
}

UR_TEST(theAppImageCarriesNoZsyncUpdateInformation) {
  // GitHub Releases answers zsync's multi-range requests with HTTP 501 and
  // the old embedded host (get.ur.network) does not resolve, so update info
  // that names either is a channel that cannot work. Updates come from the
  // in-app checker.
  const std::string script = ReadSource("../../packaging/make-appimage.sh");
  if (script.empty()) {
    UR_FAIL("could not read packaging/make-appimage.sh");
    return;
  }
  UR_EXPECT_TRUE_MSG("make-appimage.sh still passes --updateinformation",
                     !Has(script, "--updateinformation"));
  UR_EXPECT_TRUE_MSG("make-appimage.sh still names get.ur.network",
                     !Has(script, "get.ur.network"));
  UR_EXPECT_TRUE_MSG("make-appimage.sh still produces or expects a .zsync",
                     !Has(script, ".zsync"));
  UR_EXPECT_TRUE_MSG("make-appimage.sh does not say where updates come from",
                     Has(script, "UpdateChecker"));
}
