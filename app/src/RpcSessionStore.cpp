// SPDX-License-Identifier: MPL-2.0
#include "RpcSessionStore.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace urnw {
namespace {

constexpr off_t kMaxRpcSessionBytes = 256 * 1024;

struct ParsedRpcSessionFile {
  RpcSessionRecord record;
  bool legacy_plaintext = false;
};

void SetDiagnostic(std::string* out, const std::string& value) {
  if (out) *out = value;
}

std::string ParentDir(const std::string& path) {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

bool SyncParentDirectory(const std::string& path) {
  const int dirFd =
      ::open(ParentDir(path).c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (dirFd < 0) return false;
  const bool ok = ::fsync(dirFd) == 0;
  const bool closeOk = ::close(dirFd) == 0;
  return ok && closeOk;
}

std::optional<std::string> ReadSecureFile(const std::string& path,
                                          std::string* diagnostic) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    SetDiagnostic(diagnostic, errno == ENOENT ? "missing" : "open_failed");
    return std::nullopt;
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & 0077) != 0 || st.st_size <= 0 ||
      st.st_size > kMaxRpcSessionBytes) {
    ::close(fd);
    SetDiagnostic(diagnostic, "insecure_or_invalid_file");
    return std::nullopt;
  }

  std::string bytes(static_cast<size_t>(st.st_size), '\0');
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t n = ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (n > 0) {
      offset += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    ::close(fd);
    SetDiagnostic(diagnostic, "read_failed");
    return std::nullopt;
  }
  if (::close(fd) != 0) {
    SetDiagnostic(diagnostic, "read_failed");
    return std::nullopt;
  }
  return bytes;
}

std::optional<ParsedRpcSessionFile> ParseSessionFile(
    const std::string& bytes, std::string* diagnostic) {
  try {
    const auto parsed = nlohmann::json::parse(bytes);
    if (!parsed.is_object()) throw std::runtime_error("not an object");
    const int version = parsed.value("version", 0);
    if (version == kRpcSessionRecordVersion) {
      if (parsed.value("secret_backend", std::string()) != "secret-service") {
        throw std::runtime_error("unknown secret backend");
      }
      auto record = parsed.get<RpcSessionRecord>();
      if (!ValidRpcSessionMetadata(record)) {
        throw std::runtime_error("invalid metadata fields");
      }
      return ParsedRpcSessionFile{std::move(record), false};
    }
    if (version == 1) {
      RpcSessionRecord record;
      record.version = kRpcSessionRecordVersion;
      ctl::detail::Get(parsed, "state", record.state);
      ctl::detail::Get(parsed, "instance_id", record.instance_id);
      ctl::detail::Get(parsed, "rpc_session_id", record.rpc_session_id);
      ctl::detail::Get(parsed, "client_pem", record.client_pem);
      ctl::detail::Get(parsed, "server_cert_pem", record.server_cert_pem);
      ctl::detail::Get(parsed, "host_port", record.host_port);
      if (!ValidRpcSessionRecord(record)) {
        throw std::runtime_error("invalid legacy fields");
      }
      return ParsedRpcSessionFile{std::move(record), true};
    }
    throw std::runtime_error("unsupported version");
  } catch (const std::exception&) {
    SetDiagnostic(diagnostic, "corrupt");
    return std::nullopt;
  }
}

std::optional<ParsedRpcSessionFile> ReadSessionFile(
    const std::string& path, std::string* diagnostic) {
  auto bytes = ReadSecureFile(path, diagnostic);
  if (!bytes) return std::nullopt;
  return ParseSessionFile(*bytes, diagnostic);
}

bool WriteMetadataFile(const std::string& path,
                       const RpcSessionRecord& record,
                       std::string* diagnostic) {
  const std::string bytes = nlohmann::json(record).dump();
  if (bytes.empty() || bytes.size() > static_cast<size_t>(kMaxRpcSessionBytes)) {
    SetDiagnostic(diagnostic, "metadata_too_large");
    return false;
  }

  std::string tmpPattern = path + ".tmp.XXXXXX";
  std::vector<char> tmp(tmpPattern.begin(), tmpPattern.end());
  tmp.push_back('\0');
  const int fd = ::mkstemp(tmp.data());
  if (fd < 0) {
    SetDiagnostic(diagnostic, "metadata_create_failed");
    return false;
  }
  const std::string tmpPath(tmp.data());
  bool ok = ::fchmod(fd, 0600) == 0;
  size_t offset = 0;
  while (ok && offset < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (n > 0) {
      offset += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      ok = false;
    }
  }
  if (ok) ok = ::fsync(fd) == 0;
  if (::close(fd) != 0) ok = false;
  if (ok) ok = ::rename(tmpPath.c_str(), path.c_str()) == 0;
  if (!ok) {
    ::unlink(tmpPath.c_str());
    SetDiagnostic(diagnostic, "metadata_write_failed");
    return false;
  }
  // The rename is already committed and cannot be rolled back. A directory
  // fsync failure must not make the caller delete the newly referenced secret
  // and leave committed metadata unusable.
  (void)SyncParentDirectory(path);
  return true;
}

bool SameSecretIdentity(const RpcSessionRecord& lhs,
                        const RpcSessionRecord& rhs) {
  return lhs.instance_id == rhs.instance_id &&
         lhs.rpc_session_id == rhs.rpc_session_id;
}

}  // namespace

