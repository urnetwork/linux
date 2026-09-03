// SPDX-License-Identifier: MPL-2.0
#include "WalletConnect.hpp"

#include <gio/gio.h>
#include <glibmm/main.h>

#include <cstdio>
#include <map>
#include <vector>

#include <nlohmann/json.hpp>

#include "Config.hpp"
#include "SsoBridge.hpp"

namespace urnw {
namespace {

constexpr const char* kWebBridge = "https://ur.io/wallet-connect";
constexpr const char* kAppUrl = "https://ur.io";
constexpr const char* kCluster = "mainnet-beta";

std::string Esc(const std::string& s) {
  char* e = g_uri_escape_string(s.c_str(), nullptr, FALSE);
  std::string out = e ? e : s;
  if (e) g_free(e);
  return out;
}

void SplitUrl(const std::string& url, std::string& host, std::string& query) {
  auto scheme = url.find("://");
  size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
  auto q = url.find('?', start);
  auto slash = url.find('/', start);
  size_t hostEnd = std::min(q == std::string::npos ? url.size() : q,
                            slash == std::string::npos ? url.size() : slash);
  host = url.substr(start, hostEnd - start);
  query = (q == std::string::npos) ? std::string() : url.substr(q + 1);
}

std::map<std::string, std::string> ParseQuery(const std::string& query) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < query.size()) {
    auto amp = query.find('&', i);
    std::string pair =
        query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    auto eq = pair.find('=');
    if (eq != std::string::npos) {
      std::string k = pair.substr(0, eq);
      std::string v = pair.substr(eq + 1);
      char* dec = g_uri_unescape_string(v.c_str(), nullptr);
      out[k] = dec ? std::string(dec) : v;
      if (dec) g_free(dec);
    }
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return out;
}

}  // namespace

const char* WalletConnect::Host(Provider p) {
  if (p == Provider::Solflare) return "solflare";
  if (p == Provider::Bittensor) return "bittensor";
  return "phantom";
}

std::optional<WalletConnect::Provider> WalletConnect::ProviderForHost(const std::string& host) {
  if (host == "phantom-connect" || host == "phantom-sign-message") return Provider::Phantom;
  if (host == "solflare-connect" || host == "solflare-sign-message") return Provider::Solflare;
  // bittensor has no connect hop: the sign-message return carries the address
  if (host == "bittensor-sign-message") return Provider::Bittensor;
  return std::nullopt;
}

bool WalletConnect::NewKeyPair() {
  dappKeyPair_ = urnet::generateWalletKeyPair();
  return dappKeyPair_.has_value();
}

std::optional<std::string> WalletConnect::SharedSecretBase58() const {
  if (!dappKeyPair_ || !walletEncryptionPublicKey_) return std::nullopt;
  auto priv = urnet::decodeBase58(dappKeyPair_->PrivateKeyBase58);
  auto pub = urnet::decodeBase58(*walletEncryptionPublicKey_);
  if (!priv || !pub) return std::nullopt;
  auto shared = urnet::generateSharedSecret(*priv, *pub);
  if (shared.empty()) return std::nullopt;
  return urnet::encodeBase58(shared.data(), static_cast<int32_t>(shared.size()));
}

void WalletConnect::OpenUrl(const std::string& url) {
  GError* err = nullptr;
  if (!g_app_info_launch_default_for_uri(url.c_str(), nullptr, &err)) {
    std::string msg = err ? err->message : "failed to open browser";
    if (err) g_error_free(err);
    if (on_error) on_error(msg);
  }
}

void WalletConnect::Connect(Provider p) {
  // fresh ephemeral keypair per connection (matches macOS prepareForWalletConnection)
  connectedPublicKey_.reset();
  walletEncryptionPublicKey_.reset();
  session_.reset();
  currentProvider_ = p;
  if (!NewKeyPair()) {
    if (on_error) on_error("failed to generate wallet keypair");
    return;
  }
  const std::string redirect = std::string("urnetwork://") + Host(p) + "-connect";
  std::string url = std::string(kWebBridge) +
                    "?dapp_encryption_public_key=" + Esc(dappKeyPair_->PublicKeyBase58) +
                    "&cluster=" + kCluster +
                    "&app_url=" + Esc(kAppUrl) +
                    "&redirect_link=" + Esc(redirect) +
                    "&method=connect&provider=" + Host(p);
  OpenUrl(url);
}

