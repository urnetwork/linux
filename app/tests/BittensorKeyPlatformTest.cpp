// What only Linux decides for the one-tap Bittensor key (BittensorKeyPlatform.hpp;
// apple BTEASY.md, "Porting"): a Secret Service that isn't there answers the
// SDK's BittensorLocalKeyStoreAbsent, so the flow offers no key and the
// disclaimer's first sentence never shows untrue, while one that can't answer
// now answers BittensorLocalKeyStoreUnavailable; polkit's answers for Reveal and
// Delete, where a dismissed or refused dialog never passes and a desktop where
// polkit can't ask does, as Apple's device with no passcode; and the store's
// notes, kept by network id beside app_prefs.json, which a sign-out leaves.
// The notes run on a temporary directory.
// SPDX-License-Identifier: MPL-2.0
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "BittensorKeyPlatform.hpp"
#include "TestHarness.hpp"

namespace {

namespace fs = std::filesystem;
namespace key = urnw::bittensor_key;

constexpr const char* kNetworkA = "01890000-0000-7000-8000-000000000001";
constexpr const char* kNetworkB = "01890000-0000-7000-8000-000000000002";

// A fresh directory, removed with everything in it when the case ends.
class NotesDir {
 public:
  NotesDir() {
    std::string pattern = (fs::temp_directory_path() / "urnw-key-notes-XXXXXX").string();
    if (::mkdtemp(pattern.data()) != nullptr) dir_ = pattern;
  }
  ~NotesDir() {
    std::error_code error;
    if (!dir_.empty()) fs::remove_all(dir_, error);
  }
  std::string File() const { return dir_ + "/" + key::kNotesFileName; }
  bool Ok() const { return !dir_.empty(); }
  std::string Text() const {
    std::ifstream in(File(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

 private:
  std::string dir_;
};

}  // namespace

// The text `actual` is `expected`, saying both when not.
#define UR_EXPECT_TEXT(expected, actual)                                              \
  do {                                                                                \
    const std::string ur_t_e_ = (expected);                                           \
    const std::string ur_t_a_ = (actual);                                             \
    UR_EXPECT_TRUE_MSG(#actual ": expected \"" + ur_t_e_ + "\", got \"" + ur_t_a_ + "\"", \
                       ur_t_e_ == ur_t_a_);                                           \
  } while (0)

// ---- the store's answer for a failed Secret Service call -----------------------------

// Nobody on the bus to answer for the Secret Service (or, sandboxed, for the
// secret portal): there is no secure storage here, and no key is offered.
UR_TEST(BittensorKeyStore_NoSecretServiceIsAbsent) {
  for (const char* name :
       {"org.freedesktop.DBus.Error.ServiceUnknown", "org.freedesktop.DBus.Error.NameHasNoOwner",
        "org.freedesktop.DBus.Error.UnknownMethod", "org.freedesktop.DBus.Error.UnknownInterface",
        "org.freedesktop.DBus.Error.UnknownObject"}) {
    UR_EXPECT_TRUE_MSG(name, key::StoreFailureFor(name) == key::StoreFailure::Absent);
  }
}

// A locked collection, a dismissed prompt, a timeout, or a failure that never
// reached the bus: unknown, which is not "no key", and never "no storage".
UR_TEST(BittensorKeyStore_ACanNotAnswerNowIsUnavailable) {
  for (const char* name :
       {"", "org.freedesktop.Secret.Error.IsLocked", "org.freedesktop.DBus.Error.NoReply",
        "org.freedesktop.DBus.Error.Timeout", "org.freedesktop.DBus.Error.AccessDenied",
        "org.freedesktop.Secret.Error.NoSuchObject", "org.gnome.keyring.Error.Denied"}) {
    UR_EXPECT_TRUE_MSG(name, key::StoreFailureFor(name) == key::StoreFailure::Unavailable);
  }
}

// ---- device authentication ------------------------------------------------------------

// The user typed their password: authenticated. A dismissed dialog, a wrong
// password or a site's policy that says no: not, ever.
UR_TEST(BittensorKeyAuth_OnlyAnAuthorizationOrNoWayToAskPasses) {
  UR_EXPECT_TRUE(key::AuthOutcomeFor(true, false) == key::AuthOutcome::Authorized);
  UR_EXPECT_TRUE(key::AuthOutcomeFor(true, true) == key::AuthOutcome::Authorized);
  UR_EXPECT_TRUE(key::AuthOutcomeFor(false, false) == key::AuthOutcome::Refused);
  // with interaction allowed, a challenge left standing means no agent could ask
  UR_EXPECT_TRUE(key::AuthOutcomeFor(false, true) == key::AuthOutcome::CannotAsk);
  UR_EXPECT_TRUE(key::CountsAsAuthenticated(key::AuthOutcome::Authorized));
  UR_EXPECT_TRUE(key::CountsAsAuthenticated(key::AuthOutcome::CannotAsk));
  UR_EXPECT_FALSE(key::CountsAsAuthenticated(key::AuthOutcome::Refused));
}

// No polkitd, a sandbox without the name, or a polkit that doesn't know the
// action (a package from before it): polkit can't ask. A timeout, a cancelled
// check or any other failure is no authentication.
UR_TEST(BittensorKeyAuth_ErrorsThatMeanPolkitCanNotAsk) {
  UR_EXPECT_TRUE(key::AuthOutcomeWithoutBus() == key::AuthOutcome::CannotAsk);
  for (const char* name :
       {"org.freedesktop.DBus.Error.ServiceUnknown", "org.freedesktop.DBus.Error.NameHasNoOwner",
        "org.freedesktop.DBus.Error.AccessDenied", "org.freedesktop.DBus.Error.UnknownMethod",
        "org.freedesktop.DBus.Error.UnknownInterface",
        "org.freedesktop.DBus.Error.UnknownObject"}) {
    UR_EXPECT_TRUE_MSG(name, key::AuthOutcomeForError(name, "") == key::AuthOutcome::CannotAsk);
  }
  UR_EXPECT_TRUE(key::AuthOutcomeForError("org.freedesktop.PolicyKit1.Error.Failed",
                                          "Action com.bringyour.network.bittensor-key is not "
                                          "registered") == key::AuthOutcome::CannotAsk);
  // anything else polkit fails with is a refusal
  UR_EXPECT_TRUE(key::AuthOutcomeForError("org.freedesktop.PolicyKit1.Error.Failed",
                                          "Only trusted callers") == key::AuthOutcome::Refused);
  UR_EXPECT_TRUE(key::AuthOutcomeForError("org.freedesktop.PolicyKit1.Error.Cancelled", "") ==
                 key::AuthOutcome::Refused);
  // a timeout or a cancelled call carries no remote name
  UR_EXPECT_TRUE(key::AuthOutcomeForError("", "Timeout was reached") == key::AuthOutcome::Refused);
  UR_EXPECT_TRUE(key::AuthOutcomeForError("org.freedesktop.DBus.Error.NoReply", "") ==
                 key::AuthOutcome::Refused);
}

UR_TEST(BittensorKeyAuth_TheActionIsThePolicysOwn) {
  UR_EXPECT_TEXT(std::string("com.bringyour.network.bittensor-key"), std::string(key::kAuthAction));
}

// ---- the notes ----------------------------------------------------------------------------

// A note written reads back, by network and by name; "" removes it; and a
// network's notes are its own.
UR_TEST(BittensorKeyNotes_WriteReadAndRemoveByNetwork) {
  NotesDir dir;
  if (!dir.Ok()) {
    UR_FAIL("could not make a temporary directory");
    return;
  }
  UR_EXPECT_TEXT(std::string(), key::ReadNoteAt(dir.File(), kNetworkA, "saved_address"));
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), kNetworkA, "saved_address", "5Address"));
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), kNetworkA, "reminder_at_millis", "1800000000000"));
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), kNetworkB, "deleted_address", "5Other"));
  UR_EXPECT_TEXT(std::string("5Address"), key::ReadNoteAt(dir.File(), kNetworkA, "saved_address"));
  UR_EXPECT_TEXT(std::string("1800000000000"),
               key::ReadNoteAt(dir.File(), kNetworkA, "reminder_at_millis"));
  UR_EXPECT_TEXT(std::string(), key::ReadNoteAt(dir.File(), kNetworkA, "deleted_address"));
  UR_EXPECT_TEXT(std::string("5Other"), key::ReadNoteAt(dir.File(), kNetworkB, "deleted_address"));
  UR_EXPECT_TEXT(std::string(), key::ReadNoteAt(dir.File(), kNetworkB, "saved_address"));
  // "" removes the note, and a network left with none goes from the file
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), kNetworkB, "deleted_address", ""));
  UR_EXPECT_TEXT(std::string(), key::ReadNoteAt(dir.File(), kNetworkB, "deleted_address"));
  UR_EXPECT_TRUE(dir.Text().find(kNetworkB) == std::string::npos);
  UR_EXPECT_TEXT(std::string("5Address"), key::ReadNoteAt(dir.File(), kNetworkA, "saved_address"));
  // removing what isn't there writes nothing, and never fails
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), kNetworkB, "saved_address", ""));
}

