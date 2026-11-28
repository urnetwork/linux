// The '?' help beside a term the app uses without explaining it: Provide,
// Pending and Extender (owner, 2026-11-28: help UI on every app, with short
// copy from the localization store). A flat help button whose popover says
// what the term means, the info popover idiom of ProviderLocationsSheet's
// location sync row. Help only: it changes nothing.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <gtkmm.h>

#include "I18n.hpp"
#include "PaneKit.hpp"

namespace urnw {

enum class HelpTopic { Provide, Pending, Extender };

// A topic's words: the title, which is also the button's tooltip and its
// accessible name, and the explanation.
struct HelpText {
  const char* title = "";
  const char* body = "";
};

inline HelpText HelpTextFor(HelpTopic topic) {
  switch (topic) {
    case HelpTopic::Provide:
      return {T_("help_provide_title", "What is providing?"),
              T_("help_provide_body",
                 "Providing shares some of your internet with other URnetwork members: their "
                 "traffic can go out through your connection. Providers can earn rewards for the "
                 "data they share. Provide mode sets when this device provides, and you can stop "
                 "at any time.")};
    case HelpTopic::Pending:
      return {T_("help_pending_title", "What is Pending?"),
              T_("help_pending_body",
                 "Pending is data set aside for your open connections. What they don't use goes "
                 "back to your available data when they close.")};
    case HelpTopic::Extender:
      return {T_("help_extender_title", "What is an extender?"),
              T_("help_extender_body",
                 "An extender relays your connection to URnetwork where it is blocked. Your "
                 "traffic stays encrypted through it, and the extender can't read it.")};
  }
  return {};
}

// The button, owned by the container it is appended to.
inline Gtk::MenuButton* MakeHelpButton(HelpTopic topic) {
  const HelpText text = HelpTextFor(topic);
  auto* button = Gtk::make_managed<Gtk::MenuButton>();
  button->set_icon_name("help-about-symbolic");
  button->add_css_class("flat");
  button->set_valign(Gtk::Align::CENTER);
  button->set_tooltip_text(text.title);
  kit::SetAccessibleLabel(*button, text.title);
  auto* popover = Gtk::make_managed<Gtk::Popover>();
  auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
  content->set_margin(12);
  content->set_size_request(320, -1);
  auto* title = Gtk::make_managed<Gtk::Label>(text.title);
  title->add_css_class("heading");
  title->set_xalign(0);
  title->set_wrap(true);
  content->append(*title);
  auto* body = Gtk::make_managed<Gtk::Label>(text.body);
  body->set_xalign(0);
  body->set_wrap(true);
  content->append(*body);
  popover->set_child(*content);
  button->set_popover(*popover);
  return button;
}

}  // namespace urnw
