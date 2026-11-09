// The GUI's mirror of the traffic dns settings (SdkHost). The daemon's
// DeviceLocal persists the settings in its own local state, which the GUI does
// not share, so the GUI keeps a copy in its LocalState: the only one with the
// tunnel down, and what a bound device's sync takes (connect ADBLOCK3RD.md
// F2). A mark, one per network space, says the copy holds every write this
// app made since it last matched the daemon's; a copy without it may be one
// left from before the daemon split, older than the daemon's own, and is
// never applied over it.
//
// Pure, so the unit tests reach it (tests/DnsMirrorTest.cpp).
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::dns_mirror {

// Whether the mirror is marked after a write it took. The mark is cleared
// while the device and the mirror are written, so a crash or a failed write
// between them leaves it unmarked. A write the device took marks it, the
// mirror holding what the device holds; so does a write over a marked mirror.
// A write with no device over an unmarked mirror leaves it unmarked: the
// daemon's settings may be newer than the copy the write started from.
constexpr bool MarkedAfterWrite(bool deviceBound, bool wasMarked) {
  return deviceBound || wasMarked;
}

// What the sync of a bound device does with the mirror.
enum class Sync {
  Adopt,  // the mirror takes the daemon's settings, and is marked
  Apply,  // the device takes the mirror: an edit made with the tunnel down
  Keep,   // the two match, and a settings swap would restart the device's dns
};

// `present`: the mirror exists. A marked one that does not adopts too: a
// write that could neither clear the mark nor write the mirror (a full or
// read-only home) removes the mirror, so no stale copy is applied under it.
constexpr Sync SyncAction(bool marked, bool present, bool same) {
  if (!marked || !present) return Sync::Adopt;
  return same ? Sync::Keep : Sync::Apply;
}

}  // namespace urnw::dns_mirror
