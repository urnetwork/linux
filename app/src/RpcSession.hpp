// The GUI's half of the device-RPC mTLS session: the pure rules that decide
// whether a REMEMBERED session may be re-attached to, and what to do with one
// that cannot be read back.
//
// The session itself is stored by RpcSessionStore.hpp: non-secret metadata in
// a 0600 file, the mTLS client private key and the pinned server certificate
// in the desktop Secret Service. Nothing in THIS header touches either half —
// it is the predicate side, nlohmann + ControlProtocol.hpp + RpcSessionStore.hpp
// only, no glib, no GTK, no SDK, no libsecret — so every decision below is
// unit-testable without a keyring, a daemon, or a GTK loop. Its call site is
// SdkHost::StartTunnelLocked, which asks all of these questions on every
// Connect.
//
// ---- THE TWO DOORS, AND WHY THEY CANNOT RACE -------------------------------
// A relaunch that finds a tunnel already up takes one of two paths, and the
// division between them is structural rather than a matter of ordering:
//
//   attach_tunnel — THE EXPLICIT DOOR. The GUI NAMES the live session by
//     (instance_id, rpc_session_id) and re-presents no key material at all.
//     Reachable exactly when the stored record still describes the tunnel the
//     daemon reports: CanAttach() below, which is IsUsableRecord() (shape) AND
//     RpcSessionMatchesStatus() (identity + liveness + port).
//
//   start_tunnel + TunnelHost::CanAdopt — THE FALLBACK. CanAdopt compares the
//     pinning TRIPLE byte for byte, and two thirds of that triple
//     (rpc_server_pem, rpc_client_cert_pem) are the DAEMON's half, which is
//     deliberately not in the record: only the GUI's own client key and the
//     cert it pins are kept, because those are the only two this side has any
//     use for. A relaunched GUI therefore CANNOT satisfy CanAdopt — it no
//     longer holds the daemon's half to re-present — so the two mechanisms
//     cannot both be live for one relaunch. CanAdopt keeps exactly the job it
//     can still do: absorbing an identical start_tunnel re-sent inside ONE GUI
//     session, whose material is still in memory.
//
// So the order in SdkHost is attach first, start second, and "second" is a
// genuine fallback for every failure of the first — a missing record, a locked
// keyring, a stale entry, a tunnel that has since stopped, a tunnel belonging
// to another uid — never a competing reattach.
//
// ---- THE INSTANCE-ID TRAP (a real Windows post-mortem — urnetwork-windows
// app/src/Common/RpcSessionBlob.h) ------------------------------------------
// The SDK rotates the local instance id whenever the by-client JWT STRING
// changes, and a JWT refresh re-signs the same client. So
// localState_->getInstanceId() at the next launch can differ from the id the
// daemon's DeviceLocal was born with, and DeviceLocalRpc.Sync refuses forever
// a nonzero instance id that is not its own — a DeviceRemote that connects and
// never populates, with no synchronous signal anywhere. The record therefore
// stores the id the session was STARTED with, and the attach path uses THAT,
// never the one local state holds now.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

#include "ControlProtocol.hpp"
#include "RpcSessionStore.hpp"

namespace urnw::rpcsession {

// Canonical 36-char dashed uuid ONLY, and the NIL uuid is REFUSED. Applied to
// BOTH identifiers a session is named by:
//
//   instance_id — a zero id is the value the SDK uses to SKIP the pairing
//     check, so attaching with one would pair with anything (or nothing)
//     instead of failing loudly.
//
//   rpc_session_id — the generation name this GUI mints for the credential it
//     just created. TIGHTER THAN THE WIRE GATE on purpose, exactly like the
//     port gate below: ctl::LooksLikeRpcSessionId admits any 1..128 printable
//     ASCII bytes because it also guards values arriving off the control
//     socket from other clients, whose format this fork does not get to
//     dictate. A record is a value THIS app wrote, and this app mints
//     g_uuid_string_random(), so the narrower gate holds here and cannot
//     disagree with the broader one.
//
// A non-pairable id is not an error — it means "do not remember this session",
// which costs a tunnel rebuild and never a wrong pairing.
inline constexpr bool IsPairableId(std::string_view s) noexcept {
  if (s.size() != 36) return false;
  bool anyNonZero = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
      continue;
    }
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
    if (c != '0') anyNonZero = true;
  }
  return anyNonZero;
}

