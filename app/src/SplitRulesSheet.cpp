// SPDX-License-Identifier: MPL-2.0
#include "SplitRulesSheet.hpp"

#include <algorithm>
#include <unordered_set>

#include <glib.h>

#include "Formatters.hpp"
#include "I18n.hpp"
#include "Ui.hpp"

namespace urnw {
namespace {

// rule display text: the rule's host values split into names and ip literals
std::string RuleDisplayText(const std::vector<std::string>& hosts) {
  std::vector<std::string> hostNames;
  std::vector<std::string> ips;
  for (const auto& value : hosts) {
    (IsIpAddressValue(value) ? ips : hostNames).push_back(value);
  }
  return FormatHostClusterText(hostNames, ips);
}

// a + b deduped, preserving first-seen order
std::vector<std::string> OrderedUnion(const std::vector<std::string>& a,
                                      const std::vector<std::string>& b) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> values;
  for (const auto* list : {&a, &b}) {
    for (const auto& value : *list) {
      if (seen.insert(value).second) values.push_back(value);
    }
  }
  return values;
}

int64_t NowMs() { return g_get_real_time() / 1000; }

}  // namespace

SplitRulesSheet::SplitRulesSheet(Gtk::Window& parent, SdkHost& host) : host_(host) {
  EnsureDrawerCss();
  set_title(T_("split_rules", "Split rules"));
  set_transient_for(parent);
  set_modal(true);
  set_default_size(480, 540);
  set_hide_on_close(true);
  AddEscapeToClose(*this);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  set_child(*scroller);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin(16);
  scroller->set_child(*content);

  // info banner: how exclusions work
  auto* banner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  banner->add_css_class("ur-banner");
  auto* bannerLabel = Gtk::make_managed<Gtk::Label>(
      T_("split_rules_info_note",
         "Exclusions apply to the whole co-associated network cluster, so related traffic is "
         "caught together."));
  bannerLabel->add_css_class("dim-label");
  bannerLabel->set_wrap(true);
  bannerLabel->set_xalign(0);
  banner->append(*bannerLabel);
  content->append(*banner);

  content->append(*MakeCaption(T_("rules", "Rules")));
  content->append(rulesBox_);

  auto* activityHeader = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  auto* activityTitle = MakeCaption(T_("activity", "Activity"));
  activityTitle->set_hexpand(true);
  activityHeader->append(*activityTitle);
  countsLabel_.add_css_class("dim-label");
  countsLabel_.add_css_class("caption");
  activityHeader->append(countsLabel_);
  activityHeader->set_margin_top(8);
  content->append(*activityHeader);
  content->append(activityBox_);
}

void SplitRulesSheet::Open() {
  built_ = false;  // relative times go stale while hidden; always rebuild
  Refresh();
  present();
}

void SplitRulesSheet::Refresh() {
  // split rules = overrides whose route override forces local
  std::vector<RuleItem> rules;
  if (auto overrides = host_.BlockActionOverrides()) {
    for (const auto& override_ : *overrides) {
      if (!override_.OverrideId) continue;
      if (!override_.RouteOverride || !override_.RouteOverride->Local) continue;
      RuleItem rule;
      rule.id = *override_.OverrideId;
      if (override_.Hosts) rule.hosts = *override_.Hosts;
      rules.push_back(std::move(rule));
    }
  }

  // live activity, newest first
  std::vector<ActionItem> actions;
  if (auto blockActions = host_.BlockActions()) {
    actions.reserve(blockActions->size());
    for (auto it = blockActions->rbegin(); it != blockActions->rend(); ++it) {
      ActionItem item;
      item.id = it->BlockActionId ? *it->BlockActionId : std::string();
      item.timeMs = it->Time;
      if (it->Hosts) item.hosts = *it->Hosts;
      if (it->Ips) item.ips = *it->Ips;
      item.block = it->Block;
      item.local = it->Local;
      item.overrideId = it->OverrideId ? *it->OverrideId : std::string();
      item.hasBlockOverride = it->BlockOverride.has_value();
      item.hasRouteOverride = it->RouteOverride.has_value();
      item.byteCount = it->ByteCount;
      actions.push_back(std::move(item));
    }
  }

  int64_t allowed = 0;
  int64_t blocked = 0;
  if (auto stats = host_.BlockStatsSnapshot()) {
    allowed = stats->AllowedCount;
    blocked = stats->BlockedCount;
  }
  if (allowed != allowedCount_ || blocked != blockedCount_ || !built_) {
    allowedCount_ = allowed;
    blockedCount_ = blocked;
    countsLabel_.set_text(
        0 < allowed || 0 < blocked
            ? Format(T_("allowed_blocked_counts", "{0} allowed · {1} blocked"), allowed, blocked)
            : std::string());
  }

  // the SDK re-emits per routing decision; only rebuild the changed sections
  if (!built_ || rules != rules_) {
    rules_ = std::move(rules);
    RebuildRules();
  }
  if (!built_ || actions != actions_) {
    actions_ = std::move(actions);
    RebuildActivity();
  }
  built_ = true;
}

