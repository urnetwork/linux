// Secret Service/libsecret backend for GUI-owned RPC credentials.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "RpcSessionStore.hpp"

namespace urnw {

class SecretServiceRpcSessionStore final : public RpcSessionSecretStore {
 public:
  bool Load(const std::string& instanceId,
            const std::string& rpcSessionId,
            RpcSessionSecrets* secrets,
            std::string* diagnostic) override;
  bool Save(const std::string& instanceId,
            const std::string& rpcSessionId,
            const RpcSessionSecrets& secrets,
            std::string* diagnostic) override;
  bool Remove(const std::string& instanceId,
              const std::string& rpcSessionId,
              std::string* diagnostic) override;
};

}  // namespace urnw
