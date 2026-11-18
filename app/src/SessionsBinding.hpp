// The Sessions page's half of the controller contract (REVOKE-UI-FINAL.md §2,
// §6), over the calls it makes, so their order and their guards are tested
// with a fake controller (tests/SessionsBindingTest.cpp) and the page carries
// none of it:
//
//   - Attach adopts a controller just opened on the account's Api, whose
//     listener is already subscribed: it is started, and told whether the page
//     is on screen.
//   - SetVisible (the page mapped or not) and SetForeground (the window
//     presenting or hiding to the tray) go to it as they change.
//   - Refresh and Revoke go to the attached controller, and nowhere without
//     one; so do a group opened or closed and a section's Show more, which
//     only lay the list out again (no request).
//   - Release unsubscribes, then closes, and orphans every post still on its
//     way: a listener posts with the generation it was subscribed under, and
//     Accepts answers yes only for the controller attached now. No snapshot
//     lands after a release, a sign-out or the page's destruction, and a new
//     account's page never shows the previous account's answer.
//
// Pure C++17 (no GTK, no SDK); SessionsPage.cpp adapts the sdk's
// ClientSessionViewController to Controller. Main loop only.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "SessionsPresentation.hpp"

namespace urnw::sessions {

// The controller calls the page makes (sdk ClientSessionViewController, plus
// the listener subscription it holds).
class Controller {
 public:
  virtual ~Controller() = default;
  virtual void Start() = 0;
  virtual void SetVisible(bool visible) = 0;
  virtual void SetForeground(bool foreground) = 0;
  virtual void Refresh() = 0;
  virtual void RevokeSession(const std::string& sessionId) = 0;
  virtual void RevokeOtherSessions() = 0;
  // A client group opened (its first page of rows) or closed.
  virtual void SetGroupExpanded(const std::string& groupKind, bool expanded) = 0;
  // Another page of a section's rows: the device sign-ins (kGroupSignIns) or an
  // open group.
  virtual void ShowMoreSessions(const std::string& section) = 0;
  // The controller's latest snapshot, as plain data.
  virtual Snapshot Read() = 0;
  // Drops the page's listener subscription.
  virtual void Unsubscribe() = 0;
  virtual void Close() = 0;
};

class Binding {
 public:
  ~Binding() { Release(); }

  // The generation the next Attach adopts: the listener of the controller
  // about to be attached posts with it. Each attach has a generation of its
  // own; a release leaves the count alone (detaching is what orphans).
  uint64_t NextGeneration() const { return attaches_ + 1; }

  // `source` names what the controller was opened on (the Api's handle): a
  // different one later is a different Api, which needs a controller of its
  // own. A controller still attached is released first.
  void Attach(std::unique_ptr<Controller> controller, uint64_t source) {
    Release();
    if (!controller) return;
    controller_ = std::move(controller);
    generation_ = ++attaches_;
    source_ = source;
    controller_->Start();
    controller_->SetVisible(visible_);
  }

  bool Attached() const { return controller_ != nullptr; }
  uint64_t Source() const { return source_; }

  // A post from the controller attached now: render it. Anything else is from
  // a released one.
  bool Accepts(uint64_t generation) const {
    return controller_ != nullptr && generation == generation_;
  }

  void SetVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    if (controller_) controller_->SetVisible(visible);
  }

  void SetForeground(bool foreground) {
    if (foreground_ == foreground) return;
    foreground_ = foreground;
    if (controller_) controller_->SetForeground(foreground);
  }

  void Refresh() {
    if (controller_) controller_->Refresh();
  }

  void Revoke(const Target& target) {
    if (!controller_) return;
    if (target.others) {
      controller_->RevokeOtherSessions();
    } else if (!target.sessionId.empty()) {
      controller_->RevokeSession(target.sessionId);
    }
  }

  // A group heading's activation (ExpandedAfterToggle) and a Show more.
  void SetGroupExpanded(const std::string& groupKind, bool expanded) {
    if (controller_) controller_->SetGroupExpanded(groupKind, expanded);
  }

  void ShowMore(const std::string& section) {
    if (controller_) controller_->ShowMoreSessions(section);
  }

  std::optional<Snapshot> Read() {
    if (!controller_) return std::nullopt;
    return controller_->Read();
  }

  // Unsubscribe, then close (§6). Detached, nothing Accepts the posts still
  // on their way.
  void Release() {
    if (!controller_) return;
    std::unique_ptr<Controller> controller = std::move(controller_);
    source_ = 0;
    controller->Unsubscribe();
    controller->Close();
  }

 private:
  std::unique_ptr<Controller> controller_;
  uint64_t attaches_ = 0;    // controllers attached so far
  uint64_t generation_ = 0;  // the attached controller's
  uint64_t source_ = 0;
  bool visible_ = false;
  bool foreground_ = false;
};

}  // namespace urnw::sessions
