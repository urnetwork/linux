// SPDX-License-Identifier: MPL-2.0
#include "SecretServiceRpcSessionStore.hpp"

#include <libsecret/secret.h>

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace urnw {
namespace {

constexpr int kSecretPayloadVersion = 1;
constexpr size_t kMaxSecretPayloadBytes = 256 * 1024;
// The keyring attribute that scopes our secrets. Renamed with the app ID, so
// an entry written by an older build is not found by this one and the app
// falls back to a fresh RPC session — the same one-time cost as the Flatpak
// data path moving. Deliberately NOT dual-read: keeping the old attribute
// alive would leave the previous app identity holding live key material in the
// user's keyring with nothing left to clean it up.
constexpr const char* kApplicationAttribute = "com.bringyour.network";

const SecretSchema kRpcSessionSchema = [] {
  // Zero-initialize libsecret's reserved ABI fields explicitly. A short C
  // aggregate initializer leaves those fields implicit and becomes a release
  // build failure under -Wmissing-field-initializers.
  SecretSchema schema{};
  schema.name = "network.ur.rpc-session";
  schema.flags = SECRET_SCHEMA_NONE;
  schema.attributes[0] = {"application", SECRET_SCHEMA_ATTRIBUTE_STRING};
  schema.attributes[1] = {"instance_id", SECRET_SCHEMA_ATTRIBUTE_STRING};
  schema.attributes[2] = {"rpc_session_id", SECRET_SCHEMA_ATTRIBUTE_STRING};
  return schema;
}();

void SetDiagnostic(std::string* out, const std::string& value) {
  if (out) *out = value;
}

std::string ErrorDiagnostic(const GError* error,
                            const std::string& fallback) {
  if (!error) return fallback;
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return "secret_service_cancelled";
  }
  if (g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)) {
    return "secret_service_locked";
  }
  if (error->domain == G_DBUS_ERROR || error->domain == G_IO_ERROR) {
    return "secret_service_unavailable";
  }
  return fallback;
}

}  // namespace

bool SecretServiceRpcSessionStore::Load(const std::string& instanceId,
                                        const std::string& rpcSessionId,
                                        RpcSessionSecrets* secrets,
                                        std::string* diagnostic) {
  if (!secrets || instanceId.empty() || rpcSessionId.empty()) {
    SetDiagnostic(diagnostic, "secret_invalid_identity");
    return false;
  }
  GError* error = nullptr;
  gchar* password = secret_password_lookup_sync(
      &kRpcSessionSchema, nullptr, &error, "application", kApplicationAttribute,
      "instance_id", instanceId.c_str(), "rpc_session_id", rpcSessionId.c_str(),
      nullptr);
  if (!password) {
    SetDiagnostic(diagnostic,
                  error ? ErrorDiagnostic(error, "secret_service_load_failed")
                        : "secret_missing");
    if (error) g_error_free(error);
    return false;
  }

  const std::string payload(password);
  secret_password_free(password);
  if (error) g_error_free(error);
  if (payload.empty() || payload.size() > kMaxSecretPayloadBytes) {
    SetDiagnostic(diagnostic, "secret_corrupt");
    return false;
  }
  try {
    const auto parsed = nlohmann::json::parse(payload);
    if (!parsed.is_object() || parsed.value("version", 0) != kSecretPayloadVersion) {
      throw std::runtime_error("unsupported secret payload");
    }
    secrets->client_pem = parsed.value("client_pem", std::string());
    secrets->server_cert_pem =
        parsed.value("server_cert_pem", std::string());
    if (secrets->client_pem.empty() || secrets->server_cert_pem.empty()) {
      throw std::runtime_error("missing secret fields");
    }
  } catch (const std::exception&) {
    secrets->client_pem.clear();
    secrets->server_cert_pem.clear();
    SetDiagnostic(diagnostic, "secret_corrupt");
    return false;
  }
  SetDiagnostic(diagnostic, "secret_loaded");
  return true;
}

bool SecretServiceRpcSessionStore::Save(
    const std::string& instanceId, const std::string& rpcSessionId,
    const RpcSessionSecrets& secrets, std::string* diagnostic) {
  if (instanceId.empty() || rpcSessionId.empty() || secrets.client_pem.empty() ||
      secrets.server_cert_pem.empty()) {
    SetDiagnostic(diagnostic, "secret_invalid_record");
    return false;
  }
  const std::string payload =
      nlohmann::json{{"version", kSecretPayloadVersion},
                     {"client_pem", secrets.client_pem},
                     {"server_cert_pem", secrets.server_cert_pem}}
          .dump();
  if (payload.size() > kMaxSecretPayloadBytes) {
    SetDiagnostic(diagnostic, "secret_too_large");
    return false;
  }

  GError* error = nullptr;
  const gboolean ok = secret_password_store_sync(
      &kRpcSessionSchema, SECRET_COLLECTION_DEFAULT, "URnetwork RPC session",
      payload.c_str(), nullptr, &error, "application", kApplicationAttribute,
      "instance_id", instanceId.c_str(), "rpc_session_id", rpcSessionId.c_str(),
      nullptr);
  if (!ok) {
    SetDiagnostic(diagnostic,
                  ErrorDiagnostic(error, "secret_service_save_failed"));
    if (error) g_error_free(error);
    return false;
  }
  if (error) g_error_free(error);
  SetDiagnostic(diagnostic, "secret_saved");
  return true;
}

bool SecretServiceRpcSessionStore::Remove(const std::string& instanceId,
                                          const std::string& rpcSessionId,
                                          std::string* diagnostic) {
  if (instanceId.empty() || rpcSessionId.empty()) {
    SetDiagnostic(diagnostic, "secret_invalid_identity");
    return false;
  }
  GError* error = nullptr;
  const gboolean removed = secret_password_clear_sync(
      &kRpcSessionSchema, nullptr, &error, "application", kApplicationAttribute,
      "instance_id", instanceId.c_str(), "rpc_session_id", rpcSessionId.c_str(),
      nullptr);
  if (error) {
    SetDiagnostic(diagnostic,
                  ErrorDiagnostic(error, "secret_service_remove_failed"));
    g_error_free(error);
    return false;
  }
  // No matching item is already the desired state.
  SetDiagnostic(diagnostic, removed ? "secret_removed" : "secret_missing");
  return true;
}

}  // namespace urnw