// The file is the app's alone (0600 in the 0700 config directory), and a file
// that is not the notes' object reads as no notes rather than failing.
UR_TEST(BittensorKeyNotes_PrivateFileAndMalformedReadsAsNone) {
  NotesDir dir;
  if (!dir.Ok()) {
    UR_FAIL("could not make a temporary directory");
    return;
  }
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), kNetworkA, "saved_address", "5Address"));
  struct stat info {};
  UR_EXPECT_EQ(0, ::stat(dir.File().c_str(), &info));
  UR_EXPECT_EQ(0600, static_cast<int>(info.st_mode & 0777));
  for (const char* text : {"not json", "[1, 2]", "{\"01890000-0000-7000-8000-000000000001\": 7}",
                           "{\"01890000-0000-7000-8000-000000000001\": {\"saved_address\": 7}}"}) {
    std::ofstream(dir.File(), std::ios::binary | std::ios::trunc) << text;
    UR_EXPECT_TRUE_MSG(text, key::ReadNoteAt(dir.File(), kNetworkA, "saved_address").empty());
  }
  // and a write over it starts a good object again
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), kNetworkA, "saved_address", "5Address"));
  UR_EXPECT_TEXT(std::string("5Address"), key::ReadNoteAt(dir.File(), kNetworkA, "saved_address"));
}

// No network id or no name is no note, and writes nothing.
UR_TEST(BittensorKeyNotes_NoNetworkIsNoNote) {
  NotesDir dir;
  if (!dir.Ok()) {
    UR_FAIL("could not make a temporary directory");
    return;
  }
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), "", "saved_address", "5Address"));
  UR_EXPECT_EQ(0, key::WriteNoteAt(dir.File(), kNetworkA, "", "5Address"));
  UR_EXPECT_FALSE(fs::exists(dir.File()));
  UR_EXPECT_TEXT(std::string(), key::ReadNoteAt(dir.File(), "", "saved_address"));
}
