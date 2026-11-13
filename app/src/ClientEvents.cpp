// SPDX-License-Identifier: MPL-2.0
#include "ClientEvents.hpp"

#include <glib.h>
#include <urnetwork_sdk.h>

namespace urnw {

namespace {

// A GLib language name as a BCP 47 tag: "en_US.UTF-8" is "en-US"; the C and
// POSIX locales name no language and are "".
std::string LanguageNameTag(std::string tag) {
  if (const auto dot = tag.find('.'); dot != std::string::npos) tag.erase(dot);
  if (const auto at = tag.find('@'); at != std::string::npos) tag.erase(at);
  if (tag.empty() || tag == "C" || tag == "POSIX") return std::string();
  for (auto& c : tag) {
    if (c == '_') c = '-';
  }
  return tag;
}

}  // namespace

std::string ClientEventLocale() {
  const char* const* names = g_get_language_names();
  // an unknown language is not English: the server falls back to the
  // reader's country for an unknown language, and never for English
  return LanguageNameTag(names && names[0] ? names[0] : "");
}

std::vector<std::string> DeviceLanguages() {
  std::vector<std::string> languages;
  const char* const* names = g_get_language_names();
  for (size_t i = 0; names && names[i]; ++i) {
    std::string tag = LanguageNameTag(names[i]);
    if (tag.empty()) continue;
    bool seen = false;
    for (const auto& language : languages) {
      if (language == tag) seen = true;
    }
    if (!seen) languages.push_back(std::move(tag));
  }
  return languages;
}

std::string LocalTimeZoneId() {
  std::string id;
  GTimeZone* tz = g_time_zone_new_local();
  if (tz) {
    const char* identifier = g_time_zone_get_identifier(tz);
    if (identifier) id = identifier;
    g_time_zone_unref(tz);
  }
  // GLib reports "localtime" (or a bare offset) when TZ is unset and the zone
  // file carries no name; the /etc/localtime symlink usually names it
  const bool named = !id.empty() && id != "localtime" && id[0] != '+' && id[0] != '-' &&
                     id.find('/') != std::string::npos;
  if (!named) {
    char* target = g_file_read_link("/etc/localtime", nullptr);
    if (target) {
      const std::string link(target);
      g_free(target);
      if (const auto at = link.find("zoneinfo/"); at != std::string::npos) {
        id = link.substr(at + 9);
      }
    }
  }
  return id;
}

ClientEventQueue::ClientEventQueue(uint64_t networkSpace, const std::string& appVersion,
                                   const std::string& locale)
    : handle_(urnet_new_client_event_queue(networkSpace, "linux", appVersion.c_str(),
                                           locale.c_str())) {}

ClientEventQueue::~ClientEventQueue() {
  if (!handle_) return;
  urnet_client_event_queue_close(handle_);
  urnet_release(handle_);
  handle_ = 0;
}

void ClientEventQueue::Flush() {
  if (handle_) urnet_client_event_queue_flush(handle_);
}

void ClientEventQueue::FlushAndWait(int64_t timeoutMillis) {
  if (handle_) urnet_client_event_queue_flush_and_wait(handle_, timeoutMillis);
}

void ClientEventQueue::NewSession() {
  if (handle_) urnet_client_event_queue_new_session(handle_);
}

int64_t ClientEventQueue::Pending() const {
  return handle_ ? urnet_client_event_queue_pending_count(handle_) : 0;
}

std::string ClientEventQueue::Session() const {
  if (!handle_) return "";
  char* s = urnet_client_event_queue_get_session(handle_);
  std::string out = s ? s : "";
  if (s) urnet_free_string(s);
  return out;
}

void ClientEventQueue::Add(char* sdkEventJson) {
  if (!sdkEventJson) return;
  if (handle_) urnet_client_event_queue_add(handle_, sdkEventJson);
  urnet_free_string(sdkEventJson);
}

// ---- the facade -------------------------------------------------------------

void ClientEventQueue::OnboardingStepShown(const std::string& step, int64_t index, int64_t elapsedMs) {
  Add(urnet_new_onboarding_step_shown_event(step.c_str(), index, elapsedMs));
}
void ClientEventQueue::OnboardingStepCompleted(const std::string& step, int64_t index, int64_t elapsedMs) {
  Add(urnet_new_onboarding_step_completed_event(step.c_str(), index, elapsedMs));
}
void ClientEventQueue::OnboardingStepSkipped(const std::string& step, int64_t index, int64_t elapsedMs) {
  Add(urnet_new_onboarding_step_skipped_event(step.c_str(), index, elapsedMs));
}
void ClientEventQueue::ConnectFirst() { Add(urnet_new_connect_first_event()); }
void ClientEventQueue::FeedbackSubmitted(int64_t rating, const std::string& reason,
                                         const std::string& text) {
  Add(urnet_new_feedback_submitted_event(rating, reason.c_str(), text.c_str()));
}
void ClientEventQueue::PurchaseStarted(const std::string& store, const std::string& product,
                                       const std::string& plan, bool trial, double price,
                                       const std::string& currency) {
  Add(urnet_new_purchase_started_event(store.c_str(), product.c_str(), plan.c_str(), trial, price,
                                       currency.c_str()));
}
void ClientEventQueue::PurchaseCompleted(const std::string& store, const std::string& product,
                                         const std::string& plan, bool trial, double price,
                                         const std::string& currency) {
  Add(urnet_new_purchase_completed_event(store.c_str(), product.c_str(), plan.c_str(), trial,
                                         price, currency.c_str()));
}
void ClientEventQueue::PurchaseCancelled(const std::string& store, const std::string& product,
                                         const std::string& plan, bool trial, double price,
                                         const std::string& currency) {
  Add(urnet_new_purchase_cancelled_event(store.c_str(), product.c_str(), plan.c_str(), trial,
                                         price, currency.c_str()));
}
void ClientEventQueue::PurchaseFailed(const std::string& store, const std::string& product,
                                      const std::string& plan, bool trial, double price,
                                      const std::string& currency, const std::string& errorClass) {
  Add(urnet_new_purchase_failed_event(store.c_str(), product.c_str(), plan.c_str(), trial, price,
                                      currency.c_str(), errorClass.c_str()));
}
void ClientEventQueue::OfferScreenShown(const std::string& surface, const std::string& experiment,
                                        const std::string& variant, const std::string& tier,
                                        double priceShown, const std::string& currency,
                                        int64_t expiresInS) {
  Add(urnet_new_offer_screen_shown_event(surface.c_str(), experiment.c_str(), variant.c_str(),
                                         tier.c_str(), priceShown, currency.c_str(), expiresInS));
}
void ClientEventQueue::OfferCardTapped(const std::string& plan) {
  Add(urnet_new_offer_card_tapped_event(plan.c_str()));
}
void ClientEventQueue::OfferCtaTapped(const std::string& plan, const std::string& store) {
  Add(urnet_new_offer_cta_tapped_event(plan.c_str(), store.c_str()));
}
void ClientEventQueue::OfferDeclined(const std::string& control, int64_t elapsedMs) {
  Add(urnet_new_offer_declined_event(control.c_str(), elapsedMs));
}
void ClientEventQueue::SignupOptoutChanged(bool productUpdates) {
  Add(urnet_new_signup_optout_changed_event(productUpdates));
}
void ClientEventQueue::WidgetAdded(const std::string& kind) {
  Add(urnet_new_widget_added_event(kind.c_str()));
}

}  // namespace urnw
