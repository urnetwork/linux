// SPDX-License-Identifier: MPL-2.0
#include "PostQuantumIdentity.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <gdkmm/general.h>
#include <gdkmm/pixbufloader.h>
#include <glib.h>

#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// identicon display sizes (apple PostQuantumIdentityStore); rasters render at
// 2x these for crispness
constexpr int kDeckIdenticonSize = 28;
constexpr int kRowIdenticonSize = 40;
// the panel's own-identity identicon: 2x a list row
constexpr int kPanelIdenticonSize = 80;
// the share dialog identicon: 4x the panel, sized for screenshots
constexpr int kShareIdenticonSize = 320;
// at most this many provider identicons in the deck; the peer count label
// carries the total
constexpr int kMaxDeckIdenticons = 5;
// how far each deck identicon tucks under the previous one
constexpr int kDeckOverlap = 10;

std::vector<uint8_t> DecodeBase64(const std::string& base64) {
  std::vector<uint8_t> out;
  if (base64.empty()) return out;
  gsize len = 0;
  guchar* data = g_base64_decode(base64.c_str(), &len);
  if (data) {
    out.assign(data, data + len);
    g_free(data);
  }
  return out;
}

// split the canonical hash into 4-char groups
std::vector<std::string> HashGroups(const std::string& hash) {
  std::vector<std::string> groups;
  for (size_t i = 0; i < hash.size(); i += 4) {
    groups.push_back(hash.substr(i, 4));
  }
  return groups;
}

std::string JoinGroups(const std::vector<std::string>& groups) {
  std::string out;
  for (const auto& group : groups) {
    if (!out.empty()) out += ' ';
    out += group;
  }
  return out;
}

// The peer count indicator, always shown ("0 peers" included). Two keys, not
// a gettext plural: peer_count_other is a printf-style key shared with every
// platform.
std::string PeerCountText(int count) {
  if (count == 1) return T_("peer_count_one", "1 peer");
  gchar* text = g_strdup_printf(T_("peer_count_other", "%d peers"), count);
  std::string out = text ? text : "";
  g_free(text);
  return out;
}

// the standard identicon rounding: radius = size / 6, everywhere
void RoundedRectPath(const Cairo::RefPtr<Cairo::Context>& cr, double inset, double size,
                     double radius) {
  const double lo = inset;
  const double hi = size - inset;
  const double r = std::max(0.0, radius - inset);
  cr->begin_new_path();
  cr->arc(hi - r, lo + r, r, -G_PI / 2, 0);
  cr->arc(hi - r, hi - r, r, 0, G_PI / 2);
  cr->arc(lo + r, hi - r, r, G_PI / 2, G_PI);
  cr->arc(lo + r, lo + r, r, G_PI, 3 * G_PI / 2);
  cr->close_path();
}

// click-to-copy: pointer cursor + released gesture
void MakeClickable(Gtk::Widget& widget, std::function<void()> action) {
  SetPointerCursor(widget);
  auto gesture = Gtk::GestureClick::create();
  gesture->signal_released().connect(
      [action = std::move(action)](int, double, double) { action(); });
  widget.add_controller(gesture);
}

}  // namespace

// The providers with an established, identity-verified e2e session, decoded
// from the JSON-crossing ProviderIdentityList (PublicKey crosses as base64).
// The hash is computed through the canonical SDK rule, like apple's
// identity.getPublicKeyHash(). Exposed (see the header) so the
// provider-locations badge joins against the same set.
std::vector<IdentityRow> ReadProviderIdentityRows(SdkHost& host) {
  std::vector<IdentityRow> rows;
  if (auto list = host.ProviderIdentities()) {
    rows.reserve(list->size());
    for (const auto& identity : *list) {
      if (!identity.ClientId || identity.ClientId->empty()) continue;
      std::vector<uint8_t> key = DecodeBase64(identity.PublicKey);
      if (key.empty()) continue;
      IdentityRow row;
      row.clientId = *identity.ClientId;
      row.hash = urnet::publicIdentityKeyHash(key.data(), static_cast<int32_t>(key.size()));
      row.key = std::move(key);
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

bool SameIdentityRows(const std::vector<IdentityRow>& a, const std::vector<IdentityRow>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].clientId != b[i].clientId || a[i].hash != b[i].hash) return false;
  }
  return true;
}

// ---- hash display rules (apple PostQuantumIdentityStore) --------------------

std::string FormatIdentityKeyHashForDisplay(const std::string& hash) {
  const std::vector<std::string> groups = HashGroups(hash);
  if (groups.size() <= 6) return JoinGroups(groups);
  std::vector<std::string> shown(groups.begin(), groups.begin() + 4);
  shown.push_back("…");
  shown.insert(shown.end(), groups.end() - 2, groups.end());
  return JoinGroups(shown);
}

