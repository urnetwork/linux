// UpdateSignature -- the updater's second check (staged): an ed25519
// signature over the release's SHA256SUMS, on top of GitHub's per-asset
// digest (ReleaseSelection.hpp).
//
// The contract, fixed with the release pipeline:
//   SHA256SUMS      lines of `<64 hex>  <asset name>`
//   SHA256SUMS.sig  the base64 (standard alphabet, padded) ed25519 signature
//                   over the exact bytes of SHA256SUMS, then a newline
//   the public key  kUpdateSigningPublicKey below, base64 of the raw 32 bytes
//
// The decision (CheckReleaseSignature):
//   .sig attached   the signature must verify under the key (refused
//                   otherwise), and the downloaded file's SHA-256 must equal
//                   its line in SHA256SUMS (refused otherwise). A .sig whose
//                   SHA256SUMS or own bytes could not be fetched is refused
//                   too: an attached signature is never skipped.
//   .sig absent     refused while kRequireUpdateSignature is true; until then
//                   a warning, and the existing digest check alone decides.
//
// GTK-free: libcrypto (OpenSSL 3, EVP_PKEY_ED25519) and the standard library
// only, so tests/UpdateSignatureTest.cpp runs it with its own key.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

namespace urnw::update {

inline constexpr const char* kUpdateSigningPublicKey =
    "Qm4V/UTZcuIRMFJuAzDhxf2CK6ycwxF0HFOcFhXBIYQ=";

// Staged: flips to true once every stable release carries the signature.
inline constexpr bool kRequireUpdateSignature = false;

inline constexpr const char* kSumsAssetName = "SHA256SUMS";
inline constexpr const char* kSumsSigAssetName = "SHA256SUMS.sig";

// Both are tiny; anything larger is not ours.
inline constexpr std::uint64_t kMaxSumsBytes = 1ull * 1024 * 1024;
inline constexpr std::uint64_t kMaxSigBytes = 4ull * 1024;

// Strict standard base64 (RFC 4648 section 4, padding required, no
// whitespace). nullopt on any deviation.
inline std::optional<std::vector<unsigned char>> DecodeBase64Std(std::string_view in) {
  const auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (in.empty() || in.size() % 4 != 0) return std::nullopt;
  std::vector<unsigned char> out;
  out.reserve(in.size() / 4 * 3);
  for (std::size_t i = 0; i < in.size(); i += 4) {
    const bool last = i + 4 == in.size();
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = in[i + k];
      if (c == '=') {
        if (!last || k < 2) return std::nullopt;
        ++pad;
        v[k] = 0;
        continue;
      }
      if (pad) return std::nullopt;  // data after padding
      v[k] = value(c);
      if (v[k] < 0) return std::nullopt;
    }
    const std::uint32_t n = (static_cast<std::uint32_t>(v[0]) << 18) |
                            (static_cast<std::uint32_t>(v[1]) << 12) |
                            (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
    out.push_back(static_cast<unsigned char>((n >> 16) & 0xff));
    if (pad < 2) out.push_back(static_cast<unsigned char>((n >> 8) & 0xff));
    if (pad < 1) out.push_back(static_cast<unsigned char>(n & 0xff));
    // Non-canonical trailing bits are refused: one signature, one encoding.
    if (pad == 1 && (v[2] & 0x3) != 0) return std::nullopt;
    if (pad == 2 && (v[1] & 0xf) != 0) return std::nullopt;
  }
  return out;
}

// ed25519 over `message` with a raw 32-byte public key and a 64-byte
// signature. Any malformed input is false.
inline bool VerifyEd25519(const std::vector<unsigned char>& publicKey, std::string_view message,
                          const std::vector<unsigned char>& signature) {
  if (publicKey.size() != 32 || signature.size() != 64) return false;
  EVP_PKEY* key =
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, publicKey.data(), publicKey.size());
  if (!key) return false;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  bool ok = false;
  if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
    ok = EVP_DigestVerify(ctx, signature.data(), signature.size(),
                          reinterpret_cast<const unsigned char*>(message.data()),
                          message.size()) == 1;
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  return ok;
}

// SHA256SUMS -> name -> lowercase hex. A line that does not parse is
// ignored; a name listed twice with different hashes maps to "" (which never
// equals a computed hash), so an ambiguous file cannot vouch for either.
inline std::map<std::string, std::string> ParseSha256Sums(std::string_view text) {
  std::map<std::string, std::string> out;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() < 64 + 2 + 1) continue;
    std::string hex;
    bool ok = true;
    for (std::size_t i = 0; i < 64; ++i) {
      char c = line[i];
      if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
      ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
      hex += c;
    }
    // `<hex>  <name>` (sha256sum's text mode) or `<hex> *<name>` (binary).
    if (!ok || line[64] != ' ' || (line[65] != ' ' && line[65] != '*')) continue;
    const std::string name(line.substr(66));
    const auto it = out.find(name);
    if (it == out.end()) {
      out.emplace(name, hex);
    } else if (it->second != hex) {
      it->second.clear();
    }
  }
  return out;
}

enum class SignatureVerdict {
  Verified,  // signature good and the file's line matches
  Unsigned,  // no .sig on the release and none required: warn, proceed
  Refused,   // do not install; `reason` says why
};

struct SignatureCheck {
  SignatureVerdict verdict = SignatureVerdict::Refused;
  std::string reason;
};

// `sigAttached`: the release lists a SHA256SUMS.sig asset. `sums` / `sig`:
// the fetched bytes, nullopt when not fetched (absent, or the fetch failed).
// `actualHex`: the downloaded file's SHA-256, lowercase hex.
inline SignatureCheck CheckReleaseSignature(bool sigAttached, const std::optional<std::string>& sums,
                                            const std::optional<std::string>& sig,
                                            std::string_view assetName, std::string_view actualHex,
                                            std::string_view publicKeyB64, bool required) {
  if (!sigAttached) {
    if (required) return {SignatureVerdict::Refused, "the release carries no SHA256SUMS.sig"};
    return {SignatureVerdict::Unsigned, "the release carries no SHA256SUMS.sig"};
  }
  if (!sig) return {SignatureVerdict::Refused, "SHA256SUMS.sig could not be fetched"};
  if (!sums) return {SignatureVerdict::Refused, "SHA256SUMS could not be fetched"};
  const auto key = DecodeBase64Std(publicKeyB64);
  if (!key || key->size() != 32) return {SignatureVerdict::Refused, "the signing key is malformed"};
  std::string_view sigText = *sig;
  // "then newline": the one trailing line ending goes, nothing else.
  if (!sigText.empty() && sigText.back() == '\n') sigText.remove_suffix(1);
  if (!sigText.empty() && sigText.back() == '\r') sigText.remove_suffix(1);
  const auto signature = DecodeBase64Std(sigText);
  if (!signature || signature->size() != 64) {
    return {SignatureVerdict::Refused, "SHA256SUMS.sig is not a base64 ed25519 signature"};
  }
  if (!VerifyEd25519(*key, *sums, *signature)) {
    return {SignatureVerdict::Refused, "SHA256SUMS.sig does not verify"};
  }
  const auto lines = ParseSha256Sums(*sums);
  const auto it = lines.find(std::string(assetName));
  if (it == lines.end() || it->second.empty()) {
    return {SignatureVerdict::Refused, "SHA256SUMS has no single line for " + std::string(assetName)};
  }
  if (actualHex.empty() || it->second != actualHex) {
    return {SignatureVerdict::Refused,
            "the download does not match its SHA256SUMS line (expected " + it->second + ")"};
  }
  return {SignatureVerdict::Verified, {}};
}

}  // namespace urnw::update