void SplitRulesSheet::RebuildRules() {
  RemoveAllChildren(rulesBox_);
  if (rules_.empty()) {
    auto* empty = Gtk::make_managed<Gtk::Label>(
        T_("split_rules_hint", "Tap traffic below to route it locally, bypassing the tunnel."));
    empty->add_css_class("dim-label");
    empty->set_wrap(true);
    empty->set_xalign(0);
    rulesBox_.append(*empty);
    return;
  }
  for (const auto& rule : rules_) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_margin_top(2);
    row->set_margin_bottom(2);
    SetPointerCursor(*row);

    auto* textColumn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    textColumn->set_hexpand(true);
    auto* display = Gtk::make_managed<Gtk::Label>(RuleDisplayText(rule.hosts));
    display->set_wrap(true);
    display->set_xalign(0);
    textColumn->append(*display);
    auto* hostCount = Gtk::make_managed<Gtk::Label>(
        Format(TN_("host_count", "{} host", "{} hosts", rule.hosts.size()), rule.hosts.size()));
    hostCount->add_css_class("dim-label");
    hostCount->add_css_class("caption");
    hostCount->set_xalign(0);
    textColumn->append(*hostCount);
    row->append(*textColumn);

    row->append(*MakeChip(T_("local", "Local"), "green", true));

    auto* remove = Gtk::make_managed<Gtk::Button>();
    remove->set_icon_name("user-trash-symbolic");
    remove->add_css_class("flat");
    remove->set_valign(Gtk::Align::CENTER);
    const std::string ruleId = rule.id;
    remove->signal_clicked().connect([this, ruleId] {
      host_.RemoveBlockActionOverride(ruleId);
      Refresh();  // no override-change event fires on the local-state fallback path
    });
    row->append(*remove);

    auto gesture = Gtk::GestureClick::create();
    const RuleItem ruleCopy = rule;
    gesture->signal_released().connect(
        [this, ruleCopy](int, double, double) { OpenEditorForRule(ruleCopy); });
    row->add_controller(gesture);

    rulesBox_.append(*row);
  }
}

void SplitRulesSheet::RebuildActivity() {
  RemoveAllChildren(activityBox_);
  if (actions_.empty()) {
    auto* empty = Gtk::make_managed<Gtk::Label>(
        T_("split_rules_activity_hint", "Routing activity appears here while connected."));
    empty->add_css_class("dim-label");
    empty->set_wrap(true);
    empty->set_xalign(0);
    activityBox_.append(*empty);
    return;
  }
  const int64_t nowMs = NowMs();
  for (const auto& action : actions_) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_margin_top(2);
    row->set_margin_bottom(2);
    SetPointerCursor(*row);

    auto* textColumn = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    textColumn->set_hexpand(true);
    auto* display = Gtk::make_managed<Gtk::Label>(FormatHostClusterText(action.hosts, action.ips));
    display->set_wrap(true);
    display->set_xalign(0);
    textColumn->append(*display);
    std::string caption = RelativeTime((nowMs - action.timeMs) / 1000);
    if (0 < action.byteCount) caption += "  " + FormatByteCountCompact(action.byteCount);
    auto* captionLabel = Gtk::make_managed<Gtk::Label>(caption);
    captionLabel->add_css_class("dim-label");
    captionLabel->add_css_class("ur-caption-11");
    captionLabel->set_xalign(0);
    textColumn->append(*captionLabel);
    row->append(*textColumn);

    // the decision chips light solid when an override decided this action
    row->append(*MakeChip(action.block ? T_("blocked", "Blocked") : T_("allowed", "Allowed"),
                          action.block ? "coral" : "muted", action.hasBlockOverride));
    row->append(*MakeChip(action.local ? T_("local", "Local") : T_("remote", "Remote"),
                          action.local ? "green" : "muted", action.hasRouteOverride));

    auto gesture = Gtk::GestureClick::create();
    const ActionItem actionCopy = action;
    gesture->signal_released().connect(
        [this, actionCopy](int, double, double) { OpenEditorForAction(actionCopy); });
    row->add_controller(gesture);

    activityBox_.append(*row);
  }
}