std::string FormatIdentityKeyHashForShare(const std::string& hash) {
  return JoinGroups(HashGroups(hash));
}

// ---- identicon raster cache -------------------------------------------------

Glib::RefPtr<Gdk::Pixbuf> IdenticonCache::Get(const std::vector<uint8_t>& key,
                                              const std::string& hash, int size) {
  const std::string cacheKey = hash + ":" + std::to_string(size);
  if (auto it = cache_.find(cacheKey); it != cache_.end()) return it->second;
  Glib::RefPtr<Gdk::Pixbuf> pixbuf;
  try {
    // ALWAYS the canonical SDK raster, at 2x the display size (the widget
    // scales it back down, so it stays crisp on hidpi)
    const std::vector<uint8_t> png = urnet::renderIdenticonPng(key, size * 2);
    auto loader = Gdk::PixbufLoader::create();
    loader->write(png.data(), png.size());
    loader->close();
    pixbuf = loader->get_pixbuf();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[pqi] identicon render failed: %s\n", e.what());
    return {};
  }
  cache_[cacheKey] = pixbuf;
  return pixbuf;
}

// ---- IdenticonWidget --------------------------------------------------------

IdenticonWidget::IdenticonWidget(int size, bool ring) : size_(size), ring_(ring) {
  set_content_width(size_);
  set_content_height(size_);
  set_draw_func(sigc::mem_fun(*this, &IdenticonWidget::Draw));
}

void IdenticonWidget::SetPixbuf(Glib::RefPtr<Gdk::Pixbuf> pixbuf) {
  pixbuf_ = std::move(pixbuf);
  queue_draw();
}

void IdenticonWidget::Draw(const Cairo::RefPtr<Cairo::Context>& cr, int, int) {
  const double size = size_;
  const double radius = size / 6.0;
  cr->save();
  RoundedRectPath(cr, 0, size, radius);
  cr->clip();
  if (pixbuf_ && 0 < pixbuf_->get_width()) {
    const double scale = size / pixbuf_->get_width();
    cr->scale(scale, scale);
    Gdk::Cairo::set_source_pixbuf(cr, pixbuf_, 0, 0);
    cr->paint();
  } else {
    // key not available yet: a quiet placeholder with the same footprint, so
    // the layout does not jump when the key loads
    cr->set_source_rgba(0.5, 0.5, 0.5, 0.15);
    cr->paint();
  }
  cr->restore();
  if (ring_) {
    // a ring in the card background color separates the overlapped deck
    // identicons (stroked just inside the bounds so it never clips)
    RoundedRectPath(cr, 1.0, size, radius);
    cr->set_source_rgba(kUrCardBackground.r, kUrCardBackground.g, kUrCardBackground.b, 1.0);
    cr->set_line_width(2.0);
    cr->stroke();
  }
}

// ---- PostQuantumIdentityPanel ----------------------------------------------

