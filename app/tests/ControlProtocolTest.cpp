// Control-protocol unit tests (linux/MIGRATION.md contract): frame round-trip
// encode/decode for every verb, the version-negotiation accept/reject matrix
// in BOTH rejection directions (the check Windows declared and never enforced
// — APPIMAGE.md §11b), and the SO_PEERCRED uid/group authorization decision,
// factored pure so it needs no socket. SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "ControlProtocol.hpp"

namespace ctl = urnw::ctl;
namespace provide = urnw::provide;

// ---- framing ---------------------------------------------------------------

UR_TEST(controlFrameIsOneJsonObjectPerLine) {
  const std::string frame = ctl::EncodeFrame(ctl::MakeRequest(ctl::Verb::Status, 7));
  UR_EXPECT_TRUE(!frame.empty());
  UR_EXPECT_TRUE(frame.back() == '\n');
  // exactly one newline: the terminator; dump() escapes any newline in strings
  UR_EXPECT_TRUE(frame.find('\n') == frame.size() - 1);
  auto decoded = ctl::DecodeFrame(frame);
  UR_EXPECT_TRUE(decoded.has_value());
  UR_EXPECT_TRUE(ctl::RequestVerb(*decoded) == ctl::Verb::Status);
  UR_EXPECT_TRUE(ctl::FrameId(*decoded) == 7);
}

UR_TEST(controlFrameStringsWithNewlinesStayOneLine) {
  ctl::StartTunnelRequest req;
  req.by_jwt = "line1\nline2";  // must be escaped, never a raw newline
  req.instance_id = "i";
  req.app_version = "v";
  const std::string frame =
      ctl::EncodeFrame(ctl::MakeRequest(ctl::Verb::StartTunnel, 1, nlohmann::json(req)));
  UR_EXPECT_TRUE(frame.find('\n') == frame.size() - 1);
  auto decoded = ctl::DecodeFrame(frame);
  UR_EXPECT_TRUE(decoded.has_value());
  const auto parsed = decoded->get<ctl::StartTunnelRequest>();
  UR_EXPECT_TRUE(parsed.by_jwt == "line1\nline2");
}

UR_TEST(controlFrameRejectsGarbageAndNonObjects) {
  UR_EXPECT_FALSE(ctl::DecodeFrame("not json").has_value());
  UR_EXPECT_FALSE(ctl::DecodeFrame("").has_value());
  UR_EXPECT_FALSE(ctl::DecodeFrame("[1,2,3]").has_value());  // an object is required
  UR_EXPECT_FALSE(ctl::DecodeFrame("42").has_value());
  UR_EXPECT_FALSE(ctl::DecodeFrame("\"hello\"").has_value());
  // trailing newline is accepted (the reader may or may not strip it)
  UR_EXPECT_TRUE(ctl::DecodeFrame("{}\n").has_value());
}

// The daemon replies with strings it did not write (SDK errors, tool output,
// SDK log lines). A byte that is not UTF-8 must cost one U+FFFD, never the
// frame: a plain dump() throws, and the daemon sends from glib callbacks.
UR_TEST(controlFrameInvalidUtf8BecomesReplacementCharacter) {
  const std::string invalid = "x\xe2\x82";  // a three-byte sequence cut after two
  const nlohmann::json reply = ctl::MakeErrorReply(9, invalid, ctl::kCodeTunOpenFailed);
  bool plainDumpThrew = false;
  try {
    (void)reply.dump();
  } catch (const nlohmann::json::type_error&) {
    plainDumpThrew = true;
  }
  UR_EXPECT_TRUE(plainDumpThrew);  // why EncodeFrame does not use the plain dump

  std::string frame;
  bool threw = false;
  try {
    frame = ctl::EncodeFrame(reply);
  } catch (const std::exception&) {
    threw = true;
  }
  UR_EXPECT_FALSE(threw);
  UR_EXPECT_TRUE(frame.find('\n') == frame.size() - 1);
  UR_EXPECT_TRUE(frame.find("\xef\xbf\xbd") != std::string::npos);
  const auto decoded = ctl::DecodeFrame(frame);
  UR_EXPECT_TRUE(decoded.has_value());
  if (decoded) {
    UR_EXPECT_TRUE(ctl::FrameId(*decoded) == 9);
    UR_EXPECT_FALSE(ctl::ReplyOk(*decoded));
    UR_EXPECT_TRUE(ctl::ReplyCode(*decoded) == ctl::kCodeTunOpenFailed);
    UR_EXPECT_TRUE(ctl::ReplyError(*decoded) == "x\xef\xbf\xbd");
  }
}

UR_TEST(controlFrameStatusWithInvalidUtf8KeepsItsFields) {
  ctl::StatusReply status;
  status.tunnel_state = ctl::TunnelState::Up;
  status.kill_switch = ctl::KillSwitchState::Armed;
  status.dns_detail = "resolvectl: \xff";
  status.kill_switch_detail = "nft: \xc3";
  nlohmann::json payload = status;
  std::string frame;
  bool threw = false;
  try {
    frame = ctl::EncodeFrame(ctl::MakeReply(4, true, payload));
  } catch (const std::exception&) {
    threw = true;
  }
  UR_EXPECT_FALSE(threw);
  const auto decoded = ctl::DecodeFrame(frame);
  UR_EXPECT_TRUE(decoded.has_value());
  if (decoded) {
    const auto parsed = decoded->get<ctl::StatusReply>();
    UR_EXPECT_TRUE(parsed.tunnel_state == ctl::TunnelState::Up);
    UR_EXPECT_TRUE(parsed.kill_switch == ctl::KillSwitchState::Armed);
    UR_EXPECT_TRUE(parsed.dns_detail == "resolvectl: \xef\xbf\xbd");
    UR_EXPECT_TRUE(parsed.kill_switch_detail == "nft: \xef\xbf\xbd");
  }
}

// Valid text, multibyte included, goes out byte for byte.
UR_TEST(controlFrameValidUtf8IsUnchanged) {
  const std::string text = "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x90\xb8";
  const auto decoded = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeErrorReply(1, text)));
  UR_EXPECT_TRUE(decoded.has_value());
  if (decoded) UR_EXPECT_TRUE(ctl::ReplyError(*decoded) == text);
}

UR_TEST(controlFrameUnknownVerbAndMissingIdAreExplicit) {
  auto j = ctl::DecodeFrame("{\"verb\":\"frobnicate\",\"id\":3}");
  UR_EXPECT_TRUE(j.has_value());
  UR_EXPECT_TRUE(ctl::RequestVerb(*j) == ctl::Verb::Unknown);
  auto noId = ctl::DecodeFrame("{\"verb\":\"status\"}");
  UR_EXPECT_TRUE(noId.has_value());
  UR_EXPECT_TRUE(ctl::FrameId(*noId) == -1);
}

// ---- verb + state name round-trips ----------------------------------------

UR_TEST(controlVerbNamesRoundTrip) {
  const ctl::Verb verbs[] = {
      ctl::Verb::Hello,          ctl::Verb::Status,
      ctl::Verb::StartTunnel,    ctl::Verb::AttachTunnel,
      ctl::Verb::StopTunnel,     ctl::Verb::SetProvide,
      ctl::Verb::StartProvider,  ctl::Verb::ProviderStats,
      ctl::Verb::SetProvideExtender, ctl::Verb::SetMeasurementsEnabled,
      ctl::Verb::SetProvideExtenderPortMapping,
      ctl::Verb::ResetExtenders,
      ctl::Verb::LocationOverrideAvailable,
      ctl::Verb::LocationOverrideWrite, ctl::Verb::LocationOverrideClear,
      ctl::Verb::UploadLogs,     ctl::Verb::Logout,
  };
  for (const ctl::Verb v : verbs) {
    UR_EXPECT_TRUE_MSG(ctl::ToString(v), ctl::VerbFromString(ctl::ToString(v)) == v);
  }
  // the MIGRATION.md wire names, literally
  UR_EXPECT_TRUE(ctl::VerbFromString("hello") == ctl::Verb::Hello);
  UR_EXPECT_TRUE(ctl::VerbFromString("status") == ctl::Verb::Status);
  UR_EXPECT_TRUE(ctl::VerbFromString("start_tunnel") == ctl::Verb::StartTunnel);
  UR_EXPECT_TRUE(ctl::VerbFromString("attach_tunnel") == ctl::Verb::AttachTunnel);
  UR_EXPECT_TRUE(ctl::VerbFromString("stop_tunnel") == ctl::Verb::StopTunnel);
  UR_EXPECT_TRUE(ctl::VerbFromString("set_provide") == ctl::Verb::SetProvide);
  UR_EXPECT_TRUE(ctl::VerbFromString("start_provider") == ctl::Verb::StartProvider);
  UR_EXPECT_TRUE(ctl::VerbFromString("provider_stats") == ctl::Verb::ProviderStats);
  UR_EXPECT_TRUE(ctl::VerbFromString("set_provide_extender") == ctl::Verb::SetProvideExtender);
  UR_EXPECT_TRUE(ctl::VerbFromString("set_measurements_enabled") ==
                 ctl::Verb::SetMeasurementsEnabled);
  UR_EXPECT_TRUE(ctl::VerbFromString("set_provide_extender_port_mapping") ==
                 ctl::Verb::SetProvideExtenderPortMapping);
  UR_EXPECT_TRUE(ctl::VerbFromString("reset_extenders") == ctl::Verb::ResetExtenders);
  UR_EXPECT_TRUE(ctl::VerbFromString("location_override_available") ==
                 ctl::Verb::LocationOverrideAvailable);
  UR_EXPECT_TRUE(ctl::VerbFromString("location_override_write") ==
                 ctl::Verb::LocationOverrideWrite);
  UR_EXPECT_TRUE(ctl::VerbFromString("location_override_clear") ==
                 ctl::Verb::LocationOverrideClear);
  UR_EXPECT_TRUE(ctl::VerbFromString("upload_logs") == ctl::Verb::UploadLogs);
  UR_EXPECT_TRUE(ctl::VerbFromString("logout") == ctl::Verb::Logout);
}

UR_TEST(controlTunnelStateNamesRoundTrip) {
  const ctl::TunnelState states[] = {
      ctl::TunnelState::Stopped, ctl::TunnelState::Starting, ctl::TunnelState::Up,
      ctl::TunnelState::Stopping, ctl::TunnelState::Error,
  };
  for (const ctl::TunnelState s : states) {
    UR_EXPECT_TRUE_MSG(ctl::ToString(s), ctl::TunnelStateFromString(ctl::ToString(s)) == s);
  }
  // unknown text degrades to Stopped, never throws
  UR_EXPECT_TRUE(ctl::TunnelStateFromString("warp-speed") == ctl::TunnelState::Stopped);
}

// ---- payload round-trips (every verb with a payload, both directions) ------

UR_TEST(controlHelloRoundTripCarriesProtocolAndSdkVersionBothWays) {
  // request
  ctl::HelloRequest req;
  req.protocol_version = 3;
  req.sdk_version = "sdk-2026.08.05";
  auto reqJson = ctl::MakeRequest(ctl::Verb::Hello, 1, nlohmann::json(req));
  auto reqBack = ctl::DecodeFrame(ctl::EncodeFrame(reqJson))->get<ctl::HelloRequest>();
  UR_EXPECT_EQ(3, reqBack.protocol_version);
  UR_EXPECT_TRUE(reqBack.sdk_version == "sdk-2026.08.05");
  // reply
  ctl::HelloReply reply;
  reply.protocol_version = 2;
  reply.daemon_version = "1.4.0";
  reply.sdk_version = "sdk-2026.08.05";
  auto replyJson = ctl::MakeReply(1, true, nlohmann::json(reply));
  auto decoded = ctl::DecodeFrame(ctl::EncodeFrame(replyJson));
  UR_EXPECT_TRUE(ctl::ReplyOk(*decoded));
  const auto replyBack = decoded->get<ctl::HelloReply>();
  UR_EXPECT_EQ(2, replyBack.protocol_version);
  UR_EXPECT_TRUE(replyBack.daemon_version == "1.4.0");
  UR_EXPECT_TRUE(replyBack.sdk_version == "sdk-2026.08.05");
}

