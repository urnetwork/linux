// The Change Network API normalization contract, mirrored from the Windows
// netserver namespace so the two clients cannot drift on what a typed host or
// url means. Cases mirror the semantics documented in NetworkServerUtils.hpp.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>

#include "NetworkServerUtils.hpp"

using urnw::netserver::DerivedServiceUrl;
using urnw::netserver::HasInsecureScheme;
using urnw::netserver::NormalizeApiUrl;
using urnw::netserver::NormalizeConnectUrl;
using urnw::netserver::NormalizeHost;

UR_TEST(hostStripsSchemePathAndPort) {
  UR_EXPECT_TRUE_MSG(std::string("ur.network"), NormalizeHost("  HTTPS://Ur.Network/path?q#f  ") == std::string("ur.network"));
  UR_EXPECT_TRUE_MSG(std::string("example.com"), NormalizeHost("example.com:8443") == std::string("example.com"));
  UR_EXPECT_TRUE_MSG(std::string("example.com"), NormalizeHost("user@example.com") == std::string("example.com"));
  UR_EXPECT_TRUE_MSG(std::string("example.com"), NormalizeHost(".example.com.") == std::string("example.com"));
}

UR_TEST(hostLeavesBareIpv6AloneButStripsBracketedPort) {
  // a bare IPv6 address's colons are indistinguishable from a port separator
  UR_EXPECT_TRUE_MSG(std::string("2001:db8::1"), NormalizeHost("2001:db8::1") == std::string("2001:db8::1"));
  UR_EXPECT_TRUE_MSG(std::string("[2001:db8::1]"), NormalizeHost("[2001:db8::1]:8080") == std::string("[2001:db8::1]"));
}

UR_TEST(urlNormalizationAddsSecureSchemeAndTrimsSlashes) {
  UR_EXPECT_TRUE_MSG(std::string("https://api.example.com"), NormalizeApiUrl("api.example.com/") == std::string("https://api.example.com"));
  UR_EXPECT_TRUE_MSG(std::string("https://api.example.com:8443"), NormalizeApiUrl("https://api.example.com:8443") == std::string("https://api.example.com:8443"));
  UR_EXPECT_TRUE_MSG(std::string("wss://connect.example.com"), NormalizeConnectUrl("connect.example.com") == std::string("wss://connect.example.com"));
  UR_EXPECT_TRUE_MSG(std::string(""), NormalizeApiUrl("   ") == std::string(""));
}

UR_TEST(insecureSchemeIsAdvisoryOnlyForExplicitSchemes) {
  UR_EXPECT_TRUE(HasInsecureScheme("http://api.example.com", "https"));
  UR_EXPECT_TRUE(HasInsecureScheme("ws://connect.example.com", "wss"));
  UR_EXPECT_TRUE(!HasInsecureScheme("api.example.com", "https"));  // scheme gets added
  UR_EXPECT_TRUE(!HasInsecureScheme("https://api.example.com", "https"));
}

UR_TEST(derivedServiceUrlsFollowMigrationAndEnvRules) {
  // production carries the migration domain; main env has no prefix
  UR_EXPECT_TRUE_MSG(std::string("https://api.bringyour.com"), DerivedServiceUrl("ur.network", "bringyour.com", "main", "https", "api") == std::string("https://api.bringyour.com"));
  // a custom deployment derives straight off its own name
  UR_EXPECT_TRUE_MSG(std::string("wss://connect.example.com"), DerivedServiceUrl("example.com", "", "main", "wss", "connect") == std::string("wss://connect.example.com"));
  // a non-main env prefixes the service host
  UR_EXPECT_TRUE_MSG(std::string("https://test-api.example.com"), DerivedServiceUrl("example.com", "", "test", "https", "api") == std::string("https://test-api.example.com"));
}
