// The GUI's mirror of the traffic dns settings (DnsMirror.hpp): when a write
// leaves it marked, and what a bound device's sync does with it.
//
// The defects these rules close: a write made with no device over an
// unmarked mirror, one left from before the daemon split, marked it, so the
// next start applied that old copy over the daemon's newer settings; and
// every start applied the mirror whether or not it differed from the daemon's,
// which restarts the device's dns resolvers mid-session.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include "DnsMirror.hpp"

namespace {

namespace mirror = urnw::dns_mirror;

}  // namespace

UR_TEST(DnsMirror_AWriteMarksTheMirrorOnlyWhenItHoldsTheDevicesSettings) {
  // the device took the write: the mirror holds what the device holds
  UR_EXPECT_TRUE(mirror::MarkedAfterWrite(/*deviceBound=*/true, /*wasMarked=*/false));
  UR_EXPECT_TRUE(mirror::MarkedAfterWrite(/*deviceBound=*/true, /*wasMarked=*/true));
  // no device, over a mirror that held every write: it still does
  UR_EXPECT_TRUE(mirror::MarkedAfterWrite(/*deviceBound=*/false, /*wasMarked=*/true));
  // no device, over an unmarked mirror (perhaps a copy from before the daemon
  // split): the daemon's settings may be newer than what the write started from
  UR_EXPECT_FALSE(mirror::MarkedAfterWrite(/*deviceBound=*/false, /*wasMarked=*/false));
}

UR_TEST(DnsMirror_TheSyncAdoptsAppliesOrKeeps) {
  // an unmarked mirror takes the daemon's settings, whatever it holds
  UR_EXPECT_TRUE(mirror::SyncAction(/*marked=*/false, /*present=*/true, /*same=*/false) ==
                 mirror::Sync::Adopt);
  UR_EXPECT_TRUE(mirror::SyncAction(/*marked=*/false, /*present=*/true, /*same=*/true) ==
                 mirror::Sync::Adopt);
  UR_EXPECT_TRUE(mirror::SyncAction(/*marked=*/false, /*present=*/false, /*same=*/false) ==
                 mirror::Sync::Adopt);
  // a marked mirror that differs is an edit made with the tunnel down
  UR_EXPECT_TRUE(mirror::SyncAction(/*marked=*/true, /*present=*/true, /*same=*/false) ==
                 mirror::Sync::Apply);
  // a marked mirror that matches leaves the device alone
  UR_EXPECT_TRUE(mirror::SyncAction(/*marked=*/true, /*present=*/true, /*same=*/true) ==
                 mirror::Sync::Keep);
}

// A write that could neither clear the mark nor write the mirror (a full or
// read-only home) removes the mirror: marked but missing, it adopts the
// daemon's settings rather than leave a stale copy to be applied under the
// mark.
UR_TEST(DnsMirror_AMarkedMirrorThatIsMissingAdopts) {
  UR_EXPECT_TRUE(mirror::SyncAction(/*marked=*/true, /*present=*/false, /*same=*/false) ==
                 mirror::Sync::Adopt);
}