UR_TEST(controlHelloWithoutProtocolVersionParsesAsZero) {
  // a pre-versioning (or broken) peer must land BELOW every minimum, so the
  // absent field must never default to the current version
  const auto req = ctl::DecodeFrame("{\"verb\":\"hello\",\"id\":1}")->get<ctl::HelloRequest>();
  UR_EXPECT_EQ(0, req.protocol_version);
  UR_EXPECT_FALSE(ctl::DaemonAcceptsClientProtocol(req.protocol_version));
  UR_EXPECT_TRUE(req.sdk_version.empty());  // absent -> "" -> fails closed below
  const auto reply = ctl::DecodeFrame("{\"id\":1,\"ok\":true}")->get<ctl::HelloReply>();
  UR_EXPECT_EQ(0, reply.protocol_version);
  UR_EXPECT_FALSE(ctl::ClientAcceptsDaemonProtocol(reply.protocol_version));
  UR_EXPECT_TRUE(reply.sdk_version.empty());
}

// The SDK-build agreement is a SEPARATE, stricter check from the control
// protocol version: the gob device RPC between DeviceRemote and DeviceLocal
// has no version field of its own and drifts silently (a renamed field
// decodes as zero), and Linux is the first platform where the two SDK copies
// update on independent schedules. Exact match only; unreported fails closed.
UR_TEST(versionSdkAgreementIsExactMatchAndFailsClosed) {
  UR_EXPECT_TRUE(ctl::SdkVersionsAgree("1.2.3", "1.2.3"));
  UR_EXPECT_FALSE(ctl::SdkVersionsAgree("1.2.3", "1.2.4"));   // any drift refuses
  UR_EXPECT_FALSE(ctl::SdkVersionsAgree("1.2.3", "1.2.3 "));  // no fuzzy matching
  UR_EXPECT_FALSE(ctl::SdkVersionsAgree("1.2.3", ""));  // peer predates the field
  UR_EXPECT_FALSE(ctl::SdkVersionsAgree("", "1.2.3"));  // our side unset: fail closed
  UR_EXPECT_FALSE(ctl::SdkVersionsAgree("", ""));       // two unknowns never "agree"
}

UR_TEST(controlStartTunnelRoundTrip) {
  ctl::StartTunnelRequest req;
  req.by_jwt = "jwt-abc";
  req.instance_id = "0f0f-1234";
  req.app_version = "2.3.4";
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::StartTunnel, 9, nlohmann::json(req))));
  UR_EXPECT_TRUE(ctl::RequestVerb(*back) == ctl::Verb::StartTunnel);
  const auto parsed = back->get<ctl::StartTunnelRequest>();
  UR_EXPECT_TRUE(parsed.by_jwt == "jwt-abc");
  UR_EXPECT_TRUE(parsed.instance_id == "0f0f-1234");
  UR_EXPECT_TRUE(parsed.app_version == "2.3.4");

  ctl::StartTunnelReply reply;
  reply.rpc_port = ctl::kDeviceRpcPort;
  const auto replyBack = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(9, true, nlohmann::json(reply))))->get<ctl::StartTunnelReply>();
  UR_EXPECT_EQ(12025, replyBack.rpc_port);
}

// instance_id is the DEVICE PAIRING KEY between the two processes: the GUI
// sends its localState instance id, the daemon constructs the DeviceLocal
// with exactly the received value, and the GUI hands the same value to
// newDeviceRemoteWithDefaults. The device rpc rejects a sync whose InstanceId
// differs from the DeviceLocal's (sdk/device_rpc.go:6545) — and that failure
// is a silent never-populates, so the wire field and its validation are
// pinned here.
UR_TEST(controlStartTunnelInstanceIdIsThePairingContract) {
  ctl::StartTunnelRequest sent;  // what SdkHost::StartTunnel builds
  sent.by_jwt = "jwt";
  sent.instance_id = "3f2a8c1e-instance";
  sent.app_version = "1.0.0";
  // The device-RPC pin is MANDATORY since the mTLS change: without all three
  // parts the daemon would fall back to an unpinned plaintext listener on
  // loopback, which is the whole thing that change exists to prevent.
  sent.rpc_server_pem = "-----BEGIN CERTIFICATE-----\nserver\n-----END CERTIFICATE-----\n";
  sent.rpc_client_cert_pem = "-----BEGIN CERTIFICATE-----\nclient\n-----END CERTIFICATE-----\n";
  sent.rpc_listen_hostport = "127.0.0.1:12042";
  // ...and so is the NAME of that generation, which is what attach_tunnel
  // matches later and what the daemon latches into StatusReply.
  sent.rpc_session_id = "7c9f0b2a-4e51-4d0a-9c33-6a1f5e2b8d40";
  // what the daemon's ControlServer parses and TunnelHost constructs with
  const auto received = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::StartTunnel, 1, nlohmann::json(sent))))
      ->get<ctl::StartTunnelRequest>();
  UR_EXPECT_TRUE(received.instance_id == sent.instance_id);
  UR_EXPECT_FALSE(ctl::ValidateStartTunnelRequest(received).has_value());

  // an id-less request must fail validation LOUDLY — a daemon-generated
  // fallback id would pair-mismatch and hang silently
  ctl::StartTunnelRequest noId = sent;
  noId.instance_id.clear();
  const auto idError = ctl::ValidateStartTunnelRequest(noId);
  UR_EXPECT_TRUE(idError.has_value());
  UR_EXPECT_TRUE(idError->message.find("instance_id") != std::string::npos);

  ctl::StartTunnelRequest noJwt = sent;
  noJwt.by_jwt.clear();
  const auto jwtError = ctl::ValidateStartTunnelRequest(noJwt);
  UR_EXPECT_TRUE(jwtError.has_value());
  UR_EXPECT_TRUE(jwtError->message.find("by_jwt") != std::string::npos);

  // FAIL CLOSED ON THE PIN. Each part is individually required: a request that
  // drops any one of them must be REFUSED, never quietly served over an
  // unpinned plaintext listener. This is the assertion whose absence let the
  // stale version of this test pass while the pin was not enforced at all.
  for (int part = 0; part < 4; ++part) {
    ctl::StartTunnelRequest noPin = sent;
    if (part == 0) noPin.rpc_server_pem.clear();
    if (part == 1) noPin.rpc_client_cert_pem.clear();
    if (part == 2) noPin.rpc_listen_hostport.clear();
    if (part == 3) noPin.rpc_session_id.clear();
    UR_EXPECT_TRUE(ctl::ValidateStartTunnelRequest(noPin).has_value());
  }
  // A non-loopback listener must be refused outright — the pin is worthless if
  // the daemon can be told to listen on a routable address.
  ctl::StartTunnelRequest offBox = sent;
  offBox.rpc_listen_hostport = "0.0.0.0:12042";
  UR_EXPECT_TRUE(ctl::ValidateStartTunnelRequest(offBox).has_value());
  // ...and so must a malformed PEM, which is how a handle-0 key generation
  // arrives: four empty getters that still look like "something was returned".
  ctl::StartTunnelRequest badPem = sent;
  badPem.rpc_server_pem = "not a pem";
  UR_EXPECT_TRUE(ctl::ValidateStartTunnelRequest(badPem).has_value());
}

// THE GENERATION IS ATOMIC — name and material are minted together, travel
// together and are validated together. That coupling is the whole reason a
// session can be re-attached to by name at all: the daemon latches this id
// beside the material it pinned, so an id that arrives without material would
// name a listener that was never created, and material that arrives without an
// id would build a session no relaunched GUI could ever ask for again (it would
// silently rebuild the tunnel instead, which is exactly what adoption exists to
// prevent).
UR_TEST(controlStartTunnelSessionIdTravelsWithTheMaterial) {
  ctl::StartTunnelRequest sent;
  sent.by_jwt = "jwt";
  sent.instance_id = "3f2a8c1e-instance";
  sent.app_version = "1.0.0";
  sent.rpc_server_pem = "-----BEGIN CERTIFICATE-----\nserver\n-----END CERTIFICATE-----\n";
  sent.rpc_client_cert_pem = "-----BEGIN CERTIFICATE-----\nclient\n-----END CERTIFICATE-----\n";
  sent.rpc_listen_hostport = "127.0.0.1:12042";
  sent.rpc_session_id = "7c9f0b2a-4e51-4d0a-9c33-6a1f5e2b8d40";
  UR_EXPECT_FALSE(ctl::ValidateStartTunnelRequest(sent).has_value());

  // it survives the wire, or the daemon would latch an empty name onto a live
  // session and every later attach would miss
  const auto received = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::StartTunnel, 2, nlohmann::json(sent))))
      ->get<ctl::StartTunnelRequest>();
  UR_EXPECT_TRUE(received.rpc_session_id == sent.rpc_session_id);

  // material with no name: refused, and with the SAME code as a missing pin —
  // the client's fix is identical, generate a whole generation
  ctl::StartTunnelRequest unnamed = sent;
  unnamed.rpc_session_id.clear();
  const auto unnamedError = ctl::ValidateStartTunnelRequest(unnamed);
  UR_EXPECT_TRUE(unnamedError.has_value());
  UR_EXPECT_TRUE(unnamedError->code == ctl::kCodeRpcPinRequired);
  UR_EXPECT_TRUE(unnamedError->message.find("rpc_session_id") != std::string::npos);

  // a name with no material: refused too. This is the direction that would
  // otherwise ask the daemon to publish an identity for a listener it never
  // pinned.
  ctl::StartTunnelRequest nameOnly;
  nameOnly.by_jwt = "jwt";
  nameOnly.instance_id = "3f2a8c1e-instance";
  nameOnly.rpc_session_id = sent.rpc_session_id;
  UR_EXPECT_TRUE(ctl::ValidateStartTunnelRequest(nameOnly).has_value());

  // SHAPE, not entropy: nothing on the wire can prove a value was drawn from a
  // CSPRNG, but a control byte or an unbounded blob is not an identifier. The
  // newline case is the one with teeth — the daemon writes refusals to a
  // line-oriented journal.
  for (const std::string& bad : {std::string("with space"), std::string("line\nbreak"),
                                std::string("nul\0byte", 8), std::string(129, 'a')}) {
    ctl::StartTunnelRequest malformed = sent;
    malformed.rpc_session_id = bad;
    const auto error = ctl::ValidateStartTunnelRequest(malformed);
    UR_EXPECT_TRUE(error.has_value());
    UR_EXPECT_TRUE(error->code == ctl::kCodeRpcPinInvalid);
  }
  // and the alphabet stays open: uuid, hex and base64url all pass, so this
  // gate never dictates the client's format
  for (const std::string& good : {std::string("7c9f0b2a-4e51-4d0a-9c33-6a1f5e2b8d40"),
                                 std::string(64, 'f'), std::string("a-_9AZ=="),
                                 std::string(128, 'x')}) {
    UR_EXPECT_TRUE(ctl::LooksLikeRpcSessionId(good));
  }
  UR_EXPECT_FALSE(ctl::LooksLikeRpcSessionId(""));
}

// ---- attach_tunnel ---------------------------------------------------------