PostQuantumIdentityPanel::PostQuantumIdentityPanel(SdkHost& host, Gtk::Window& parent)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 0), host_(host) {
  EnsureDrawerCss();
  add_css_class("ur-card");

  auto* title =
      Gtk::make_managed<Gtk::Label>(T_("post_quantum_identity", "Post Quantum Identity"));
  title->add_css_class("dim-label");
  title->set_xalign(0);
  title->set_margin_bottom(12);
  append(*title);

  // this device's own identity: the large identicon, then the key hash and
  // client id, each click to copy. Hidden as a block until the device exposes
  // its identity key (tunnel up).
  ownBox_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

  // click opens the share dialog (screenshot-friendly identicon + hash +
  // client id, with a share affordance)
  ownIdenticon_ = Gtk::make_managed<IdenticonWidget>(kPanelIdenticonSize);
  ownIdenticon_->set_halign(Gtk::Align::START);
  ownIdenticon_->set_margin_bottom(12);
  MakeClickable(*ownIdenticon_, [this] {
    if (!ownRow_.hash.empty()) shareSheet_->Open(ownRow_);
  });
  ownBox_->append(*ownIdenticon_);

  hashLabel_ = Gtk::make_managed<Gtk::Label>();
  hashLabel_->add_css_class("ur-mono-13");
  hashLabel_->set_xalign(0);
  hashLabel_->set_wrap(true);
  hashLabel_->set_margin_bottom(4);
  // copy always uses the full un-grouped hash, never the display form
  MakeClickable(*hashLabel_, [this] {
    if (!ownRow_.hash.empty()) {
      Copy(ownRow_.hash, T_("identity_key_hash_copied", "Identity key hash copied"));
    }
  });
  ownBox_->append(*hashLabel_);

  clientIdLabel_ = Gtk::make_managed<Gtk::Label>();
  clientIdLabel_->add_css_class("ur-mono-11");
  clientIdLabel_->add_css_class("ur-label-faint");
  clientIdLabel_->set_xalign(0);
  clientIdLabel_->set_wrap(true);
  MakeClickable(*clientIdLabel_, [this] {
    if (!ownRow_.clientId.empty()) {
      Copy(ownRow_.clientId, T_("client_id_copied", "Client ID copied"));
    }
  });
  ownBox_->append(*clientIdLabel_);

  ownBox_->set_margin_bottom(12);
  ownBox_->set_visible(false);
  append(*ownBox_);

  // the connected peer identity deck with the peer count: ALWAYS visible (a
  // "0 peers" status when none, keeping the row height), click for details
  auto* deckRow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  deckRow->set_size_request(-1, kDeckIdenticonSize);
  deckHolder_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
  deckHolder_->set_valign(Gtk::Align::CENTER);
  // hidden while empty so the count label sits flush left (no stray spacing)
  deckHolder_->set_visible(false);
  deckRow->append(*deckHolder_);
  peerCountLabel_ = Gtk::make_managed<Gtk::Label>(PeerCountText(0));
  peerCountLabel_->add_css_class("dim-label");
  peerCountLabel_->set_xalign(0);
  peerCountLabel_->set_valign(Gtk::Align::CENTER);
  deckRow->append(*peerCountLabel_);
  deckRow->add_css_class("ur-card-tappable");
  MakeClickable(*deckRow, [this] { identitiesSheet_->Open(); });
  deckRow->set_margin_bottom(12);
  append(*deckRow);

  auto* explanation = Gtk::make_managed<Gtk::Label>(T_(
      "post_quantum_identity_explanation",
      "Your identity key is stored locally on this device. If any peer's key appears different "
      "than their locally stored key, it means the network operator cannot be trusted."));
  explanation->add_css_class("dim-label");
  explanation->add_css_class("caption");
  explanation->set_xalign(0);
  explanation->set_wrap(true);
  append(*explanation);

  identitiesSheet_ = std::make_unique<ProviderIdentitiesSheet>(parent, host_);
  shareSheet_ = std::make_unique<PostQuantumIdentityShareSheet>(parent);

  Refresh();
}

PostQuantumIdentityPanel::~PostQuantumIdentityPanel() = default;

void PostQuantumIdentityPanel::Copy(const std::string& value, const char* message) {
  if (auto clipboard = get_clipboard()) clipboard->set_text(value);
  ShowToast(*this, message);
}

void PostQuantumIdentityPanel::Refresh() {
  // the device's own identity, shaped like a provider row (apple renders both
  // through the same shape)
  IdentityRow own;
  own.hash = host_.PublicIdentityKeyHash();
  own.key = host_.PublicIdentityKey();
  own.clientId = host_.ClientId();
  const bool haveOwn = !own.hash.empty() && !own.key.empty();
  if (haveOwn) {
    if (own.hash != ownRow_.hash || own.clientId != ownRow_.clientId) {
      hashLabel_->set_text(FormatIdentityKeyHashForDisplay(own.hash));
      clientIdLabel_->set_text(own.clientId);
      ownIdenticon_->SetPixbuf(cache_.Get(own.key, own.hash, kPanelIdenticonSize));
    }
    ownRow_ = std::move(own);
  } else {
    ownRow_ = IdentityRow{};
  }
  ownBox_->set_visible(haveOwn);

  // the peer identity deck: up to kMaxDeckIdenticons overlapping identicons;
  // the count label carries the total and is always shown
  std::vector<IdentityRow> rows = ReadProviderIdentityRows(host_);
  peerCountLabel_->set_text(PeerCountText(static_cast<int>(rows.size())));
  deckHolder_->set_visible(!rows.empty());
  if (!SameIdentityRows(rows, deckRows_)) {
    RemoveAllChildren(*deckHolder_);
    if (!rows.empty()) {
      const int shown = std::min<int>(static_cast<int>(rows.size()), kMaxDeckIdenticons);
      const int step = kDeckIdenticonSize - kDeckOverlap;
      // a fixed container gives the negative-spacing overlap a Box cannot;
      // later children draw on top, like the apple deck
      auto* fixed = Gtk::make_managed<Gtk::Fixed>();
      fixed->set_size_request(step * (shown - 1) + kDeckIdenticonSize, kDeckIdenticonSize);
      for (int i = 0; i < shown; ++i) {
        auto* icon = Gtk::make_managed<IdenticonWidget>(kDeckIdenticonSize, /*ring=*/true);
        icon->SetPixbuf(cache_.Get(rows[i].key, rows[i].hash, kDeckIdenticonSize));
        fixed->put(*icon, i * step, 0);
      }
      deckHolder_->append(*fixed);
    }
    deckRows_ = std::move(rows);
  }

  // device down and no peers: nothing left referencing the rasters
  if (!haveOwn && deckRows_.empty()) cache_.Clear();

  // cascade to the open identities list (it re-reads the same accessor)
  if (identitiesSheet_ && identitiesSheet_->is_visible()) identitiesSheet_->Refresh();
}

