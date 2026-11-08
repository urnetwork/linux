// The client limit notice's presentation and SdkHost's feed of the hold
// (ClientLimitNotice.hpp). The decisions are the sdk's (the prompt, and the
// notification's ClientLimitNoticeTracker, tested in sdk
// client_limit_status_test.go); what is pinned here is what the GUI adds to
// them: the words and buttons for each prompt, what a refused sign-in says, when
// the notification is sent, and a feed that never shows a hold the device no
// longer has.
//
// SPDX-License-Identifier: MPL-2.0
#include <cstring>
#include <string>

#include "ClientLimitNotice.hpp"
#include "TestHarness.hpp"

namespace cl = urnw::client_limit;

namespace {

cl::Status HeldStatus(cl::Prompt prompt) {
  cl::Status status;
  status.exceeded = true;
  status.retryTimeMillis = 1700000000000;
  status.prompt = prompt;
  return status;
}

cl::Reading Available(const cl::Status& status) {
  cl::Reading reading;
  reading.available = true;
  reading.status = status;
  return reading;
}

bool SameText(const cl::Text& a, const cl::Text& b) {
  return std::strcmp(a.key, b.key) == 0 && std::strcmp(a.english, b.english) == 0;
}

}  // namespace

// ---- the notice ----------------------------------------------------------------

// Shown exactly while the device is held and a connection is requested: the
// traffic then goes nowhere until the user acts. Not before the hold, not after
// it, not with no connection requested, and never from a feed that cannot say.
UR_TEST(ClientLimitNotice_ShowsWhileHeldAndAConnectionIsRequested) {
  const cl::Reading held = Available(HeldStatus(cl::Prompt::Upgrade));
  UR_EXPECT_TRUE(cl::NoticeFor(held, /*connectRequested=*/true).shows);
  UR_EXPECT_FALSE(cl::NoticeFor(held, /*connectRequested=*/false).shows);
  UR_EXPECT_FALSE(cl::NoticeFor(Available(cl::Status{}), true).shows);
  cl::Reading unavailable = held;
  unavailable.available = false;
  UR_EXPECT_FALSE(cl::NoticeFor(unavailable, true).shows);
  UR_EXPECT_FALSE(cl::Held(unavailable));
  UR_EXPECT_TRUE(cl::Held(held));
}

// A free plan: the upgrade body, with Get Pro beside Retry.
UR_TEST(ClientLimitNotice_AFreePlanIsOfferedGetPro) {
  const cl::Notice notice = cl::NoticeFor(Available(HeldStatus(cl::Prompt::Upgrade)), true);
  UR_EXPECT_TRUE(notice.offersUpgrade);
  UR_EXPECT_TRUE(SameText(notice.body, cl::kUpgradeBody));
}

// A Pro plan, already the highest: free a slot, with no upgrade on offer.
UR_TEST(ClientLimitNotice_AProPlanIsToldToFreeASlot) {
  const cl::Notice notice =
      cl::NoticeFor(Available(HeldStatus(cl::Prompt::RemoveClient)), true);
  UR_EXPECT_FALSE(notice.offersUpgrade);
  UR_EXPECT_TRUE(SameText(notice.body, cl::kRemoveClientBody));
}

// A daemon too old to send a prompt reads as free, as the apple app reads it.
UR_TEST(ClientLimitNotice_NoPromptReadsAsFree) {
  const cl::Notice notice = cl::NoticeFor(Available(HeldStatus(cl::Prompt::None)), true);
  UR_EXPECT_TRUE(notice.offersUpgrade);
  UR_EXPECT_TRUE(SameText(notice.body, cl::kUpgradeBody));
}

// The words are the store's three keys (CatalogLookupTest holds the English to
// po/urnetwork.pot byte for byte).
UR_TEST(ClientLimitNotice_TheWordsAreTheStoresKeys) {
  UR_EXPECT_TRUE(std::string(cl::kTitle.key) == "client_limit_reached");
  UR_EXPECT_TRUE(std::string(cl::kUpgradeBody.key) == "client_limit_upgrade_message");
  UR_EXPECT_TRUE(std::string(cl::kRemoveClientBody.key) == "client_limit_remove_client_message");
}

// ---- a refused sign-in ---------------------------------------------------------

// The client limit refused the device's client: the sign-in screens say what to
// do, by the sdk's prompt, never the server's words (which, with x402 on, were
// the payment terms). Any other refusal keeps them.
UR_TEST(ClientLimitNotice_ARefusedSignInSaysWhatToDo) {
  const cl::Text* upgrade = cl::SignInRefusalBody(cl::Prompt::Upgrade);
  const cl::Text* removeClient = cl::SignInRefusalBody(cl::Prompt::RemoveClient);
  UR_EXPECT_TRUE(upgrade != nullptr && SameText(*upgrade, cl::kUpgradeBody));
  UR_EXPECT_TRUE(removeClient != nullptr && SameText(*removeClient, cl::kRemoveClientBody));
  UR_EXPECT_TRUE(cl::SignInRefusalBody(cl::Prompt::None) == nullptr);
}

// ---- the desktop notification --------------------------------------------------

