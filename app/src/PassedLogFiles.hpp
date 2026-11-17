// PassedLogFiles — the GUI's own log files in "send feedback with logs". The GUI
// and urnetworkd are separate processes, each with its own glog directory, and
// the server keeps one zip per feedback, which the daemon sends
// (LogUpload.hpp). So the GUI's files ride in the daemon's zip, under gui/
// (the sdk's DeviceLocal.UploadLogsWithFiles).
//
// They travel as open descriptors, never as paths. The daemon is root, may not
// read a user's home at all (urnetworkd.service: ProtectHome=yes), and answers
// every local uid, so it must never open a path a client names. The GUI opens
// its newest glog files with its own rights and passes the descriptors with
// the upload_logs frame (ControlProtocol.hpp gui_log_files, FdPassing.hpp).
// The daemon takes only regular files under glog names, the sdk reads them
// only through duplicates of the descriptors before its call returns, and the
// daemon closes them then. Nothing goes anywhere the upload did not go before:
// the same zip, to the same POST, within the same cap.
//
// Plain POSIX and the standard library, so it is unit-tested on any host
// (PassedLogFilesTest.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace urnw::logupload {

// The most files the GUI passes, and the daemon takes, with one request. A glog
// directory holds a handful (retention keeps 4 at a start, and a long run adds
// one each time a 16 MiB file fills); the sdk's cap decides which fit.
inline constexpr size_t kMaxGuiLogFiles = 16;

// The GUI's folder in the upload's zip.
inline constexpr const char* kGuiLogFilesSource = "gui";

// A name glog writes (<program>.<host>.<user>.log.<SEVERITY>.<time>.<pid>) that
// can name one zip entry: at most 255 bytes, no leading dot, no path separator,
// no colon and no control character. The sdk applies the same test
// (isUploadLogsFileName); both halves apply it here so that neither hands over
// or keeps a descriptor the upload would leave out.
inline bool LooksLikeGlogFileName(std::string_view name) {
  if (name.empty() || name.size() > 255 || name.front() == '.') return false;
  for (const char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f || c == '/' || c == '\\' || c == ':') return false;
  }
  constexpr std::array<std::string_view, 4> kSeverities = {".log.INFO", ".log.WARNING",
                                                           ".log.ERROR", ".log.FATAL"};
  return std::any_of(kSeverities.begin(), kSeverities.end(), [name](std::string_view severity) {
    return name.find(severity) != std::string_view::npos;
  });
}

// The GUI's Go crash file, which the sdk writes beside its glog files when the
// GUI dies of a Go fatal (sdk crash_output.go). It is not a glog name, and the
// sdk's upload takes it as it takes them (sdk log_special_files.go, which masks
// it), so without it a crash of the GUI never reached support.
inline constexpr const char* kGuiGoCrashFileName = "go-crash.txt";

// A name the GUI hands over and the daemon keeps: a glog name, or the GUI's Go
// crash file.
inline bool LooksLikeGuiLogFileName(std::string_view name) {
  return LooksLikeGlogFileName(name) || name == kGuiGoCrashFileName;
}

// Log files open by descriptor, each under its glog name, owned: closed when
// this is destroyed or CloseAll runs. Move-only, so a descriptor has one owner
// from the GUI's open (or the daemon's receive) to its close.
class PassedLogFiles {
 public:
  struct File {
    std::string name;
    int fd = -1;
  };

  PassedLogFiles() = default;
  ~PassedLogFiles() { CloseAll(); }

  PassedLogFiles(const PassedLogFiles&) = delete;
  PassedLogFiles& operator=(const PassedLogFiles&) = delete;

  PassedLogFiles(PassedLogFiles&& other) noexcept : files_(std::exchange(other.files_, {})) {}
  PassedLogFiles& operator=(PassedLogFiles&& other) noexcept {
    if (this != &other) {
      CloseAll();
      files_ = std::exchange(other.files_, {});
    }
    return *this;
  }

  // Takes `fd`, under `name`.
  void Add(std::string name, int fd) { files_.push_back(File{std::move(name), fd}); }

  const std::vector<File>& Files() const { return files_; }
  size_t Size() const { return files_.size(); }

  std::vector<std::string> Names() const {
    std::vector<std::string> names;
    names.reserve(files_.size());
    for (const File& file : files_) names.push_back(file.name);
    return names;
  }

  std::vector<int> Fds() const {
    std::vector<int> fds;
    fds.reserve(files_.size());
    for (const File& file : files_) fds.push_back(file.fd);
    return fds;
  }

  void CloseAll() {
    for (const File& file : files_) {
      if (file.fd >= 0) ::close(file.fd);
    }
    files_.clear();
  }

 private:
  std::vector<File> files_;
};

// The daemon's half: pairs the descriptors that came with an upload_logs frame
// (`queuedFds`, in the order they came) with the names the request gave them,
// and takes them off the queue. A pair whose name is not one the GUI hands over
// (LooksLikeGuiLogFileName), or whose
// descriptor is not a regular file (a directory, a pipe, a socket, a device),
// is closed and left out, as is everything past kMaxGuiLogFiles. Descriptors
// with no name stay queued; the server closes them once no frame is pending.
inline PassedLogFiles TakePassedLogFiles(std::vector<int>& queuedFds,
                                         const std::vector<std::string>& names) {
  PassedLogFiles files;
  const size_t count = std::min({names.size(), queuedFds.size(), kMaxGuiLogFiles});
  for (size_t i = 0; i < count; ++i) {
    const int fd = queuedFds[i];
    struct stat fileStat{};
    if (!LooksLikeGuiLogFileName(names[i]) || ::fstat(fd, &fileStat) != 0 ||
        !S_ISREG(fileStat.st_mode)) {
      ::close(fd);
      continue;
    }
    files.Add(names[i], fd);
  }
  queuedFds.erase(queuedFds.begin(), queuedFds.begin() + static_cast<std::ptrdiff_t>(count));
  return files;
}

}  // namespace urnw::logupload
