// "Split rules" sheet (port of the apple SplitRulesView): the pinned split
// rules (block action overrides whose route override forces local) above the
// live block-action activity feed, with an editor sheet to create/update a
// rule from a host-value checklist. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "SdkHost.hpp"

namespace urnw {

class SplitRulesSheet : public Gtk::Window {
 public:
  SplitRulesSheet(Gtk::Window& parent, SdkHost& host);

  void Open();
  // Re-read rules/activity/stats; rebuilds only the sections that changed.
  void Refresh();

 private:
  struct RuleItem {
    std::string id;
    std::vector<std::string> hosts;
    bool operator==(const RuleItem& o) const { return id == o.id && hosts == o.hosts; }
  };
  struct ActionItem {
    std::string id;
    int64_t timeMs = 0;
    std::vector<std::string> hosts;
    std::vector<std::string> ips;
    bool block = false;
    bool local = false;
    std::string overrideId;
    bool hasBlockOverride = false;
    bool hasRouteOverride = false;
    int64_t byteCount = 0;
    bool operator==(const ActionItem& o) const {
      return id == o.id && timeMs == o.timeMs && hosts == o.hosts && ips == o.ips &&
             block == o.block && local == o.local && overrideId == o.overrideId &&
             hasBlockOverride == o.hasBlockOverride && hasRouteOverride == o.hasRouteOverride &&
             byteCount == o.byteCount;
    }
  };

  void RebuildRules();
  void RebuildActivity();
  void OpenEditorForRule(const RuleItem& rule);
  void OpenEditorForAction(const ActionItem& action);
  // ruleId empty -> create a new rule from the selected candidates.
  void OpenEditor(const std::vector<std::string>& candidates,
                  const std::set<std::string>& selected, const std::string& ruleId);

  SdkHost& host_;
  Gtk::Label countsLabel_;      // "A allowed · B blocked"
  Gtk::Box rulesBox_{Gtk::Orientation::VERTICAL, 4};
  Gtk::Box activityBox_{Gtk::Orientation::VERTICAL, 4};

  bool built_ = false;
  std::vector<RuleItem> rules_;
  std::vector<ActionItem> actions_;  // newest first
  int64_t allowedCount_ = 0;
  int64_t blockedCount_ = 0;

  std::unique_ptr<Gtk::Window> editor_;
};

}  // namespace urnw
