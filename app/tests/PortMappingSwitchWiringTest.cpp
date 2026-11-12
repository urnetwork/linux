// The call sites behind the router port mapping switch (connect EXTENDER.md
// S8): directly under the connect page's lag.dev speed tests switch, whether
// the extender role asks the router, by UPnP, NAT-PMP or PCP, to forward its
// ports (tcp 443, udp 443 and udp 4053) to this device. Off by default on every
// app (owner, 2026-11-11). It is the speed tests switch's twin:
//
//   * the GUI reads and writes it through a bound DeviceRemote, and with none
//     through the daemon (set_provide_extender_port_mapping) only while
//     provider_stats said it takes that write
//     (provide::PortMappingSwitchSourceFor), and reads off whenever nothing
//     answers;
//   * the daemon takes the verb behind set_provide_extender's owner gate,
//     writes it through the device that runs, refuses with none (the sdk
//     writes the setting only through a device), and reports it beside the
//     role;
//   * the page draws it beside the Extender row as just drawn, directly under
//     the speed tests switch, hidden, never disabled, off until a reading says
//     otherwise, and never writes it while hidden (N1).
//
// The pure halves are tested in ControlProtocolTest.cpp, ProvideLifecycleTest.cpp
// and ExtenderProvidePresentationTest.cpp; these need glib, GTK and the SDK, so
// this reads their sources.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"
#include "WiringSource.hpp"

#include <string>

#include "ControlClient.hpp"

namespace {

using namespace urnw::testing::wiring;

// The text from `from` up to the next `to` after it; "" when `from` is absent.
std::string From(const std::string& text, const std::string& from, const std::string& to) {
  const size_t at = text.find(from);
  if (at == std::string::npos) return std::string();
  return text.substr(at, text.find(to, at + 1) - at);
}

}  // namespace

// The daemon: the verb behind set_provide_extender's gate, the write through
// the device that runs and never into the space, and the setting beside the
// role.
UR_TEST(PortMapping_TheDaemonTakesTheSwitch) {
  const std::string server = ReadCode("daemon/ControlServer.cpp");
  const std::string authorized = FunctionBody(server, "void ControlServer::DispatchAuthorized(");
  UR_EXPECT_TRUE(Contains(authorized, "case ctl::Verb::SetProvideExtenderPortMapping: {"));
  const std::string branch =
      From(authorized, "case ctl::Verb::SetProvideExtenderPortMapping: {", "case ctl::Verb::");
  UR_EXPECT_TRUE(
      Before(branch, "CheckTunnelOwner(conn, id, &denied, &crossUid, authorizedCrossUid)",
             "tunnel_.SetProvideExtenderPortMapping(req.provide_extender_port_mapping, &error)"));
  UR_EXPECT_TRUE(
      Before(branch, "request.get<ctl::SetProvideExtenderPortMappingRequest>()",
             "tunnel_.SetProvideExtenderPortMapping(req.provide_extender_port_mapping, &error)"));
  // a write nothing took is an error reply, never ok
  UR_EXPECT_TRUE(Before(branch, "ctl::MakeErrorReply(", "reply(ctl::MakeReply(id, true));"));
  // its own setting only
  UR_EXPECT_FALSE(Contains(branch, "tunnel_.SetProvideExtender("));
  UR_EXPECT_FALSE(Contains(branch, "SetMeasurementsEnabled"));

  const std::string host = ReadCode("daemon/TunnelHost.cpp");
  const std::string write = FunctionBody(
      host, "bool TunnelHost::SetProvideExtenderPortMapping(bool on, std::string* error)");
  // refused while a bring-up owns the session, never waited behind
  UR_EXPECT_TRUE(Before(write, "std::unique_lock<std::mutex> lock(opMutex_, std::try_to_lock);",
                        "provide::ExtenderSettingTargetFor("));
  UR_EXPECT_TRUE(Contains(write, "providerDevice_->setProvideExtenderPortMapping(on);"));
  UR_EXPECT_TRUE(Contains(write, "device_->setProvideExtenderPortMapping(on);"));
  UR_EXPECT_FALSE(Contains(write, "setProvideExtender("));
  UR_EXPECT_FALSE(Contains(write, "setMeasurementsEnabled("));
  // With no device running it is refused, never reported as written: the
  // sdk's LocalState has no writer for this setting, so nothing writes the
  // space directly.
  UR_EXPECT_FALSE(Contains(write, "getLocalState()"));
  const std::string spaceCase = From(write, "case provide::ExtenderSettingTarget::NetworkSpace:",
                                     "case provide::ExtenderSettingTarget::None:");
  UR_EXPECT_TRUE(Before(spaceCase, "*error = ", "return false;"));
  UR_EXPECT_FALSE(Contains(spaceCase, "break;"));
  const std::string noneCase =
      From(write, "case provide::ExtenderSettingTarget::None:", "} catch (");
  UR_EXPECT_TRUE(Before(noneCase, "*error = ", "return false;"));
  UR_EXPECT_FALSE(Contains(noneCase, "return true;"));

  // reported beside the setting the speed tests switch shows, said to be
  // written only once read, and left off and unwritable when the read throws
  const std::string stats =
      FunctionBody(host, "ctl::ProviderStatsReply TunnelHost::ProviderStats(");
  UR_EXPECT_TRUE(Before(stats, "reply.measurements_enabled_writable = true;",
                        "reply.provide_extender_port_mapping = "
                        "providerDevice_->getProvideExtenderPortMapping();"));
  UR_EXPECT_TRUE(Before(stats,
                        "reply.provide_extender_port_mapping = "
                        "providerDevice_->getProvideExtenderPortMapping();",
                        "reply.provide_extender_port_mapping_writable = true;"));
  UR_EXPECT_TRUE(Before(stats, "reply.provide_extender_port_mapping_writable = true;",
                        "noteReadFailure(\"router port mapping setting\", e);"));
}

