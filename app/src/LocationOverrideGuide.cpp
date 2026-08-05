// SPDX-License-Identifier: MPL-2.0
#include "LocationOverrideGuide.hpp"

#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {

LocationOverrideGuide::LocationOverrideGuide(Gtk::Window& parent,
                                             LocationOverrideController* controller)
    : controller_(controller) {
  EnsureDrawerCss();
  set_title(T_("mock_location_guide_title", "Sync device location"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(460, 560);
  set_hide_on_close(true);
  AddEscapeToClose(*this);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  set_child(*scroller);

  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
  root->set_margin(20);
  scroller->set_child(*root);

  // what the feature does
  auto* intro = Gtk::make_managed<Gtk::Label>(T_(
      "mock_location_guide_intro",
      "When enabled, apps on this device see the location of the provider you have been "
      "connected to the longest, instead of your real location."));
  intro->set_xalign(0);
  intro->set_wrap(true);
  root->append(*intro);

  // the steps, each marked from its own raw signal
  auto* steps = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
  steps->add_css_class("ur-card");
  geoclueStep_ = AppendStep(*steps, T_("location_override_step_geoclue",
                                       "Install GeoClue 2.7 or newer. It is not on every "
                                       "system, and Ubuntu 22.04 and Debian 12 ship an older "
                                       "version that cannot do this."));
  staticSourceStep_ =
      AppendStep(*steps, T_("location_override_step_static_source",
                            "Keep GeoClue's static location source enabled. It is on by "
                            "default; check /etc/geoclue/geoclue.conf if you have changed it."));
  serviceStep_ = AppendStep(
      *steps, T_("location_override_step_service",
                 "Install the URnetwork system service. The system location file lives under "
                 "/etc, so a privileged helper has to write it — URnetwork never asks you to "
                 "run the app itself as an administrator."));
  root->append(*steps);

  readyLabel_ = Gtk::make_managed<Gtk::Label>();
  readyLabel_->set_xalign(0);
  readyLabel_->set_wrap(true);
  readyLabel_->add_css_class("ur-value-on");
  root->append(*readyLabel_);

  // Honest disclosure: what this covers and, just as prominently, what it does
  // not. The override moves what GeoClue reports; anything that does not ask
  // GeoClue is untouched.
  root->append(*MakeCaption(T_("mock_location_disclosure_device_wide",
                               "This changes the location reported to every app on your "
                               "device, not just URnetwork.")));
  auto* coverage = Gtk::make_managed<Gtk::Label>(
      T_("location_override_coverage_geoclue",
         "Apps that ask the system for your location through GeoClue — including GNOME "
         "Settings and Maps, Firefox, and sandboxed Flatpak and Snap apps — report the "
         "provider's location instead of yours."));
  coverage->set_xalign(0);
  coverage->set_wrap(true);
  coverage->add_css_class("dim-label");
  root->append(*coverage);

  auto* gaps = Gtk::make_managed<Gtk::Label>(
      T_("location_override_coverage_gaps",
         "It does not cover everything. Chrome and KDE Plasma never ask GeoClue, and any app "
         "that works out your location from your IP address on its own is unaffected. Firefox "
         "falls back to its own lookup if the system does not answer quickly."));
  gaps->set_xalign(0);
  gaps->set_wrap(true);
  gaps->add_css_class("dim-label");
  root->append(*gaps);

  cleanupLabel_ = Gtk::make_managed<Gtk::Label>(
      T_("location_override_cleanup_required",
         "URnetwork could not remove the system location file, so this machine may still "
         "report a provider's location. Start the URnetwork system service and turn this off, "
         "or remove /etc/geolocation as an administrator."));
  cleanupLabel_->set_xalign(0);
  cleanupLabel_->set_wrap(true);
  cleanupLabel_->add_css_class("ur-error-text");
  cleanupLabel_->set_visible(false);
  root->append(*cleanupLabel_);

  // The setup signals have no change notification (they are files on disk), so
  // re-probe whenever the guide appears -- the desktop equivalent of android's
  // re-check on ON_RESUME, and the reason the user can follow the steps in
  // another window and come back to see them tick over.
  signal_show().connect(sigc::mem_fun(*this, &LocationOverrideGuide::Refresh));
}

LocationOverrideGuide::StepRow LocationOverrideGuide::AppendStep(Gtk::Box& box,
                                                                const std::string& text) {
  auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  StepRow step;
  step.marker = Gtk::make_managed<Gtk::Label>();
  step.marker->set_valign(Gtk::Align::START);
  row->append(*step.marker);
  step.text = Gtk::make_managed<Gtk::Label>(text);
  step.text->set_xalign(0);
  step.text->set_wrap(true);
  step.text->set_hexpand(true);
  row->append(*step.text);
  box.append(*row);
  return step;
}

void LocationOverrideGuide::Refresh() {
  if (controller_ == nullptr) return;
  controller_->RefreshSignals();
  const LocationOverrideState state = controller_->State();

  // Each step is marked from its OWN raw signal, never from `status` -- the
  // guide has to be truthful while the feature is still switched off.
  auto mark = [](const StepRow& step, bool done) {
    if (step.marker == nullptr) return;
    step.marker->set_markup(done ? "<span foreground='#87fb67'>✔</span>"
                                 : "<span foreground='#989898'>○</span>");
    step.text->remove_css_class("dim-label");
    if (done) step.text->add_css_class("dim-label");
  };
  mark(geoclueStep_, state.geoclueInstalled);
  mark(staticSourceStep_, state.staticSourceEnabled);
  mark(serviceStep_, state.writerAvailable);

  if (readyLabel_ != nullptr) {
    readyLabel_->set_visible(state.setupComplete());
    readyLabel_->set_text(T_("location_override_ready",
                             "Ready. Turn on the toggle to report the connected provider's "
                             "location as this machine's location."));
  }
  if (cleanupLabel_ != nullptr) {
    cleanupLabel_->set_visible(state.status == LocationOverrideStatus::Orphaned);
  }
}

void LocationOverrideGuide::Open() {
  Refresh();
  present();
}

}  // namespace urnw