// The port half of the shape gate, TIGHTER than ctl::IsLoopbackRpcHostPort —
// which accepts any 1024..65535 because it also guards values arriving off the
// control socket from other clients. A record is a value THIS app wrote, and
// this app draws its listener port from [kRpcPortMin, kRpcPortMax]
// (SdkHost.cpp RandomLoopbackRpcHostPort), a range that deliberately excludes
// ctl::kDeviceRpcPort.
//
// THE ONE THAT MATTERS IS 12025. The GUI reserves 127.0.0.1:12025 for the
// whole process (SdkHost::HoldDeviceRpcDefaultPortLocked) so the SDK's
// hard-coded unpinned first dial lands on a dead address. A record naming that
// same port would therefore have us dial the address this process is holding:
// the connection can never come up, and the only symptom is a DeviceRemote
// that never connects until the bind watchdog kills the session 8 s later.
// Refusing the record costs one tunnel rebuild instead.
inline bool IsUsableRpcHostPort(const std::string& host_port) {
  const int port = ctl::RpcPortFromHostPort(host_port);
  return ctl::kRpcPortMin <= port && port <= ctl::kRpcPortMax;
}

// SHAPE ONLY: is this record one we could dial at all? Deliberately stricter
// than RpcSessionStore's ValidRpcSessionRecord, which asks only that the
// fields are non-empty — that is the storage layer's business. This is the
// dialling layer's: two pairable uuids, two PEMs that parse, and a port in our
// own draw range.
inline bool IsUsableRecord(const RpcSessionRecord& record) {
  return ValidRpcSessionRecord(record) && IsPairableId(record.instance_id) &&
         IsPairableId(record.rpc_session_id) && IsUsableRpcHostPort(record.host_port) &&
         ctl::LooksLikePem(record.client_pem) && ctl::LooksLikePem(record.server_cert_pem);
}

// THE ATTACH GATE. Both halves are required and neither is redundant:
//
//   IsUsableRecord           — we can dial it (shape).
//   RpcSessionMatchesStatus  — it is the tunnel that is ACTUALLY running
//                              (tunnel_state Up, the same instance_id, the
//                              same rpc_session_id, and the daemon's live
//                              rpc_port equal to the host_port we would dial).
//
// The identity comparison is what keeps a stale or foreign record from
// attaching to somebody else's session: a record naming a session that is not
// the live one fails here, on this side, before a frame is sent — and the
// daemon refuses the same case again with kCodeRpcSessionMismatch if a client
// asks anyway. Cross-uid take-over is a separate axis and stays where it
// belongs: the daemon charges kActionTakeOverTunnel for it.
inline bool CanAttach(const RpcSessionRecord& record, const ctl::StatusReply& status) {
  return IsUsableRecord(record) && RpcSessionMatchesStatus(record, status);
}

// ---- what to do with a record that would not load --------------------------
// LoadRpcSessionRecord reports WHY it refused, and the answers fall into two
// groups that must not be treated alike. Getting this wrong in either
// direction is a user-visible defect:
//
//   * Discarding too eagerly throws away a good credential because the keyring
//     happened to be locked at login, and orphans its Secret Service item.
//   * Retaining too eagerly wedges a user behind a record that can never load,
//     re-failing on every launch forever.
//
// Neither ever blocks connecting: every fault below falls back to a fresh
// start_tunnel. The only question is whether the stored record survives it.
enum class StoredSessionFault {
  None,                // it loaded
  Absent,              // no record at all — the ordinary first run
  Unreadable,          // the metadata file cannot be parsed, is not ours, or is
                       // not securely owned. Includes the fork's OLD plaintext
                       // blob format, which carried both private keys on disk
                       // and has no `version`: discarding it is how that file
                       // finally leaves the disk.
  MigrationFailed,     // a v1 record could not be committed to the Secret
                       // Service (a locked keyring lands here)
  KeyringLocked,       // the collection is locked and was not unlocked
  KeyringUnavailable,  // no Secret Service on the bus, or the prompt was
                       // cancelled
  KeyringMissing,      // metadata references an item that is not there
  KeyringCorrupt,      // the item is there and is not a session payload
  Unknown,             // a diagnostic this build does not recognise
};

