// Post Quantum Identity (PQI) surfaces — port of the apple
// PostQuantumIdentityPanel / ProviderIdentitiesView /
// PostQuantumIdentityShareSheet (+ PostQuantumIdentityStore's formatting and
// caching rules):
//   * PostQuantumIdentityPanel — the drawer card with this device's own
//     identity: the identicon at 2x row size (click opens the share dialog),
//     the canonical key hash and client id (each click-copies with a toast),
//     the ALWAYS-visible peer identity deck (up to 5 overlapping identicons +
//     the peer count; click opens the provider identities list), and the
//     explanation footer.
//   * ProviderIdentitiesSheet — the live "Provider Identities" list: one row
//     per provider with an established, identity-verified e2e session
//     (identicon | grouped hash | client id; the texts click-copy).
//   * PostQuantumIdentityShareSheet — the screenshot-friendly dialog behind
//     the panel's own identicon: a 4x identicon, the FULL grouped hash
//     (nothing truncated), the client id, and a Share affordance. This app
//     has no xdg share-portal plumbing, so Share is the pragmatic fallback:
//     it copies "hash\nclientId" to the clipboard (with a toast) and saves
//     the canonical identicon png through a file-save dialog.
//
// Identicons are ALWAYS the canonical SDK raster
// (urnet::renderIdenticonPng, rendered at 2x the display size for
// crispness), cached per (hash, size) and clipped with the standard slight
// rounding (radius = size / 6) — the same bytes every platform renders for a
// key, so shared icons compare exactly.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <adwaita.h>
#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

// The canonical identity key hash display rule, shared by every platform:
// split the 52-char hash into 4-char groups and show the first 4 groups, an
// ellipsis, then the last 2 groups. Copy always uses the full un-grouped
// hash, never this display form.
std::string FormatIdentityKeyHashForDisplay(const std::string& hash);
// The full grouped hash for the share view: every 4-char group, nothing
// truncated — the share dialog exists for reading, screenshots, and
// side-channel verification.
std::string FormatIdentityKeyHashForShare(const std::string& hash);

// Identicon raster cache, keyed by (key hash, display size) — the raster
// derives from the key, which the hash captures. Renders through the
// canonical SDK png at 2x the display size.
class IdenticonCache {
 public:
  Glib::RefPtr<Gdk::Pixbuf> Get(const std::vector<uint8_t>& key, const std::string& hash,
                                int size);
  void Clear() { cache_.clear(); }

 private:
  std::map<std::string, Glib::RefPtr<Gdk::Pixbuf>> cache_;
};

// The one widget that renders an identity key identicon: the (2x) SDK raster
// scaled to `size` and clipped with the standard rounding (radius = size / 6).
// With no pixbuf it keeps its footprint with a quiet placeholder, so layouts
// do not jump when the key loads. `ring` strokes a 2px card-background ring
// (the panel's overlapping peer deck).
class IdenticonWidget : public Gtk::DrawingArea {
 public:
  explicit IdenticonWidget(int size, bool ring = false);
  void SetPixbuf(Glib::RefPtr<Gdk::Pixbuf> pixbuf);

 private:
  void Draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);

  int size_;
  bool ring_;
  Glib::RefPtr<Gdk::Pixbuf> pixbuf_;
};

// One identity row (the device's own identity on the panel and each provider
// in the deck/list share this shape): the client id, the canonical hash, and
// the raw public identity key the identicons render from.
struct IdentityRow {
  std::string clientId;
  std::string hash;
  std::vector<uint8_t> key;
};

class ProviderIdentitiesSheet;
class PostQuantumIdentityShareSheet;

class PostQuantumIdentityPanel : public Gtk::Box {
 public:
  PostQuantumIdentityPanel(SdkHost& host, Gtk::Window& parent);
  ~PostQuantumIdentityPanel() override;

  // Re-read the own identity + provider identities through the SdkHost
  // accessors (fired on the ProviderIdentities drawer event and on full
  // resyncs) and cascade to the identities list while it is open.
  void Refresh();

 private:
  void Copy(const std::string& value, const char* message);

  SdkHost& host_;
  IdenticonCache cache_;

  // own identity block (hidden until the device exposes its identity key)
  Gtk::Box* ownBox_ = nullptr;
  IdenticonWidget* ownIdenticon_ = nullptr;
  Gtk::Label* hashLabel_ = nullptr;
  Gtk::Label* clientIdLabel_ = nullptr;
  IdentityRow ownRow_;

  // peer identity deck (always visible; "0 peers" keeps the row height)
  Gtk::Box* deckHolder_ = nullptr;
  Gtk::Label* peerCountLabel_ = nullptr;
  std::vector<IdentityRow> deckRows_;  // last applied deck, to skip rebuilds

  std::unique_ptr<ProviderIdentitiesSheet> identitiesSheet_;
  std::unique_ptr<PostQuantumIdentityShareSheet> shareSheet_;
};

class ProviderIdentitiesSheet : public Gtk::Window {
 public:
  ProviderIdentitiesSheet(Gtk::Window& parent, SdkHost& host);

  void Open();
  // Live-update: rebuild the rows from the SdkHost accessor (the identity set
  // changes rarely and stays small, so a full rebuild per change is fine).
  void Refresh();

 private:
  SdkHost& host_;
  IdenticonCache cache_;
  AdwToastOverlay* toastOverlay_ = nullptr;
  Gtk::Box listBox_{Gtk::Orientation::VERTICAL};
  std::vector<IdentityRow> rows_;  // last applied rows, to skip rebuilds
};

class PostQuantumIdentityShareSheet : public Gtk::Window {
 public:
  explicit PostQuantumIdentityShareSheet(Gtk::Window& parent);

  // Present with the device's own identity (captured at open time).
  void Open(const IdentityRow& row);

 private:
  void Share();  // clipboard copy + png file-save (the no-portal fallback)

  IdenticonCache cache_;
  IdentityRow row_;
  AdwToastOverlay* toastOverlay_ = nullptr;
  IdenticonWidget* identicon_ = nullptr;
  Gtk::Label* hashLabel_ = nullptr;
  Gtk::Label* clientIdLabel_ = nullptr;
};

}  // namespace urnw
