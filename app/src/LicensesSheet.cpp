// SPDX-License-Identifier: MPL-2.0
#include "LicensesSheet.hpp"

#include <glib.h>

#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "I18n.hpp"
#include "PaneKit.hpp"
#include "Ui.hpp"
#include "UrTheme.hpp"
#include "urnetwork_sdk.hpp"

namespace urnw {
namespace {

// A sheet over the Settings page: wide enough for an 80-column license text in
// the 12px monospace face without wrapping most lines, tall enough for a
// screenful of rows.
constexpr int kSheetWidth = 640;
constexpr int kSheetHeight = 720;
constexpr int kRowTall = 44;  // the settings row species (SettingsPage kRowTall)
constexpr int kInset = 24;    // the sheets' content margin

// ---- the process-wide cache ---------------------------------------------------
// The list is a constant of this build (the SDK embeds it), so it is read once
// per process on a worker and every later open renders from here. A failed read
// is NOT cached: the next open tries again.
struct LicenseCache {
  std::mutex mutex;
  std::shared_ptr<const LicenseSections> sections;  // set once loaded
  bool running = false;
  std::vector<std::function<void(std::shared_ptr<const LicenseSections>)>> waiters;
};

LicenseCache& Cache() {
  static LicenseCache cache;
  return cache;
}

// Runs on the worker. nullptr = the read failed or came back empty.
std::shared_ptr<const LicenseSections> ReadLicenses() {
  std::optional<urnet::LicenseInfoList> list;
  try {
    // The package-level read, not host.device().getLicenses(): this process's
    // device is a DeviceRemote that exists only while signed in with the
    // daemon up, and its handle is owned by SdkHost's lifecycle, which a
    // detached worker must not race. The SDK's DeviceRemote.GetLicenses is this
    // same package function (device_rpc.go), so the list is identical.
    list = urnet::getLicenses(URNET_LICENSE_APP_LINUX);
  } catch (const std::exception& e) {
    g_warning("licenses: urnet::getLicenses threw: %s", e.what());
    return nullptr;
  }
  if (!list) {
    g_warning("licenses: urnet::getLicenses returned no list");
    return nullptr;
  }
  std::vector<LicenseEntry> entries;
  entries.reserve(list->size());
  for (auto& info : *list) {
    LicenseEntry entry;
    entry.name = std::move(info.Name);
    entry.version = std::move(info.Version);
    entry.kind = std::move(info.Kind);
    entry.url = std::move(info.Url);
    entry.spdx = std::move(info.Spdx);
    entry.copyright = std::move(info.Copyright);
    entry.notice = std::move(info.Notice);
    entry.text = std::move(info.Text);
    entries.push_back(std::move(entry));
  }
  auto sections = std::make_shared<LicenseSections>(PartitionLicenses(std::move(entries)));
  if (sections->data.empty() && sections->software.empty()) {
    // The SDK returns an empty list only when its embedded file failed to
    // parse. That is a defect, and an empty sheet would hide it.
    g_warning("licenses: the SDK returned no entries for %s", URNET_LICENSE_APP_LINUX);
    return nullptr;
  }
  return sections;
}

// Calls `done` on the GTK loop with the cached list, loading it first when
// needed. Concurrent callers share one worker.
void LoadLicenses(std::function<void(std::shared_ptr<const LicenseSections>)> done) {
  LicenseCache& cache = Cache();
  std::shared_ptr<const LicenseSections> ready;
  {
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.sections) {
      ready = cache.sections;
    } else {
      cache.waiters.push_back(std::move(done));
      if (cache.running) return;
      cache.running = true;
    }
  }
  if (ready) {
    PostToMain([done = std::move(done), ready] { done(ready); });
    return;
  }
  std::thread([] {
    auto sections = ReadLicenses();
    std::vector<std::function<void(std::shared_ptr<const LicenseSections>)>> waiters;
    {
      LicenseCache& cache = Cache();
      std::lock_guard<std::mutex> lock(cache.mutex);
      if (sections) cache.sections = sections;
      cache.running = false;
      waiters.swap(cache.waiters);
    }
    PostToMain([waiters = std::move(waiters), sections] {
      for (const auto& waiter : waiters) waiter(sections);
    });
  }).detach();
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

}  // namespace

LicensesSheet::LicensesSheet(Gtk::Window& parent) {
  EnsureBrandCss();   // the pane-row vocabulary the list is built from
  EnsureDrawerCss();  // .ur-caption / .ur-banner / .ur-mono-12
  set_transient_for(parent);
  set_modal(true);
  set_title(T_("licenses", "Licenses"));
  set_default_size(kSheetWidth, kSheetHeight);
  set_hide_on_close(true);
  add_css_class("ur-sheet");

  // Escape steps back before it closes: from an entry's detail it returns to
  // the list (the way the detail's back button does), from the list it closes.
  auto keys = Gtk::EventControllerKey::create();
  keys->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType) -> bool {
        if (keyval != GDK_KEY_Escape) return false;
        if (stack_.get_visible_child_name() == "detail") {
          ShowList();
        } else {
          set_visible(false);
        }
        return true;
      },
      false);
  add_controller(keys);

  stack_.set_transition_type(Gtk::StackTransitionType::CROSSFADE);
  stack_.set_hexpand(true);
  stack_.set_vexpand(true);
  BuildListPage();
  BuildDetailPage();
  stack_.set_visible_child("list");
  set_child(stack_);
}

