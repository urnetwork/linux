// Setup guide for the device-location override, the linux counterpart of the
// android MockLocationGuideScreen.
//
// Every step is marked from the controller's RAW setup signals rather than from
// `status`, which collapses to Disabled whenever the toggle is off and so can
// never answer "is this machine set up?". That collapse is exactly the bug the
// android version had.
//
// The guide is also where the honest disclosure lives: this mechanism moves the
// location reported through GeoClue and nothing else.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gtkmm.h>

#include "LocationOverride.hpp"

namespace urnw {

class LocationOverrideGuide : public Gtk::Window {
 public:
  LocationOverrideGuide(Gtk::Window& parent, LocationOverrideController* controller);

  void Open();

 private:
  struct StepRow {
    Gtk::Label* marker = nullptr;
    Gtk::Label* text = nullptr;
  };

  StepRow AppendStep(Gtk::Box& box, const std::string& text);
  void Refresh();

  LocationOverrideController* controller_ = nullptr;
  StepRow geoclueStep_;
  StepRow staticSourceStep_;
  StepRow serviceStep_;
  Gtk::Label* readyLabel_ = nullptr;
  Gtk::Label* cleanupLabel_ = nullptr;  // orphan recovery, shown only when orphaned
};

}  // namespace urnw
