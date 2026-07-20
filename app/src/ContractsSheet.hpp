// Live contract-details sheet (port of the apple ContractDetailsView, revised
// per-contract design): a scrollable list, one row per peer client id. Each row
// shows every contract of that peer separately -- no pairing, no aggregation --
// as two independent stacks: send contracts (green) and receive contracts
// (pink), newest on top. Laid out as four columns mirrored around the row
// center: send-stats | send-circles | receive-circles | receive-stats. Each
// circle is one contract, its outer ring area-proportional to the largest
// contract in that stack ("max N" anchor beneath), its inner disc the used
// fraction, brightened while it is moving bytes.
//
// The grouping, ordering (including the at-top activity sort and the scrolled-away
// freeze), direction resolution, closing/eject lifecycle, the ~1/s rows-update
// throttle and the "N new" pending count all live in the shared SDK
// ContractDetailsViewController (single-feed, identical across apple/android/linux);
// this sheet only maps its already-ordered rows onto widgets, animates the
// per-contract stacks (contracts sliding off + the pile settling), reports its
// scroll position, and shows the "N new" chip.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <adwaita.h>
#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

class ContractStackView;  // one direction's animated pile (ContractsSheet.cpp)

// Whether the sheet shows this device's own client traffic or the traffic it
// relays as a provider (apple ContractDetailsMode). Selects the sheet's title +
// empty-state copy; the client and provider feeds are two instances of the same
// single-feed SDK view controller. Only the client sheet is created today (SdkHost
// wires the client feed); a provider sheet would open its own VC.
enum class ContractDetailsMode { Client, Provider };

class ContractsSheet : public Gtk::Window {
 public:
  ContractsSheet(Gtk::Window& parent, SdkHost& host,
                 ContractDetailsMode mode = ContractDetailsMode::Client);
  ~ContractsSheet() override;

  void Open();
  // Re-read the SDK view controller's already-ordered rows (+ pending count) and
  // reconcile them onto the list. The SDK owns membership + order -- it freezes
  // them while scrolled away -- and coalesces/throttles the change streams, so this
  // runs straight off the change event and just renders what it is handed.
  void Refresh();

 private:
  // one contract, un-aggregated (mirror of SDK ContractEntry / apple ContractEntry)
  struct EntryVM {
    std::string id;
    int64_t used = 0;
    int64_t total = 0;
    int64_t bitRate = 0;
    // a stream contract (its transfer path carries a stream id) -- drawn as a
    // double concentric outer ring so streams are visually distinct
    bool hasStream = false;
  };

  // one peer client's two contract stacks (mirror of SDK ContractPeerRow). The SDK
  // already ordered these rows and resolved the closing/eject lifecycle, so the
  // activity timestamp / closing flag it tracks internally are not mirrored here --
  // the sheet renders the rows in the order given.
  struct PeerRowVM {
    std::string clientId;
    std::vector<EntryVM> send;     // newest first
    std::vector<EntryVM> receive;  // newest first
    // cumulative bytes moved to / from this peer in the current run (accumulated
    // across the peer's contracts, reset when it goes idle), for the stack headers
    int64_t sendByteCount = 0;
    int64_t receiveByteCount = 0;
  };

  // the live-updated widgets of one peer row, keyed by client id so identity is
  // stable across refreshes (stacks keep their animation state)
  struct RowWidgets {
    Gtk::Box* container = nullptr;  // the row box + trailing divider
    ContractStackView* send = nullptr;
    ContractStackView* receive = nullptr;
  };

  std::vector<PeerRowVM> ReadRows();
  // reconcile the list widgets to the SDK-ordered `rows_` (add/remove/reorder rows)
  // and push live values into every shown row's stacks
  void ApplyRows();
  Gtk::Widget* BuildRow(const PeerRowVM& row, RowWidgets& outWidgets);
  void UpdateRow(const PeerRowVM& row, RowWidgets& widgets);
  void UpdateChip();
  // forward the scroll position to the SDK VC (which owns the ordering + freeze)
  // when it changes, and refresh the chip
  void ReportAtTop(bool atTop);
  void CopyClientId(const std::string& clientId);
  // fade a removed peer's row out (~250ms) then drop it from the list
  void FadeOutAndRemove(Gtk::Widget* container);

  SdkHost& host_;
  ContractDetailsMode mode_;

  AdwToastOverlay* toastOverlay_ = nullptr;
  Gtk::Overlay* overlay_ = nullptr;
  Gtk::ScrolledWindow* scroller_ = nullptr;
  Gtk::Stack stack_;
  Gtk::Box listBox_{Gtk::Orientation::VERTICAL};
  Gtk::Button* chipButton_ = nullptr;
  Gtk::Label* chipLabel_ = nullptr;

  std::vector<PeerRowVM> rows_;  // latest rows, already ordered by the SDK VC
  std::map<std::string, RowWidgets> rowWidgets_;
  bool isAtTop_ = true;  // seeded true (a fresh list is at the top); gates the chip
};

}  // namespace urnw