// Upstream's own contract, restored: an attach names a session, so BOTH
// identifiers are load-bearing and neither may default to empty. A half-named
// attach would either pair with whatever is up under the same instance id
// after a credential rotation, or pin a session without pinning the device.
UR_TEST(controlAttachTunnelRequiresBothLiveIdentifiers) {
  ctl::AttachTunnelRequest sent;
  sent.instance_id = "3f2a8c1e-instance";
  sent.rpc_session_id = "session-1";
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::AttachTunnel, 3, nlohmann::json(sent))))
                        ->get<ctl::AttachTunnelRequest>();
  UR_EXPECT_TRUE(back.instance_id == sent.instance_id);
  UR_EXPECT_TRUE(back.rpc_session_id == sent.rpc_session_id);
  UR_EXPECT_FALSE(ctl::ValidateAttachTunnelRequest(back).has_value());

  ctl::AttachTunnelRequest noSession = back;
  noSession.rpc_session_id.clear();
  UR_EXPECT_TRUE(ctl::ValidateAttachTunnelRequest(noSession).has_value());
  ctl::AttachTunnelRequest noInstance = back;
  noInstance.instance_id.clear();
  UR_EXPECT_TRUE(ctl::ValidateAttachTunnelRequest(noInstance).has_value());
}

// THE POLKIT DECISION, pinned. attach_tunnel takes over a running tunnel, so
// the tempting rule is "always kActionTakeOverTunnel". It is deliberately the
// same row as start_tunnel instead: reattaching to your OWN uid's tunnel is
// what start_tunnel's adoption path already does under control-tunnel, and
// charging an admin password for the verb that says so out loud would only
// teach clients to use the other door. The take-over price applies to the case
// that IS one — a live tunnel under a different uid.
UR_TEST(controlAttachTunnelIsPricedLikeStartNotAlwaysAsTakeOver) {
  UR_EXPECT_TRUE(ctl::ActionIdForVerb(ctl::Verb::AttachTunnel, /*is_log_tail=*/false,
                                      /*cross_uid=*/false) ==
                 std::string(ctl::kActionControlTunnel));
  UR_EXPECT_TRUE(ctl::ActionIdForVerb(ctl::Verb::AttachTunnel, /*is_log_tail=*/false,
                                      /*cross_uid=*/true) ==
                 std::string(ctl::kActionTakeOverTunnel));
  // identical to start_tunnel in both slots — that is the claim
  for (const bool cross : {false, true}) {
    UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::AttachTunnel, false, cross)) ==
                   std::string(ctl::ActionIdForVerb(ctl::Verb::StartTunnel, false, cross)));
  }
  // a human pressed something, so the agent dialog is allowed to appear
  UR_EXPECT_TRUE(ctl::VerbWantsInteraction(ctl::Verb::AttachTunnel, /*is_log_tail=*/false));
  // and a log_tail frame is still read-log whatever verb name it carries
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::AttachTunnel, true, false)) ==
                 std::string(ctl::kActionReadLog));
}

// THREE REFUSALS A CLIENT MUST BE ABLE TO TELL APART, because each one has a
// different next move and getting them confused destroys either a working
// tunnel or a good credential:
//   rpc_session_mismatch      — the session you named is not the one running.
//                               Your record is stale: discard it and build a
//                               new session with start_tunnel.
//   tunnel_already_running    — you named the LIVE session but presented
//                               different material. Do NOT retry as sent and do
//                               NOT expect a rebuild: mint a new generation
//                               (name and material together) and start again.
//   tunnel_owned_by_other_client — an ownership refusal, nothing to do with
//                               credentials at all.
// The code this test used to pin, rpc_session_not_persisted, is deliberately
// gone: the daemon now latches and publishes the live generation, so the state
// it named ("a tunnel is up and has no identity to compare") cannot occur.
UR_TEST(controlAttachTunnelRefusalsAreDistinguishable) {
  UR_EXPECT_TRUE(std::string(ctl::kCodeRpcSessionMismatch) !=
                 std::string(ctl::kCodeTunnelAlreadyRunning));
  UR_EXPECT_TRUE(std::string(ctl::kCodeTunnelAlreadyRunning) !=
                 std::string(ctl::kCodeTunnelOwnedByOtherClient));
  UR_EXPECT_TRUE(std::string(ctl::kCodeTunnelAlreadyRunning) == "tunnel_already_running");

  const auto mismatch = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeErrorReply(6, "running tunnel identity or RPC session does not match",
                          ctl::kCodeRpcSessionMismatch)));
  UR_EXPECT_TRUE(mismatch.has_value());
  UR_EXPECT_FALSE(ctl::ReplyOk(*mismatch));
  UR_EXPECT_TRUE(ctl::ReplyCode(*mismatch) == "rpc_session_mismatch");
  // the mismatch message must not report WHICH half differed — that would make
  // the verb an oracle for guessing a live session name
  UR_EXPECT_TRUE(ctl::ReplyError(*mismatch).find("instance_id") == std::string::npos);

  const auto alreadyRunning = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeErrorReply(
      5, "a tunnel is already running under this rpc session id with different pinning "
         "material", ctl::kCodeTunnelAlreadyRunning)));
  UR_EXPECT_TRUE(alreadyRunning.has_value());
  UR_EXPECT_FALSE(ctl::ReplyOk(*alreadyRunning));
  UR_EXPECT_TRUE(ctl::ReplyCode(*alreadyRunning) == "tunnel_already_running");
}

// The attach reply IS the start reply, so the live identity has to survive the
// round trip on both. And an absent rpc_session_id — what a daemon predating
// the fields sends, and what an ACCEPTED ASYNC START still sends, since the
// identity is latched at the up edge — must parse as empty rather than as
// anything a client could mistake for a session to send back.
UR_TEST(controlStartTunnelReplyCarriesTheLiveIdentity) {
  ctl::StartTunnelReply reply;
  reply.rpc_port = 12042;
  reply.tunnel_state = ctl::TunnelState::Up;
  reply.rpc_pinned = true;
  reply.instance_id = "3f2a8c1e-instance";
  reply.rpc_session_id = "session-1";
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(7, true, nlohmann::json(reply))))->get<ctl::StartTunnelReply>();
  UR_EXPECT_TRUE(back.instance_id == "3f2a8c1e-instance");
  UR_EXPECT_TRUE(back.rpc_session_id == "session-1");
  UR_EXPECT_TRUE(back.rpc_pinned);

  // a daemon predating the fields and an async start that has been accepted but
  // has not reached its up edge are indistinguishable on the wire, and both
  // parse empty — which a client must read as "nothing to attach to yet", never
  // as a session named ""
  const auto older = ctl::DecodeFrame("{\"id\":7,\"ok\":true,\"rpc_port\":12042}")
                         ->get<ctl::StartTunnelReply>();
  UR_EXPECT_TRUE(older.instance_id.empty());
  UR_EXPECT_TRUE(older.rpc_session_id.empty());
  UR_EXPECT_EQ(12042, older.rpc_port);
}

UR_TEST(controlStatusReplyRoundTrip) {
  ctl::StatusReply status;
  status.tunnel_state = ctl::TunnelState::Up;
  status.rpc_port = ctl::kDeviceRpcPort;
  status.client_id = "client-1";
  status.error = "";
  status.ipv6_blocked = true;
  status.ipv6_captured = true;
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(4, true, nlohmann::json(status))))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(back.tunnel_state == ctl::TunnelState::Up);
  UR_EXPECT_EQ(12025, back.rpc_port);
  UR_EXPECT_TRUE(back.client_id == "client-1");
  UR_EXPECT_TRUE(back.error.empty());
  UR_EXPECT_TRUE(back.ipv6_blocked);
  UR_EXPECT_TRUE(back.ipv6_captured);

  // An older daemon sends no ipv6_captured: silence understates protection.
  nlohmann::json older = nlohmann::json(status);
  older.erase("ipv6_captured");
  const auto fromOlder = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(5, true, older)))->get<ctl::StatusReply>();
  UR_EXPECT_FALSE(fromOlder.ipv6_captured);
}

UR_TEST(controlSetProvideRoundTrip) {
  ctl::SetProvideRequest req;
  req.mode = "network";
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::SetProvide, 5, nlohmann::json(req))));
  UR_EXPECT_TRUE(back->get<ctl::SetProvideRequest>().mode == "network");
}

// ---- start_provider: providing while disconnected (support inbox 1521) ------

namespace {

ctl::StartProviderRequest ProviderRequest(const char* mode) {
  ctl::StartProviderRequest req;
  req.by_jwt = "jwt";
  req.instance_id = "instance";
  req.app_version = "2026.10.5";
  req.provide_mode = mode;
  return req;
}

}  // namespace

UR_TEST(controlStartProviderRoundTrip) {
  ctl::StartProviderRequest req = ProviderRequest("always");
  req.network_space_json = "{\"host_name\":\"example.com\"}";
  req.provider_transport_settings_json = "{\"mode\":1}";
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::StartProvider, 9, nlohmann::json(req))));
  UR_EXPECT_TRUE(ctl::RequestVerb(*back) == ctl::Verb::StartProvider);
  const auto parsed = back->get<ctl::StartProviderRequest>();
  UR_EXPECT_TRUE(parsed.by_jwt == "jwt");
  UR_EXPECT_TRUE(parsed.instance_id == "instance");
  UR_EXPECT_TRUE(parsed.app_version == "2026.10.5");
  UR_EXPECT_TRUE(parsed.network_space_json == req.network_space_json);
  UR_EXPECT_TRUE(parsed.provide_mode == "always");
  UR_EXPECT_TRUE(parsed.provider_transport_settings_json == req.provider_transport_settings_json);
}

// Both halves refuse the same requests: a missing credential, a mode that does
// not provide while disconnected, and a provider policy that is not an object.
UR_TEST(controlStartProviderValidationIsTheLifecycleRule) {
  for (const char* mode : {"always", "network", "auto"}) {
    UR_EXPECT_TRUE_MSG(mode, !ctl::ValidateStartProviderRequest(ProviderRequest(mode)));
  }
  for (const char* mode : {"never", "manual", "", "Always"}) {
    const auto invalid = ctl::ValidateStartProviderRequest(ProviderRequest(mode));
    UR_EXPECT_TRUE_MSG(mode, invalid.has_value());
    UR_EXPECT_TRUE_MSG(mode, invalid && invalid->code != nullptr &&
                                 std::string(invalid->code) == ctl::kCodeProvideModeOff);
  }
  ctl::StartProviderRequest noJwt = ProviderRequest("always");
  noJwt.by_jwt.clear();
  UR_EXPECT_TRUE(ctl::ValidateStartProviderRequest(noJwt).has_value());
  ctl::StartProviderRequest noInstance = ProviderRequest("always");
  noInstance.instance_id.clear();
  UR_EXPECT_TRUE(ctl::ValidateStartProviderRequest(noInstance).has_value());
  ctl::StartProviderRequest badSettings = ProviderRequest("always");
  badSettings.provider_transport_settings_json = "[1,2]";
  UR_EXPECT_TRUE(ctl::ValidateStartProviderRequest(badSettings).has_value());
  badSettings.provider_transport_settings_json = "not json";
  UR_EXPECT_TRUE(ctl::ValidateStartProviderRequest(badSettings).has_value());
  badSettings.provider_transport_settings_json = "{}";
  UR_EXPECT_FALSE(ctl::ValidateStartProviderRequest(badSettings).has_value());
}

// Priced like set_provide (control-tunnel, take-over across uids), and never
// interactive: the GUI's health poll sends it, so it must not raise a dialog.
UR_TEST(controlStartProviderIsControlTunnelAndNeverPrompts) {
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::StartProvider, false, false)) ==
                 ctl::kActionControlTunnel);
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::StartProvider, false, true)) ==
                 ctl::kActionTakeOverTunnel);
  UR_EXPECT_FALSE(ctl::VerbWantsInteraction(ctl::Verb::StartProvider, /*is_log_tail=*/false));
}