LicensesSheet::~LicensesSheet() { *alive_ = false; }

void LicensesSheet::Open() {
  ShowList();
  if (state_ != LoadState::Loaded) Load();
  present();
}

// ---- list page ----------------------------------------------------------------

void LicensesSheet::BuildListPage() {
  listScroll_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  listScroll_->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  listScroll_->set_vexpand(true);

  auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  auto* intro = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  intro->set_margin_start(kInset);
  intro->set_margin_end(kInset);
  intro->set_margin_top(kInset);
  intro->set_margin_bottom(16);
  auto* heading = MakeWrappedLabel(T_("licenses", "Licenses"), "ur-step-heading");
  intro->append(*heading);
  intro->append(*MakeWrappedLabel(
      T_("licenses_intro",
         "URnetwork is built with the open source software and data below. Each entry "
         "shows its license and any notice it requires."),
      "ur-caption"));
  column->append(*intro);

  status_ = MakeWrappedLabel({}, "ur-caption");
  status_->set_margin_start(kInset);
  status_->set_margin_end(kInset);
  status_->set_margin_top(8);
  column->append(*status_);

  listRows_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  column->append(*listRows_);

  listScroll_->set_child(*column);
  stack_.add(*listScroll_, "list");
}

void LicensesSheet::Load() {
  if (loadInFlight_) return;  // a reopen while the worker runs waits on it
  loadInFlight_ = true;
  state_ = LoadState::Loading;
  status_->set_text(T_("loading", "Loading..."));
  status_->remove_css_class("ur-error-text");
  status_->set_visible(true);
  kit::SetBusy(*listRows_, true);
  auto alive = alive_;
  LoadLicenses([this, alive](std::shared_ptr<const LicenseSections> sections) {
    if (!*alive) return;  // the sheet was destroyed while the worker ran
    loadInFlight_ = false;
    kit::SetBusy(*listRows_, false);
    if (!sections) {
      state_ = LoadState::Failed;
      status_->set_text(T_("something_went_wrong", "Something went wrong."));
      status_->add_css_class("ur-error-text");
      status_->set_visible(true);
      return;
    }
    state_ = LoadState::Loaded;
    sections_ = std::move(sections);
    status_->set_visible(false);
    Render();
  });
}

void LicensesSheet::Render() {
  RemoveAllChildren(*listRows_);
  if (!sections_) return;
  AppendSection(T_("licenses_data_header", "Data attributions"), sections_->data);
  AppendSection(T_("licenses_software_header", "Open source software"),
                sections_->software);
}

void LicensesSheet::AppendSection(const Glib::ustring& header,
                                  const std::vector<LicenseEntry>& entries) {
  if (entries.empty()) return;  // a header over nothing reads as a failed load
  listRows_->append(
      *kit::MakePaneGroupHeader(header, std::to_string(entries.size())).root);
  for (const LicenseEntry& entry : entries) {
    const std::string subtitle = LicenseSubtitle(entry);
    auto row = kit::MakePaneTwoLineRowButton(entry.name, subtitle, kRowTall);
    // Long module paths keep both ends (the host and the package name are the
    // parts that identify it); the full name heads the detail.
    row.title->set_ellipsize(Pango::EllipsizeMode::MIDDLE);

    Glib::ustring accessible = entry.name;
    if (!subtitle.empty()) accessible += ". " + subtitle;
    if (!entry.notice.empty()) {
      // THE NOTICE IS SHOWN IN FULL, never trimmed: it is text the entry's
      // license requires be published (GeoLite2's MaxMind attribution).
      auto* notice = MakeWrappedLabel(entry.notice, "ur-row-note");
      notice->set_margin_top(2);
      notice->set_margin_bottom(8);
      if (auto* text = dynamic_cast<Gtk::Box*>(row.title->get_parent())) {
        text->append(*notice);
      }
      accessible += ". " + entry.notice;
    }
    kit::SetAccessibleLabel(*row.root, accessible);

    const LicenseEntry* target = &entry;  // owned by sections_, which outlives the row
    row.root->signal_clicked().connect([this, target] { ShowDetail(*target); });
    listRows_->append(*row.root);
  }
}

