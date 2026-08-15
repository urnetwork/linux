// SPDX-License-Identifier: MPL-2.0
#include "SeedphraseSheet.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {

SeedphraseSheet::SeedphraseSheet(Gtk::Window& parent, const std::string& seedphrase)
    : phrase_(seedphrase) {
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("your_seedphrase", "Your Seedphrase"));
  set_default_size(440, -1);
  set_resizable(false);
  add_css_class("ur-sheet");

  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  box->set_margin(24);

  auto* heading = Gtk::make_managed<Gtk::Label>(T_("your_seedphrase", "Your Seedphrase"));
  heading->add_css_class("ur-step-heading");
  heading->set_xalign(0);
  box->append(*heading);

  auto* warn = Gtk::make_managed<Gtk::Label>(
      T_("seedphrase_only_time", "⚠️ This is the ONLY time you'll see this."));
  warn->set_xalign(0);
  warn->set_wrap(true);
  box->append(*warn);
  auto* warn2 = Gtk::make_managed<Gtk::Label>(
      T_("seedphrase_store_safely",
         "Write it down and store it somewhere safe. If you lose it, you'll lose access to "
         "your account."));
  warn2->add_css_class("dim-label");
  warn2->set_xalign(0);
  warn2->set_wrap(true);
  box->append(*warn2);

  // the numbered word grid, two columns, monospace
  std::vector<std::string> words;
  {
    std::istringstream stream(phrase_);
    std::string word;
    while (stream >> word) words.push_back(word);
  }
  auto* grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(6);
  grid->set_column_spacing(24);
  grid->add_css_class("ur-card-bordered");
  const size_t rows = (words.size() + 1) / 2;
  for (size_t i = 0; i < words.size(); ++i) {
    auto* cell =
        Gtk::make_managed<Gtk::Label>(std::to_string(i + 1) + ".  " + words[i]);
    cell->add_css_class("ur-mono-13");
    cell->set_xalign(0);
    grid->attach(*cell, static_cast<int>(i / rows), static_cast<int>(i % rows));
  }
  box->append(*grid);

  auto* copyBtn =
      Gtk::make_managed<Gtk::Button>(T_("copy_to_clipboard", "Copy to Clipboard"));
  copyBtn->signal_clicked().connect([this, copyBtn] {
    // Not a bare set_text: the copy carries the KDE password-manager hint so
    // clipboard managers that honor it (Klipper, several history extensions)
    // exclude the credential from their history — the closest Linux analogue
    // of the windows sheet's ExcludeClipboardContentFromMonitorProcessing.
    // Best-effort: managers that ignore the hint still get plain text.
    GdkContentProvider* providers[2];
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_STRING);
    g_value_set_string(&value, phrase_.c_str());
    providers[0] = gdk_content_provider_new_for_value(&value);
    g_value_unset(&value);
    GBytes* hint = g_bytes_new_static("secret", 6);
    providers[1] = gdk_content_provider_new_for_bytes("x-kde-passwordManagerHint", hint);
    g_bytes_unref(hint);
    GdkContentProvider* combined = gdk_content_provider_new_union(providers, 2);
    gdk_clipboard_set_content(get_clipboard()->gobj(), combined);
    g_object_unref(combined);
    copyBtn->set_label(T_("copied", "Copied!"));
  });
  box->append(*copyBtn);

  auto* confirmBtn = Gtk::make_managed<Gtk::Button>(
      T_("i_ve_saved_my_seedphrase", "I've Saved My Seedphrase"));
  confirmBtn->add_css_class("suggested-action");
  confirmBtn->signal_clicked().connect([this] {
    confirmed_ = true;
    // zero the copy this sheet held for the clipboard button
    std::fill(phrase_.begin(), phrase_.end(), '\0');
    phrase_.clear();
    if (on_confirm) on_confirm();
    close();
  });
  box->append(*confirmBtn);

  set_child(*box);

  // confirming is the ONLY way out: an unconfirmed close would abandon an
  // account whose only credential was never read
  signal_close_request().connect([this]() -> bool { return !confirmed_; }, false);
}

}  // namespace urnw
