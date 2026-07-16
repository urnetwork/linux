// "Client contracts" sheet (port of the apple ContractDetailsView, client
// mode): a scrollable list with one row per peer client, showing that peer's
// egress ("contract", green) and ingress ("companion", pink) contracts. Each
// row shows the full client id (click to copy), an area-proportional usage
// circle per contract side, and the live transfer lines between them.
//
// This is a thin adapter over the SDK ContractDetailsViewController: that view
// controller owns the egress+ingress coalescing, renewal-atomic slot holds,
// per-peer aggregation, and the closing/eject lifecycle (all shared with the
// apple/android/windows apps). The sheet only maps its rows onto widgets and
// animates the circle swap/eject (mirroring the apple ContractRing).
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

class ContractCircle;  // eased area-proportional usage circle (ContractsSheet.cpp)
class TransferLines;   // directional transfer-rate lines (ContractsSheet.cpp)

class ContractsSheet : public Gtk::Window {
 public:
  ContractsSheet(Gtk::Window& parent, SdkHost& host);

  void Open();
  // Re-read the SDK view controller's rows and map them onto the list. Same peer
  // set: the rows update in place (usage discs ease, a contract swap ejects the
  // old ring); membership/order changes rebuild the list. The SDK already
  // coalesced the change streams, so this is driven directly off the event.
  void Refresh();

 private:
  // One peer client's rendered contract pair (mirror of the SDK ContractClientRow
  // / apple ContractClientRow); the SDK produced these already aggregated.
  struct PeerRow {
    std::string clientId;
    // signatures of the peer's active contract ids; a change means a contract was
    // replaced -> swap (eject) the ring rather than just resizing the usage disc
    std::string contractId;
    std::string companionContractId;
    int64_t contractUsed = 0;
    int64_t contractTotal = 0;
    int64_t contractBitRate = 0;
    int64_t companionUsed = 0;
    int64_t companionTotal = 0;
    int64_t companionBitRate = 0;
    int pairCount = 0;
    // the peer's last contract closed; the SDK keeps the row briefly so its
    // circles can eject, then removes it
    bool closing = false;

    bool operator==(const PeerRow& o) const {
      return clientId == o.clientId && contractId == o.contractId &&
             companionContractId == o.companionContractId && contractUsed == o.contractUsed &&
             contractTotal == o.contractTotal && contractBitRate == o.contractBitRate &&
             companionUsed == o.companionUsed && companionTotal == o.companionTotal &&
             companionBitRate == o.companionBitRate && pairCount == o.pairCount &&
             closing == o.closing;
    }
    bool operator!=(const PeerRow& o) const { return !(*this == o); }
  };

  // the live-updated widgets of one peer row
  struct RowWidgets {
    ContractCircle* contractCircle = nullptr;
    Gtk::Label* contractUsed = nullptr;
    Gtk::Label* contractTotal = nullptr;
    ContractCircle* companionCircle = nullptr;
    Gtk::Label* companionUsed = nullptr;
    Gtk::Label* companionTotal = nullptr;
    TransferLines* lines = nullptr;
    Gtk::Label* pairCount = nullptr;
  };

  std::vector<PeerRow> ReadRows();
  void RebuildList();
  Gtk::Widget* BuildRow(const PeerRow& row, RowWidgets& outWidgets);
  void UpdateRowWidgets(const PeerRow& row, RowWidgets& widgets);
  void CopyClientId(const std::string& clientId);

  SdkHost& host_;
  AdwToastOverlay* toastOverlay_ = nullptr;
  Gtk::Stack stack_;
  Gtk::Box listBox_{Gtk::Orientation::VERTICAL};

  bool built_ = false;  // first Refresh always builds the list
  std::vector<PeerRow> rows_;
  std::vector<std::string> rowIds_;               // current row order (SDK order)
  std::map<std::string, RowWidgets> rowWidgets_;  // per-peer live widgets
};

}  // namespace urnw
