// SPDX-License-Identifier: MPL-2.0
#include "ReleaseNotesSheet.hpp"

#include <glib.h>

#include <optional>
#include <string>
#include <utility>

#include "I18n.hpp"
#include "PaneKit.hpp"
#include "RuntimePaths.hpp"
#include "Ui.hpp"
#include "UrTheme.hpp"

// The build stamp (meson passes -DUR_APP_VERSION): the line under the note's
// title names the version the App version row the user pressed shows. The
// fallback keeps this TU self-contained the way SettingsPage.cpp's does.
#ifndef UR_APP_VERSION
#define UR_APP_VERSION "0.0.0"
#endif

// The installed data dir (meson passes -DUR_PKGDATADIR); the shared ladder
// (RuntimePaths.hpp) prefixes $APPDIR inside an AppImage and falls back to the
// build tree's assets/.
#ifndef UR_PKGDATADIR
#define UR_PKGDATADIR "/usr/share/urnetwork"
#endif

namespace urnw {
namespace {

constexpr int kSheetWidth = 480;
constexpr int kSheetMaxHeight = 560;  // a longer note scrolls
constexpr int kInset = 24;            // the sheets' content margin (LicensesSheet)

std::shared_ptr<const release_notes::ReleaseNotes> ReadBundledReleaseNotes() {
  const std::string path = ResolveRuntimePath(UR_PKGDATADIR "/release-notes.json",
                                              G_FILE_TEST_IS_REGULAR, "assets/release-notes.json");
  if (path.empty()) {
    g_warning("release notes: release-notes.json not found; the version row opens nothing");
    return nullptr;
  }
  gchar* contents = nullptr;
  gsize length = 0;
  GError* error = nullptr;
  if (!g_file_get_contents(path.c_str(), &contents, &length, &error)) {
    g_warning("release notes: cannot read %s: %s", path.c_str(),
              error ? error->message : "unknown error");
    g_clear_error(&error);
    return nullptr;
  }
  const std::string text(contents, length);
  g_free(contents);
  std::optional<release_notes::ReleaseNotes> notes = release_notes::Parse(text);
  if (!notes) {
    g_warning("release notes: %s is not a valid release note; the version row opens nothing",
              path.c_str());
    return nullptr;
  }
  return std::make_shared<const release_notes::ReleaseNotes>(std::move(*notes));
}

// The UI's language, for the note to match the strings around it: gettext
// answers msgid "" with the header of the catalog it loaded, which names its
// language. Through g_dgettext, the lookup every T_ makes, so this reads ""
// (and the note reads en) exactly when the app's strings are English.
std::string UiCatalogLanguage() {
  const char* header = g_dgettext(GETTEXT_PACKAGE, "");
  return release_notes::CatalogLanguage(header ? header : "");
}

// The note's date in this machine's locale ("%x", the app's date form:
// Formatters.cpp LocalDate); the file's own YYYY-MM-DD when GLib cannot form
// it. UTC midnight, so no zone can move the day.
std::string LocalNoteDate(const std::string& date) {
  const std::optional<release_notes::Date> day = release_notes::ParseDate(date);
  if (!day) return date;
  GDateTime* moment = g_date_time_new_utc(day->year, day->month, day->day, 0, 0, 0);
  if (!moment) return date;
  gchar* text = g_date_time_format(moment, "%x");
  g_date_time_unref(moment);
  const std::string out = text ? text : "";
  g_free(text);
  return out.empty() ? date : out;
}

Gtk::Label* MakeWrappedLabel(const Glib::ustring& text, const char* cssClass) {
  auto* label = Gtk::make_managed<Gtk::Label>(text);
  label->add_css_class(cssClass);
  label->set_xalign(0);
  label->set_wrap(true);
  label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
  label->set_hexpand(true);
  return label;
}

// GTK keeps a direction per widget and never hands it down (a widget without
// one takes the default, not its parent's), so a subtree takes it widget by
// widget: the boxes for their packing, the labels for alignment and for the
// order of neutral runs such as "2026.11.18-1101907820 · 19.11.2026".
void SetDirectionDeep(Gtk::Widget& widget, Gtk::TextDirection direction) {
  widget.set_direction(direction);
  for (Gtk::Widget* child = widget.get_first_child(); child != nullptr;
       child = child->get_next_sibling()) {
    SetDirectionDeep(*child, direction);
  }
}

}  // namespace

std::shared_ptr<const release_notes::ReleaseNotes> BundledReleaseNotes() {
  // a missing or broken file is a fact of this build: read once, warn once
  static const std::shared_ptr<const release_notes::ReleaseNotes> notes =
      ReadBundledReleaseNotes();
  return notes;
}

ReleaseNotesSheet::ReleaseNotesSheet(Gtk::Window& parent,
                                     std::shared_ptr<const release_notes::ReleaseNotes> notes)
    : notes_(std::move(notes)) {
  EnsureBrandCss();   // .ur-sheet / .ur-step-heading / .ur-body / the group header
  EnsureDrawerCss();  // .ur-caption
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("release_notes", "Release notes"));
  set_default_size(kSheetWidth, -1);
  set_hide_on_close(true);
  add_css_class("ur-sheet");
  AddEscapeToClose(*this);

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  // the UI's language does not change while the app runs: chosen once
  if (notes_) {
    const std::string locale = release_notes::PickLocale(*notes_, UiCatalogLanguage());
    if (const auto it = notes_->locales.find(locale); it != notes_->locales.end()) {
      AppendNote(*column, it->second);
      // The note in its own language's direction, the chrome (the window and
      // Close) in the UI's: the English fallback reads left to right in an
      // Arabic or Hebrew UI.
      SetDirectionDeep(*column, release_notes::IsRightToLeft(locale) ? Gtk::TextDirection::RTL
                                                                     : Gtk::TextDirection::LTR);
    }
  }

