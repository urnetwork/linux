// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>

#include "RpcSessionStore.hpp"

namespace {

class FakeSecretStore final : public urnw::RpcSessionSecretStore {
 public:
  bool failLoad = false;
  bool failSave = false;
  bool failRemove = false;
  std::function<void()> afterSave;
  std::map<std::string, urnw::RpcSessionSecrets> values;

  static std::string Key(const std::string& instanceId,
                         const std::string& rpcSessionId) {
    return instanceId + "\n" + rpcSessionId;
  }

  bool Load(const std::string& instanceId, const std::string& rpcSessionId,
            urnw::RpcSessionSecrets* secrets,
            std::string* diagnostic) override {
    if (failLoad) {
      if (diagnostic) *diagnostic = "secret_service_locked";
      return false;
    }
    const auto found = values.find(Key(instanceId, rpcSessionId));
    if (found == values.end()) {
      if (diagnostic) *diagnostic = "secret_missing";
      return false;
    }
    *secrets = found->second;
    if (diagnostic) *diagnostic = "secret_loaded";
    return true;
  }

  bool Save(const std::string& instanceId, const std::string& rpcSessionId,
            const urnw::RpcSessionSecrets& secrets,
            std::string* diagnostic) override {
    if (failSave) {
      if (diagnostic) *diagnostic = "secret_service_unavailable";
      return false;
    }
    values[Key(instanceId, rpcSessionId)] = secrets;
    if (afterSave) afterSave();
    if (diagnostic) *diagnostic = "secret_saved";
    return true;
  }

  bool Remove(const std::string& instanceId, const std::string& rpcSessionId,
              std::string* diagnostic) override {
    if (failRemove) {
      if (diagnostic) *diagnostic = "secret_service_locked";
      return false;
    }
    values.erase(Key(instanceId, rpcSessionId));
    if (diagnostic) *diagnostic = "secret_removed";
    return true;
  }
};

struct TempSessionPath {
  std::string path;

  TempSessionPath() {
    char pattern[] = "/tmp/urnetwork-rpc-session-test-XXXXXX";
    const int fd = ::mkstemp(pattern);
    if (fd >= 0) ::close(fd);
    path = pattern;
    ::unlink(path.c_str());
  }

  ~TempSessionPath() { ::unlink(path.c_str()); }
};

struct TempSessionDirectory {
  std::string directory;
  std::string movedDirectory;
  std::string path;

  TempSessionDirectory() {
    char pattern[] = "/tmp/urnetwork-rpc-session-dir-test-XXXXXX";
    const char* made = ::mkdtemp(pattern);
    if (made) directory = made;
    movedDirectory = directory + ".moved";
    path = directory + "/rpc-session.json";
  }

  ~TempSessionDirectory() {
    if (::access(movedDirectory.c_str(), F_OK) == 0) {
      (void)::rename(movedDirectory.c_str(), directory.c_str());
    }
    ::unlink(path.c_str());
    ::rmdir(directory.c_str());
  }
};

urnw::RpcSessionRecord TestRecord(const std::string& state = "pending") {
  urnw::RpcSessionRecord record;
  record.state = state;
  record.instance_id = "instance-1";
  record.rpc_session_id = "session-1";
  record.client_pem = "client-private";
  record.server_cert_pem = "server-public";
  record.host_port = "127.0.0.1:12025";
  return record;
}

std::string ReadAll(const std::string& path) {
  std::ifstream input(path);
  std::ostringstream bytes;
  bytes << input.rdbuf();
  return bytes.str();
}

void WriteLegacy(const std::string& path) {
  const auto record = TestRecord();
  const std::string bytes =
      nlohmann::json{{"version", 1},
                     {"state", record.state},
                     {"instance_id", record.instance_id},
                     {"rpc_session_id", record.rpc_session_id},
                     {"client_pem", record.client_pem},
                     {"server_cert_pem", record.server_cert_pem},
                     {"host_port", record.host_port}}
          .dump();
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) throw std::runtime_error("could not create legacy fixture");
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written > 0) {
      offset += static_cast<size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      ::close(fd);
      throw std::runtime_error("could not write legacy fixture");
    }
  }
  if (::close(fd) != 0) {
    throw std::runtime_error("could not close legacy fixture");
  }
}

}  // namespace

UR_TEST(rpcSessionMetadataIsVersionedAndNeverSerializesSecrets) {
  auto record = TestRecord();
  const auto serialized = nlohmann::json(record);
  UR_EXPECT_EQ(urnw::kRpcSessionRecordVersion,
               serialized.value("version", 0));
  UR_EXPECT_TRUE(serialized.value("state", std::string()) == "pending");
  UR_EXPECT_TRUE(serialized.value("secret_backend", std::string()) ==
                 "secret-service");
  UR_EXPECT_FALSE(serialized.contains("client_pem"));
  UR_EXPECT_FALSE(serialized.contains("server_cert_pem"));
  UR_EXPECT_TRUE(urnw::ValidRpcSessionRecord(record));

  record.state = "confirmed";
  UR_EXPECT_TRUE(urnw::ValidRpcSessionRecord(record));
  record.state = "mystery";
  UR_EXPECT_FALSE(urnw::ValidRpcSessionRecord(record));
}