UR_TEST(controlStatusCarriesTheProviderOnlyDevice) {
  ctl::StatusReply status;
  status.provider_running = true;
  status.provider_control_mode = "auto";
  status.provider_mode = 1;
  status.provider_network_key = true;
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(6, true, nlohmann::json(status))))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(back.provider_running);
  UR_EXPECT_TRUE(back.provider_control_mode == "auto");
  UR_EXPECT_EQ(1, back.provider_mode);
  UR_EXPECT_TRUE(back.provider_network_key);

  // A daemon predating start_provider says nothing: nothing runs.
  nlohmann::json older = nlohmann::json(status);
  for (const char* key : {"provider_running", "provider_control_mode", "provider_mode",
                          "provider_network_key"}) {
    older.erase(key);
  }
  const auto fromOlder = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(7, true, older)))->get<ctl::StatusReply>();
  UR_EXPECT_FALSE(fromOlder.provider_running);
  UR_EXPECT_EQ(0, fromOlder.provider_mode);

  // Another user's view names none of it.
  const ctl::StatusReply redacted = ctl::RedactStatusForForeignUid(status);
  UR_EXPECT_FALSE(redacted.provider_running);
  UR_EXPECT_TRUE(redacted.provider_control_mode.empty());
  UR_EXPECT_EQ(0, redacted.provider_mode);
  UR_EXPECT_FALSE(redacted.provider_network_key);
}

// The provider-only device's client count, which the provide line reads
// while disconnected. A count of 0 is a count; nothing read, a daemon that
// predates the field and another user's view all give -1.
UR_TEST(controlStatusCarriesTheProviderClientCount) {
  ctl::StatusReply status;
  status.provider_running = true;
  status.provider_mode = 3;
  status.provider_client_count = 3;
  const nlohmann::json wire = nlohmann::json(status);
  UR_EXPECT_TRUE(wire.contains("provider_client_count"));
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(10, true, wire)))->get<ctl::StatusReply>();
  UR_EXPECT_EQ(3, back.provider_client_count);

  status.provider_client_count = 0;
  const auto none = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(11, true, nlohmann::json(status))))->get<ctl::StatusReply>();
  UR_EXPECT_EQ(0, none.provider_client_count);

  UR_EXPECT_EQ(-1, ctl::StatusReply().provider_client_count);

  nlohmann::json older = wire;
  older.erase("provider_client_count");
  const auto fromOlder = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(12, true, older)))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(fromOlder.provider_running);
  UR_EXPECT_EQ(-1, fromOlder.provider_client_count);

  status.provider_client_count = 3;
  UR_EXPECT_EQ(-1, ctl::RedactStatusForForeignUid(status).provider_client_count);
}

// The provider-only device's connected peers ride with the count. A daemon
// that predates the field sends none (""), which the GUI reads as no list, and
// another user's view carries none.
UR_TEST(controlStatusCarriesTheProviderNetworkPeers) {
  ctl::StatusReply status;
  status.provider_running = true;
  status.provider_client_count = 1;
  status.provider_network_peers_json =
      R"([{"ClientId":"a","ProvideEnabled":true,"DeviceName":"Desk","DeviceSpec":"Fedora"}])";
  const nlohmann::json wire = nlohmann::json(status);
  UR_EXPECT_TRUE(wire.contains("provider_network_peers_json"));
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(13, true, wire)))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(back.provider_network_peers_json == status.provider_network_peers_json);

  UR_EXPECT_TRUE(ctl::StatusReply().provider_network_peers_json.empty());
  nlohmann::json older = wire;
  older.erase("provider_network_peers_json");
  const auto fromOlder = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(14, true, older)))->get<ctl::StatusReply>();
  UR_EXPECT_EQ(1, fromOlder.provider_client_count);
  UR_EXPECT_TRUE(fromOlder.provider_network_peers_json.empty());

  UR_EXPECT_TRUE(ctl::RedactStatusForForeignUid(status).provider_network_peers_json.empty());
}

// The network country the daemon read (P052) reaches the GUI; a daemon that
// predates it reports none, and another user's view names none.
UR_TEST(controlStatusCarriesTheNetworkCountry) {
  ctl::StatusReply status;
  status.network_country_code = "ru";
  const nlohmann::json wire = nlohmann::json(status);
  UR_EXPECT_TRUE(wire.contains("network_country_code"));
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(8, true, wire)))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(back.network_country_code == "ru");

  nlohmann::json older = wire;
  older.erase("network_country_code");
  const auto fromOlder = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(9, true, older)))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(fromOlder.network_country_code.empty());

  UR_EXPECT_TRUE(ctl::RedactStatusForForeignUid(status).network_country_code.empty());
}

// The dead-tunnel failsafe's countdown and its stop reasons reach the GUI; a
// daemon without the failsafe reports no countdown, and another user's view
// carries none.
UR_TEST(controlStatusCarriesTheFailsafe) {
  ctl::StatusReply status;
  status.tunnel_state = ctl::TunnelState::Up;
  status.failsafe_armed = true;
  const nlohmann::json wire = nlohmann::json(status);
  UR_EXPECT_TRUE(wire.contains("failsafe_armed"));
  auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(10, true, wire)))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(back.failsafe_armed);

  nlohmann::json older = wire;
  older.erase("failsafe_armed");
  const auto fromOlder = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(11, true, older)))->get<ctl::StatusReply>();
  UR_EXPECT_FALSE(fromOlder.failsafe_armed);
  UR_EXPECT_FALSE(ctl::RedactStatusForForeignUid(status).failsafe_armed);

  ctl::StatusReply stopped;
  stopped.tunnel_state = ctl::TunnelState::Error;
  stopped.stop_reason = ctl::kStopReasonFailsafeNoInbound;
  stopped.error_code = ctl::kCodeTunnelDead;
  back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(12, true, nlohmann::json(stopped))))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(ctl::IsFailsafeStop(back.stop_reason));
  UR_EXPECT_TRUE(back.error_code == "tunnel_dead");
  for (const char* reason : {ctl::kStopReasonFailsafeNoExit, ctl::kStopReasonFailsafeNoInbound,
                             ctl::kStopReasonFailsafeSdkUnresponsive}) {
    UR_EXPECT_TRUE(ctl::IsFailsafeStop(reason));
  }
  UR_EXPECT_FALSE(ctl::IsFailsafeStop("io_loop"));
  UR_EXPECT_FALSE(ctl::IsFailsafeStop(""));
}

// A stopped or failed tunnel is no session; starting, up and stopping are.
UR_TEST(controlProviderFactsReadOneStatus) {
  ctl::StatusReply status;
  status.provider_running = true;
  status.provider_control_mode = "always";
  status.owner_connected = true;
  status.kill_switch = ctl::KillSwitchState::Armed;
  for (const ctl::TunnelState state : {ctl::TunnelState::Stopped, ctl::TunnelState::Error}) {
    status.tunnel_state = state;
    const urnw::provide::DaemonProviderFacts facts = ctl::ProviderFactsFrom(status);
    UR_EXPECT_TRUE_MSG(ctl::ToString(state), facts.answered && !facts.tunnelSession);
    UR_EXPECT_TRUE(facts.providerRunning && facts.ownerConnected && facts.killSwitchArmed);
    UR_EXPECT_TRUE(facts.providerControlMode == "always");
    UR_EXPECT_FALSE(facts.redacted);
  }
  for (const ctl::TunnelState state :
       {ctl::TunnelState::Starting, ctl::TunnelState::Up, ctl::TunnelState::Stopping}) {
    status.tunnel_state = state;
    UR_EXPECT_TRUE_MSG(ctl::ToString(state), ctl::ProviderFactsFrom(status).tunnelSession);
  }
  status.kill_switch = ctl::KillSwitchState::Failed;
  UR_EXPECT_FALSE(ctl::ProviderFactsFrom(status).killSwitchArmed);
  UR_EXPECT_TRUE(ctl::ProviderFactsFrom(ctl::RedactStatusForForeignUid(status)).redacted);
}

// A saved network space value (the bootstrap DoH servers, P216) is a different
// provider device: the daemon replaces the running one instead of keeping it
// on the old servers. A new mode alone keeps the device.
UR_TEST(controlSameProviderDeviceReplacesOnAChangedSpace) {
  ctl::StartProviderRequest running = ProviderRequest("always");
  running.network_space_json = R"({"values":{}})";
  running.provider_transport_settings_json = R"({"mode":"auto"})";
  UR_EXPECT_TRUE(ctl::SameProviderDevice(running, running));

  ctl::StartProviderRequest remoded = running;
  remoded.provide_mode = "network";
  UR_EXPECT_TRUE(ctl::SameProviderDevice(running, remoded));

  ctl::StartProviderRequest doh = running;
  doh.network_space_json = R"({"values":{"control_doh_urls_ipv4":["https://192.0.2.53/"]}})";
  UR_EXPECT_FALSE(ctl::SameProviderDevice(running, doh));

  ctl::StartProviderRequest jwt = running;
  jwt.by_jwt = "another jwt";
  UR_EXPECT_FALSE(ctl::SameProviderDevice(running, jwt));
  ctl::StartProviderRequest instance = running;
  instance.instance_id = "another instance";
  UR_EXPECT_FALSE(ctl::SameProviderDevice(running, instance));
  ctl::StartProviderRequest version = running;
  version.app_version = "2026.10.6";
  UR_EXPECT_FALSE(ctl::SameProviderDevice(running, version));
  // the provider transport policy is applied to the running device in place
  ctl::StartProviderRequest policy = running;
  policy.provider_transport_settings_json.clear();
  UR_EXPECT_TRUE(ctl::SameProviderDevice(running, policy));
}

namespace {
// A jwt with these claims, as the server signs one: header.payload.signature,
// base64url without padding. The signature is never read.
std::string JwtWith(const std::string& claims) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  int value = 0;
  int bits = -6;
  for (const unsigned char c : claims) {
    value = (value << 8) + c;
    bits += 8;
    while (bits >= 0) {
      out.push_back(kAlphabet[(value >> bits) & 0x3F]);
      bits -= 6;
    }
  }
  if (bits > -6) out.push_back(kAlphabet[((value << 8) >> (bits + 8)) & 0x3F]);
  return "eyJhbGciOiJFZERTQSJ9." + out + ".c2lnbmF0dXJl";
}
}  // namespace

// The credential counts by the client it names, never by the token: the GUI
// refreshes its token on every connect, and the disconnect that keeps the
// session's device must not see the GUI's next start_provider as another
// device. A token that names no client is compared whole, as before.
UR_TEST(controlSameProviderDeviceCountsTheClientTheTokenNames) {
  const std::string first =
      JwtWith(R"({"network_id":"n1","client_id":"c1","create_time":"2026-11-04T00:00:00Z"})");
  const std::string refreshed =
      JwtWith(R"({"network_id":"n1","client_id":"c1","create_time":"2026-11-05T00:00:00Z"})");
  UR_EXPECT_TRUE(first != refreshed);
  UR_EXPECT_TRUE(ctl::JwtSubject(first) == "client:n1/c1");
  UR_EXPECT_TRUE(ctl::JwtSubject(first) == ctl::JwtSubject(refreshed));

  ctl::StartProviderRequest running = ProviderRequest("always");
  running.by_jwt = first;
  ctl::StartProviderRequest again = running;
  again.by_jwt = refreshed;
  UR_EXPECT_TRUE(ctl::SameProviderDevice(running, again));

  ctl::StartProviderRequest otherClient = running;
  otherClient.by_jwt = JwtWith(R"({"network_id":"n1","client_id":"c2"})");
  UR_EXPECT_FALSE(ctl::SameProviderDevice(running, otherClient));
  ctl::StartProviderRequest otherNetwork = running;
  otherNetwork.by_jwt = JwtWith(R"({"network_id":"n2","client_id":"c1"})");
  UR_EXPECT_FALSE(ctl::SameProviderDevice(running, otherNetwork));

  // no client named: the token itself
  for (const std::string& opaque :
       {std::string("not a jwt"), std::string("a.b"), JwtWith(R"({"network_id":"n1"})"),
        JwtWith(R"({"client_id":""})"), JwtWith("[1,2]"), std::string("a.!!!.c")}) {
    UR_EXPECT_TRUE_MSG(opaque, ctl::JwtSubject(opaque) == opaque);
  }

  // a start_tunnel with the same identity is the same device
  ctl::StartTunnelRequest start;
  start.by_jwt = refreshed;
  start.instance_id = running.instance_id;
  start.app_version = running.app_version;
  start.network_space_json = running.network_space_json;
  UR_EXPECT_TRUE(provide::SameIdentity(ctl::IdentityOf(running), ctl::IdentityOf(start)));
  UR_EXPECT_TRUE(provide::DifferingIdentityFields(ctl::IdentityOf(running), ctl::IdentityOf(start))
                     .empty());
  start.instance_id = "another instance";
  start.network_space_json = R"({"values":{"x":1}})";
  UR_EXPECT_TRUE(provide::DifferingIdentityFields(ctl::IdentityOf(running),
                                                  ctl::IdentityOf(start)) ==
                 "network_space_json, instance_id");
}