  scroll_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroll_->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroll_->set_propagate_natural_height(true);
  scroll_->set_max_content_height(kSheetMaxHeight);
  scroll_->set_vexpand(true);
  scroll_->set_child(*column);

  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  root->append(*scroll_);
  auto* close = Gtk::make_managed<Gtk::Button>(T_("close", "Close"));
  close->set_halign(Gtk::Align::END);
  close->set_margin(16);
  close->signal_clicked().connect([this] { set_visible(false); });
  root->append(*close);
  set_child(*root);
}

void ReleaseNotesSheet::Open() {
  if (auto adjustment = scroll_->get_vadjustment()) adjustment->set_value(0);
  present();
}

void ReleaseNotesSheet::AppendNote(Gtk::Box& column, const release_notes::Note& note) {
  auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  head->set_margin_start(kInset);
  head->set_margin_end(kInset);
  head->set_margin_top(kInset);
  head->set_margin_bottom(16);
  head->append(*MakeWrappedLabel(note.title, "ur-step-heading"));
  // the running build's version, as the App version row shows it, beside the
  // note's date (owner 2026-11-19); the note's own version is not shown
  head->append(*MakeWrappedLabel(
      release_notes::MetaLine(UR_APP_VERSION, LocalNoteDate(notes_->date)), "ur-caption"));
  if (!note.intro.empty()) {
    auto* intro = MakeWrappedLabel(note.intro, "ur-body");
    intro->set_margin_top(8);
    head->append(*intro);
  }
  column.append(*head);

  // Each section under its heading (the group header the Licenses sheet's
  // sections wear), its items a bulleted list. The text is set, never parsed
  // as markup: it is data from a generated file.
  for (const release_notes::Section& section : note.sections) {
    if (!section.heading.empty()) {
      column.append(*kit::MakePaneGroupHeader(section.heading).root);
    }
    auto* list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    list->set_margin_start(kInset);
    list->set_margin_end(kInset);
    list->set_margin_top(12);
    list->set_margin_bottom(16);
    for (const std::string& item : section.items) {
      auto* line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
      auto* bullet = Gtk::make_managed<Gtk::Label>("•");
      bullet->add_css_class("ur-body");
      bullet->set_valign(Gtk::Align::START);  // beside an item's first line
      kit::MarkDecorative(*bullet);           // the item's text is what is read
      line->append(*bullet);
      line->append(*MakeWrappedLabel(item, "ur-body"));
      list->append(*line);
    }
    column.append(*list);
  }
}

}  // namespace urnw