UR_TEST(rpcSessionStoreKeepsPrivateMaterialOnlyInSecretBackend) {
  TempSessionPath temp;
  FakeSecretStore secrets;
  const auto record = TestRecord();
  std::string diagnostic;

  UR_EXPECT_TRUE(
      urnw::SaveRpcSessionRecord(temp.path, record, secrets, &diagnostic));
  const std::string disk = ReadAll(temp.path);
  UR_EXPECT_TRUE(disk.find("client-private") == std::string::npos);
  UR_EXPECT_TRUE(disk.find("server-public") == std::string::npos);
  UR_EXPECT_TRUE(disk.find("secret-service") != std::string::npos);

  struct stat st {};
  UR_EXPECT_TRUE(::stat(temp.path.c_str(), &st) == 0);
  UR_EXPECT_TRUE((st.st_mode & 0077) == 0);

  const auto loaded =
      urnw::LoadRpcSessionRecord(temp.path, secrets, &diagnostic);
  UR_EXPECT_TRUE(loaded.has_value());
  UR_EXPECT_TRUE(loaded && loaded->client_pem == "client-private");
  UR_EXPECT_TRUE(loaded && loaded->server_cert_pem == "server-public");

  UR_EXPECT_TRUE(
      urnw::RemoveRpcSessionRecord(temp.path, secrets, &diagnostic));
  UR_EXPECT_TRUE(secrets.values.empty());
  UR_EXPECT_TRUE(::access(temp.path.c_str(), F_OK) != 0);
}

UR_TEST(rpcSessionLegacyPlaintextMigratesBeforeItCanBeUsed) {
  TempSessionPath temp;
  WriteLegacy(temp.path);
  FakeSecretStore secrets;
  std::string diagnostic;

  const auto loaded =
      urnw::LoadRpcSessionRecord(temp.path, secrets, &diagnostic);
  UR_EXPECT_TRUE(loaded.has_value());
  UR_EXPECT_TRUE(diagnostic == "migrated_pending");
  UR_EXPECT_TRUE(loaded && loaded->client_pem == "client-private");
  const std::string disk = ReadAll(temp.path);
  UR_EXPECT_TRUE(disk.find("client-private") == std::string::npos);
  UR_EXPECT_TRUE(disk.find("\"version\":2") != std::string::npos);
  UR_EXPECT_TRUE(secrets.values.size() == 1);
}

UR_TEST(rpcSessionLegacyMigrationFailsClosedWhenSecretServiceUnavailable) {
  TempSessionPath temp;
  WriteLegacy(temp.path);
  FakeSecretStore secrets;
  secrets.failSave = true;
  std::string diagnostic;

  const auto loaded =
      urnw::LoadRpcSessionRecord(temp.path, secrets, &diagnostic);
  UR_EXPECT_FALSE(loaded.has_value());
  UR_EXPECT_TRUE(diagnostic == "migration_secret_service_unavailable");
  // Preserve the pre-upgrade record for a later migration attempt, but never
  // return its private key to the caller before Secret Service commits it.
  UR_EXPECT_TRUE(ReadAll(temp.path).find("client-private") != std::string::npos);
}

UR_TEST(rpcSessionLegacyMigrationRemovesKeyringOrphanWhenMetadataCommitFails) {
  TempSessionDirectory temp;
  WriteLegacy(temp.path);
  FakeSecretStore secrets;
  secrets.afterSave = [&temp] {
    (void)::rename(temp.directory.c_str(), temp.movedDirectory.c_str());
  };
  std::string diagnostic;

  const auto loaded =
      urnw::LoadRpcSessionRecord(temp.path, secrets, &diagnostic);
  (void)::rename(temp.movedDirectory.c_str(), temp.directory.c_str());

  UR_EXPECT_FALSE(loaded.has_value());
  UR_EXPECT_TRUE(diagnostic == "migration_metadata_create_failed");
  UR_EXPECT_TRUE(secrets.values.empty());
  UR_EXPECT_TRUE(ReadAll(temp.path).find("client-private") != std::string::npos);
}

UR_TEST(rpcSessionMetadataCommitFailureRemovesNewOrphanSecret) {
  FakeSecretStore secrets;
  const auto record = TestRecord();
  std::string diagnostic;
  UR_EXPECT_FALSE(urnw::SaveRpcSessionRecord(
      "/definitely/missing/urnetwork/rpc-session", record, secrets,
      &diagnostic));
  UR_EXPECT_TRUE(secrets.values.empty());
}

UR_TEST(rpcSessionAdoptionRequiresExactLiveIdentityAndSession) {
  auto record = TestRecord("confirmed");

  urnw::ctl::StatusReply status;
  status.tunnel_state = urnw::ctl::TunnelState::Up;
  status.rpc_port = 12025;
  status.instance_id = "instance-1";
  status.rpc_session_id = "session-1";
  UR_EXPECT_TRUE(urnw::RpcSessionMatchesStatus(record, status));

  status.instance_id = "foreign-instance";
  UR_EXPECT_FALSE(urnw::RpcSessionMatchesStatus(record, status));
  status.instance_id = record.instance_id;
  status.rpc_session_id = "foreign-session";
  UR_EXPECT_FALSE(urnw::RpcSessionMatchesStatus(record, status));
  status.rpc_session_id = record.rpc_session_id;
  status.tunnel_state = urnw::ctl::TunnelState::Stopped;
  UR_EXPECT_FALSE(urnw::RpcSessionMatchesStatus(record, status));

  status.tunnel_state = urnw::ctl::TunnelState::Up;
  record.state = "pending";
  UR_EXPECT_TRUE(urnw::RpcSessionMatchesStatus(record, status));
}