void SplitRulesSheet::OpenEditorForRule(const RuleItem& rule) {
  OpenEditor(rule.hosts, std::set<std::string>(rule.hosts.begin(), rule.hosts.end()), rule.id);
}

void SplitRulesSheet::OpenEditorForAction(const ActionItem& action) {
  std::vector<std::string> hostValues = action.hosts;
  hostValues.insert(hostValues.end(), action.ips.begin(), action.ips.end());

  // an action decided by a still-existing rule edits that rule
  const RuleItem* rule = nullptr;
  if (!action.overrideId.empty()) {
    for (const auto& r : rules_) {
      if (r.id == action.overrideId) {
        rule = &r;
        break;
      }
    }
  }
  if (rule) {
    OpenEditor(OrderedUnion(rule->hosts, hostValues),
               std::set<std::string>(rule->hosts.begin(), rule->hosts.end()), rule->id);
  } else {
    // new rule from the action's host values, host names pre-selected (or the
    // ips when the action has no host names)
    const auto& preselect = action.hosts.empty() ? action.ips : action.hosts;
    OpenEditor(hostValues, std::set<std::string>(preselect.begin(), preselect.end()),
               std::string());
  }
}

void SplitRulesSheet::OpenEditor(const std::vector<std::string>& candidates,
                                 const std::set<std::string>& selected,
                                 const std::string& ruleId) {
  const bool editing = !ruleId.empty();

  editor_ = std::make_unique<Gtk::Window>();
  editor_->set_title(editing ? T_("edit_split_rule", "Edit split rule")
                             : T_("new_split_rule", "New split rule"));
  editor_->set_transient_for(*this);
  editor_->set_modal(true);
  editor_->set_default_size(400, 380);
  editor_->set_hide_on_close(true);
  AddEscapeToClose(*editor_);

  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
  content->set_margin(16);
  editor_->set_child(*content);

  auto* subtitle = Gtk::make_managed<Gtk::Label>(
      T_("split_rule_description", "Selected hosts are routed locally and bypass the tunnel."));
  subtitle->add_css_class("dim-label");
  subtitle->set_wrap(true);
  subtitle->set_xalign(0);
  content->append(*subtitle);

  auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
  scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  scroller->set_vexpand(true);
  auto* checklist = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
  scroller->set_child(*checklist);
  content->append(*scroller);

  auto* updateBtn =
      Gtk::make_managed<Gtk::Button>(editing ? T_("update", "Update") : T_("create", "Create"));
  updateBtn->add_css_class("suggested-action");
  updateBtn->add_css_class("pill");

  // the checklist of candidate host values
  auto checks = std::make_shared<std::vector<std::pair<std::string, Gtk::CheckButton*>>>();
  auto updateSensitivity = [updateBtn, checks, editing] {
    bool any = false;
    for (const auto& entry : *checks) {
      if (entry.second->get_active()) {
        any = true;
        break;
      }
    }
    updateBtn->set_sensitive(editing || any);
  };
  for (const auto& candidate : candidates) {
    auto* check = Gtk::make_managed<Gtk::CheckButton>(candidate);
    check->set_active(selected.count(candidate) != 0);
    check->signal_toggled().connect(updateSensitivity);
    checklist->append(*check);
    checks->emplace_back(candidate, check);
  }
  updateSensitivity();

  updateBtn->signal_clicked().connect([this, checks, ruleId, editing] {
    urnet::StringList hosts;
    for (const auto& entry : *checks) {
      if (entry.second->get_active()) hosts.push_back(entry.first);
    }
    if (editing) {
      if (hosts.empty()) {
        // an empty selection removes the rule
        host_.RemoveBlockActionOverride(ruleId);
      } else {
        host_.SetBlockActionOverrideHosts(ruleId, hosts);
      }
    } else if (!hosts.empty()) {
      urnet::BlockActionOverride override_;
      override_.OverrideId = urnet::newId();
      override_.Hosts = hosts;
      urnet::RouteOverride route;
      route.Local = true;
      override_.RouteOverride = route;
      host_.AddBlockActionOverride(override_);
    }
    editor_->set_visible(false);
    Refresh();  // no override-change event fires on the local-state fallback path
  });
  content->append(*updateBtn);

  if (editing) {
    auto* removeBtn = Gtk::make_managed<Gtk::Button>(T_("remove_rule", "Remove rule"));
    removeBtn->add_css_class("flat");
    removeBtn->add_css_class("destructive-action");
    removeBtn->signal_clicked().connect([this, ruleId] {
      host_.RemoveBlockActionOverride(ruleId);
      editor_->set_visible(false);
      Refresh();
    });
    content->append(*removeBtn);
  }

  editor_->present();
}

}  // namespace urnw
