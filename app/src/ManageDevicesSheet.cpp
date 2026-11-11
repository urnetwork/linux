// SPDX-License-Identifier: MPL-2.0
#include "ManageDevicesSheet.hpp"

#include <glib.h>

#include <utility>

#include "DisplayText.hpp"
#include "Formatters.hpp"  // RelativeTime, the sessions screen's "Last used" words
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// The sdk's snapshot in the plain values the presentation reads, by the sdk's
// own spellings of its kinds and errors. A device's name and spec are external
// text (a user named the device), filtered as every other name is.
manage_devices::Snapshot PlainOf(const urnet::ManageDevicesSnapshot& snapshot) {
  manage_devices::Snapshot plain;
  if (snapshot.Devices) {
    for (const urnet::ManagedDevice& device : *snapshot.Devices) {
      manage_devices::Device row;
      row.clientId = device.ClientId.value_or(std::string());
      row.proxy = device.Kind == urnet::ManagedDeviceKindProxy;
      row.name = SanitizeExternalDisplayText(device.Name);
      row.spec = SanitizeExternalDisplayText(device.Spec);
      row.thisDevice = device.ThisDevice;
      row.connected = device.Connected;
      row.lastSeenMillis = device.LastSeenTime;
      plain.devices.push_back(std::move(row));
    }
  }
  plain.loaded = snapshot.Loaded;
  plain.loading = snapshot.Loading;
  plain.removingClientId = snapshot.RemovingClientId.value_or(std::string());
  if (snapshot.ErrorId == urnet::ManageDevicesErrorSignInRequired) {
    plain.error = manage_devices::Error::SignInRequired;
  } else if (snapshot.ErrorId == urnet::ManageDevicesErrorLoadFailed) {
    plain.error = manage_devices::Error::LoadFailed;
  } else if (snapshot.ErrorId == urnet::ManageDevicesErrorRemoveFailed) {
    plain.error = manage_devices::Error::RemoveFailed;
  }
  plain.addRetryCount = snapshot.AddRetryCount;
  // the network's Pro features in use, by the sdk's prompt values; a list
  // from a daemon on an older sdk carries none, and says nothing
  if (snapshot.ProFeatures) {
    if (snapshot.ProFeatures->Prompt == urnet::ProFeaturesPromptUpgrade) {
      plain.proFeatures = pro_features::Prompt::Upgrade;
    } else if (snapshot.ProFeatures->Prompt == urnet::ProFeaturesPromptStopped) {
      plain.proFeatures = pro_features::Prompt::Stopped;
    }
  }
  return plain;
}

Gtk::Label* MakeLine(const Glib::ustring& text, const char* cssClass) {
  auto* line = Gtk::make_managed<Gtk::Label>(text);
  line->set_xalign(0);
  line->set_wrap(true);
  if (cssClass) line->add_css_class(cssClass);
  return line;
}

void RemoveChildren(Gtk::Box& box) {
  while (Gtk::Widget* child = box.get_first_child()) box.remove(*child);
}

}  // namespace

ManageDevicesSheet::ManageDevicesSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
  EnsureDrawerCss();
  set_title(T_(manage_devices::kTitleText.key, manage_devices::kTitleText.english));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(440, -1);
  set_hide_on_close(true);
  AddEscapeToClose(*this);
  BuildUi();
  // however it hides, the list closes with it
  property_visible().signal_changed().connect([this] {
    if (!get_visible()) OnHidden();
  });
}

ManageDevicesSheet::~ManageDevicesSheet() {
  // the window goes with the app: nothing posted for it runs, and a hide raised
  // on the way out asks nothing more of the host
  *alive_ = false;
}

