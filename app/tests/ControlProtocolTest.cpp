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
      ctl::Verb::LocationOverrideAvailable,
      ctl::Verb::LocationOverrideWrite, ctl::Verb::LocationOverrideClear,
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
  UR_EXPECT_TRUE(ctl::VerbFromString("location_override_available") ==
                 ctl::Verb::LocationOverrideAvailable);
  UR_EXPECT_TRUE(ctl::VerbFromString("location_override_write") ==
                 ctl::Verb::LocationOverrideWrite);
  UR_EXPECT_TRUE(ctl::VerbFromString("location_override_clear") ==
                 ctl::Verb::LocationOverrideClear);
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
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeReply(4, true, nlohmann::json(status))))->get<ctl::StatusReply>();
  UR_EXPECT_TRUE(back.tunnel_state == ctl::TunnelState::Up);
  UR_EXPECT_EQ(12025, back.rpc_port);
  UR_EXPECT_TRUE(back.client_id == "client-1");
  UR_EXPECT_TRUE(back.error.empty());
}

UR_TEST(controlSetProvideRoundTrip) {
  ctl::SetProvideRequest req;
  req.mode = "network";
  const auto back = ctl::DecodeFrame(ctl::EncodeFrame(
      ctl::MakeRequest(ctl::Verb::SetProvide, 5, nlohmann::json(req))));
  UR_EXPECT_TRUE(back->get<ctl::SetProvideRequest>().mode == "network");
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
