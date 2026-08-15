// SupportPage — the SUPPORT destination of the Windows-parity shell
// (windows MainWindow.xaml SupportView + SettingsPage's support surface:
// strings, send flow, log upload). A CARD-MODEL page, not a pane shell: the
// shell keeps the destination header ("Support" in the display face — a card
// page with no title reads as content that started halfway down), and the
// body is one scroll column with the 24px card-page gutter, width-capped and
// centred (windows: * : 1000* : * star columns with MaxWidth on the middle;
// here an AdwClamp, the equivalent centering container).
//
// One wide gate (>= 1000 dip, kit::kWideBreakpointDip) moves three things at
// once: the cap (560 <-> 1080), the side column, and the "reach a human"
// card's position (beside the form / stacked under it). There is no separate
// stacking breakpoint.
//
// The page loads nothing — there is no loading state. Its five states are
// distinguishable by construction: idle form / in-flight (Send disabled) /
// success (timed snackbar + cleared fields) / failure (persistent snackbar,
// fields intact) / signed-out (persistent warning, nothing sent).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SdkHost.hpp"

namespace urnw {

class SupportPage : public Gtk::Box {
 public:
  explicit SupportPage(SdkHost& host);
  ~SupportPage() override;

  // nav-select + auth-change hook. The page has no async fields to load; this
  // bumps the stale-async epoch (orphaning any in-flight send from a previous
  // session) and resets the in-flight Send gating.
  void Load();

  // The spec's single wide rule, applied by the shell's window-level
  // ApplyBreakpoint: wide (>= 1000 dip) = cap 1080 + contact card beside the
  // form (20 dip gutter, both top-aligned); narrow = cap 560 + contact card
  // stacked under the form (16 dip gap).
  void ApplyBreakpoint(int widthDip);

  // Preview harness parity (--preview-ui=support): raise the success snackbar
  // once — deliberately the timing-out severity, paired with wallet's
  // persistent-error preview so both snackbar behaviours are reviewable.
  void ShowPreviewSnackbar();

  // The snackbar surface belongs to the shell (windows: the in-flow
  // SupportInfo InfoBar; linux: the window snackbar) — the page emits, the
  // shell shows. error=true is the persistent treatment (kit::Snackbar
  // Warning/Error: stays until dismissed — the message is often the user's
  // only diagnostic); error=false the timed Success one (auto-dismiss 4s).
  std::function<void(const Glib::ustring& message, bool error)> on_snackbar;

 private:
  void BuildMainStack();
  void BuildSideStack();
  void SetRating(int value);
  void PaintStars();
  void OnSendFeedback();
  void UploadLogs(const std::string& feedbackId);
  void Snack(const Glib::ustring& message, bool error);

  SdkHost& host_;
  // stale-async guard: bumped by Load() and the destructor; a send callback
  // carrying an older value is dropped before it touches the page
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);

  // layout: clamp (the width cap) > column > [ topRow: form | side ] + side
  AdwClamp* clamp_ = nullptr;  // owned by the scroller; cap 560/1080
  Gtk::Box column_{Gtk::Orientation::VERTICAL, 0};
  Gtk::Box topRow_{Gtk::Orientation::HORIZONTAL, 0};
  Gtk::Box mainStack_{Gtk::Orientation::VERTICAL, 12};  // the feedback form
  // the "reach a human" card — a MEMBER widget so the wrapper keeps it alive
  // across the breakpoint's remove/append reparenting
  Gtk::Box sideStack_{Gtk::Orientation::VERTICAL, 12};
  std::optional<bool> wide_;  // last applied gate; empty until first apply

  // form state
  // Windows RatingControl default: no initial Value = -1 ("unset"), and the
  // send flow submits that -1 AS-IS (spec FLAG: near-perfect parity means
  // replicating it; clamping is the owner's call, not this port's).
  int rating_ = -1;
  std::vector<Gtk::Image*> starIcons_;
  Gtk::TextView* feedbackView_ = nullptr;
  Gtk::CheckButton* includeLogs_ = nullptr;
  Gtk::Button* sendButton_ = nullptr;
  bool sending_ = false;  // in-flight: Send disabled, nothing else gated
};

}  // namespace urnw