void ManageDevicesSheet::BuildUi() {
  auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  root->set_margin(24);
  set_child(*root);

  auto* heading = MakeLine(T_(manage_devices::kTitleText.key, manage_devices::kTitleText.english),
                           "ur-step-heading");
  root->append(*heading);

  // why a refused sign-in opened it: what the refusal said
  message_ = MakeLine("", "ur-body");
  message_->set_visible(false);
  root->append(*message_);

  // the network's Pro features in use, above the list of its proxies: the
  // prompt's title and body with Get Pro, and no close (Render)
  proFeaturesBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  proFeaturesTitle_ = MakeLine("", "ur-row-title");
  proFeaturesBox_->append(*proFeaturesTitle_);
  proFeaturesBody_ = MakeLine("", "ur-body");
  proFeaturesBox_->append(*proFeaturesBody_);
  proFeaturesGetPro_ = Gtk::make_managed<Gtk::Button>(T_("get_pro", "Get Pro"));
  proFeaturesGetPro_->set_halign(Gtk::Align::START);
  proFeaturesGetPro_->signal_clicked().connect([this] {
    // this modal sheet steps aside, so the upgrade comes up over the window
    set_visible(false);
    if (on_get_pro) on_get_pro();
  });
  proFeaturesBox_->append(*proFeaturesGetPro_);
  proFeaturesBox_->set_visible(false);
  root->append(*proFeaturesBox_);

  listBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  listBox_->append(
      *MakeLine(T_(manage_devices::kHelpText.key, manage_devices::kHelpText.english), "ur-caption"));
  auto* status = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  statusText_ = MakeLine("", "ur-caption");
  statusText_->set_hexpand(true);
  statusText_->set_visible(false);
  status->append(*statusText_);
  retryButton_ = Gtk::make_managed<Gtk::Button>(T_("retry", "Retry"));
  retryButton_->add_css_class("flat");
  retryButton_->set_valign(Gtk::Align::CENTER);
  retryButton_->set_visible(false);
  retryButton_->signal_clicked().connect([this] { host_.RefreshManageDevices(); });
  status->append(*retryButton_);
  listBox_->append(*status);
  loading_ = Gtk::make_managed<Gtk::Spinner>();
  loading_->set_halign(Gtk::Align::START);
  loading_->set_visible(false);
  listBox_->append(*loading_);
  rowsBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_propagate_natural_height(true);
  scroller->set_max_content_height(360);
  scroller->set_child(*rowsBox_);
  listBox_->append(*scroller);
  root->append(*listBox_);

  // the confirmation, in the list's place while it asks
  confirmBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  confirmBox_->set_visible(false);
  confirmBox_->append(*MakeLine(
      T_(manage_devices::kRemoveTitleText.key, manage_devices::kRemoveTitleText.english),
      "ur-row-title"));
  confirmBody_ = MakeLine("", "ur-body");
  confirmBox_->append(*confirmBody_);
  auto* answers = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  answers->set_halign(Gtk::Align::END);
  auto* cancel = Gtk::make_managed<Gtk::Button>(T_("cancel", "Cancel"));
  cancel->signal_clicked().connect([this] { CancelRemove(); });
  answers->append(*cancel);
  auto* remove = Gtk::make_managed<Gtk::Button>(T_("remove", "Remove"));
  remove->add_css_class("destructive-action");  // red IS the confirmation context
  remove->signal_clicked().connect([this] { ConfirmRemove(); });
  answers->append(*remove);
  confirmBox_->append(*answers);
  root->append(*confirmBox_);

  auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  actions->set_halign(Gtk::Align::END);
  auto* done = Gtk::make_managed<Gtk::Button>(T_("done", "Done"));
  done->add_css_class("suggested-action");
  done->signal_clicked().connect([this] { set_visible(false); });
  actions->append(*done);
  root->append(*actions);
}

void ManageDevicesSheet::Open(bool forSignIn, const Glib::ustring& message) {
  forSignIn_ = forSignIn;
  // a new open: what an earlier one sent is not this list's
  const uint64_t issued = ++*epoch_;
  confirming_.reset();
  confirmBox_->set_visible(false);
  listBox_->set_visible(true);
  snapshot_ = manage_devices::Snapshot{};
  canManage_ = true;
  SetMessage(message);
  Render();
  std::weak_ptr<bool> alive = alive_;
  std::weak_ptr<uint64_t> epoch = epoch_;
  host_.OpenManageDevices(forSignIn, [this, alive, epoch, issued](SdkHost::ManageDevicesUpdate update) {
    PostToMain([this, alive, epoch, issued, update = std::move(update)] {
      const auto live = alive.lock();
      const auto current = epoch.lock();
      if (!live || !*live || !current || *current != issued) return;
      Apply(update);
    });
  });
  present();
}

void ManageDevicesSheet::SetMessage(const Glib::ustring& message) {
  message_->set_text(message);
  message_->set_visible(!message.empty());
}

void ManageDevicesSheet::Dismiss() {
  if (get_visible()) set_visible(false);
}

void ManageDevicesSheet::OnHidden() {
  if (!*alive_) return;
  ++*epoch_;
  confirming_.reset();
  host_.CloseManageDevices();
  g_message("app: the device list closed");
  if (on_closed) on_closed();
}

