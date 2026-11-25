// UpdateChecker -- the in-app updater (windows App/UpdateChecker.{h,cpp}
// parity, the Linux half).
//
// ONE worker thread polls the official release list (ReleaseSelection.hpp
// names the repo; nothing else does), decides with the pure SelectRelease,
// and publishes a Snapshot to ONE handler on the GTK main loop. Checks run
// thirty seconds after launch when the persisted six-hour throttle says so,
// every six hours while the app lives, and on Settings' Check now and the
// developer screen's button. The "check automatically" preference (Settings)
// gates the timed checks only; a manual check always runs and reports. No
// check, manual or timed, is sent while GitHub has asked this network to wait
// (UpdateSchedule.hpp).
//
// WHAT AN APPLY DOES depends on how the GUI is installed (DetectInstallKind):
//
//   AppImage   download the own-arch AppImage to a dot-file BESIDE $APPIMAGE,
//              verify its SHA-256 against the release asset's digest, chmod
//              +x, keep the running image as <APPIMAGE>.bak and rename the
//              new file over $APPIMAGE (atomic: same directory), then wait for
//              the user's Relaunch click, which execs the new file. The .bak
//              is removed by the NEXT launch -- proof that the new image
//              starts. When $APPIMAGE's directory is not writable (the
//              package-managed /usr/lib/urnetwork copy, a root-owned ~/Apps)
//              the verified file lands in the Downloads folder instead and
//              the notice names it.
//   everything else
//              no download at all: the notice names the release page, the
//              asset for this install and the package-manager command. The
//              app never elevates and never runs a package manager.
//
// NOTHING THAT FAILS ITS DIGEST IS EVER INSTALLED OR KEPT ON DISK.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <gio/gio.h>

#include "ReleaseSelection.hpp"
#include "UpdateSchedule.hpp"

namespace urnw {

class UpdateChecker {
 public:
  enum class Phase {
    None,         // nothing offered
    Available,    // a newer release is offered; the apply has not started
    Downloading,  // AppImage apply in flight (stage 1 of 3)
    Verifying,    // stage 2
    Installing,   // stage 3: the swap
    Ready,        // the new AppImage is in place: Relaunch is the next click
    Downloaded,   // verified, but $APPIMAGE could not be replaced: see installedPath
    Failed,       // see failure
  };
  enum class Failure { None, Download, Checksum, Install };
  // GTK-free, so Settings' line under Check for updates is decided in a
  // header the tests build (UpdateSchedule.hpp, UpdateStatePresentation.hpp).
  using CheckOutcome = update::CheckOutcome;

  struct Snapshot {
    Phase phase = Phase::None;
    Failure failure = Failure::None;
    CheckOutcome lastCheck = CheckOutcome::NeverRan;
    update::InstallKind kind = update::InstallKind::Unknown;
    // The offered release (v-less), empty when phase == None.
    std::string version;
    std::string releasePage;
    std::string assetName;  // the file for THIS install
    std::string command;    // PackageManagerCommand, empty for the AppImage
    // Ready: the path that was replaced ($APPIMAGE); Downloaded: where the
    // verified file was saved instead.
    std::string installedPath;
    // The newest stable release the last completed check parsed, whether or
    // not it outranks this build -- the developer line names it either way.
    std::string newestVersion;
    // Its release code, 0 when none: Settings says "up to date" only when it
    // does not outrank this build.
    std::uint64_t newestCode = 0;
    // Unix seconds of the last check that reached GitHub (the release list
    // came back), persisted; before any has, the first launch that tried.
    std::int64_t lastSuccessUnix = 0;
    // Automatic checks are on and none has reached GitHub for more than 72
    // hours (update::CheckIsStale): Settings says since when.
    bool checkStale = false;
    // When GitHub asked for no request before: holdUntilUnix in Unix
    // seconds, the time Settings names (0 when none), and holdUntil the same
    // moment on the steady clock, which says whether it has come, so a
    // system clock set back cannot stretch it (its epoch when none).
    std::int64_t holdUntilUnix = 0;
    std::chrono::steady_clock::time_point holdUntil{};
  };

  // Invoked ON THE GTK MAIN LOOP (PostToMain), never with a lock held.
  using Handler = std::function<void(const Snapshot&)>;

  UpdateChecker();
  ~UpdateChecker();
  UpdateChecker(const UpdateChecker&) = delete;
  UpdateChecker& operator=(const UpdateChecker&) = delete;

  // Spawn the worker: startup hygiene first (the previous image's .bak and
  // any half-written .part beside $APPIMAGE), then the throttled launch check
  // and the six-hour cadence.
  void Start();
  // Signal and JOIN the worker. A download in flight notices within one read.
  void Stop();

  Snapshot Current();
  // Store only -- never invokes. Bind, then replay Current() yourself.
  void SetHandler(Handler h);

  // Queue a check now (Settings' Check now, the developer screen's button).
  // Coalesces.
  void CheckNow();
  // Queue the download/verify/swap of the offered AppImage. Ignored unless
  // the snapshot is Available, Failed or Downloaded (a retry), or when the
  // install kind cannot replace itself.
  void BeginInstall();
  // UI thread. Exec the new image at installedPath with the AppImage runtime's
  // environment scrubbed (ReleaseSelection.hpp ScrubRelaunchEnv). Returns
  // only on failure, which is also published as Failed/Install.
  void Relaunch();

  // The "Check for updates automatically" preference (Settings): app_prefs
  // key "check_updates_automatically", default ON. Turning it ON schedules a
  // check right away -- the user just asked for updates.
  static bool AutoCheckEnabled();
  void SetAutoCheckEnabled(bool on);

  // The runtime probe behind update::DetectInstallKind. Public so the
  // Settings notice can be built before the first check.
  static update::InstallKind DetectInstallKind();

  // A Unix second in this machine's zone: the locale's short date, and that
  // date with the time, for the stale and held lines.
  static std::string LocalDate(std::int64_t unixSeconds);
  static std::string LocalDateTime(std::int64_t unixSeconds);

 private:
  // Everything an apply needs, captured at check time so a release list that
  // changes mid-flight cannot redirect an apply the user already clicked.
  struct Offer {
    std::uint64_t code = 0;
    std::string version;
    std::string assetName;
    std::string assetUrl;
    std::string digestHex;
    bool sigAttached = false;
    std::string sumsUrl;
    std::string sigUrl;
  };

  void WorkerLoop();
  void RunCheck();
  void RunApply();
  void CleanupStaleFiles();
  // A check that did not reach GitHub: Failed, and stale once none has for
  // three days.
  void CheckFailed();
  // Copy under the lock, mutate, publish outside it.
  void Mutate(const std::function<void(Snapshot&)>& fn);
  void Publish(const Snapshot& copy);

  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  // Cancelled by Stop(): a fetch in flight returns within one read instead
  // of finishing a 100 MB download the app is quitting under.
  GCancellable* cancellable_ = nullptr;
  bool stop_ = false;
  bool checkRequested_ = false;
  bool applyRequested_ = false;
  bool autoCheck_ = true;
  std::int64_t nextAutoUnix_ = 0;  // unix seconds; seeded from the persisted throttle
  // When GitHub said it may be asked again (a refused list's Retry-After or
  // rate-limit reset, at most a day out), on the steady clock, so a system
  // clock set back cannot stretch it; the clock's epoch when there is no
  // hold. No check is sent before it, manual or automatic.
  std::chrono::steady_clock::time_point holdUntil_{};
  Snapshot snapshot_;
  Offer offer_;

  std::mutex handlerMutex_;
  Handler handler_;
};

}  // namespace urnw