// Pure mapping from LoadRpcSessionRecord's diagnostic. Prefix-matched for
// "migration_*", exact otherwise, and anything unrecognised is Unknown rather
// than silently folded into a neighbour.
inline StoredSessionFault FaultFromDiagnostic(std::string_view diagnostic) {
  if (diagnostic.rfind("migration_", 0) == 0) return StoredSessionFault::MigrationFailed;
  if (diagnostic == "missing") return StoredSessionFault::Absent;
  if (diagnostic == "corrupt" || diagnostic == "open_failed" ||
      diagnostic == "read_failed" || diagnostic == "insecure_or_invalid_file") {
    return StoredSessionFault::Unreadable;
  }
  if (diagnostic == "secret_service_locked") return StoredSessionFault::KeyringLocked;
  if (diagnostic == "secret_service_unavailable" ||
      diagnostic == "secret_service_cancelled" ||
      diagnostic == "secret_service_load_failed" ||
      diagnostic == "secret_invalid_identity") {
    return StoredSessionFault::KeyringUnavailable;
  }
  if (diagnostic == "secret_missing") return StoredSessionFault::KeyringMissing;
  if (diagnostic == "secret_corrupt") return StoredSessionFault::KeyringCorrupt;
  return StoredSessionFault::Unknown;
}

// True when the stored record can NEVER load again, so keeping it only costs
// the user a failed lookup on every launch. False for everything transient —
// and, deliberately, for Unknown: a diagnostic this build does not understand
// is not evidence that the credential is dead, and destroying key material on
// a string we cannot interpret is the wrong direction to fail in.
inline bool ShouldForget(StoredSessionFault fault) {
  switch (fault) {
    case StoredSessionFault::Unreadable:
    case StoredSessionFault::KeyringMissing:
    case StoredSessionFault::KeyringCorrupt:
      return true;
    case StoredSessionFault::None:
    case StoredSessionFault::Absent:
    case StoredSessionFault::MigrationFailed:
    case StoredSessionFault::KeyringLocked:
    case StoredSessionFault::KeyringUnavailable:
    case StoredSessionFault::Unknown:
      return false;
  }
  return false;
}

// One sentence for the log, so a user who cannot reattach can be told why in
// words rather than left with a diagnostic token. Never rendered in the UI:
// every one of these ends in a normal fresh connect, which is not a failure the
// user has to act on.
inline const char* Explain(StoredSessionFault fault) {
  switch (fault) {
    case StoredSessionFault::None:
      return "the stored session loaded";
    case StoredSessionFault::Absent:
      return "no session is remembered on this machine";
    case StoredSessionFault::Unreadable:
      return "the stored session file is unreadable or is not in this app's format";
    case StoredSessionFault::MigrationFailed:
      return "the stored session could not be moved into the login keyring";
    case StoredSessionFault::KeyringLocked:
      return "the login keyring is locked";
    case StoredSessionFault::KeyringUnavailable:
      return "no login keyring answered";
    case StoredSessionFault::KeyringMissing:
      return "the login keyring no longer holds this session's credentials";
    case StoredSessionFault::KeyringCorrupt:
      return "the credentials in the login keyring are not usable";
    case StoredSessionFault::Unknown:
      return "the stored session was refused for an unrecognised reason";
  }
  return "the stored session was refused";
}

}  // namespace urnw::rpcsession