// stop_tunnel says when it is the owning user's Disconnect. Absent is every
// other stop, which is what a GUI predating the field sends.
UR_TEST(controlStopTunnelCarriesTheDisconnect) {
  ctl::StopTunnelRequest disconnect;
  disconnect.disconnect = true;
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeRequest(
      ctl::Verb::StopTunnel, 15, nlohmann::json(disconnect))))->get<ctl::StopTunnelRequest>();
  UR_EXPECT_TRUE(back.disconnect);
  const nlohmann::json plain = nlohmann::json(ctl::StopTunnelRequest());
  UR_EXPECT_TRUE(plain.is_object());
  UR_EXPECT_FALSE(plain.contains("disconnect"));
  const auto older = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeRequest(
      ctl::Verb::StopTunnel, 16, nlohmann::json::object())))->get<ctl::StopTunnelRequest>();
  UR_EXPECT_FALSE(older.disconnect);
}

// ---- provider_stats: the provider-only device's statistics -----------------

UR_TEST(controlProviderStatsRoundTrip) {
  ctl::ProviderStatsRequest req;
  req.poll_status = true;
  const auto reqBack = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::ProviderStats, 11, nlohmann::json(req))));
  UR_EXPECT_TRUE(ctl::RequestVerb(*reqBack) == ctl::Verb::ProviderStats);
  UR_EXPECT_TRUE(reqBack->get<ctl::ProviderStatsRequest>().poll_status);
  // absent asks for no polling
  UR_EXPECT_FALSE(nlohmann::json::object().get<ctl::ProviderStatsRequest>().poll_status);

  ctl::ProviderStatsReply stats;
  stats.running = true;
  stats.has_provider_stats = true;
  stats.provider_throughput_points_json = R"([{"Time":1,"Remote":{"EgressByteCount":2048}}])";
  stats.provider_transport_distribution_json = R"({"ByteCount":2048,"Active":true})";
  stats.status_open = true;
  stats.status_loaded = true;
  stats.status_last_fetch_error = "timeout";
  stats.provider_status_json = R"({"reason":"reliability_warming_up"})";
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(12, true, nlohmann::json(stats))))->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(back.running);
  UR_EXPECT_TRUE(back.has_provider_stats);
  UR_EXPECT_TRUE(back.provider_throughput_points_json == stats.provider_throughput_points_json);
  UR_EXPECT_TRUE(back.provider_transport_distribution_json ==
                 stats.provider_transport_distribution_json);
  UR_EXPECT_TRUE(back.status_open && back.status_loaded);
  UR_EXPECT_TRUE(back.status_last_fetch_error == "timeout");
  UR_EXPECT_TRUE(back.provider_status_json == stats.provider_status_json);
  // the SDK payloads stay strings, so the frame is still one line
  const std::string frame = ctl::EncodeFrame(ctl::MakeReply(12, true, nlohmann::json(stats)));
  UR_EXPECT_TRUE(frame.find('\n') == frame.size() - 1);
}

// An empty reply (no provider-only device, a foreign uid, a field the daemon
// did not send) reads as nothing running and nothing to draw.
UR_TEST(controlProviderStatsAbsentFieldsMeanNothingRuns) {
  const auto empty = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(13, true)))
                         ->get<ctl::ProviderStatsReply>();
  UR_EXPECT_FALSE(empty.running);
  UR_EXPECT_FALSE(empty.has_provider_stats);
  UR_EXPECT_TRUE(empty.provider_throughput_points_json.empty());
  UR_EXPECT_TRUE(empty.provider_transport_distribution_json.empty());
  UR_EXPECT_FALSE(empty.status_open);
  UR_EXPECT_FALSE(empty.status_loaded);
  UR_EXPECT_TRUE(empty.provider_status_json.empty());
  UR_EXPECT_TRUE(empty.extender_provide_status_json.empty());
  UR_EXPECT_TRUE(empty.extender_throughput_points_json.empty());
  const nlohmann::json defaults = nlohmann::json(ctl::ProviderStatsReply());
  UR_EXPECT_FALSE(defaults.at("running").get<bool>());
}

// The provider-only device's extender role travels beside its provider
// statistics, so the Earnings extender row and plot can show while
// disconnected: the status the connected path reads off the device and the
// series it reads off the contract view controller, in the SDK's own JSON.
UR_TEST(controlProviderStatsCarriesTheExtenderRole) {
  ctl::ProviderStatsReply stats;
  stats.running = true;
  stats.has_provider_stats = true;
  stats.extender_provide_status_json =
      R"({"Supported":true,"State":"active","Enabled":true,"ActivatedV4":true})";
  stats.extender_throughput_points_json = R"([{"Time":1,"Remote":{"EgressByteCount":512}}])";
  const nlohmann::json encoded = nlohmann::json(stats);
  UR_EXPECT_TRUE(encoded.value("extender_provide_status_json", std::string()) ==
                 stats.extender_provide_status_json);
  UR_EXPECT_TRUE(encoded.value("extender_throughput_points_json", std::string()) ==
                 stats.extender_throughput_points_json);
  const std::string frame = ctl::EncodeFrame(ctl::MakeReply(15, true, encoded));
  UR_EXPECT_TRUE(frame.find('\n') == frame.size() - 1);
  const auto back = ctl::DecodeFrame(frame)->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(back.extender_provide_status_json == stats.extender_provide_status_json);
  UR_EXPECT_TRUE(back.extender_throughput_points_json == stats.extender_throughput_points_json);
  // the payload is the SDK's object, carried whole
  const nlohmann::json role =
      nlohmann::json::parse(back.extender_provide_status_json, nullptr, /*allow_exceptions=*/false);
  UR_EXPECT_TRUE(role.is_object() && role.value("Enabled", false));
}

// A daemon from before the extender fields answers the provider statistics
// alone. They still read in full, and the role reads as absent: the GUI keeps
// the extender row and plot hidden, exactly as before.
UR_TEST(controlProviderStatsFromAnOlderDaemonHasNoExtenderRole) {
  const nlohmann::json older = {
      {"running", true},
      {"has_provider_stats", true},
      {"provider_throughput_points_json", R"([{"Time":1}])"},
      {"provider_transport_distribution_json", R"({"ByteCount":1})"},
      {"status_open", true},
      {"status_loaded", true},
      {"status_last_fetch_error", ""},
      {"provider_status_json", R"({"reason":""})"},
  };
  const auto back =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(16, true, older)))->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(back.running && back.has_provider_stats);
  UR_EXPECT_TRUE(back.provider_throughput_points_json == R"([{"Time":1}])");
  UR_EXPECT_TRUE(back.extender_provide_status_json.empty());
  UR_EXPECT_TRUE(back.extender_throughput_points_json.empty());
  // and a null field (a peer that wrote one) reads as absent too
  nlohmann::json withNull = older;
  withNull["extender_provide_status_json"] = nullptr;
  UR_EXPECT_TRUE(withNull.get<ctl::ProviderStatsReply>().extender_provide_status_json.empty());
}

// Answered like status: polled, so no polkit check and no prompt. A daemon
// that predates the verb says so with the one error every daemon gives.
UR_TEST(controlProviderStatsIsUngatedAndNeverPrompts) {
  UR_EXPECT_TRUE(ctl::ActionIdForVerb(ctl::Verb::ProviderStats, false, false) == nullptr);
  UR_EXPECT_TRUE(ctl::ActionIdForVerb(ctl::Verb::ProviderStats, false, true) == nullptr);
  UR_EXPECT_FALSE(ctl::VerbWantsInteraction(ctl::Verb::ProviderStats, /*is_log_tail=*/false));
  UR_EXPECT_TRUE(std::string(ctl::kErrorUnknownVerb) == "unknown verb");
  const auto older =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeErrorReply(14, ctl::kErrorUnknownVerb)));
  UR_EXPECT_FALSE(ctl::ReplyOk(*older));
  UR_EXPECT_TRUE(ctl::ReplyError(*older) == ctl::kErrorUnknownVerb);
}

// ---- upload_logs: the daemon's logs for a feedback, connected or not ---------

namespace {
ctl::UploadLogsRequest SampleUploadLogs() {
  ctl::UploadLogsRequest req;
  // synthetic, shaped like the server's ids
  req.feedback_id = "f00dfeed-0000-4000-8000-000000000001";
  req.by_jwt = "client jwt";
  req.instance_id = "instance-1";
  req.app_version = "2026.10.5";
  req.network_space_json = R"({"host_name":"network.example","env_name":"test"})";
  return req;
}
}  // namespace

UR_TEST(controlUploadLogsRoundTrip) {
  const ctl::UploadLogsRequest req = SampleUploadLogs();
  const auto reqBack = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::UploadLogs, 15, nlohmann::json(req))));
  UR_EXPECT_TRUE(ctl::RequestVerb(*reqBack) == ctl::Verb::UploadLogs);
  const auto back = reqBack->get<ctl::UploadLogsRequest>();
  UR_EXPECT_TRUE(back.feedback_id == req.feedback_id);
  UR_EXPECT_TRUE(back.by_jwt == req.by_jwt);
  UR_EXPECT_TRUE(back.instance_id == req.instance_id);
  UR_EXPECT_TRUE(back.app_version == req.app_version);
  UR_EXPECT_TRUE(back.network_space_json == req.network_space_json);
  // the compiled-in default space travels as silence, as start_provider's does
  ctl::UploadLogsRequest defaultSpace = req;
  defaultSpace.network_space_json.clear();
  UR_EXPECT_FALSE(nlohmann::json(defaultSpace).contains("network_space_json"));

  ctl::UploadLogsReply reply;
  reply.carrier = "standalone";
  reply.upload_id = 1759700000123;
  const auto replyBack = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(16, true, nlohmann::json(reply))))->get<ctl::UploadLogsReply>();
  UR_EXPECT_TRUE(replyBack.carrier == "standalone");
  UR_EXPECT_EQ(1759700000123LL, static_cast<long long>(replyBack.upload_id));
  // absent says nothing about the device, and names no upload
  UR_EXPECT_TRUE(nlohmann::json::object().get<ctl::UploadLogsReply>().carrier.empty());
  UR_EXPECT_EQ(0LL, static_cast<long long>(
                        nlohmann::json::object().get<ctl::UploadLogsReply>().upload_id));
}

// The GUI's own log files ride with the request: their names in it, in the
// order their descriptors ride with the frame. Additive: a request that hands
// over none sends no field, as an older GUI did, and an older GUI's request
// names none.
UR_TEST(controlUploadLogsCarriesTheGuisLogFileNames) {
  ctl::UploadLogsRequest req = SampleUploadLogs();
  req.gui_log_files = {"urnetwork-gui.host.user.log.INFO.20260901-000000.101",
                       "urnetwork-gui.host.user.log.ERROR.20260901-000000.101"};
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeRequest(
                                         ctl::Verb::UploadLogs, 17, nlohmann::json(req))))
                        ->get<ctl::UploadLogsRequest>();
  UR_EXPECT_TRUE(back.gui_log_files == req.gui_log_files);

  const ctl::UploadLogsRequest none = SampleUploadLogs();
  UR_EXPECT_FALSE(nlohmann::json(none).contains("gui_log_files"));
  UR_EXPECT_TRUE(nlohmann::json(none).get<ctl::UploadLogsRequest>().gui_log_files.empty());
  // a request is still valid with or without them
  UR_EXPECT_FALSE(ctl::ValidateUploadLogsRequest(req).has_value());
}