// ---- detail page --------------------------------------------------------------

void LicensesSheet::BuildDetailPage() {
  auto* page = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  // back to the list (Escape does the same)
  auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  bar->set_margin_start(12);
  bar->set_margin_end(12);
  bar->set_margin_top(8);
  auto* back = Gtk::make_managed<Gtk::Button>();
  back->set_icon_name("go-previous-symbolic");
  back->add_css_class("flat");
  back->set_tooltip_text(T_("back", "Back"));
  kit::SetAccessibleLabel(*back, T_("back", "Back"));
  back->signal_clicked().connect([this] { ShowList(); });
  bar->append(*back);
  page->append(*bar);

  detailScroll_ = Gtk::make_managed<Gtk::ScrolledWindow>();
  detailScroll_->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  detailScroll_->set_vexpand(true);

  auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  body->set_margin_start(kInset);
  body->set_margin_end(kInset);
  body->set_margin_top(8);
  body->set_margin_bottom(kInset);

  detailName_ = MakeWrappedLabel({}, "ur-step-heading");
  detailName_->set_selectable(true);
  body->append(*detailName_);

  detailMeta_ = MakeWrappedLabel({}, "ur-caption");
  detailMeta_->set_selectable(true);
  body->append(*detailMeta_);

  // The notice, EMPHASIZED: the banner card and the body face at full
  // contrast, so a required attribution reads as the most important line here.
  detailNotice_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  detailNotice_->add_css_class("ur-banner");
  detailNotice_->set_margin_top(8);
  detailNoticeText_ = MakeWrappedLabel({}, "ur-body");
  detailNoticeText_->set_selectable(true);
  detailNotice_->append(*detailNoticeText_);
  body->append(*detailNotice_);

  detailCopyright_ = MakeWrappedLabel({}, "ur-caption");
  detailCopyright_->set_selectable(true);
  detailCopyright_->set_margin_top(8);
  body->append(*detailCopyright_);

  // the label's default activate-link opens the URI (the Stay-in-touch rows'
  // idiom); only an http(s) URL is ever made a link
  detailLink_ = MakeWrappedLabel({}, "ur-row-title");
  detailLink_->set_margin_top(4);
  body->append(*detailLink_);

  // The full license text: monospace, selectable (read-only, no caret), laid
  // out at its own line breaks and wrapped only where the sheet is narrower.
  detailText_ = Gtk::make_managed<Gtk::TextView>();
  detailText_->set_editable(false);
  detailText_->set_cursor_visible(false);
  detailText_->set_monospace(true);
  detailText_->add_css_class("ur-mono-12");
  detailText_->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
  detailText_->set_left_margin(12);
  detailText_->set_right_margin(12);
  detailText_->set_top_margin(12);
  detailText_->set_bottom_margin(12);
  detailText_->set_margin_top(12);
  detailText_->set_hexpand(true);
  body->append(*detailText_);

  detailScroll_->set_child(*body);
  page->append(*detailScroll_);
  stack_.add(*page, "detail");
}

void LicensesSheet::ShowDetail(const LicenseEntry& entry) {
  detailName_->set_text(entry.name);
  kit::SetTextOrCollapse(*detailMeta_, LicenseSubtitle(entry));

  detailNoticeText_->set_text(entry.notice);
  detailNotice_->set_visible(!entry.notice.empty());

  std::string copyright;
  for (const auto& line : LicenseCopyrightLines(entry.copyright)) {
    if (!copyright.empty()) copyright += '\n';
    copyright += line;
  }
  kit::SetTextOrCollapse(*detailCopyright_, copyright);

  if (LicenseHasProjectPage(entry)) {
    detailLink_->set_markup("<a href=\"" + Glib::Markup::escape_text(entry.url) + "\">" +
                            Glib::Markup::escape_text(
                                T_("licenses_project_page", "Project page")) +
                            "</a>");
    detailLink_->set_tooltip_text(entry.url);
    detailLink_->set_visible(true);
  } else {
    detailLink_->set_text("");
    detailLink_->set_visible(false);
  }

  detailText_->get_buffer()->set_text(entry.text);
  detailText_->set_visible(!entry.text.empty());
  kit::SetAccessibleLabel(*detailText_, entry.name);

  stack_.set_visible_child("detail");
  if (auto adjustment = detailScroll_->get_vadjustment()) adjustment->set_value(0);
}

void LicensesSheet::ShowList() { stack_.set_visible_child("list"); }

}  // namespace urnw