// Out of the user's sight the tracker's post alerts and its update replaces the
// notice quietly; in front, the Connect page's notice says it and nothing is
// sent. A withdraw always goes through.
UR_TEST(ClientLimitNotice_TheNotificationGoesOnlyWhileTheAppIsAway) {
  using Action = cl::TrackerAction;
  using Step = cl::NotificationStep;
  UR_EXPECT_TRUE(cl::NotificationStepFor(Action::Post, /*background=*/true) == Step::Send);
  UR_EXPECT_TRUE(cl::NotificationStepFor(Action::Post, /*background=*/false) == Step::None);
  UR_EXPECT_TRUE(cl::NotificationStepFor(Action::Update, true) == Step::SendQuietly);
  UR_EXPECT_TRUE(cl::NotificationStepFor(Action::Update, false) == Step::None);
  UR_EXPECT_TRUE(cl::NotificationStepFor(Action::Remove, true) == Step::Withdraw);
  UR_EXPECT_TRUE(cl::NotificationStepFor(Action::Remove, false) == Step::Withdraw);
  UR_EXPECT_TRUE(cl::NotificationStepFor(Action::None, true) == Step::None);
}

// ---- SdkHost's feed ------------------------------------------------------------

// Nothing shows until the remote connects; the hold shows over a connected
// remote; a remote that drops takes the hold with it, and a reconnect shows
// none until the device says so again (its sync replays the status).
UR_TEST(ClientLimitNotice_TheFeedShowsAHoldOnlyOverAConnectedRemote) {
  cl::Feed feed;
  const uint64_t generation = feed.Bind();
  UR_EXPECT_FALSE(feed.Current().available);
  UR_EXPECT_FALSE(feed.NoteStatus(generation, HeldStatus(cl::Prompt::Upgrade)));
  UR_EXPECT_FALSE(feed.Current().available);
  UR_EXPECT_TRUE(feed.NoteRemote(generation, true));
  UR_EXPECT_TRUE(cl::Held(feed.Current()));
  UR_EXPECT_TRUE(feed.NoteRemote(generation, false));
  UR_EXPECT_FALSE(feed.Current().available);
  UR_EXPECT_FALSE(feed.Current().status.exceeded);
  UR_EXPECT_TRUE(feed.NoteRemote(generation, true));
  UR_EXPECT_FALSE(cl::Held(feed.Current()));
  UR_EXPECT_TRUE(feed.NoteStatus(generation, HeldStatus(cl::Prompt::RemoveClient)));
  UR_EXPECT_TRUE(cl::Held(feed.Current()));
  UR_EXPECT_TRUE(feed.Current().status.prompt == cl::Prompt::RemoveClient);
  // the hold ends
  UR_EXPECT_TRUE(feed.NoteStatus(generation, cl::Status{}));
  UR_EXPECT_FALSE(cl::Held(feed.Current()));
}

// A push from a device torn down since changes nothing, and neither does one
// after the feed was cleared.
UR_TEST(ClientLimitNotice_ATornDownDevicesPushIsDropped) {
  cl::Feed feed;
  const uint64_t first = feed.Bind();
  feed.NoteRemote(first, true);
  const uint64_t second = feed.Bind();
  UR_EXPECT_FALSE(feed.NoteRemote(first, true));
  UR_EXPECT_FALSE(feed.NoteStatus(first, HeldStatus(cl::Prompt::Upgrade)));
  UR_EXPECT_FALSE(feed.Current().available);
  feed.NoteRemote(second, true);
  feed.NoteStatus(second, HeldStatus(cl::Prompt::Upgrade));
  UR_EXPECT_TRUE(cl::Held(feed.Current()));
  feed.Clear();
  UR_EXPECT_FALSE(feed.Current().available);
  UR_EXPECT_FALSE(feed.NoteStatus(second, HeldStatus(cl::Prompt::Upgrade)));
  UR_EXPECT_FALSE(feed.Current().available);
}

// The reads after registering lose to a push that landed since: a push is
// always newer than the read.
UR_TEST(ClientLimitNotice_AReadLosesToAPushSinceIt) {
  cl::Feed feed;
  const uint64_t generation = feed.Bind();
  const uint64_t statusRevision = feed.StatusRevision();
  const uint64_t remoteRevision = feed.RemoteRevision();
  // the listeners report while the reads are in flight
  feed.NoteRemote(generation, true);
  feed.NoteStatus(generation, cl::Status{});
  // the reads, taken before those pushes, say otherwise and lose
  UR_EXPECT_FALSE(feed.SeedRemote(generation, remoteRevision, false));
  UR_EXPECT_FALSE(feed.SeedStatus(generation, statusRevision, HeldStatus(cl::Prompt::Upgrade)));
  UR_EXPECT_TRUE(feed.Current().available);
  UR_EXPECT_FALSE(cl::Held(feed.Current()));
  // with no push since, a read applies
  cl::Feed quiet;
  const uint64_t g = quiet.Bind();
  const uint64_t sr = quiet.StatusRevision();
  const uint64_t rr = quiet.RemoteRevision();
  UR_EXPECT_TRUE(quiet.SeedRemote(g, rr, true));
  UR_EXPECT_TRUE(quiet.SeedStatus(g, sr, HeldStatus(cl::Prompt::Upgrade)));
  UR_EXPECT_TRUE(cl::Held(quiet.Current()));
}

// The window drops a reading marshaled late behind a newer one by its serial;
// SdkHost numbers a reading only when it changed (SameAs).
UR_TEST(ClientLimitNotice_AReadingIsTheSameByItsFacts) {
  cl::Reading a = Available(HeldStatus(cl::Prompt::Upgrade));
  cl::Reading b = a;
  b.serial = a.serial + 7;
  UR_EXPECT_TRUE(a.SameAs(b));
  b.status.prompt = cl::Prompt::RemoveClient;
  UR_EXPECT_FALSE(a.SameAs(b));
  cl::Reading c = a;
  c.available = false;
  UR_EXPECT_FALSE(a.SameAs(c));
}
