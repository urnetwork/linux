// The updater's signed SHA256SUMS check (src/UpdateSignature.hpp), with a
// test key of its own: a fixed ed25519 seed signs here (ed25519 is
// deterministic, so every run sees the same bytes), and the decision is
// handed that key's public half instead of the release key.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include "ReleaseSelection.hpp"
#include "UpdateSignature.hpp"

#include <openssl/evp.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using namespace urnw::update;

const std::string kAsset = "URnetwork-2026.11.25-123456789-x86_64.AppImage";
const std::string kHexA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kHexB = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

std::string Base64(const std::vector<unsigned char>& bytes) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const unsigned n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
  }
  if (bytes.size() - i == 1) {
    const unsigned n = bytes[i] << 16;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += "==";
  } else if (bytes.size() - i == 2) {
    const unsigned n = (bytes[i] << 16) | (bytes[i + 1] << 8);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

struct TestKey {
  std::string publicB64;
  EVP_PKEY* key = nullptr;
  explicit TestKey(unsigned char seedByte) {
    std::vector<unsigned char> seed(32, seedByte);
    key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size());
    std::vector<unsigned char> pub(32);
    std::size_t len = pub.size();
    if (key) EVP_PKEY_get_raw_public_key(key, pub.data(), &len);
    publicB64 = Base64(pub);
  }
  ~TestKey() { EVP_PKEY_free(key); }
  // The .sig file as the pipeline writes it: base64, then a newline.
  std::string SigFile(const std::string& message) const {
    std::vector<unsigned char> sig(64);
    std::size_t len = sig.size();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
    EVP_DigestSign(ctx, sig.data(), &len, reinterpret_cast<const unsigned char*>(message.data()),
                   message.size());
    EVP_MD_CTX_free(ctx);
    return Base64(sig) + "\n";
  }
};

std::string Sums() {
  return kHexB + "  URnetwork-2026.11.25-123456789-aarch64.AppImage\n" + kHexA + "  " + kAsset +
         "\n";
}

SignatureCheck Check(const std::optional<std::string>& sums, const std::optional<std::string>& sig,
                     const std::string& actual, const std::string& key, bool required = false) {
  return CheckReleaseSignature(true, sums, sig, kAsset, actual, key, required);
}

}  // namespace

UR_TEST(updateSignatureReleaseKeyIsAnEd25519Key) {
  const auto key = DecodeBase64Std(kUpdateSigningPublicKey);
  UR_EXPECT_TRUE(key.has_value() && key->size() == 32);
  UR_EXPECT_FALSE(kRequireUpdateSignature);  // staged
}

UR_TEST(updateSignatureGoodSignatureAndMatchingLineVerifies) {
  const TestKey k(7);
  const std::string sums = Sums();
  const SignatureCheck c = Check(sums, k.SigFile(sums), kHexA, k.publicB64);
  UR_EXPECT_TRUE_MSG(c.reason, c.verdict == SignatureVerdict::Verified);
  // also with the signature required
  UR_EXPECT_TRUE(Check(sums, k.SigFile(sums), kHexA, k.publicB64, true).verdict ==
                 SignatureVerdict::Verified);
}

UR_TEST(updateSignatureFileNotMatchingItsLineIsRefused) {
  const TestKey k(7);
  const std::string sums = Sums();
  // kHexB is the OTHER asset's line: a signed file listing a different hash
  UR_EXPECT_TRUE(Check(sums, k.SigFile(sums), kHexB, k.publicB64).verdict ==
                 SignatureVerdict::Refused);
  UR_EXPECT_TRUE(Check(sums, k.SigFile(sums), "", k.publicB64).verdict ==
                 SignatureVerdict::Refused);
}

UR_TEST(updateSignatureWrongKeyOrTamperedSumsIsRefused) {
  const TestKey k(7);
  const TestKey other(9);
  const std::string sums = Sums();
  const std::string sig = k.SigFile(sums);
  UR_EXPECT_TRUE(Check(sums, sig, kHexA, other.publicB64).verdict == SignatureVerdict::Refused);
  // the release key did not sign the test file either
  UR_EXPECT_TRUE(Check(sums, sig, kHexA, kUpdateSigningPublicKey).verdict ==
                 SignatureVerdict::Refused);
  std::string tampered = sums;
  tampered[0] = tampered[0] == '0' ? '1' : '0';
  UR_EXPECT_TRUE(Check(tampered, sig, kHexA, k.publicB64).verdict == SignatureVerdict::Refused);
  // one extra byte (a trailing newline) is a different message
  UR_EXPECT_TRUE(Check(sums + "\n", sig, kHexA, k.publicB64).verdict == SignatureVerdict::Refused);
}

