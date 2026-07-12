// "Client contracts" sheet (port of the apple ContractDetailsView, client
// mode): a scrollable list with one row per peer client, aggregating that
// peer's egress ("contract", green) and ingress ("companion", pink) contracts.
// Each row shows the full client id (click to copy), an area-proportional
// usage circle per contract side, and the live transfer lines between them.
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
  // Re-read + re-aggregate. Same peer set: the rows update in place (usage
  // discs ease to their new size); membership/order changes rebuild the list.
  void Refresh();

 private:
  // Aggregated contract pairs for one peer client (apple ContractClientRow).
  struct PeerRow {
    std::string clientId;
    int64_t contractUsed = 0;
    int64_t contractTotal = 0;
    int64_t contractBitRate = 0;
    int64_t companionUsed = 0;
    int64_t companionTotal = 0;
    int64_t companionBitRate = 0;
    int pairCount = 0;

    bool operator==(const PeerRow& o) const {
      return clientId == o.clientId && contractUsed == o.contractUsed &&
             contractTotal == o.contractTotal && contractBitRate == o.contractBitRate &&
             companionUsed == o.companionUsed && companionTotal == o.companionTotal &&
             companionBitRate == o.companionBitRate && pairCount == o.pairCount;
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
  std::vector<std::string> rowIds_;                // current row order
  std::map<std::string, RowWidgets> rowWidgets_;   // per-peer live widgets
  // first-seen order per client id; newer clients sort to the top
  std::map<std::string, int> clientOrder_;
};

}  // namespace urnw
