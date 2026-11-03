// SPDX-License-Identifier: MPL-2.0
#include "BittensorKeyStore.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <libsecret/secret.h>

#include <mutex>
#include <string>

#include "BittensorKeyDbus.hpp"
#include "BittensorKeyPlatform.hpp"

namespace urnw {
namespace {

// The keyring attribute that scopes this app's items, as the RPC session's.
constexpr const char* kApplicationAttribute = "com.bringyour.network";
// What the desktop's keyring manager (Seahorse, KWalletManager) lists the item
// as. Not translated: it is the item's name, as the RPC session's is.
constexpr const char* kItemLabel = "URnetwork Bittensor recovery phrase";

const SecretSchema kKeySchema = [] {
  // Zero-initialize libsecret's reserved ABI fields explicitly (the RPC
  // session's schema says why).
  SecretSchema schema{};
  schema.name = "network.ur.bittensor-local-key";
  schema.flags = SECRET_SCHEMA_NONE;
  schema.attributes[0] = {"application", SECRET_SCHEMA_ATTRIBUTE_STRING};
  schema.attributes[1] = {"network_id", SECRET_SCHEMA_ATTRIBUTE_STRING};
  // the key's ss58 address: not a secret, so the card reads it without the phrase
  schema.attributes[2] = {"address", SECRET_SCHEMA_ATTRIBUTE_STRING};
  return schema;
}();

// One call at a time: the flow's own goroutines and the page's worker share
// the store, and an unlock prompt is one prompt.
std::mutex& StoreLock() {
  static std::mutex lock;
  return lock;
}

std::string NotesPath() {
  const std::string dir = std::string(g_get_user_config_dir()) + "/urnetwork";
  g_mkdir_with_parents(dir.c_str(), 0700);
  return dir + "/" + bittensor_key::kNotesFileName;
}

// The GTK main loop never waits on a keyring prompt: there a read asks
// nothing, and a locked item answers that the store can't answer now. Nor
// does a store made not to ask.
SecretSearchFlags SearchFlags(bool loadSecrets, bool mayAskToUnlock) {
  int flags = SECRET_SEARCH_ALL;
  if (mayAskToUnlock && !g_main_context_is_owner(g_main_context_default())) {
    flags |= SECRET_SEARCH_UNLOCK;
  }
  if (loadSecrets) flags |= SECRET_SEARCH_LOAD_SECRETS;
  return static_cast<SecretSearchFlags>(flags);
}

// The answer for a failed call, and the error freed.
std::string FailureAnswer(GError* error) {
  const bittensor_key::StoreFailure failure =
      bittensor_key::StoreFailureFor(bittensor_key::DbusErrorName(error));
  if (error != nullptr) g_error_free(error);
  return failure == bittensor_key::StoreFailure::Absent ? urnet::BittensorLocalKeyStoreAbsent
                                                        : urnet::BittensorLocalKeyStoreUnavailable;
}

// The network's items, or the store's answer when the search could not run.
struct Search {
  GList* items = nullptr;  // SecretRetrievable*, owned
  std::string failure;     // "" when the search ran
  ~Search() {
    if (items != nullptr) g_list_free_full(items, g_object_unref);
  }
};

void SearchNetwork(const std::string& networkId, bool loadSecrets, bool mayAskToUnlock,
                   Search& out) {
  GError* error = nullptr;
  out.items = secret_password_search_sync(&kKeySchema, SearchFlags(loadSecrets, mayAskToUnlock),
                                          nullptr, &error,
                                          "application", kApplicationAttribute, "network_id",
                                          networkId.c_str(), nullptr);
  if (error != nullptr) out.failure = FailureAnswer(error);
}

// An item whose collection stayed locked (the prompt was dismissed, or it was
// never asked, on the main loop) says nothing.
bool Locked(gpointer item) {
  return SECRET_IS_ITEM(item) && secret_item_get_locked(SECRET_ITEM(item));
}

// The one item's address; "" with none; the store's answer otherwise. Two items
// for one network were not written by this app, which never replaces one: it
// can't say which is the key.
std::string AddressOf(const Search& search) {
  if (!search.failure.empty()) return search.failure;
  if (search.items == nullptr) return {};
  if (search.items->next != nullptr || Locked(search.items->data)) {
    return urnet::BittensorLocalKeyStoreUnavailable;
  }
  GHashTable* attributes =
      secret_retrievable_get_attributes(SECRET_RETRIEVABLE(search.items->data));
  std::string address;
  if (attributes != nullptr) {
    if (const auto* value =
            static_cast<const gchar*>(g_hash_table_lookup(attributes, "address"))) {
      address = value;
    }
    g_hash_table_unref(attributes);
  }
  return address.empty() ? std::string(urnet::BittensorLocalKeyStoreUnavailable) : address;
}

bool AddKey(const std::string& networkId, const std::string& mnemonic,
            const std::string& address, bool mayAskToUnlock) {
  if (networkId.empty() || mnemonic.empty() || address.empty()) return false;
  std::scoped_lock lock(StoreLock());
  {
    // never replace an item: one there, or a store that can't say, refuses
    Search search;
    SearchNetwork(networkId, false, mayAskToUnlock, search);
    if (!search.failure.empty() || search.items != nullptr) return false;
  }
  GError* error = nullptr;
  const gboolean stored = secret_password_store_sync(
      &kKeySchema, SECRET_COLLECTION_DEFAULT, kItemLabel, mnemonic.c_str(), nullptr, &error,
      "application", kApplicationAttribute, "network_id", networkId.c_str(), "address",
      address.c_str(), nullptr);
  if (error != nullptr) {
    g_error_free(error);
    return false;
  }
  return stored != FALSE;
}

std::string KeyPhrase(const std::string& networkId, bool mayAskToUnlock) {
  if (networkId.empty()) return urnet::BittensorLocalKeyStoreUnavailable;
  std::scoped_lock lock(StoreLock());
  Search search;
  SearchNetwork(networkId, true, mayAskToUnlock, search);
  const std::string address = AddressOf(search);
  if (address.empty() || address == urnet::BittensorLocalKeyStoreAbsent ||
      address == urnet::BittensorLocalKeyStoreUnavailable) {
    return address;
  }
  GError* error = nullptr;
  SecretValue* value = secret_retrievable_retrieve_secret_sync(
      SECRET_RETRIEVABLE(search.items->data), nullptr, &error);
  if (error != nullptr) {
    if (value != nullptr) secret_value_unref(value);
    return FailureAnswer(error);
  }
  if (value == nullptr) return urnet::BittensorLocalKeyStoreUnavailable;
  const gchar* text = secret_value_get_text(value);
  std::string phrase = text != nullptr ? std::string(text) : std::string();
  secret_value_unref(value);
  return phrase.empty() ? std::string(urnet::BittensorLocalKeyStoreUnavailable) : phrase;
}

std::string KeyAddress(const std::string& networkId, bool mayAskToUnlock) {
  if (networkId.empty()) return urnet::BittensorLocalKeyStoreUnavailable;
  std::scoped_lock lock(StoreLock());
  Search search;
  SearchNetwork(networkId, false, mayAskToUnlock, search);
  return AddressOf(search);
}

// No item is not a refusal; the flow reads again to see it gone.
bool DeleteKey(const std::string& networkId) {
  if (networkId.empty()) return false;
  std::scoped_lock lock(StoreLock());
  GError* error = nullptr;
  secret_password_clear_sync(&kKeySchema, nullptr, &error, "application",
                             kApplicationAttribute, "network_id", networkId.c_str(), nullptr);
  if (error != nullptr) {
    g_error_free(error);
    return false;
  }
  return true;
}

}  // namespace

urnet::BittensorLocalKeyStore NewBittensorKeyStore(bool mayAskToUnlock) {
  urnet::BittensorLocalKeyStore store;
  store.add_key = [mayAskToUnlock](std::string networkId, std::string mnemonic,
                                   std::string address) {
    return AddKey(networkId, mnemonic, address, mayAskToUnlock);
  };
  store.delete_key = [](std::string networkId) { return DeleteKey(networkId); };
  store.key_address = [mayAskToUnlock](std::string networkId) {
    return KeyAddress(networkId, mayAskToUnlock);
  };
  store.key_phrase = [mayAskToUnlock](std::string networkId) {
    return KeyPhrase(networkId, mayAskToUnlock);
  };
  store.read_note = [](std::string networkId, std::string name) {
    return bittensor_key::ReadNoteAt(NotesPath(), networkId, name);
  };
  store.write_note = [](std::string networkId, std::string name, std::string value) {
    // a note that could not be written is read as before; the flow decides
    // from what it reads
    (void)bittensor_key::WriteNoteAt(NotesPath(), networkId, name, value);
  };
  return store;
}

}  // namespace urnw