void ManageDevicesSheet::Apply(const SdkHost::ManageDevicesUpdate& update) {
  snapshot_ = PlainOf(update.snapshot);
  canManage_ = update.canManage;
  // the device being confirmed went (removed elsewhere, or by this sheet):
  // nothing left to ask about
  if (confirming_) {
    bool listed = false;
    for (const auto& device : snapshot_.devices) {
      if (device.clientId == confirming_->clientId) listed = true;
    }
    if (!listed) CancelRemove();
  }
  Render();
}

manage_devices::Words ManageDevicesSheet::MakeWords() const {
  manage_devices::Words words;
  words.lookup = [](const manage_devices::Text& text) {
    return std::string(T_(text.key, text.english));
  };
  words.format = [](const std::string& pattern, const std::string& arg) {
    return FormatArgs(pattern.c_str(), {arg});
  };
  const int64_t nowMillis = g_get_real_time() / 1000;
  words.relative = [nowMillis](int64_t unixMillis) {
    return RelativeTime((nowMillis - unixMillis) / 1000);
  };
  return words;
}

void ManageDevicesSheet::Render() {
  // the network's Pro features in use, whatever the list's own state says
  const pro_features::Notice proFeatures = pro_features::NoticeFor(snapshot_.proFeatures);
  proFeaturesBox_->set_visible(proFeatures.shows);
  proFeaturesTitle_->set_text(T_(proFeatures.title.key, proFeatures.title.english));
  proFeaturesBody_->set_text(T_(proFeatures.body.key, proFeatures.body.english));
  proFeaturesGetPro_->set_visible(pro_features::ListOffersUpgrade(forSignIn_));

  const manage_devices::Status status = manage_devices::StatusOf(snapshot_, canManage_);
  if (status.line) {
    statusText_->set_text(T_(status.line->key, status.line->english));
    if (status.error) {
      statusText_->add_css_class("ur-error-text");
    } else {
      statusText_->remove_css_class("ur-error-text");
    }
    statusText_->set_visible(true);
  } else {
    statusText_->set_visible(false);
  }
  retryButton_->set_visible(status.offersRetry);
  loading_->set_visible(status.loading);
  if (status.loading) {
    loading_->start();
  } else {
    loading_->stop();
  }

  RemoveChildren(*rowsBox_);
  if (!status.showsRows) return;
  const manage_devices::Words words = MakeWords();
  for (const manage_devices::Device& device : snapshot_.devices) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_margin_top(6);
    row->set_margin_bottom(6);
    auto* lines = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    lines->set_hexpand(true);
    lines->append(*MakeLine(manage_devices::TitleOf(device, words), "ur-row-title"));
    const std::string detail = manage_devices::DetailOf(device, words);
    if (!detail.empty()) lines->append(*MakeLine(detail, "ur-caption"));
    row->append(*lines);

    if (manage_devices::Removable(device)) {
      if (snapshot_.removingClientId == device.clientId) {
        auto* removing = Gtk::make_managed<Gtk::Spinner>();
        removing->set_valign(Gtk::Align::CENTER);
        removing->start();
        row->append(*removing);
      } else {
        auto* remove = Gtk::make_managed<Gtk::Button>(T_("remove", "Remove"));
        remove->add_css_class("flat");
        remove->add_css_class("destructive-action");
        remove->set_valign(Gtk::Align::CENTER);
        remove->set_sensitive(status.removeEnabled);
        remove->signal_clicked().connect([this, device] { AskRemove(device); });
        row->append(*remove);
      }
    }
    rowsBox_->append(*row);
  }
}

void ManageDevicesSheet::AskRemove(const manage_devices::Device& device) {
  if (!manage_devices::Removable(device)) return;
  confirming_ = device;
  const std::string title = manage_devices::TitleOf(device, MakeWords());
  confirmBody_->set_text(Format(
      T_(manage_devices::kRemoveBodyText.key, manage_devices::kRemoveBodyText.english), title));
  listBox_->set_visible(false);
  confirmBox_->set_visible(true);
}

void ManageDevicesSheet::ConfirmRemove() {
  if (!confirming_) return;
  const std::string clientId = confirming_->clientId;
  CancelRemove();
  // the sdk removes it, then adds this device again once the slot is free
  host_.RemoveManagedDevice(clientId);
}

void ManageDevicesSheet::CancelRemove() {
  confirming_.reset();
  confirmBox_->set_visible(false);
  listBox_->set_visible(true);
}

}  // namespace urnw