// The outcome of an upload reaches the GUI through status: the upload's id
// and where it is, additive within v1 (an older daemon's status names no
// upload), and never in a status cut down for another uid, whose feedback it
// describes.
UR_TEST(controlStatusCarriesTheLogUpload) {
  ctl::StatusReply status;
  status.log_upload_id = 1759700000123;
  status.log_upload_state = "refused";
  status.log_upload_carrier = "provider";
  const auto back =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(18, true, nlohmann::json(status))))
          ->get<ctl::StatusReply>();
  UR_EXPECT_EQ(1759700000123LL, static_cast<long long>(back.log_upload_id));
  UR_EXPECT_TRUE(back.log_upload_state == "refused");
  UR_EXPECT_TRUE(back.log_upload_carrier == "provider");

  nlohmann::json older = nlohmann::json(status);
  older.erase("log_upload_id");
  older.erase("log_upload_state");
  older.erase("log_upload_carrier");
  const auto olderBack = older.get<ctl::StatusReply>();
  UR_EXPECT_EQ(0LL, static_cast<long long>(olderBack.log_upload_id));
  UR_EXPECT_TRUE(olderBack.log_upload_state.empty());

  const ctl::StatusReply redacted = ctl::RedactStatusForForeignUid(status);
  UR_EXPECT_EQ(0LL, static_cast<long long>(redacted.log_upload_id));
  UR_EXPECT_TRUE(redacted.log_upload_state.empty());
  UR_EXPECT_TRUE(redacted.log_upload_carrier.empty());
}

// The feedback id becomes a path segment of the url root's device posts to, so
// only the server's own ids (uuids) pass; credentials are required because the
// daemon may have to build a device from them.
UR_TEST(controlUploadLogsValidation) {
  UR_EXPECT_FALSE(ctl::ValidateUploadLogsRequest(SampleUploadLogs()).has_value());
  ctl::UploadLogsRequest upper = SampleUploadLogs();
  upper.feedback_id = "F00DFEED-0000-4000-8000-000000000001";
  UR_EXPECT_FALSE(ctl::ValidateUploadLogsRequest(upper).has_value());
  ctl::UploadLogsRequest defaultSpace = SampleUploadLogs();
  defaultSpace.network_space_json.clear();
  UR_EXPECT_FALSE(ctl::ValidateUploadLogsRequest(defaultSpace).has_value());

  for (const char* id : {
           "",
           "not-a-feedback-id",
           "../../network/provider-status",
           "f00dfeed-0000-4000-8000-00000000000",    // one short
           "f00dfeed-0000-4000-8000-0000000000012",  // one long
           "f00dfeed/0000-4000-8000-000000000001",   // a path separator for a dash
           "f00dfeed-0000-4000-8000-00000000000?",   // a query
           "f00dfeed-0000-4000-8000-00000000000 ",   // whitespace
           "{00dfeed-0000-4000-8000-00000000000}",   // braces
           "f00dfeed0000040000800000000000000001",   // no dashes
           "g00dfeed-0000-4000-8000-000000000001",   // not hex
       }) {
    ctl::UploadLogsRequest bad = SampleUploadLogs();
    bad.feedback_id = id;
    UR_EXPECT_TRUE_MSG(id, ctl::ValidateUploadLogsRequest(bad).has_value());
    UR_EXPECT_FALSE(ctl::LooksLikeFeedbackId(id));
  }
  ctl::UploadLogsRequest noJwt = SampleUploadLogs();
  noJwt.by_jwt.clear();
  UR_EXPECT_TRUE(ctl::ValidateUploadLogsRequest(noJwt).has_value());
  ctl::UploadLogsRequest noInstance = SampleUploadLogs();
  noInstance.instance_id.clear();
  UR_EXPECT_TRUE(ctl::ValidateUploadLogsRequest(noInstance).has_value());
}

// The log's gate, whoever runs the tunnel: read-log as for log_tail, never the
// take-over action, and never a prompt after "Thanks for the feedback!".
UR_TEST(controlUploadLogsIsGatedLikeTheLogAndNeverPrompts) {
  for (const bool crossUid : {false, true}) {
    // nullptr would mean "no polkit check", which the daemon answers as an
    // unknown verb
    const char* action = ctl::ActionIdForVerb(ctl::Verb::UploadLogs, false, crossUid);
    UR_EXPECT_TRUE(action != nullptr && std::string(action) == ctl::kActionReadLog);
  }
  UR_EXPECT_FALSE(ctl::VerbWantsInteraction(ctl::Verb::UploadLogs, /*is_log_tail=*/false));
  UR_EXPECT_TRUE(std::string(ctl::kCodeLogUploadFailed) == "log_upload_failed");
  UR_EXPECT_TRUE(std::string(ctl::kCodeLogUploadBusy) == "log_upload_busy");
  // a daemon that predates the verb says so with the one error every daemon gives
  const auto older =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeErrorReply(17, ctl::kErrorUnknownVerb)));
  UR_EXPECT_TRUE(ctl::ReplyError(*older) == ctl::kErrorUnknownVerb);
}

// ---- set_provide_extender: the Extender switch while disconnected ------------

UR_TEST(controlSetProvideExtenderRoundTrip) {
  for (const bool on : {false, true}) {
    ctl::SetProvideExtenderRequest req;
    req.provide_extender = on;
    const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
        ctl::MakeRequest(ctl::Verb::SetProvideExtender, 21, nlohmann::json(req))));
    UR_EXPECT_TRUE(ctl::RequestVerb(*back) == ctl::Verb::SetProvideExtender);
    UR_EXPECT_TRUE(back->get<ctl::SetProvideExtenderRequest>().provide_extender == on);
  }
}

// A write whose value did not arrive writes nothing, not a default the user
// never chose: the daemon answers the throw as an error reply.
UR_TEST(controlSetProvideExtenderWithoutAValueIsRefused) {
  for (const char* malformed : {R"({})", R"({"provide_extender":null})",
                                R"({"provide_extender":"false"})", R"({"provide_extender":0})"}) {
    bool threw = false;
    try {
      (void)nlohmann::json::parse(malformed).get<ctl::SetProvideExtenderRequest>();
    } catch (const std::exception&) {
      threw = true;
    }
    UR_EXPECT_TRUE_MSG(malformed, threw);
  }
}

// Priced like set_provide, whose provider-only session it writes into
// (control-tunnel, take-over across uids), and a press of the switch, so it
// may prompt.
UR_TEST(controlSetProvideExtenderIsGatedLikeSetProvide) {
  for (const bool crossUid : {false, true}) {
    UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetProvideExtender, false,
                                                    crossUid)) ==
                   ctl::ActionIdForVerb(ctl::Verb::SetProvide, false, crossUid));
  }
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetProvideExtender, false, false)) ==
                 ctl::kActionControlTunnel);
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetProvideExtender, false, true)) ==
                 ctl::kActionTakeOverTunnel);
  UR_EXPECT_TRUE(ctl::VerbWantsInteraction(ctl::Verb::SetProvideExtender, /*is_log_tail=*/false));
}

// ---- set_measurements_enabled: the lag.dev speed tests switch -------------

UR_TEST(controlSetMeasurementsEnabledRoundTrip) {
  for (const bool on : {false, true}) {
    ctl::SetMeasurementsEnabledRequest req;
    req.measurements_enabled = on;
    const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
        ctl::MakeRequest(ctl::Verb::SetMeasurementsEnabled, 24, nlohmann::json(req))));
    UR_EXPECT_TRUE(ctl::RequestVerb(*back) == ctl::Verb::SetMeasurementsEnabled);
    UR_EXPECT_TRUE(back->get<ctl::SetMeasurementsEnabledRequest>().measurements_enabled == on);
  }
}

// A write whose value did not arrive writes nothing, not a default the user
// never chose, and the Extender switch's value is not this switch's.
UR_TEST(controlSetMeasurementsEnabledWithoutAValueIsRefused) {
  for (const char* malformed :
       {R"({})", R"({"measurements_enabled":null})", R"({"measurements_enabled":"false"})",
        R"({"measurements_enabled":0})", R"({"provide_extender":false})"}) {
    bool threw = false;
    try {
      (void)nlohmann::json::parse(malformed).get<ctl::SetMeasurementsEnabledRequest>();
    } catch (const std::exception&) {
      threw = true;
    }
    UR_EXPECT_TRUE_MSG(malformed, threw);
  }
}

// Priced like set_provide_extender, the switch it sits under: control-tunnel,
// take-over across uids, and a press, so it may prompt. It needs a grant.
UR_TEST(controlSetMeasurementsEnabledIsGatedLikeSetProvideExtender) {
  for (const bool crossUid : {false, true}) {
    UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetMeasurementsEnabled, false,
                                                    crossUid)) ==
                   ctl::ActionIdForVerb(ctl::Verb::SetProvideExtender, false, crossUid));
  }
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetMeasurementsEnabled, false,
                                                  false)) == ctl::kActionControlTunnel);
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetMeasurementsEnabled, false,
                                                  true)) == ctl::kActionTakeOverTunnel);
  UR_EXPECT_TRUE(
      ctl::VerbWantsInteraction(ctl::Verb::SetMeasurementsEnabled, /*is_log_tail=*/false));
}

// provider_stats carries the measurements setting beside the role, and the
// daemon's word that it takes the speed tests switch's write.
UR_TEST(controlProviderStatsCarriesTheSpeedTestsSwitch) {
  ctl::ProviderStatsReply stats;
  stats.running = true;
  stats.extender_provide_status_json = R"({"Supported":true,"State":"active","Enabled":true})";
  stats.provide_extender = true;
  stats.provide_extender_writable = true;
  stats.measurements_enabled = false;
  stats.measurements_enabled_writable = true;
  const auto back =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(25, true, nlohmann::json(stats))))
          ->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(back.measurements_enabled_writable);
  UR_EXPECT_FALSE(back.measurements_enabled);
  // the Extender switch's setting beside it stays its own
  UR_EXPECT_TRUE(back.provide_extender);
}

// A daemon from before the verb: no writer, so the switch stays hidden while
// disconnected, and the setting reads as its default, on.
UR_TEST(controlProviderStatsFromAnOlderDaemonHasNoSpeedTestsWriter) {
  const nlohmann::json older = {
      {"running", true},
      {"extender_provide_status_json", R"({"Supported":true,"State":"active","Enabled":true})"},
      {"provide_extender", true},
      {"provide_extender_writable", true},
  };
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(26, true, older)))
                        ->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(back.provide_extender_writable);
  UR_EXPECT_FALSE(back.measurements_enabled_writable);
  UR_EXPECT_TRUE(back.measurements_enabled);
  UR_EXPECT_FALSE(ctl::ProviderStatsReply().measurements_enabled_writable);
  UR_EXPECT_TRUE(ctl::ProviderStatsReply().measurements_enabled);
}

// ---- set_provide_extender_port_mapping: the router port mapping switch ------

UR_TEST(controlSetProvideExtenderPortMappingRoundTrip) {
  for (const bool on : {false, true}) {
    ctl::SetProvideExtenderPortMappingRequest req;
    req.provide_extender_port_mapping = on;
    const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
        ctl::MakeRequest(ctl::Verb::SetProvideExtenderPortMapping, 27, nlohmann::json(req))));
    UR_EXPECT_TRUE(ctl::RequestVerb(*back) == ctl::Verb::SetProvideExtenderPortMapping);
    UR_EXPECT_TRUE(
        back->get<ctl::SetProvideExtenderPortMappingRequest>().provide_extender_port_mapping == on);
  }
  // the request's own default is the apps' default, off
  UR_EXPECT_FALSE(ctl::SetProvideExtenderPortMappingRequest().provide_extender_port_mapping);
}