// The GUI: read and written by the switch's own source, off whenever nothing
// answers, the daemon's write read back at once, and the client's request
// gated and retried as the speed tests switch's.
UR_TEST(PortMapping_TheGuiReadsAndWritesByItsOwnSource) {
  const std::string host = ReadCode("SdkHost.cpp");
  const std::string source = FunctionBody(
      host, "provide::ExtenderSwitchSource SdkHost::PortMappingSwitchSourceLocked() const");
  UR_EXPECT_TRUE(Contains(source, "provide::PortMappingSwitchSourceFor("));
  UR_EXPECT_TRUE(Contains(source, "daemonProviderStats_->provideExtenderPortMappingWritable"));
  UR_EXPECT_TRUE(Contains(source, "daemonProviderStats_->provideExtenderWritable"));
  UR_EXPECT_FALSE(Contains(source, "measurementsEnabledWritable"));

  const std::string writable = FunctionBody(host, "bool SdkHost::PortMappingSwitchWritable()");
  UR_EXPECT_TRUE(
      Contains(writable, "PortMappingSwitchSourceLocked() != provide::ExtenderSwitchSource::None"));

  const std::string get = FunctionBody(host, "bool SdkHost::GetProvideExtenderPortMapping()");
  UR_EXPECT_TRUE(Contains(get, "PortMappingSwitchSourceLocked()"));
  UR_EXPECT_TRUE(Contains(get, "device_->getProvideExtenderPortMapping()"));
  UR_EXPECT_TRUE(Contains(get, "daemonProviderStats_->provideExtenderPortMapping"));
  // the apps' default, off, when nothing takes the switch's write
  UR_EXPECT_TRUE(Before(get, "case provide::ExtenderSwitchSource::None:", "return false;"));
  UR_EXPECT_FALSE(Contains(get, "return true;"));
  UR_EXPECT_FALSE(Contains(get, "MeasurementsSwitchSourceLocked()"));

  const std::string set =
      FunctionBody(host, "void SdkHost::SetProvideExtenderPortMapping(bool on)");
  UR_EXPECT_TRUE(Contains(set, "PortMappingSwitchSourceLocked()"));
  UR_EXPECT_TRUE(Contains(set, "device_->setProvideExtenderPortMapping(on);"));
  UR_EXPECT_TRUE(Before(set, "control_.SetProvideExtenderPortMapping(on, &error)",
                        "PollDaemonProviderStatsLocked();"));
  UR_EXPECT_TRUE(Before(set, "PollDaemonProviderStatsLocked();",
                        "EmitDrawerEvent(DrawerEvent::ExtenderProvideStatus);"));
  UR_EXPECT_FALSE(Contains(set, "ExtenderSwitchSourceLocked()"));
  UR_EXPECT_FALSE(Contains(set, "MeasurementsSwitchSourceLocked()"));
  UR_EXPECT_FALSE(Contains(set, "setMeasurementsEnabled("));

  // a poll that moves only this setting or its writer still redraws the page
  const std::string poll = FunctionBody(host, "void SdkHost::PollDaemonProviderStatsLocked()");
  UR_EXPECT_TRUE(
      Contains(poll, "stats.provideExtenderPortMapping = reply->provide_extender_port_mapping;"));
  UR_EXPECT_TRUE(Contains(poll, "stats.provideExtenderPortMappingWritable = "
                                "reply->provide_extender_port_mapping_writable;"));
  UR_EXPECT_TRUE(Before(poll, "const bool extenderChanged =",
                        "daemonProviderStats_->provideExtenderPortMapping != "
                        "stats.provideExtenderPortMapping"));
  UR_EXPECT_TRUE(Before(poll, "const bool extenderChanged =",
                        "daemonProviderStats_->provideExtenderPortMappingWritable !="));
  UR_EXPECT_TRUE(Before(poll, "daemonProviderStats_->provideExtenderPortMappingWritable !=",
                        "daemonProviderStats_ = std::move(stats);"));

  const std::string client = ReadCode("ControlClient.cpp");
  const std::string send =
      FunctionBody(client, "bool ControlClient::SetProvideExtenderPortMapping(bool on,");
  UR_EXPECT_TRUE(Contains(send, "req.provide_extender_port_mapping = on;"));
  UR_EXPECT_TRUE(Contains(
      send, "CallLocked(ctl::Verb::SetProvideExtenderPortMapping, nlohmann::json(req), error,"));
  UR_EXPECT_TRUE(Contains(send, "PolkitAwareTimeoutLocked()"));
  // a successful reply is a grant, so the authorization verdict reads Authorized
  UR_EXPECT_TRUE(urnw::VerbNeedsAuthorization(urnw::ctl::Verb::SetProvideExtenderPortMapping));
}

