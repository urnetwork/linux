// The GUI's own log files in "send feedback with logs" (PassedLogFiles.hpp,
// FdPassing.hpp). The GUI and urnetworkd log into separate directories, the
// daemon's zip is the one upload a feedback keeps, and the daemon may not read
// the user's home or open a path a client names. So the GUI's files reach the
// daemon as descriptors the GUI opened, over the control socket, and the
// daemon keeps only regular files under glog names. These run the real
// socket calls over a socketpair and real files in a temporary directory.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include "FdPassing.hpp"
#include "PassedLogFiles.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// A temporary directory, removed with what the test wrote into it.
class TestingDir {
 public:
  TestingDir() {
    char pattern[] = "/tmp/urnetwork-passed-log-files-XXXXXX";
    const char* made = ::mkdtemp(pattern);
    if (made != nullptr) path_ = made;
  }
  ~TestingDir() {
    for (const std::string& file : files_) ::unlink(file.c_str());
    if (!path_.empty()) ::rmdir(path_.c_str());
  }
  const std::string& Path() const { return path_; }
  // Writes `content` to `name` in the directory and returns its path.
  std::string Write(const std::string& name, const std::string& content) {
    const std::string file = path_ + "/" + name;
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
      const ssize_t written = ::write(fd, content.data(), content.size());
      (void)written;
      ::close(fd);
    }
    files_.push_back(file);
    return file;
  }

 private:
  std::string path_;
  std::vector<std::string> files_;
};

int OpenForReading(const std::string& path) { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); }

bool IsOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// The whole of a file read through `fd` from its start, as the sdk reads it.
std::string ReadFromStart(int fd) {
  std::string content;
  char buffer[256];
  off_t offset = 0;
  for (;;) {
    const ssize_t n = ::pread(fd, buffer, sizeof(buffer), offset);
    if (n <= 0) break;
    content.append(buffer, static_cast<size_t>(n));
    offset += n;
  }
  return content;
}

}  // namespace

// The names the daemon keeps, and the GUI hands over, are the names the sdk's
// upload takes (isUploadLogsFileName): glog's, and nothing that is a path.
UR_TEST(PassedLogFiles_OnlyGlogNamesAreLogFileNames) {
  for (const char* name : {"urnetwork-gui.host.user.log.INFO.20260901-000000.101",
                           "urnetwork-gui.host.user.log.WARNING.20260901-000000.101",
                           "urnetwork-gui.host.user.log.ERROR.20260901-000000.101",
                           "urnetwork-gui.host.user.log.FATAL.20260901-000000.101"}) {
    UR_EXPECT_TRUE_MSG(name, urnw::logupload::LooksLikeGlogFileName(name));
  }
  for (const char* name : {"", "jwt", "settings.json", "urnetwork-gui.INFO",
                           ".hidden.log.INFO.1", "../jwt.log.INFO.1", "a/b.log.INFO.1",
                           "a\\b.log.INFO.1", "C:a.log.INFO.1", "a\nb.log.INFO.1"}) {
    UR_EXPECT_TRUE_MSG(name, !urnw::logupload::LooksLikeGlogFileName(name));
  }
  UR_EXPECT_FALSE(urnw::logupload::LooksLikeGlogFileName(std::string(250, 'a') + ".log.INFO.1"));
}

// Besides its glog files the GUI hands over its Go crash file, which the sdk
// writes when the GUI dies of a Go fatal and its upload takes as it takes them
// (sdk log_special_files.go); nothing else.
UR_TEST(PassedLogFiles_TheGuisGoCrashFileIsALogFile) {
  for (const char* name : {"urnetwork-gui.host.user.log.INFO.20260901-000000.101", "go-crash.txt"}) {
    UR_EXPECT_TRUE_MSG(name, urnw::logupload::LooksLikeGuiLogFileName(name));
  }
  for (const char* name : {"", "jwt", "settings.json", "go-crash.txt.tmp", ".go-crash.txt",
                           "a/go-crash.txt", "go-crash.log", "urnetwork-gui.INFO"}) {
    UR_EXPECT_TRUE_MSG(name, !urnw::logupload::LooksLikeGuiLogFileName(name));
  }
}