// ---- ProviderIdentitiesSheet ------------------------------------------------

ProviderIdentitiesSheet::ProviderIdentitiesSheet(Gtk::Window& parent, SdkHost& host)
    : host_(host) {
  EnsureDrawerCss();
  set_title(T_("provider_identities", "Provider Identities"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(480, 560);
  set_hide_on_close(true);
  AddEscapeToClose(*this);

  // AdwToastOverlay (C API) hosts the copied-to-clipboard toasts
  toastOverlay_ = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  gtk_window_set_child(GTK_WINDOW(gobj()), GTK_WIDGET(toastOverlay_));

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  adw_toast_overlay_set_child(toastOverlay_, GTK_WIDGET(scroller->gobj()));

  listBox_.set_margin(16);
  listBox_.set_valign(Gtk::Align::START);
  scroller->set_child(listBox_);
}

void ProviderIdentitiesSheet::Open() {
  Refresh();
  present();
}

void ProviderIdentitiesSheet::Refresh() {
  std::vector<IdentityRow> rows = ReadProviderIdentityRows(host_);
  if (SameIdentityRows(rows, rows_)) return;  // rows carry no live values

  RemoveAllChildren(listBox_);
  auto copyable = [this](Gtk::Label* label, std::string value, bool isHash) {
    MakeClickable(*label, [this, value = std::move(value), isHash] {
      get_clipboard()->set_text(value);
      adw_toast_overlay_add_toast(
          toastOverlay_,
          adw_toast_new(isHash ? T_("identity_key_hash_copied", "Identity key hash copied")
                               : T_("client_id_copied", "Client ID copied")));
    });
  };
  for (const IdentityRow& row : rows) {
    auto* rowBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    rowBox->set_margin_top(12);
    rowBox->set_margin_bottom(12);

    auto* icon = Gtk::make_managed<IdenticonWidget>(kRowIdenticonSize);
    icon->SetPixbuf(cache_.Get(row.key, row.hash, kRowIdenticonSize));
    icon->set_valign(Gtk::Align::CENTER);
    rowBox->append(*icon);

    auto* column = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    column->set_hexpand(true);
    column->set_valign(Gtk::Align::CENTER);

    // the identity key hash, click to copy the full hash
    auto* hashLabel = Gtk::make_managed<Gtk::Label>(FormatIdentityKeyHashForDisplay(row.hash));
    hashLabel->add_css_class("ur-mono-13");
    hashLabel->set_xalign(0);
    hashLabel->set_wrap(true);
    copyable(hashLabel, row.hash, /*isHash=*/true);
    column->append(*hashLabel);

    // the client id, click to copy
    auto* idLabel = Gtk::make_managed<Gtk::Label>(row.clientId);
    idLabel->add_css_class("ur-mono-11");
    idLabel->add_css_class("ur-label-faint");
    idLabel->set_xalign(0);
    idLabel->set_wrap(true);
    copyable(idLabel, row.clientId, /*isHash=*/false);
    column->append(*idLabel);

    rowBox->append(*column);
    listBox_.append(*rowBox);
    listBox_.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
  }
  rows_ = std::move(rows);
}

// ---- PostQuantumIdentityShareSheet -----------------------------------------

PostQuantumIdentityShareSheet::PostQuantumIdentityShareSheet(Gtk::Window& parent) {
  EnsureDrawerCss();
  set_title(T_("post_quantum_identity", "Post Quantum Identity"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(420, 560);  // the mac sheet frame
  set_hide_on_close(true);
  AddEscapeToClose(*this);

  toastOverlay_ = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  gtk_window_set_child(GTK_WINDOW(gobj()), GTK_WIDGET(toastOverlay_));

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  adw_toast_overlay_set_child(toastOverlay_, GTK_WIDGET(scroller->gobj()));

  // laid out for an easy screenshot: the identicon + full hash + client id
  // are the complete side-channel verification payload
  auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  box->set_margin_start(24);
  box->set_margin_end(24);
  box->set_valign(Gtk::Align::CENTER);

  auto* title =
      Gtk::make_managed<Gtk::Label>(T_("post_quantum_identity", "Post Quantum Identity"));
  title->add_css_class("dim-label");
  title->set_halign(Gtk::Align::CENTER);
  title->set_margin_top(32);
  title->set_margin_bottom(24);
  box->append(*title);

  identicon_ = Gtk::make_managed<IdenticonWidget>(kShareIdenticonSize);
  identicon_->set_halign(Gtk::Align::CENTER);
  identicon_->set_margin_bottom(24);
  box->append(*identicon_);

  // the full grouped hash: the share view is for reading and screenshots, so
  // nothing is truncated
  hashLabel_ = Gtk::make_managed<Gtk::Label>();
  hashLabel_->add_css_class("ur-mono-15");
  hashLabel_->set_wrap(true);
  hashLabel_->set_justify(Gtk::Justification::CENTER);
  hashLabel_->set_halign(Gtk::Align::CENTER);
  hashLabel_->set_max_width_chars(40);
  hashLabel_->set_margin_bottom(8);
  box->append(*hashLabel_);

  clientIdLabel_ = Gtk::make_managed<Gtk::Label>();
  clientIdLabel_->add_css_class("ur-mono-12");
  clientIdLabel_->add_css_class("dim-label");
  clientIdLabel_->set_wrap(true);
  clientIdLabel_->set_justify(Gtk::Justification::CENTER);
  clientIdLabel_->set_halign(Gtk::Align::CENTER);
  clientIdLabel_->set_margin_bottom(32);
  box->append(*clientIdLabel_);

  auto* shareBtn = Gtk::make_managed<Gtk::Button>();
  shareBtn->add_css_class("suggested-action");
  shareBtn->set_halign(Gtk::Align::CENTER);
  shareBtn->set_margin_bottom(32);
  {
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name("send-to-symbolic");
    content->append(*icon);
    content->append(*Gtk::make_managed<Gtk::Label>(T_("share", "Share")));
    shareBtn->set_child(*content);
  }
  shareBtn->signal_clicked().connect(sigc::mem_fun(*this, &PostQuantumIdentityShareSheet::Share));
  box->append(*shareBtn);

  scroller->set_child(*box);
}

void PostQuantumIdentityShareSheet::Open(const IdentityRow& row) {
  row_ = row;
  hashLabel_->set_text(FormatIdentityKeyHashForShare(row_.hash));
  clientIdLabel_->set_text(row_.clientId);
  identicon_->SetPixbuf(cache_.Get(row_.key, row_.hash, kShareIdenticonSize));
  present();
}

// The share affordance. This app has no xdg share-portal plumbing, so this is
// the pragmatic fallback: copy the text payload ("hash\nclientId", the apple
// ShareLink message) to the clipboard with a toast, and save the canonical
// identicon png through a file-save dialog.
void PostQuantumIdentityShareSheet::Share() {
  if (row_.hash.empty()) return;
  get_clipboard()->set_text(row_.hash + "\n" + row_.clientId);
  adw_toast_overlay_add_toast(toastOverlay_, adw_toast_new(T_("site_app_copied", "Copied")));

  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, T_("share", "Share"));
  gtk_file_dialog_set_initial_name(dialog, "urnetwork-identity.png");
  // `this` outlives the dialog: the sheet is owned by the panel for the
  // window's whole lifetime (hide-on-close)
  gtk_file_dialog_save(
      dialog, GTK_WINDOW(gobj()), nullptr,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
        if (!file) return;  // dismissed
        auto* self = static_cast<PostQuantumIdentityShareSheet*>(data);
        try {
          // the canonical png at 2x the share display size — the same bytes
          // every platform exports for this key, so shared icons compare
          // exactly (apple IdentityIdenticonTransferable)
          const std::vector<uint8_t> png =
              urnet::renderIdenticonPng(self->row_.key, kShareIdenticonSize * 2);
          GError* error = nullptr;
          if (!g_file_replace_contents(file, reinterpret_cast<const char*>(png.data()),
                                       png.size(), nullptr, false,
                                       G_FILE_CREATE_REPLACE_DESTINATION, nullptr, nullptr,
                                       &error)) {
            std::fprintf(stderr, "[pqi] identicon save failed: %s\n",
                         error ? error->message : "unknown");
            g_clear_error(&error);
          }
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[pqi] identicon render failed: %s\n", e.what());
        }
        g_object_unref(file);
      },
      this);
  g_object_unref(dialog);
}

}  // namespace urnw
