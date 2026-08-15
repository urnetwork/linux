// SeedphraseDisplaySheet — the one-time display of an instant account's
// freshly minted seedphrase (windows:AuthSheets SeedphraseDisplaySheet /
// macOS parity). A numbered word grid, "only time you'll see this" warning,
// copy button, and a confirm that is THE ONLY WAY OUT: the sheet refuses
// Esc/close, because dismissing it unconfirmed would abandon an account whose
// only credential was never read. Confirming runs on_confirm (which registers
// the device); the caller handles the discard path.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#include <gtkmm.h>

namespace urnw {

class SeedphraseSheet : public Gtk::Window {
 public:
  SeedphraseSheet(Gtk::Window& parent, const std::string& seedphrase);

  std::function<void()> on_confirm;  // fires once; the sheet closes itself

 private:
  bool confirmed_ = false;
  std::string phrase_;  // held for the copy button; zeroed on confirm
};

}  // namespace urnw
