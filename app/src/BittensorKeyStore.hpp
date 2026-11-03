// The SDK's secret store for the one-tap Bittensor key on Linux (sdk
// BittensorLocalKeyStore; apple BittensorLocalKeyStore.swift): one Secret
// Service item per network id, through libsecret, as the RPC session's
// credentials already are (SecretServiceRpcSessionStore.cpp). The item lives
// in the user's default collection ("login" on GNOME and KDE), which the
// desktop unlocks at sign-in; in the Flatpak, libsecret keeps it in the app's
// own keyring file, encrypted with a key the secret portal holds in the
// host's Secret Service. A sign-out of this app leaves the item, so the key
// comes back when its network signs in again, and another network asks for
// another item and finds none.
//
// The address is an attribute of the item, so the Earnings card names the key
// without reading the secret. The non-secret notes the SDK keeps (addresses
// and numbers, never a phrase) are a file beside app_prefs.json
// (BittensorKeyPlatform.hpp), which a sign-out leaves too.
//
// No Secret Service at all (no provider on the session bus, or in the Flatpak
// no secret portal): every read answers BittensorLocalKeyStoreAbsent, and the
// flow offers no key here, only the wallet the user already has. A collection
// that stays locked, or any other failure, answers
// BittensorLocalKeyStoreUnavailable. Never a plain file.
//
// This holds no decisions: the SDK reads back what it writes before it signs,
// checks a deletion by reading again, and never replaces an item (AddKey
// refuses while one is there). On the GTK main loop the reads never ask the
// desktop to unlock a collection, so the window never waits on a keyring
// prompt; the Earnings page reads again on its worker, where they may
// (BittensorKeyHost). Nothing here logs.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <urnetwork_sdk.hpp>

namespace urnw {

// The store the SDK's flow takes. Its calls may come on any thread, and are
// taken one at a time. `mayAskToUnlock` false: no read ever asks the desktop to
// unlock a collection (the sign-out's question, which a locked store answers
// with no warning, as the SDK has it).
urnet::BittensorLocalKeyStore NewBittensorKeyStore(bool mayAskToUnlock = true);

}  // namespace urnw
