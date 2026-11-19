// Settings -> About -> App version: the release note this build bundles
// (ReleaseNotes.hpp), in the sheet idiom of its neighbour Licenses, by the one
// rule every app follows (the localization store's README, "Release notes").
// The note's title, the note's own version and date under it (the row keeps
// the app's version), the intro, then each section under its heading as a
// bulleted list, and Close. The note is laid out in its own language's
// direction and the window and Close in the UI's.
//
// The note is a local file, so the sheet needs no session, device or daemon.
// It is read once per process (BundledReleaseNotes), and the version row is a
// button only when that read gave a note: a missing or broken file leaves the
// plain value row, never a sheet with nothing in it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include <gtkmm.h>

#include "ReleaseNotes.hpp"

namespace urnw {

// The bundled release note, read and parsed on the first call and kept for the
// process; nullptr when the file is missing or is not a valid note. The file is
// a few kilobytes of JSON, so the one read runs on the caller's thread (the GTK
// loop, as Settings builds its version rows).
std::shared_ptr<const release_notes::ReleaseNotes> BundledReleaseNotes();

class ReleaseNotesSheet : public Gtk::Window {
 public:
  ReleaseNotesSheet(Gtk::Window& parent,
                    std::shared_ptr<const release_notes::ReleaseNotes> notes);

  // Presents the sheet at the top of the note.
  void Open();

 private:
  void AppendNote(Gtk::Box& column, const release_notes::Note& note);

  std::shared_ptr<const release_notes::ReleaseNotes> notes_;
  Gtk::ScrolledWindow* scroll_ = nullptr;
};

}  // namespace urnw