// The GUI's files reach the daemon process over the control socket as
// descriptors attached to the request's frame: the daemon reads them whole,
// from their start, without opening any path, and the GUI may close its own
// copies as soon as the frame is sent.
UR_TEST(PassedLogFiles_TheGuisFilesCrossTheSocketWithTheirFrame) {
  TestingDir dir;
  UR_EXPECT_TRUE(!dir.Path().empty());
  const std::string infoPath =
      dir.Write("urnetwork-gui.host.user.log.INFO.20260901-000000.202", "gui line\n");
  const std::string errorPath =
      dir.Write("urnetwork-gui.host.user.log.ERROR.20260901-000000.202", "gui error line\n");

  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    UR_FAIL("socketpair failed");
    return;
  }
  const int gui = sockets[0];
  const int daemon = sockets[1];

  // an earlier frame without descriptors, then the request with them
  const std::string earlier = "{\"type\":\"status\"}\n";
  const std::string frame = "{\"type\":\"upload_logs\"}\n";
  UR_EXPECT_EQ(static_cast<ssize_t>(earlier.size()),
               urnw::fdpass::SendWithFds(gui, earlier.data(), earlier.size(), {}, 0));
  {
    const int infoFd = OpenForReading(infoPath);
    const int errorFd = OpenForReading(errorPath);
    UR_EXPECT_TRUE(infoFd >= 0 && errorFd >= 0);
    UR_EXPECT_EQ(static_cast<ssize_t>(frame.size()),
                 urnw::fdpass::SendWithFds(gui, frame.data(), frame.size(), {infoFd, errorFd}, 0));
    // the kernel holds its own references from here
    ::close(infoFd);
    ::close(errorFd);
  }

  std::string received;
  std::vector<int> fds;
  bool truncated = false;
  while (received.size() < earlier.size() + frame.size()) {
    char buffer[64];
    const ssize_t n = urnw::fdpass::ReceiveWithFds(daemon, buffer, sizeof(buffer), &fds, &truncated);
    if (n <= 0) break;
    received.append(buffer, static_cast<size_t>(n));
  }
  UR_EXPECT_TRUE(received == earlier + frame);
  UR_EXPECT_FALSE(truncated);
  UR_EXPECT_EQ(2, fds.size());
  if (fds.size() == 2) {
    UR_EXPECT_TRUE(ReadFromStart(fds[0]) == "gui line\n");
    UR_EXPECT_TRUE(ReadFromStart(fds[1]) == "gui error line\n");
    // close-on-exec, so a tool the daemon runs never inherits them
    UR_EXPECT_TRUE((::fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0);
    UR_EXPECT_TRUE((::fcntl(fds[1], F_GETFD) & FD_CLOEXEC) != 0);
  }
  for (const int fd : fds) ::close(fd);
  ::close(gui);
  ::close(daemon);
}

// More than one request's worth of descriptors is refused before anything is
// sent.
UR_TEST(PassedLogFiles_TheClientSendsNoMoreThanOneRequestsWorth) {
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    UR_FAIL("socketpair failed");
    return;
  }
  const std::vector<int> tooMany(urnw::fdpass::kMaxFds + 1, sockets[0]);
  const std::string frame = "{}\n";
  errno = 0;
  UR_EXPECT_EQ(-1, urnw::fdpass::SendWithFds(sockets[0], frame.data(), frame.size(), tooMany, 0));
  UR_EXPECT_EQ(EINVAL, errno);
  ::close(sockets[0]);
  ::close(sockets[1]);
}

// The daemon pairs the descriptors that came with the frame with the names the
// request gave them, in order, and keeps only a regular file under a glog name:
// a directory, a pipe, a name that is not glog's are closed at once. A
// descriptor with no name is left queued for the connection to close, and the
// request's names past what came are ignored.
UR_TEST(PassedLogFiles_TheDaemonKeepsOnlyRegularFilesUnderGlogNames) {
  TestingDir dir;
  const std::string logName = "urnetwork-gui.host.user.log.INFO.20260901-000000.303";
  const std::string logPath = dir.Write(logName, "gui line\n");
  const std::string otherPath = dir.Write("jwt", "not a log\n");

  const int logFd = OpenForReading(logPath);
  const int directoryFd = ::open(dir.Path().c_str(), O_RDONLY | O_CLOEXEC);
  int pipeFds[2] = {-1, -1};
  UR_EXPECT_TRUE(::pipe(pipeFds) == 0);
  const int namedOtherFd = OpenForReading(otherPath);
  const int unnamedFd = OpenForReading(logPath);

  std::vector<int> queued = {logFd, directoryFd, pipeFds[0], namedOtherFd, unnamedFd};
  const std::vector<std::string> names = {logName, "urnetwork-gui.host.user.log.INFO.1",
                                          "urnetwork-gui.host.user.log.INFO.2", "jwt"};
  urnw::logupload::PassedLogFiles files = urnw::logupload::TakePassedLogFiles(queued, names);

  UR_EXPECT_EQ(1, files.Size());
  if (files.Size() == 1) {
    UR_EXPECT_TRUE(files.Files()[0].name == logName);
    UR_EXPECT_EQ(logFd, files.Files()[0].fd);
    UR_EXPECT_TRUE(ReadFromStart(files.Files()[0].fd) == "gui line\n");
  }
  UR_EXPECT_FALSE(IsOpen(directoryFd));
  UR_EXPECT_FALSE(IsOpen(pipeFds[0]));
  UR_EXPECT_FALSE(IsOpen(namedOtherFd));
  // the one no name was given for stays with the connection
  UR_EXPECT_EQ(1, queued.size());
  if (queued.size() == 1) UR_EXPECT_EQ(unnamedFd, queued[0]);
  UR_EXPECT_TRUE(IsOpen(unnamedFd));

  files.CloseAll();
  UR_EXPECT_FALSE(IsOpen(logFd));
  ::close(unnamedFd);
  ::close(pipeFds[1]);
}

