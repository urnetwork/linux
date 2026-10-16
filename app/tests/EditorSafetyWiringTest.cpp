// The DNS and transport editors never overwrite settings they did not see
// (apple's editor safety, APPLE_FIXES.md "Added 2026-10-16"). Each Update is a
// compare-and-set through SdkHost (SetDnsResolverSettingsIf,
// SetTransportSettingsIf): written only while the settings still read what the
// editor was seeded from, checked and written under one hold of SdkHost's
// lock, and otherwise the form re-seeds from what they read now. An editor
// whose settings are unknown does not apply: the DNS editor does not open, and
// the transport editor keeps Update off while a bound device's policy cannot
// be read (it drafts from the default, which is not the daemon's policy).
//
// Before, both Updates wrote their draft unconditionally, so an editor opened
// over settings that changed since (another surface, the daemon's sync) or
// over a failed read (the transport editor's default) overwrote the real
// resolver or policy. The editors are GTK, so their call sites are read from
// the sources, the way the other wiring tests read them.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"
#include "WiringSource.hpp"

#include <string>

namespace {

namespace wiring = urnw::testing::wiring;

}  // namespace

UR_TEST(EditorSafety_TheDnsUpdateIsACompareAndSet) {
  const std::string sheet = wiring::ReadCode("DnsSheet.cpp");
  // no unconditional write is left in the editor
  UR_EXPECT_FALSE(wiring::Contains(sheet, "host_.SetDnsResolverSettings("));
  UR_EXPECT_TRUE(wiring::Contains(sheet, "host_.SetDnsResolverSettingsIf("));
  // compared with what the editor was seeded from, and re-seeded when moved
  const std::string build = wiring::FunctionBody(sheet, "void DnsSheet::BuildUi() {");
  UR_EXPECT_TRUE(wiring::Before(build, "const urnet::DnsResolverSettings shown = original_;",
                                "host_.SetDnsResolverSettingsIf("));
  UR_EXPECT_TRUE(wiring::Before(build, "return SettingsEqual(normalized, shown);", "Seed(*current);"));
  // unknown settings open no editor
  const std::string open = wiring::FunctionBody(sheet, "bool DnsSheet::Open() {");
  UR_EXPECT_TRUE(wiring::Before(open, "if (!settings) return false;", "Seed(*settings);"));
}

UR_TEST(EditorSafety_TheTransportUpdateIsACompareAndSet) {
  const std::string sheet = wiring::ReadCode("TransportSheet.cpp");
  UR_EXPECT_FALSE(wiring::Contains(sheet, "host_.SetTransportSettings("));
  UR_EXPECT_FALSE(wiring::Contains(sheet, "host_.SetProviderTransportSettings("));
  const std::string build = wiring::FunctionBody(sheet, "void TransportSheet::BuildUi() {");
  UR_EXPECT_TRUE(wiring::Before(build, "if (!draft_ || unknown_) {", "host_.SetTransportSettingsIf("));
  UR_EXPECT_TRUE(wiring::Contains(build, "return urnet::transportSettingsEqual(now ? now : fallback, shown);"));
  UR_EXPECT_TRUE(wiring::Before(build, "host_.SetTransportSettingsIf(", "draft_ = original_;"));
  // a bound device whose policy cannot be read is unknown, and Update stays off
  const std::string open = wiring::FunctionBody(sheet, "void TransportSheet::Open() {");
  UR_EXPECT_TRUE(wiring::Contains(open, "unknown_ = !read && host_.hasDevice();"));
  UR_EXPECT_TRUE(wiring::Contains(
      sheet, "updateBtn_->set_sensitive(!unknown_ && !urnet::transportSettingsEqual(draft_, original_));"));
}

UR_TEST(EditorSafety_TheHostComparesAndWritesUnderOneLock) {
  const std::string host = wiring::ReadCode("SdkHost.cpp");
  const std::string dns = wiring::FunctionBody(host, "bool SdkHost::SetDnsResolverSettingsIf(");
  UR_EXPECT_TRUE(wiring::Before(dns, "std::scoped_lock lock(mutex_);", "GetDnsResolverSettingsLocked();"));
  UR_EXPECT_TRUE(wiring::Before(dns, "if (!stillShown(now)) {", "SetDnsResolverSettingsLocked(settings);"));
  UR_EXPECT_EQ(1, int(wiring::CountOf(dns, "scoped_lock")));
  const std::string transport = wiring::FunctionBody(host, "bool SdkHost::SetTransportSettingsIf(");
  UR_EXPECT_TRUE(wiring::Before(transport, "std::scoped_lock lock(mutex_);", "if (!stillShown(now)) {"));
  UR_EXPECT_TRUE(wiring::Before(transport, "if (!stillShown(now)) {", "SetProviderTransportSettingsLocked(settings);"));
  UR_EXPECT_TRUE(wiring::Before(transport, "if (!stillShown(now)) {", "SetTransportSettingsLocked(settings);"));
  UR_EXPECT_EQ(1, int(wiring::CountOf(transport, "scoped_lock")));
}