// A write whose value did not arrive writes nothing, not a default the user
// never chose, so nothing opens a port on the router unasked; and neither the
// Extender switch's value nor the speed tests switch's is this switch's.
UR_TEST(controlSetProvideExtenderPortMappingWithoutAValueIsRefused) {
  for (const char* malformed :
       {R"({})", R"({"provide_extender_port_mapping":null})",
        R"({"provide_extender_port_mapping":"true"})", R"({"provide_extender_port_mapping":1})",
        R"({"provide_extender":true})", R"({"measurements_enabled":true})",
        R"({"port_mapping":true})"}) {
    bool threw = false;
    try {
      (void)nlohmann::json::parse(malformed).get<ctl::SetProvideExtenderPortMappingRequest>();
    } catch (const std::exception&) {
      threw = true;
    }
    UR_EXPECT_TRUE_MSG(malformed, threw);
  }
}

// Priced like set_provide_extender, the switch it sits under: control-tunnel,
// take-over across uids, and a press, so it may prompt. It needs a grant.
UR_TEST(controlSetProvideExtenderPortMappingIsGatedLikeSetProvideExtender) {
  for (const bool crossUid : {false, true}) {
    UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetProvideExtenderPortMapping,
                                                    false, crossUid)) ==
                   ctl::ActionIdForVerb(ctl::Verb::SetProvideExtender, false, crossUid));
  }
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetProvideExtenderPortMapping, false,
                                                  false)) == ctl::kActionControlTunnel);
  UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::SetProvideExtenderPortMapping, false,
                                                  true)) == ctl::kActionTakeOverTunnel);
  UR_EXPECT_TRUE(
      ctl::VerbWantsInteraction(ctl::Verb::SetProvideExtenderPortMapping, /*is_log_tail=*/false));
}

// provider_stats carries the router port mapping setting beside the role, and
// the daemon's word that it takes that switch's write.
UR_TEST(controlProviderStatsCarriesThePortMappingSwitch) {
  ctl::ProviderStatsReply stats;
  stats.running = true;
  stats.extender_provide_status_json = R"({"Supported":true,"State":"active","Enabled":true})";
  stats.provide_extender = true;
  stats.provide_extender_writable = true;
  stats.measurements_enabled = false;
  stats.measurements_enabled_writable = true;
  stats.provide_extender_port_mapping = true;
  stats.provide_extender_port_mapping_writable = true;
  const auto back =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(28, true, nlohmann::json(stats))))
          ->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(back.provide_extender_port_mapping_writable);
  UR_EXPECT_TRUE(back.provide_extender_port_mapping);
  // the settings beside it stay their own
  UR_EXPECT_TRUE(back.provide_extender);
  UR_EXPECT_FALSE(back.measurements_enabled);

  stats.provide_extender_port_mapping = false;
  const auto off =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(29, true, nlohmann::json(stats))))
          ->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(off.provide_extender_port_mapping_writable);
  UR_EXPECT_FALSE(off.provide_extender_port_mapping);
}

// A daemon from before the verb: no writer, so the switch stays hidden while
// disconnected, and the setting reads as its default on every app, off. A
// null reads the same as an absent field.
UR_TEST(controlProviderStatsFromAnOlderDaemonHasNoPortMappingWriter) {
  const nlohmann::json older = {
      {"running", true},
      {"extender_provide_status_json", R"({"Supported":true,"State":"active","Enabled":true})"},
      {"provide_extender", true},
      {"provide_extender_writable", true},
      {"measurements_enabled", true},
      {"measurements_enabled_writable", true},
  };
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(30, true, older)))
                        ->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(back.provide_extender_writable);
  UR_EXPECT_TRUE(back.measurements_enabled_writable);
  UR_EXPECT_FALSE(back.provide_extender_port_mapping_writable);
  UR_EXPECT_FALSE(back.provide_extender_port_mapping);

  nlohmann::json nulls = older;
  nulls["provide_extender_port_mapping"] = nullptr;
  nulls["provide_extender_port_mapping_writable"] = nullptr;
  const auto nulled = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(31, true, nulls)))
                          ->get<ctl::ProviderStatsReply>();
  UR_EXPECT_FALSE(nulled.provide_extender_port_mapping_writable);
  UR_EXPECT_FALSE(nulled.provide_extender_port_mapping);

  UR_EXPECT_FALSE(ctl::ProviderStatsReply().provide_extender_port_mapping_writable);
  UR_EXPECT_FALSE(ctl::ProviderStatsReply().provide_extender_port_mapping);
}

// logout names the account's network space; an absent one parses empty (the
// compiled-in default space, as for start_tunnel).
UR_TEST(controlLogoutRoundTrip) {
  ctl::LogoutRequest req;
  req.network_space_json = R"({"key":{"host_name":"network.example","env_name":"main"}})";
  const auto back = ctl::DecodeFrame(
      ctl::EncodeFrame(ctl::MakeRequest(ctl::Verb::Logout, 33, nlohmann::json(req))));
  UR_EXPECT_TRUE(ctl::RequestVerb(*back) == ctl::Verb::Logout);
  UR_EXPECT_TRUE(back->get<ctl::LogoutRequest>().network_space_json == req.network_space_json);
  UR_EXPECT_TRUE(nlohmann::json::parse("{}").get<ctl::LogoutRequest>().network_space_json.empty());
}

// logout costs what a stop costs and never the take-over action: another
// user's live session is never cleared by a sign-out, so there is nothing an
// administrator could authorize. The health poll re-sends an owed one, so it
// never prompts.
UR_TEST(controlLogoutIsControlTunnelEvenAcrossUidsAndNeverPrompts) {
  for (const bool crossUid : {false, true}) {
    UR_EXPECT_TRUE(std::string(ctl::ActionIdForVerb(ctl::Verb::Logout, false, crossUid)) ==
                   ctl::kActionControlTunnel);
  }
  UR_EXPECT_TRUE(!ctl::VerbWantsInteraction(ctl::Verb::Logout, /*is_log_tail=*/false));
}

// provider_stats carries the setting beside the role, and the daemon's word
// that it takes the switch's write.
UR_TEST(controlProviderStatsCarriesTheExtenderSwitch) {
  ctl::ProviderStatsReply stats;
  stats.running = true;
  stats.extender_provide_status_json = R"({"Supported":true,"State":"off","Enabled":false})";
  stats.provide_extender = false;
  stats.provide_extender_writable = true;
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(22, true, nlohmann::json(stats))))
                        ->get<ctl::ProviderStatsReply>();
  UR_EXPECT_TRUE(back.provide_extender_writable);
  UR_EXPECT_FALSE(back.provide_extender);
  stats.provide_extender = true;
  UR_EXPECT_TRUE(nlohmann::json(stats).get<ctl::ProviderStatsReply>().provide_extender);
}

// A daemon from before the verb sends the role without the setting or the
// writer: the GUI reads no writer, and the switch stays hidden while
// disconnected, as before.
UR_TEST(controlProviderStatsFromAnOlderDaemonHasNoExtenderWriter) {
  const nlohmann::json older = {
      {"running", true},
      {"extender_provide_status_json", R"({"Supported":true,"State":"active","Enabled":true})"},
      {"extender_throughput_points_json", "[]"},
  };
  const auto back =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeReply(23, true, older)))->get<ctl::ProviderStatsReply>();
  UR_EXPECT_FALSE(back.extender_provide_status_json.empty());
  UR_EXPECT_FALSE(back.provide_extender_writable);
  UR_EXPECT_FALSE(back.provide_extender);
  UR_EXPECT_FALSE(ctl::ProviderStatsReply().provide_extender_writable);
}

// ---- reset_extenders: Account > Extenders' Reset extenders --------------------

namespace {

ctl::ResetExtendersRequest SampleResetExtenders() {
  ctl::ResetExtendersRequest req;
  req.host_name = "network.example";
  req.env_name = "main";
  req.extender_reset_id = "f00dfeed-0000-4000-8000-000000000001";
  return req;
}

}  // namespace

// The space's key and the reset's id travel under their wire names, and the
// reply says whether the daemon applied it; absent reads as nothing applied.
UR_TEST(controlResetExtendersRoundTrip) {
  const ctl::ResetExtendersRequest req = SampleResetExtenders();
  const nlohmann::json wire = nlohmann::json(req);
  UR_EXPECT_TRUE(wire.at("host_name") == "network.example");
  UR_EXPECT_TRUE(wire.at("env_name") == "main");
  UR_EXPECT_TRUE(wire.at("extender_reset_id") == "f00dfeed-0000-4000-8000-000000000001");
  const auto back = ctl::DecodeFrame(
      ctl::EncodeFrame(ctl::MakeRequest(ctl::Verb::ResetExtenders, 31, wire)));
  UR_EXPECT_TRUE(ctl::RequestVerb(*back) == ctl::Verb::ResetExtenders);
  const auto parsed = back->get<ctl::ResetExtendersRequest>();
  UR_EXPECT_TRUE(parsed.host_name == req.host_name);
  UR_EXPECT_TRUE(parsed.env_name == req.env_name);
  UR_EXPECT_TRUE(parsed.extender_reset_id == req.extender_reset_id);

  for (const bool reset : {false, true}) {
    ctl::ResetExtendersReply reply;
    reply.reset = reset;
    const auto replyBack = ctl::DecodeFrame(
        ctl::EncodeFrame(ctl::MakeReply(32, true, nlohmann::json(reply))));
    UR_EXPECT_TRUE(ctl::ReplyOk(*replyBack));
    UR_EXPECT_TRUE(replyBack->get<ctl::ResetExtendersReply>().reset == reset);
  }
  UR_EXPECT_FALSE(nlohmann::json::object().get<ctl::ResetExtendersReply>().reset);
  // a daemon that predates the verb says so with the one error every daemon gives
  const auto older =
      ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeErrorReply(33, ctl::kErrorUnknownVerb)));
  UR_EXPECT_TRUE(ctl::ReplyError(*older) == ctl::kErrorUnknownVerb);
}

// A reset that does not name its space and its id is refused when parsed, never
// applied to a default: each field missing, null, empty or not a string throws,
// which the daemon answers as an error reply.
UR_TEST(controlResetExtendersWithoutASpaceOrAResetIsRefused) {
  for (const char* field : {"host_name", "env_name", "extender_reset_id"}) {
    const nlohmann::json complete = nlohmann::json(SampleResetExtenders());
    nlohmann::json missing = complete;
    missing.erase(field);
    nlohmann::json null = complete;
    null[field] = nullptr;
    nlohmann::json empty = complete;
    empty[field] = "";
    nlohmann::json number = complete;
    number[field] = 7;
    for (const nlohmann::json& malformed : {missing, null, empty, number}) {
      bool threw = false;
      try {
        (void)malformed.get<ctl::ResetExtendersRequest>();
      } catch (const std::exception&) {
        threw = true;
      }
      UR_EXPECT_TRUE_MSG(malformed.dump(), threw);
    }
  }
  UR_EXPECT_FALSE(ctl::ValidateResetExtendersRequest(SampleResetExtenders()).has_value());
}