// The daemon keeps the GUI's Go crash file beside its glog files, a regular
// file under that name, as the sdk's upload takes it.
UR_TEST(PassedLogFiles_TheDaemonKeepsTheGuisGoCrashFile) {
  TestingDir dir;
  const std::string crashPath = dir.Write("go-crash.txt", "panic: test\n");
  const int crashFd = OpenForReading(crashPath);
  std::vector<int> queued = {crashFd};
  const std::vector<std::string> names = {"go-crash.txt"};
  urnw::logupload::PassedLogFiles files = urnw::logupload::TakePassedLogFiles(queued, names);
  UR_EXPECT_EQ(1, files.Size());
  if (files.Size() == 1) {
    UR_EXPECT_TRUE(files.Files()[0].name == "go-crash.txt");
    UR_EXPECT_TRUE(ReadFromStart(files.Files()[0].fd) == "panic: test\n");
  }
  UR_EXPECT_EQ(0, queued.size());
  files.CloseAll();
  UR_EXPECT_FALSE(IsOpen(crashFd));
}

// No more than one request's worth is taken, whatever the request names.
UR_TEST(PassedLogFiles_TheDaemonTakesNoMoreThanTheCap) {
  TestingDir dir;
  const std::string logPath =
      dir.Write("urnetwork-gui.host.user.log.INFO.20260901-000000.404", "gui line\n");
  std::vector<int> queued;
  std::vector<std::string> names;
  for (size_t i = 0; i < urnw::logupload::kMaxGuiLogFiles + 2; ++i) {
    queued.push_back(OpenForReading(logPath));
    names.push_back("urnetwork-gui.host.user.log.INFO.20260901-000000." + std::to_string(500 + i));
  }
  urnw::logupload::PassedLogFiles files = urnw::logupload::TakePassedLogFiles(queued, names);
  UR_EXPECT_EQ(urnw::logupload::kMaxGuiLogFiles, files.Size());
  UR_EXPECT_EQ(2, queued.size());
  for (const int fd : queued) ::close(fd);
}

// The files have one owner at a time: a move hands them over, and the owner
// closes them, when told or when it goes.
UR_TEST(PassedLogFiles_TheOwnerClosesTheDescriptors) {
  TestingDir dir;
  const std::string logPath =
      dir.Write("urnetwork-gui.host.user.log.INFO.20260901-000000.606", "gui line\n");
  const int first = OpenForReading(logPath);
  const int second = OpenForReading(logPath);
  {
    urnw::logupload::PassedLogFiles files;
    files.Add("urnetwork-gui.host.user.log.INFO.1", first);
    files.Add("urnetwork-gui.host.user.log.INFO.2", second);
    urnw::logupload::PassedLogFiles moved = std::move(files);
    UR_EXPECT_EQ(0, files.Size());
    UR_EXPECT_EQ(2, moved.Size());
    UR_EXPECT_TRUE(moved.Names() ==
                   (std::vector<std::string>{"urnetwork-gui.host.user.log.INFO.1",
                                             "urnetwork-gui.host.user.log.INFO.2"}));
    UR_EXPECT_TRUE(moved.Fds() == (std::vector<int>{first, second}));
    UR_EXPECT_TRUE(IsOpen(first));
    UR_EXPECT_TRUE(IsOpen(second));
  }
  UR_EXPECT_FALSE(IsOpen(first));
  UR_EXPECT_FALSE(IsOpen(second));
}