void WalletConnect::SignMessage(const std::string& message) {
  if (!dappKeyPair_ || !session_ || !walletEncryptionPublicKey_) {
    if (on_error) on_error("wallet not connected");
    return;
  }
  lastMessage_ = message;
  const std::string messageB58 = urnet::encodeBase58(
      reinterpret_cast<const uint8_t*>(message.data()), static_cast<int32_t>(message.size()));
  const std::string payload =
      nlohmann::json{{"message", messageB58}, {"session", *session_}, {"display", "utf8"}}.dump();

  auto sharedB58 = SharedSecretBase58();
  if (!sharedB58) {
    if (on_error) on_error("failed to derive shared secret");
    return;
  }
  const std::string nonce = urnet::generateNonce();
  const std::string enc = urnet::encryptData(reinterpret_cast<const uint8_t*>(payload.data()),
                                             static_cast<int32_t>(payload.size()), nonce, *sharedB58);
  if (enc.empty()) {
    if (on_error) on_error("failed to encrypt sign-message payload");
    return;
  }
  const std::string redirect =
      std::string("urnetwork://") + Host(currentProvider_) + "-sign-message";
  std::string url = std::string(kWebBridge) +
                    "?dapp_encryption_public_key=" + Esc(dappKeyPair_->PublicKeyBase58) +
                    "&cluster=" + kCluster + "&nonce=" + Esc(nonce) +
                    "&redirect_link=" + Esc(redirect) + "&payload=" + Esc(enc) +
                    "&method=signMessage&provider=" + Host(currentProvider_);
  OpenUrl(url);
}

void WalletConnect::SignInWithBittensor(const std::string& message,
                                        const std::string& purpose) {
  // One hop: the bridge connects the substrate wallet AND signs in the same page
  // load, and sr25519 signatures are public — so there is no ephemeral keypair,
  // no session, and no shared secret on this path.
  connectedPublicKey_.reset();
  walletEncryptionPublicKey_.reset();
  session_.reset();
  dappKeyPair_.reset();
  currentProvider_ = Provider::Bittensor;
  lastMessage_ = message;

  const std::string redirect =
      std::string("urnetwork://") + Host(Provider::Bittensor) + "-sign-message";
  std::string url = std::string(kWebBridge) + "?provider=" + Host(Provider::Bittensor) +
                    "&method=signMessage&message=" + Esc(message) +
                    "&redirect_link=" + Esc(redirect);
  if (!purpose.empty()) url += "&purpose=" + Esc(purpose);
  // The WalletConnect Cloud project id (Config.hpp) lets the bridge pair with a
  // wallet app over a QR code. Without one the bridge falls back to injected
  // wallets only (browser extension) — so an empty id is sent as no param at all.
  const std::string projectId = kWalletConnectProjectId;
  if (!projectId.empty()) url += "&wc_project_id=" + Esc(projectId);
  OpenUrl(url);
}

void WalletConnect::SignInWithApple(const std::string& apiUrl, const std::string& state,
                                    const std::string& nonce) {
  // Debug hook: URNETWORK_SSO_SIMULATE=1 (or =<token>) skips the browser and
  // posts the return the api's callback would send — an unsigned token
  // carrying the nonce, which the server rejects — so the whole return path
  // (state, nonce, the login call, the error surface) runs without an Apple
  // account
  if (const char* sim = g_getenv("URNETWORK_SSO_SIMULATE")) {
    const std::string given(sim);
    const std::string token = given.find('.') != std::string::npos
                                  ? given
                                  : sso::SimulatedIdentityToken(nonce, "simulated");
    const std::string uri = std::string("urnetwork://") + sso::kAppleReturnHost +
                            sso::kAppleReturnPath + "?state=" + Esc(state) +
                            "&id_token=" + Esc(token);
    Glib::signal_timeout().connect_once([this, uri] { HandleDeepLink(uri); }, 400);
    return;
  }
  if (apiUrl.empty()) {
    if (on_error) on_error("no api url for the Apple sign-in callback");
    return;
  }
  OpenUrl(sso::AppleAuthorizeUrl(apiUrl, state, nonce));
}

void WalletConnect::SignInWithGoogle(const std::string& apiUrl, const std::string& state,
                                     const std::string& nonce) {
  // the same debug hook as Apple: URNETWORK_SSO_SIMULATE posts the return the
  // api's callback would send, so the whole return path runs without a Google
  // account (the unsigned token is rejected by the server)
  if (const char* sim = g_getenv("URNETWORK_SSO_SIMULATE")) {
    const std::string given(sim);
    const std::string token = given.find('.') != std::string::npos
                                  ? given
                                  : sso::SimulatedIdentityToken(nonce, "simulated");
    const std::string uri = std::string("urnetwork://") + sso::kOAuthReturnHost +
                            sso::kGoogleReturnPath + "?state=" + Esc(state) +
                            "&id_token=" + Esc(token);
    Glib::signal_timeout().connect_once([this, uri] { HandleDeepLink(uri); }, 400);
    return;
  }
  if (apiUrl.empty()) {
    if (on_error) on_error("no api url for the Google sign-in callback");
    return;
  }
  OpenUrl(sso::GoogleAuthorizeUrl(apiUrl, state, nonce));
}

void WalletConnect::HandleOAuthReturn(const std::string& url) {
  // urnetwork://oauth/apple?state=…&id_token=…  (or &error=…), and the same
  // shape on urnetwork://oauth/google: the path names the provider
  const std::string provider = sso::OAuthReturnProvider(sso::UrlPath(url));
  if (provider.empty()) {
    if (on_error) on_error("unknown oauth callback");
    return;
  }
  const auto q = url.find('?');
  const sso::Return r =
      sso::ParseOAuthReturn(provider, q == std::string::npos ? std::string() : url.substr(q + 1));
  if (on_sso) on_sso(r.provider, r.authJwt, r.state, r.error);
}