UR_TEST(updateSignatureMalformedOrMissingPiecesAreRefused) {
  const TestKey k(7);
  const std::string sums = Sums();
  const std::string sig = k.SigFile(sums);
  UR_EXPECT_TRUE(Check(sums, std::nullopt, kHexA, k.publicB64).verdict == SignatureVerdict::Refused);
  UR_EXPECT_TRUE(Check(std::nullopt, sig, kHexA, k.publicB64).verdict == SignatureVerdict::Refused);
  UR_EXPECT_TRUE(Check(sums, "not base64!\n", kHexA, k.publicB64).verdict ==
                 SignatureVerdict::Refused);
  UR_EXPECT_TRUE(Check(sums, sig.substr(0, 40) + "\n", kHexA, k.publicB64).verdict ==
                 SignatureVerdict::Refused);
  UR_EXPECT_TRUE(Check(sums, sig, kHexA, "AAAA").verdict == SignatureVerdict::Refused);
  // a signed file that does not list this asset
  const std::string other = kHexA + "  something-else.AppImage\n";
  UR_EXPECT_TRUE(Check(other, k.SigFile(other), kHexA, k.publicB64).verdict ==
                 SignatureVerdict::Refused);
  // a signed file listing this asset twice with different hashes
  const std::string dup = Sums() + kHexB + "  " + kAsset + "\n";
  UR_EXPECT_TRUE(Check(dup, k.SigFile(dup), kHexA, k.publicB64).verdict ==
                 SignatureVerdict::Refused);
}

UR_TEST(updateSignatureAbsentSigWarnsUnlessRequired) {
  const SignatureCheck staged =
      CheckReleaseSignature(false, std::nullopt, std::nullopt, kAsset, kHexA, "", false);
  UR_EXPECT_TRUE(staged.verdict == SignatureVerdict::Unsigned);
  const SignatureCheck required =
      CheckReleaseSignature(false, std::nullopt, std::nullopt, kAsset, kHexA, "", true);
  UR_EXPECT_TRUE(required.verdict == SignatureVerdict::Refused);
}

UR_TEST(updateSignatureSumsParsing) {
  const auto m = ParseSha256Sums(kHexA + "  a\r\n" + kHexB + " *b\nbad line\n" +
                                 "0123  short\n" + std::string(64, 'A') + "  c");
  UR_EXPECT_EQ(3u, m.size());
  UR_EXPECT_TRUE(m.at("a") == kHexA);
  UR_EXPECT_TRUE(m.at("b") == kHexB);
  UR_EXPECT_TRUE(m.at("c") == std::string(64, 'a'));  // uppercase hex normalized
}

UR_TEST(updateSignatureBase64IsStrict) {
  UR_EXPECT_TRUE(DecodeBase64Std("Zm9v").value() == std::vector<unsigned char>({'f', 'o', 'o'}));
  UR_EXPECT_TRUE(DecodeBase64Std("Zm8=").value() == std::vector<unsigned char>({'f', 'o'}));
  UR_EXPECT_TRUE(DecodeBase64Std("Zg==").value() == std::vector<unsigned char>({'f'}));
  UR_EXPECT_FALSE(DecodeBase64Std("Zg=").has_value());
  UR_EXPECT_FALSE(DecodeBase64Std("Zh==").has_value());  // non-canonical bits
  UR_EXPECT_FALSE(DecodeBase64Std("Z=g=").has_value());
  UR_EXPECT_FALSE(DecodeBase64Std("Zm9v\n").has_value());
  UR_EXPECT_FALSE(DecodeBase64Std("").has_value());
}

UR_TEST(updateSignatureSelectionCarriesTheSignedPair) {
  const std::string tag = "v2026.11.25-123456789";
  Release rel;
  rel.tag = tag;
  const std::string name = AssetNameFor(InstallKind::AppImage, VersionFromTag(tag), "amd64");
  rel.assets.push_back({name, FeedAssetUrl(tag, name), "sha256:" + kHexA});
  const std::int64_t now = CodeUnixSeconds(123456789) + 60;

  Selection unsigned_ = SelectRelease({rel}, 1, InstallKind::AppImage, "amd64", now);
  UR_EXPECT_TRUE(unsigned_.code != 0);
  UR_EXPECT_FALSE(unsigned_.sigAttached);

  rel.assets.push_back({"SHA256SUMS", FeedAssetUrl(tag, "SHA256SUMS"), ""});
  rel.assets.push_back({"SHA256SUMS.sig", FeedAssetUrl(tag, "SHA256SUMS.sig"), ""});
  Selection s = SelectRelease({rel}, 1, InstallKind::AppImage, "amd64", now);
  UR_EXPECT_TRUE(s.sigAttached);
  UR_EXPECT_TRUE(s.sumsUrl == FeedAssetUrl(tag, "SHA256SUMS"));
  UR_EXPECT_TRUE(s.sigUrl == FeedAssetUrl(tag, "SHA256SUMS.sig"));

  // a .sig hosted elsewhere still counts as attached, with no URL: refused at apply
  rel.assets.back().url = "https://example.com/SHA256SUMS.sig";
  s = SelectRelease({rel}, 1, InstallKind::AppImage, "amd64", now);
  UR_EXPECT_TRUE(s.sigAttached);
  UR_EXPECT_TRUE(s.sigUrl.empty());
}
