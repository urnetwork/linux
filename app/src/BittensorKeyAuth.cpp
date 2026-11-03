// SPDX-License-Identifier: MPL-2.0
#include "BittensorKeyAuth.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <utility>

#include "BittensorKeyDbus.hpp"
#include "BittensorKeyPlatform.hpp"

namespace urnw {
namespace {

constexpr const char* kPolkitName = "org.freedesktop.PolicyKit1";
constexpr const char* kPolkitPath = "/org/freedesktop/PolicyKit1/Authority";
constexpr const char* kPolkitInterface = "org.freedesktop.PolicyKit1.Authority";
// how long an interactive check may wait for the user (the daemon's guard)
constexpr int kInteractiveTimeoutMs = 120000;
// CheckAuthorization's AllowUserInteraction flag
constexpr guint32 kAllowUserInteraction = 1;

// One check in flight: what to answer, and what CancelCheckAuthorization needs
// to take a dialog nobody answered away.
struct AuthCall {
  std::function<void(bool)> done;
  std::string cancellationId;
  GDBusConnection* bus = nullptr;  // owned once acquired
};

void Finish(AuthCall* call, bittensor_key::AuthOutcome outcome) {
  std::function<void(bool)> done = std::move(call->done);
  if (call->bus != nullptr) g_object_unref(call->bus);
  delete call;
  if (done) done(bittensor_key::CountsAsAuthenticated(outcome));
}

// A dialog left up past the timeout goes: polkit's own call to drop it.
void CancelCheck(GDBusConnection* bus, const std::string& cancellationId) {
  g_dbus_connection_call(bus, kPolkitName, kPolkitPath, kPolkitInterface,
                         "CancelCheckAuthorization",
                         g_variant_new("(s)", cancellationId.c_str()), nullptr,
                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

void OnChecked(GObject* source, GAsyncResult* result, gpointer data) {
  auto* call = static_cast<AuthCall*>(data);
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (reply == nullptr) {
    const std::string remote = bittensor_key::DbusErrorName(error);
    const std::string message = error != nullptr ? error->message : "";
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) && call->bus != nullptr) {
      CancelCheck(call->bus, call->cancellationId);
    }
    if (error != nullptr) g_error_free(error);
    Finish(call, bittensor_key::AuthOutcomeForError(remote, message));
    return;
  }
  gboolean authorized = FALSE;
  gboolean challenge = FALSE;
  GVariant* details = nullptr;
  g_variant_get(reply, "((bb@a{ss}))", &authorized, &challenge, &details);
  if (details != nullptr) g_variant_unref(details);
  g_variant_unref(reply);
  Finish(call, bittensor_key::AuthOutcomeFor(authorized != FALSE, challenge != FALSE));
}

void OnBus(GObject*, GAsyncResult* result, gpointer data) {
  auto* call = static_cast<AuthCall*>(data);
  GError* error = nullptr;
  call->bus = g_bus_get_finish(result, &error);
  if (error != nullptr) g_error_free(error);
  const char* name = call->bus != nullptr ? g_dbus_connection_get_unique_name(call->bus) : nullptr;
  if (name == nullptr) {
    Finish(call, bittensor_key::AuthOutcomeWithoutBus());
    return;
  }
  // the subject: this process, as polkit resolves its own bus name
  GVariantBuilder subject;
  g_variant_builder_init(&subject, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&subject, "{sv}", "name", g_variant_new_string(name));
  // always empty: nothing here may reach the dialog's text
  GVariantBuilder details;
  g_variant_builder_init(&details, G_VARIANT_TYPE("a{ss}"));
  GVariant* params =
      g_variant_new("((sa{sv})sa{ss}us)", "system-bus-name", &subject, bittensor_key::kAuthAction,
                    &details, kAllowUserInteraction, call->cancellationId.c_str());
  g_dbus_connection_call(call->bus, kPolkitName, kPolkitPath, kPolkitInterface,
                         "CheckAuthorization", params, G_VARIANT_TYPE("((bba{ss}))"),
                         G_DBUS_CALL_FLAGS_NONE, kInteractiveTimeoutMs, nullptr, &OnChecked, call);
}

}  // namespace

void AuthenticateForBittensorKey(std::function<void(bool authenticated)> done) {
  static std::atomic<uint64_t> next{0};
  auto* call = new AuthCall;
  call->done = std::move(done);
  // unique per check: what CancelCheckAuthorization names
  call->cancellationId =
      "urnetwork-gui-" + std::to_string(::getpid()) + "-" + std::to_string(++next);
  g_bus_get(G_BUS_TYPE_SYSTEM, nullptr, &OnBus, call);
}

}  // namespace urnw