std::optional<RpcSessionRecord> LoadRpcSessionRecord(
    const std::string& path, RpcSessionSecretStore& secretStore,
    std::string* diagnostic) {
  auto parsed = ReadSessionFile(path, diagnostic);
  if (!parsed) return std::nullopt;

  if (parsed->legacy_plaintext) {
    // Fail closed: old plaintext is never used directly. It must first commit
    // to Secret Service and be replaced by a v2 metadata-only file.
    std::string migrationDiagnostic;
    if (!SaveRpcSessionRecord(path, parsed->record, secretStore,
                              &migrationDiagnostic)) {
      SetDiagnostic(diagnostic, "migration_" + migrationDiagnostic);
      return std::nullopt;
    }
    SetDiagnostic(diagnostic, "migrated_" + parsed->record.state);
    return parsed->record;
  }

  RpcSessionSecrets secrets;
  std::string secretDiagnostic;
  if (!secretStore.Load(parsed->record.instance_id,
                        parsed->record.rpc_session_id, &secrets,
                        &secretDiagnostic)) {
    SetDiagnostic(diagnostic, secretDiagnostic.empty()
                                  ? "secret_service_load_failed"
                                  : secretDiagnostic);
    return std::nullopt;
  }
  parsed->record.client_pem = std::move(secrets.client_pem);
  parsed->record.server_cert_pem = std::move(secrets.server_cert_pem);
  if (!ValidRpcSessionRecord(parsed->record)) {
    SetDiagnostic(diagnostic, "secret_corrupt");
    return std::nullopt;
  }
  SetDiagnostic(diagnostic, parsed->record.state);
  return parsed->record;
}

bool SaveRpcSessionRecord(const std::string& path,
                          const RpcSessionRecord& record,
                          RpcSessionSecretStore& secretStore,
                          std::string* diagnostic) {
  if (!ValidRpcSessionRecord(record)) {
    SetDiagnostic(diagnostic, "invalid_record");
    return false;
  }

  std::string ignored;
  const auto previous = ReadSessionFile(path, &ignored);
  // A v1 plaintext record happens to carry the same logical identity, but it
  // does not reference a Secret Service item. Treating it as a reference would
  // retain a newly-created orphan if the migration's metadata rename failed.
  const bool previousUsesSameSecret =
      previous && !previous->legacy_plaintext &&
      SameSecretIdentity(previous->record, record);

  RpcSessionSecrets secrets{record.client_pem, record.server_cert_pem};
  std::string secretDiagnostic;
  if (!secretStore.Save(record.instance_id, record.rpc_session_id, secrets,
                        &secretDiagnostic)) {
    SetDiagnostic(diagnostic, secretDiagnostic.empty()
                                  ? "secret_service_save_failed"
                                  : secretDiagnostic);
    return false;
  }

  RpcSessionRecord metadata = record;
  metadata.version = kRpcSessionRecordVersion;
  metadata.client_pem.clear();
  metadata.server_cert_pem.clear();
  if (!WriteMetadataFile(path, metadata, diagnostic)) {
    // Preserve a secret already referenced by the previous committed file.
    // A brand-new orphan can be removed safely.
    if (!previousUsesSameSecret) {
      std::string removeDiagnostic;
      secretStore.Remove(record.instance_id, record.rpc_session_id,
                         &removeDiagnostic);
    }
    return false;
  }

  if (previous && !previous->legacy_plaintext && !previousUsesSameSecret) {
    std::string removeDiagnostic;
    if (!secretStore.Remove(previous->record.instance_id,
                            previous->record.rpc_session_id,
                            &removeDiagnostic)) {
      // The new record is committed and usable; surface orphan cleanup without
      // falsely reporting that the active credential save failed.
      SetDiagnostic(diagnostic, record.state + "_old_secret_cleanup_failed");
      return true;
    }
  }
  SetDiagnostic(diagnostic, record.state);
  return true;
}

bool RemoveRpcSessionRecord(const std::string& path,
                            RpcSessionSecretStore& secretStore,
                            std::string* diagnostic) {
  std::string readDiagnostic;
  auto parsed = ReadSessionFile(path, &readDiagnostic);
  if (!parsed) {
    if (readDiagnostic == "missing") {
      SetDiagnostic(diagnostic, "missing");
      return true;
    }
    SetDiagnostic(diagnostic, readDiagnostic);
    return false;
  }

  if (!parsed->legacy_plaintext) {
    std::string secretDiagnostic;
    if (!secretStore.Remove(parsed->record.instance_id,
                            parsed->record.rpc_session_id,
                            &secretDiagnostic)) {
      // Keep the reference so a later retry can still remove the keyring item.
      SetDiagnostic(diagnostic, secretDiagnostic.empty()
                                    ? "secret_service_remove_failed"
                                    : secretDiagnostic);
      return false;
    }
  }
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    SetDiagnostic(diagnostic, "metadata_remove_failed");
    return false;
  }
  if (!SyncParentDirectory(path)) {
    SetDiagnostic(diagnostic, "metadata_remove_sync_failed");
    return false;
  }
  SetDiagnostic(diagnostic, "removed");
  return true;
}

}  // namespace urnw