bool WalletConnect::HandleDeepLink(const std::string& url) {
  std::string host, query;
  SplitUrl(url, host, query);
  if (host == sso::kOAuthReturnHost) {
    HandleOAuthReturn(url);
    return true;
  }
  auto provider = ProviderForHost(host);
  if (!provider) return false;  // not a wallet callback
  if (*provider == Provider::Bittensor) {
    HandleBittensorSignMessage(query);
  } else if (host.find("-connect") != std::string::npos) {
    HandleConnect(*provider, query);
  } else {
    HandleSignMessage(*provider, query);
  }
  return true;
}

void WalletConnect::HandleConnect(Provider p, const std::string& query) {
  auto params = ParseQuery(query);
  if (params.count("errorCode")) {
    if (on_error) on_error(params.count("errorMessage") ? params["errorMessage"] : "wallet connect error");
    return;
  }
  const std::string keyParam =
      std::string(Host(p)) + "_encryption_public_key";  // phantom_/solflare_
  if (!params.count(keyParam) || !params.count("nonce") || !params.count("data") || !dappKeyPair_) {
    if (on_error) on_error("missing wallet connect parameters");
    return;
  }
  walletEncryptionPublicKey_ = params[keyParam];
  auto sharedB58 = SharedSecretBase58();
  if (!sharedB58) {
    if (on_error) on_error("failed to derive shared secret");
    return;
  }
  auto decrypted = urnet::decryptData(params["data"], params["nonce"], *sharedB58);
  if (decrypted.empty()) {
    if (on_error) on_error("failed to decrypt wallet connection");
    return;
  }
  try {
    auto j = nlohmann::json::parse(std::string(decrypted.begin(), decrypted.end()));
    connectedPublicKey_ = j.at("public_key").get<std::string>();
    session_ = j.at("session").get<std::string>();
    currentProvider_ = p;
    if (on_public_key) on_public_key(*connectedPublicKey_, p);
  } catch (const std::exception& e) {
    if (on_error) on_error(std::string("bad connect response: ") + e.what());
  }
}

void WalletConnect::HandleSignMessage(Provider p, const std::string& query) {
  auto params = ParseQuery(query);
  if (params.count("errorCode")) {
    if (on_error) on_error(params.count("errorMessage") ? params["errorMessage"] : "wallet signing error");
    return;
  }
  if (!params.count("nonce") || !params.count("data") || !dappKeyPair_ || !walletEncryptionPublicKey_) {
    if (on_error) on_error("missing wallet signature parameters");
    return;
  }
  auto sharedB58 = SharedSecretBase58();
  if (!sharedB58) {
    if (on_error) on_error("failed to derive shared secret");
    return;
  }
  auto decrypted = urnet::decryptData(params["data"], params["nonce"], *sharedB58);
  if (decrypted.empty()) {
    if (on_error) on_error("failed to decrypt wallet signature");
    return;
  }
  try {
    auto j = nlohmann::json::parse(std::string(decrypted.begin(), decrypted.end()));
    const std::string signatureB58 = j.at("signature").get<std::string>();
    auto sigBytes = urnet::decodeBase58(signatureB58);
    if (!sigBytes) {
      if (on_error) on_error("failed to decode wallet signature");
      return;
    }
    // The backend expects a base64 signature (macOS parity).
    char* b64 = g_base64_encode(sigBytes->data(), sigBytes->size());
    std::string base64Signature = b64 ? b64 : "";
    if (b64) g_free(b64);
    if (on_signature) on_signature(base64Signature);
  } catch (const std::exception& e) {
    if (on_error) on_error(std::string("bad signature response: ") + e.what());
  }
}

// The bittensor bridge returns PLAIN query params — no NaCl envelope, nothing to
// decrypt (mmm/ur.io react/src/components/WalletConnect.jsx):
//   urnetwork://bittensor-sign-message?address=<ss58>&signature=<0xhex>
//   urnetwork://bittensor-sign-message?errorCode=-1&errorMessage=<text>
void WalletConnect::HandleBittensorSignMessage(const std::string& query) {
  auto params = ParseQuery(query);
  if (params.count("errorCode")) {
    if (on_error)
      on_error(params.count("errorMessage") ? params["errorMessage"] : "wallet signing error");
    return;
  }
  const std::string address = params.count("address") ? params["address"] : std::string();
  const std::string signature = params.count("signature") ? params["signature"] : std::string();
  if (address.empty() || signature.empty()) {
    if (on_error) on_error("missing wallet signature parameters");
    return;
  }
  connectedPublicKey_ = address;  // ss58, the wallet_address for authLogin
  currentProvider_ = Provider::Bittensor;
  // the sr25519 signature passes through as the hex the wallet returned; the
  // server accepts it with or without the 0x prefix
  if (on_signature) on_signature(signature);
}

}  // namespace urnw
