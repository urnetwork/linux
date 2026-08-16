// Durable GUI half of a Linux daemon RPC session. Only non-secret metadata is
// written to disk. The mTLS client private key and pinned server certificate
// are stored through the desktop Secret Service implementation.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "ControlProtocol.hpp"

namespace urnw {

inline constexpr int kRpcSessionRecordVersion = 2;

struct RpcSessionSecrets {
  std::string client_pem;
  std::string server_cert_pem;
};

class RpcSessionSecretStore {
 public:
  virtual ~RpcSessionSecretStore() = default;

  virtual bool Load(const std::string& instanceId,
                    const std::string& rpcSessionId,
                    RpcSessionSecrets* secrets,
                    std::string* diagnostic) = 0;
  virtual bool Save(const std::string& instanceId,
                    const std::string& rpcSessionId,
                    const RpcSessionSecrets& secrets,
                    std::string* diagnostic) = 0;
  virtual bool Remove(const std::string& instanceId,
                      const std::string& rpcSessionId,
                      std::string* diagnostic) = 0;
};

struct RpcSessionRecord {
  int version = kRpcSessionRecordVersion;
  std::string state;  // "pending" before sync, "confirmed" after sync
  std::string instance_id;
  std::string rpc_session_id;
  // In-memory only. to_json deliberately omits both fields.
  std::string client_pem;
  std::string server_cert_pem;
  std::string host_port;
};

inline void to_json(nlohmann::json& j, const RpcSessionRecord& v) {
  j = nlohmann::json{{"version", v.version},
                     {"state", v.state},
                     {"instance_id", v.instance_id},
                     {"rpc_session_id", v.rpc_session_id},
                     {"secret_backend", "secret-service"},
                     {"host_port", v.host_port}};
}

inline void from_json(const nlohmann::json& j, RpcSessionRecord& v) {
  ctl::detail::Get(j, "version", v.version);
  ctl::detail::Get(j, "state", v.state);
  ctl::detail::Get(j, "instance_id", v.instance_id);
  ctl::detail::Get(j, "rpc_session_id", v.rpc_session_id);
  ctl::detail::Get(j, "host_port", v.host_port);
}

inline bool ValidRpcSessionMetadata(const RpcSessionRecord& v) {
  return v.version == kRpcSessionRecordVersion &&
         (v.state == "pending" || v.state == "confirmed") &&
         !v.instance_id.empty() && !v.rpc_session_id.empty() &&
         !v.host_port.empty();
}

inline bool ValidRpcSessionRecord(const RpcSessionRecord& v) {
  return ValidRpcSessionMetadata(v) && !v.client_pem.empty() &&
         !v.server_cert_pem.empty();
}

inline bool RpcSessionMatchesStatus(const RpcSessionRecord& v,
                                    const ctl::StatusReply& status) {
  return ValidRpcSessionRecord(v) && status.tunnel_state == ctl::TunnelState::Up &&
         status.instance_id == v.instance_id &&
         status.rpc_session_id == v.rpc_session_id && status.rpc_port > 0 &&
         v.host_port == "127.0.0.1:" + std::to_string(status.rpc_port);
}

std::optional<RpcSessionRecord> LoadRpcSessionRecord(
    const std::string& path, RpcSessionSecretStore& secretStore,
    std::string* diagnostic = nullptr);
bool SaveRpcSessionRecord(const std::string& path,
                          const RpcSessionRecord& record,
                          RpcSessionSecretStore& secretStore,
                          std::string* diagnostic = nullptr);
bool RemoveRpcSessionRecord(const std::string& path,
                            RpcSessionSecretStore& secretStore,
                            std::string* diagnostic = nullptr);

}  // namespace urnw
