// The connection region's GTK and SDK call sites, read as source the way
// ControlDohWiringTest reads the bootstrap DoH section's: both doors (the login
// screen's "Connecting from" control and Custom DNS's "Connection region" row)
// open the one picker over the one stored value, the space's ingress country;
// the host reads and writes it through the space's own getter and setter; the
// login screen's calls go through the space's Api; a call that got no answer
// offers the picker and nothing changes the region by itself; and every text
// is the catalog's. Support inbox 7956, 7957, 7973.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string Source(const std::string& relative) {
  return ReadFile(std::string(UR_SRC_DIR) + "/" + relative);
}

// The body of `signature` (up to the next top-level definition), "" if absent.
std::string Body(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool Has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

UR_TEST(ConnectionRegionWiring_TheHostUsesTheSpacesIngressCountry) {
  const std::string host = Source("SdkHost.cpp");
  UR_EXPECT_TRUE(Has(Body(host, "std::string SdkHost::GetIngressCountryCode("),
                     "networkSpace_->getIngressCountryCode()"));
  UR_EXPECT_TRUE(Has(Body(host, "std::string SdkHost::SetIngressCountryCode("),
                     "networkSpace_->setIngressCountryCode(countryCode)"));
  UR_EXPECT_TRUE(Has(Body(host, "std::vector<std::string> SdkHost::IngressCountryCodes("),
                     "urnet::ingressCountryCodes()"));
  UR_EXPECT_TRUE(Has(Body(host, "std::vector<std::string> SdkHost::IngressDohUrls("),
                     "urnet::ingressDohUrls(countryCode)"));
  UR_EXPECT_TRUE(Has(Body(host, "std::vector<std::string> SdkHost::IngressPlainDnsServers("),
                     "urnet::ingressPlainDnsServers(countryCode)"));
  // the egress (tunnel) dns is not written by a region save
  UR_EXPECT_FALSE(Has(Body(host, "std::string SdkHost::SetIngressCountryCode("), "DnsResolverSettings"));
}

// The login screen's calls go through api_, the space's own Api, so the
// strategy carrying the region serves them; a region save applies in place.
UR_TEST(ConnectionRegionWiring_LoginCallsGoThroughTheSpacesApi) {
  const std::string host = Source("SdkHost.cpp");
  UR_EXPECT_TRUE(Has(host, "api_ = networkSpace_->getApi();"));
  UR_EXPECT_TRUE(Has(Body(host, "void SdkHost::StartLogin("), "api_->authLogin("));
  UR_EXPECT_TRUE(Has(Body(host, "void SdkHost::LoginWithPassword("), "api_->authLoginWithPassword("));
}

// Every sign-in call reports a transport error, and only a network-level one
// reaches the window.
UR_TEST(ConnectionRegionWiring_SignInFailuresReportCantReach) {
  const std::string host = Source("SdkHost.cpp");
  for (const char* signature :
       {"void SdkHost::StartLogin(", "void SdkHost::LoginWithPassword(", "void SdkHost::LoginWithCode(",
        "void SdkHost::LoginWithSeedphrase(", "void SdkHost::CreateInstantAccount(",
        "void SdkHost::HandleNetworkCreateResult(", "void SdkHost::VerifyCode(",
        "void SdkHost::SendPasswordResetLink(", "void SdkHost::AuthLoginWithSso(",
        "void SdkHost::AuthLoginWithWallet("}) {
    UR_EXPECT_TRUE_MSG(signature, Has(Body(host, signature), "NoteApiError(err);"));
  }
  const std::string note = Body(host, "void SdkHost::NoteApiError(");
  // the SDK's one rule for every app (sdk cant_reach.go; support inbox 7956, 7973)
  UR_EXPECT_TRUE(Has(note, "urnet::isCantReachError(*err)"));
  UR_EXPECT_FALSE(Has(note, "region::IsCantReachError"));
  UR_EXPECT_TRUE(Has(note, "onApiUnreachable_(*err)"));
}

// The window shows the prompt (login screen) or a toast (any other page), both
// opening the picker, and never sets the region.
UR_TEST(ConnectionRegionWiring_ThePromptOpensThePickerAndSetsNothing) {
  const std::string window = Source("MainWindow.cpp");
  UR_EXPECT_TRUE(Has(window, "host_.SetApiUnreachableHandler("));
  UR_EXPECT_TRUE(Has(window, "PostToMain([this, error] { OnApiUnreachable(error); });"));
  UR_EXPECT_TRUE(Has(Body(window, "void MainWindow::OnApiUnreachable("), "ShowCantReachPrompt();"));
  const std::string unreachable = Body(window, "void MainWindow::ShowCantReachPrompt(");
  UR_EXPECT_TRUE(Has(unreachable, "cantReach_.Raise();"));
  UR_EXPECT_TRUE(Has(unreachable, "cantReachBtn_->set_visible(true);"));
  UR_EXPECT_TRUE(Has(unreachable, "[this] { OpenConnectionRegion(); }"));
  UR_EXPECT_FALSE(Has(unreachable, "SetIngressCountryCode"));
  UR_EXPECT_TRUE(Has(window, "cantReachBtn_->signal_clicked().connect([this] { OpenConnectionRegion(); });"));
}

// Support inbox 8050: the window places a raised prompt by the page the user
// is on (ConnectionRegion.hpp PlaceCantReachPrompt). Gated on the signed-in
// page, the toast never reached a sign-in step past the login page, whose
// failure then showed only on the login page's line, out of sight.
UR_TEST(ConnectionRegionWiring_ThePromptIsPlacedByTheVisiblePage) {
  const std::string unreachable = Body(Source("MainWindow.cpp"), "void MainWindow::ShowCantReachPrompt(");
  UR_EXPECT_TRUE(Has(unreachable, "region::PlaceCantReachPrompt(stack_.get_visible_child_name().raw())"));
  UR_EXPECT_TRUE(Has(unreachable, "if (placement.loginLine) cantReachBtn_->set_visible(true);"));
  UR_EXPECT_FALSE(Has(unreachable, "!= \"home\""));
  const size_t gate = unreachable.find("if (!placement.toast) return;");
  const size_t toast = unreachable.find("cantReachToast_.Show(stack_");
  UR_EXPECT_TRUE(gate != std::string::npos && toast != std::string::npos && gate < toast);
}

// Support inbox 7956, 7973: signed in, Windows alone raised the prompt as the
// connect window's diagnosis turned to the platform being unreachable. The
// window asks the SDK's rule with the reading before this one, ahead of
// replacing it, and a turn raises the same prompt a failed call does.
UR_TEST(ConnectionRegionWiring_ASignedInStallRaisesThePromptByTheSdksRule) {
  const std::string window = Source("MainWindow.cpp");
  const std::string apply = Body(window, "void MainWindow::ApplyConnectReading(");
  const size_t ask = apply.find("urnet::cantReachOnStall(reading_.stallReason, reading.stallReason)");
  const size_t replace = apply.find("reading_ = reading;");
  UR_EXPECT_TRUE(ask != std::string::npos && replace != std::string::npos && ask < replace);
  UR_EXPECT_TRUE(Has(apply, "if (platformTurnedUnreachable && signedIn_) OnPlatformUnreachable();"));
  UR_EXPECT_TRUE(Has(Body(window, "void MainWindow::OnPlatformUnreachable("), "ShowCantReachPrompt();"));
  // the reading carries the SDK's diagnosis of the forming window
  UR_EXPECT_TRUE(Has(Source("SdkHost.cpp"), "r.stallReason = windowStatus->StallReason;"));
}

// Both doors open ConnectionRegionSheet over SdkHost, the one store.
UR_TEST(ConnectionRegionWiring_BothDoorsOpenTheOnePicker) {
  const std::string window = Source("MainWindow.cpp");
  UR_EXPECT_TRUE(Has(window,
                     "connectionRegionBtn_->signal_clicked().connect([this] { OpenConnectionRegion(); });"));
  UR_EXPECT_TRUE(Has(Body(window, "void MainWindow::OpenConnectionRegion("),
                     "std::make_unique<ConnectionRegionSheet>(*this, host_)"));
  UR_EXPECT_TRUE(Has(Body(window, "void MainWindow::RefreshConnectionRegionControl("),
                     "T_(\"connecting_from\", \"Connecting from\")"));

  const std::string dns = Source("DnsSheet.cpp");
  UR_EXPECT_TRUE(Has(dns, "T_(\"connection_region\", \"Connection region\")"));
  UR_EXPECT_TRUE(Has(dns, "T_(\"traffic_dns\", \"Traffic DNS\")"));
  UR_EXPECT_TRUE(Has(Body(dns, "void DnsSheet::OpenConnectionRegion("),
                     "std::make_unique<ConnectionRegionSheet>(*this, host_)"));
  UR_EXPECT_TRUE(Has(Body(dns, "bool DnsSheet::Open("), "RefreshConnectionRegion();"));

  const std::string sheet = Source("ConnectionRegionSheet.hpp");
  UR_EXPECT_TRUE(Has(sheet, "return host_.GetIngressCountryCode();"));
  UR_EXPECT_TRUE(Has(sheet, "return host_.SetIngressCountryCode(countryCode);"));
  const std::string picker = Source("ConnectionRegionSheet.cpp");
  UR_EXPECT_TRUE(Has(picker, "SdkHost::IngressCountryCodes()"));
  UR_EXPECT_TRUE(Has(picker, "SdkHost::IngressDohUrls(countryCode)"));
  UR_EXPECT_TRUE(Has(picker, "SdkHost::IngressPlainDnsServers(countryCode)"));
  UR_EXPECT_TRUE(Has(picker, "const std::string stored = picker_.Select(i);"));
}

// The English is the msgid T_ looks the translation up by.
UR_TEST(ConnectionRegionWiring_EveryTextIsTheCatalogs) {
  const std::string catalog = ReadFile(std::string(UR_SRC_DIR) + "/../po/en.po");
  UR_EXPECT_TRUE(!catalog.empty());
  const std::string all = Source("ConnectionRegionSheet.cpp") + Source("MainWindow.cpp") +
                          Source("DnsSheet.cpp");
  for (const auto& [key, english] : {
           std::pair<const char*, const char*>{"connection_region", "Connection region"},
           {"connection_region_description",
            "Only affects how the app reaches URnetwork, not your traffic."},
           {"connecting_from", "Connecting from"},
           {"connection_region_undisclosed", "Other / Undisclosed"},
           {"cant_reach_urnetwork_choose_region",
            "Can't reach URnetwork — choose where you're connecting from"},
           {"where_are_you_connecting_from", "Where are you connecting from?"},
           {"connection_region_encrypted_resolvers", "Encrypted resolvers"},
           {"connection_region_default_resolvers", "Default resolvers"},
           {"connection_region_fallback_resolvers", "Unencrypted fallback (URnetwork names only)"},
           {"traffic_dns", "Traffic DNS"}}) {
    const std::string entry =
        std::string("msgctxt \"") + key + "\"\nmsgid \"" + english + "\"\n";
    UR_EXPECT_TRUE_MSG(entry, Has(catalog, entry));
    UR_EXPECT_TRUE_MSG(std::string("not shown: ") + key, Has(all, std::string("\"") + key + "\""));
  }
}