// The connect page: drawn beside the Extender row as just drawn and directly
// under the speed tests switch, the setting read only for a row that shows,
// off until a reading says otherwise, never written while hidden, and named by
// the catalog's strings.
UR_TEST(PortMapping_TheConnectPageDrawsItUnderTheSpeedTestsSwitch) {
  const std::string page = ReadCode("ConnectPage.cpp");
  const std::string apply = FunctionBody(page, "void ConnectPage::ApplyExtenderProvideState()");
  UR_EXPECT_TRUE(Before(apply, "DrawExtenderRow(", "DrawPortMappingRow("));
  UR_EXPECT_TRUE(Before(apply, "DrawSpeedTestsRow(", "DrawPortMappingRow("));
  UR_EXPECT_TRUE(Contains(apply, "extender::PortMappingRowOf("));
  UR_EXPECT_TRUE(Contains(apply, "extenderRowDrawn_, host_.PortMappingSwitchWritable(),"));
  UR_EXPECT_TRUE(Contains(apply, "[this] { return host_.GetProvideExtenderPortMapping(); }"));

  const std::string draw = FunctionBody(
      page, "void ConnectPage::DrawPortMappingRow(const extender::PortMappingRow& row)");
  UR_EXPECT_TRUE(Contains(draw, "portMappingRow_->set_visible(row.visible);"));
  UR_EXPECT_TRUE(
      Before(draw, "updatingControls_ = true;", "portMappingToggle_->set_active(row.on);"));
  UR_EXPECT_FALSE(Contains(draw, "set_sensitive("));
  UR_EXPECT_FALSE(Contains(draw, "SetProvideExtenderPortMapping("));

  const std::string toggled = FunctionBody(page, "void ConnectPage::OnPortMappingToggled()");
  UR_EXPECT_TRUE(Before(toggled, "if (!portMappingDrawn_.visible) return;",
                        "host_.SetProvideExtenderPortMapping(on);"));
  UR_EXPECT_FALSE(Contains(toggled, "SetMeasurementsEnabled("));

  // the catalog's title, and its description under it
  UR_EXPECT_TRUE(Contains(page, "T_(\"extender_port_mapping\", \"Open a port on my router\")"));
  UR_EXPECT_TRUE(Before(page, "T_(\"extender_port_mapping\", \"Open a port on my router\")",
                        "\"extender_port_mapping_description\""));
  // the switch's handler is echo-guarded, as every feed-driven switch is
  UR_EXPECT_TRUE(Contains(page, "if (updatingControls_) return;\n      OnPortMappingToggled();"));
  // off and hidden until the first reading draws it: the apps' default
  UR_EXPECT_TRUE(Contains(page, "portMappingToggle_->set_active(false);"));
  UR_EXPECT_FALSE(Contains(page, "portMappingToggle_->set_active(true);"));
  UR_EXPECT_TRUE(Contains(page, "portMappingRow_->set_visible(false);"));
  // directly under the speed tests switch, nothing appended between them
  const std::string under = From(page, "moreOptionsHost_->append(*speedTestsRow_);",
                                 "moreOptionsHost_->append(*portMappingRow_);");
  UR_EXPECT_TRUE(Before(page, "moreOptionsHost_->append(*speedTestsRow_);",
                        "moreOptionsHost_->append(*portMappingRow_);"));
  UR_EXPECT_EQ(1, int(CountOf(under, "moreOptionsHost_->append(")));
}