// The rule both halves apply: every field required, bounded and free of control
// bytes, since the daemon logs the key it reset line by line.
UR_TEST(controlResetExtendersValidation) {
  ctl::ResetExtendersRequest longest = SampleResetExtenders();
  longest.host_name = std::string(ctl::kMaxResetExtendersFieldBytes, 'a');
  UR_EXPECT_FALSE(ctl::ValidateResetExtendersRequest(longest).has_value());
  ctl::ResetExtendersRequest spaced = SampleResetExtenders();
  spaced.env_name = "custom env";
  UR_EXPECT_FALSE(ctl::ValidateResetExtendersRequest(spaced).has_value());

  using Mutate = void (*)(ctl::ResetExtendersRequest&);
  const Mutate bad[] = {
      [](ctl::ResetExtendersRequest& r) { r.host_name.clear(); },
      [](ctl::ResetExtendersRequest& r) { r.env_name.clear(); },
      [](ctl::ResetExtendersRequest& r) { r.extender_reset_id.clear(); },
      [](ctl::ResetExtendersRequest& r) {
        r.host_name = std::string(ctl::kMaxResetExtendersFieldBytes + 1, 'a');
      },
      [](ctl::ResetExtendersRequest& r) { r.host_name = "network.example\nforged line"; },
      [](ctl::ResetExtendersRequest& r) { r.env_name = std::string("ma\0in", 5); },
      [](ctl::ResetExtendersRequest& r) { r.extender_reset_id += "\x7f"; },
      [](ctl::ResetExtendersRequest& r) { r.extender_reset_id += "\r"; },
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    ctl::ResetExtendersRequest req = SampleResetExtenders();
    bad[i](req);
    UR_EXPECT_TRUE_MSG(std::to_string(i), ctl::ValidateResetExtendersRequest(req).has_value());
    bool threw = false;
    try {
      (void)nlohmann::json(req).get<ctl::ResetExtendersRequest>();
    } catch (const std::exception&) {
      threw = true;
    }
    UR_EXPECT_TRUE_MSG(std::to_string(i), threw);
  }
}

// Priced like set_provide_extender: control-tunnel for the caller's own session
// or none, the take-over action beside another uid's, since the daemon's spaces
// are every user's. A confirmed press, so it may prompt.
UR_TEST(controlResetExtendersIsGatedLikeSetProvideExtender) {
  // "" for no action, so a verb that lost its row fails here rather than crash
  auto action = [](ctl::Verb verb, bool crossUid) {
    const char* id = ctl::ActionIdForVerb(verb, /*is_log_tail=*/false, crossUid);
    return std::string(id == nullptr ? "" : id);
  };
  for (const bool crossUid : {false, true}) {
    UR_EXPECT_TRUE(action(ctl::Verb::ResetExtenders, crossUid) ==
                   action(ctl::Verb::SetProvideExtender, crossUid));
  }
  UR_EXPECT_TRUE(action(ctl::Verb::ResetExtenders, false) == ctl::kActionControlTunnel);
  UR_EXPECT_TRUE(action(ctl::Verb::ResetExtenders, true) == ctl::kActionTakeOverTunnel);
  UR_EXPECT_TRUE(ctl::VerbWantsInteraction(ctl::Verb::ResetExtenders, /*is_log_tail=*/false));
}

// A reset the GUI sends again without a press carries interactive false, which
// the daemon reads off the raw frame before it authorizes, never throwing:
// absent or null is a press, false and anything but a boolean ask for no
// dialog. A press's frame is unchanged.
UR_TEST(controlResetExtendersSentAgainAsksForNoDialog) {
  const ctl::ResetExtendersRequest press = SampleResetExtenders();
  UR_EXPECT_TRUE(press.interactive);
  const nlohmann::json pressWire = nlohmann::json(press);
  UR_EXPECT_FALSE(pressWire.contains("interactive"));
  UR_EXPECT_TRUE(ctl::ResetExtendersAllowsInteraction(
      ctl::MakeRequest(ctl::Verb::ResetExtenders, 34, pressWire)));
  UR_EXPECT_TRUE(pressWire.get<ctl::ResetExtendersRequest>().interactive);

  ctl::ResetExtendersRequest again = press;
  again.interactive = false;
  const auto frame = ctl::DecodeFrame(
      ctl::EncodeFrame(ctl::MakeRequest(ctl::Verb::ResetExtenders, 35, nlohmann::json(again))));
  UR_EXPECT_TRUE(frame->contains("interactive") && frame->at("interactive") == false);
  UR_EXPECT_FALSE(ctl::ResetExtendersAllowsInteraction(*frame));
  UR_EXPECT_FALSE(frame->get<ctl::ResetExtendersRequest>().interactive);

  nlohmann::json odd = nlohmann::json(press);
  odd["interactive"] = nullptr;
  UR_EXPECT_TRUE(ctl::ResetExtendersAllowsInteraction(odd));
  odd["interactive"] = true;
  UR_EXPECT_TRUE(ctl::ResetExtendersAllowsInteraction(odd));
  for (const nlohmann::json& value :
       {nlohmann::json("false"), nlohmann::json(0), nlohmann::json::object()}) {
    odd["interactive"] = value;
    UR_EXPECT_TRUE_MSG(value.dump(), !ctl::ResetExtendersAllowsInteraction(odd));
    // and strict once authorized: the parse refuses what is not a boolean
    bool threw = false;
    try {
      (void)odd.get<ctl::ResetExtendersRequest>();
    } catch (const std::exception&) {
      threw = true;
    }
    UR_EXPECT_TRUE_MSG(value.dump(), threw);
  }
}

UR_TEST(controlLocationOverrideRoundTrips) {
  ctl::LocationOverrideWriteRequest write;
  write.lat = 35.6762;
  write.lon = 139.6503;
  write.accuracy_m = 5000.0;
  const auto writeBack = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::LocationOverrideWrite, 6, nlohmann::json(write))))
      ->get<ctl::LocationOverrideWriteRequest>();
  UR_EXPECT_NEAR(35.6762, writeBack.lat, 1e-9);
  UR_EXPECT_NEAR(139.6503, writeBack.lon, 1e-9);
  UR_EXPECT_NEAR(5000.0, writeBack.accuracy_m, 1e-9);

  ctl::LocationOverrideAvailableReply avail;
  avail.available = false;
  avail.reason = "read_only_etc";
  const auto availBack = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(7, true, nlohmann::json(avail))))
      ->get<ctl::LocationOverrideAvailableReply>();
  UR_EXPECT_FALSE(availBack.available);
  UR_EXPECT_TRUE(availBack.reason == "read_only_etc");
}

UR_TEST(controlErrorReplyCarriesErrorAndCode) {
  const auto j = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeErrorReply(
      11, "client protocol 0 unsupported", ctl::kCodeClientProtocolTooOld)));
  UR_EXPECT_FALSE(ctl::ReplyOk(*j));
  UR_EXPECT_TRUE(ctl::ReplyError(*j) == "client protocol 0 unsupported");
  UR_EXPECT_TRUE(ctl::ReplyCode(*j) == ctl::kCodeClientProtocolTooOld);
  UR_EXPECT_TRUE(ctl::FrameId(*j) == 11);
  // a plain failure has no code
  const auto plain = ctl::DecodeFrame(ctl::EncodeFrame(ctl::MakeErrorReply(12, "boom")));
  UR_EXPECT_TRUE(ctl::ReplyCode(*plain).empty());
}

// ---- version negotiation matrix (both rejection directions) ----------------

UR_TEST(versionDaemonRejectsClientBelowMinimum) {
  // rejection direction 1: old GUI against a new daemon
  UR_EXPECT_FALSE(ctl::DaemonAcceptsClientProtocol(ctl::kMinSupportedClientProtocol - 1));
  UR_EXPECT_FALSE(ctl::DaemonAcceptsClientProtocol(0));   // pre-versioning peer
  UR_EXPECT_FALSE(ctl::DaemonAcceptsClientProtocol(-1));  // nonsense stays rejected
}

UR_TEST(versionDaemonAcceptsClientAtAndAboveMinimum) {
  UR_EXPECT_TRUE(ctl::DaemonAcceptsClientProtocol(ctl::kMinSupportedClientProtocol));
  UR_EXPECT_TRUE(ctl::DaemonAcceptsClientProtocol(ctl::kControlProtocolVersion));
  // a NEWER client is accepted here: the newer side runs its own min-check
  UR_EXPECT_TRUE(ctl::DaemonAcceptsClientProtocol(ctl::kControlProtocolVersion + 1));
}

UR_TEST(versionClientRejectsDaemonBelowMinimum) {
  // rejection direction 2: new GUI against an old daemon ("daemon too old")
  UR_EXPECT_FALSE(ctl::ClientAcceptsDaemonProtocol(ctl::kMinSupportedDaemonProtocol - 1));
  UR_EXPECT_FALSE(ctl::ClientAcceptsDaemonProtocol(0));
  UR_EXPECT_FALSE(ctl::ClientAcceptsDaemonProtocol(-1));
}

UR_TEST(versionClientAcceptsDaemonAtAndAboveMinimum) {
  UR_EXPECT_TRUE(ctl::ClientAcceptsDaemonProtocol(ctl::kMinSupportedDaemonProtocol));
  UR_EXPECT_TRUE(ctl::ClientAcceptsDaemonProtocol(ctl::kControlProtocolVersion));
  // a NEWER daemon is accepted: if it dropped support for us, ITS min-check
  // rejects our hello with kCodeClientProtocolTooOld
  UR_EXPECT_TRUE(ctl::ClientAcceptsDaemonProtocol(ctl::kControlProtocolVersion + 1));
}

UR_TEST(versionConstantsAreInternallyConsistent) {
  // this build must accept itself in both directions, and the minimums can
  // never exceed the current version
  UR_EXPECT_TRUE(ctl::DaemonAcceptsClientProtocol(ctl::kControlProtocolVersion));
  UR_EXPECT_TRUE(ctl::ClientAcceptsDaemonProtocol(ctl::kControlProtocolVersion));
  UR_EXPECT_TRUE(ctl::kMinSupportedClientProtocol <= ctl::kControlProtocolVersion);
  UR_EXPECT_TRUE(ctl::kMinSupportedDaemonProtocol <= ctl::kControlProtocolVersion);
}

// ---- SO_PEERCRED authorization decision ------------------------------------
// (uid, gid, peer group list, control-group gid) -> allow/deny. The socket
// layer resolves the numbers; the policy is pure and lives in the protocol
// header so both binaries and this test share one decision.

namespace {
constexpr int64_t kUrnetworkGid = 990;
}

UR_TEST(authRootIsAlwaysAllowed) {
  UR_EXPECT_TRUE(ctl::AuthorizeControlPeer(0, 0, {}, kUrnetworkGid));
  // root stays allowed even when the urnetwork group does not exist
  UR_EXPECT_TRUE(ctl::AuthorizeControlPeer(0, 0, {}, -1));
  // root by uid, whatever its groups say
  UR_EXPECT_TRUE(ctl::AuthorizeControlPeer(0, 12345, {54321}, kUrnetworkGid));
}

UR_TEST(authPrimaryGroupMemberIsAllowed) {
  UR_EXPECT_TRUE(ctl::AuthorizeControlPeer(1000, kUrnetworkGid, {}, kUrnetworkGid));
}

UR_TEST(authSupplementaryGroupMemberIsAllowed) {
  UR_EXPECT_TRUE(
      ctl::AuthorizeControlPeer(1000, 1000, {4, 24, kUrnetworkGid, 1000}, kUrnetworkGid));
}

UR_TEST(authNonMemberIsDenied) {
  UR_EXPECT_FALSE(ctl::AuthorizeControlPeer(1000, 1000, {}, kUrnetworkGid));
  UR_EXPECT_FALSE(ctl::AuthorizeControlPeer(1000, 1000, {4, 24, 1000}, kUrnetworkGid));
}

UR_TEST(authMissingControlGroupFailsClosed) {
  // no urnetwork group on the system: only root may pass — a peer whose gids
  // happen to collide with the "missing" sentinel must NOT sneak in
  UR_EXPECT_FALSE(ctl::AuthorizeControlPeer(1000, kUrnetworkGid, {kUrnetworkGid}, -1));
  UR_EXPECT_FALSE(ctl::AuthorizeControlPeer(1000, -1, {-1}, -1));
}

UR_TEST(authUidIsTheIdentityNeverAdjacentFields) {
  // a peer claiming gid 0 without uid 0 is NOT root
  UR_EXPECT_FALSE(ctl::AuthorizeControlPeer(1000, 0, {0}, kUrnetworkGid));
}
