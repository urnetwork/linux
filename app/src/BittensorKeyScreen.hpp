// The Key screen of the one-tap Bittensor key, the same for both flows (apple
// BittensorKeyScreen.swift; the approved mocks' 1d to 1h, 1j, 1k, 2b to 2g and
// 2i): the words hidden by default, reveal, Hide and Copy once they show, the
// not-a-wallet text always, Keep beside the one destructive action, both
// behind two of the words. What deleting means, and the two words, replace the
// screen's content rather than cover it, so the funds disclaimer shows once.
// The SDK's flow decides all of that (BittensorKeyHost mirrors it); this draws
// it, and adds the privacy controls only this platform has.
//
// Linux has no general way to keep a window out of a screenshot or a
// recording, nor to tell that one is being made, so the screen promises and
// says nothing about either (no capture line, no screenshot warning): the
// words hide whenever this window isn't the focused one (another window, the
// polkit dialog, a switched-away session), and after the SDK's 60 seconds. A
// reveal that comes back from the polkit dialog while the window is unfocused
// waits for the window to be focused again.
//
// The words are plain labels, never selectable and never in a text field, and
// are in the widget tree only while they show. The two word fields ask the
// input method to learn nothing (Gtk::InputHints::PRIVATE) and offer no spell
// check, emoji or completion.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>

#include <gtkmm.h>

#include "BittensorKeyHost.hpp"

namespace urnw {

class BittensorKeyScreen : public Gtk::Window {
 public:
  BittensorKeyScreen(Gtk::Window& parent, BittensorKeyHost& host);
  ~BittensorKeyScreen() override;

  // The host's mirror changed: draw it.
  void Apply();

 private:
  void BuildMain(Gtk::Box& column);
  void BuildExplain(Gtk::Box& column);
  void BuildConfirm(Gtk::Box& column);
  void ApplyMain(const urnet::BittensorLocalKeyScreen& screen);
  void ApplyExplain(const urnet::BittensorLocalKeyScreen& screen);
  void ApplyConfirm(const urnet::BittensorLocalKeyScreen& screen);
  void ApplyCountdown();
  void ApplyWordCheck();
  void OnReveal();
  void OnCopy();
  void OnSubmit();
  // leaving: a phrase in memory is discarded; never while busy
  bool OnCloseRequest();

  BittensorKeyHost& host_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  // the app's own step: what deleting means, before the two words are asked
  bool deleteExplained_ = false;
  // the confirmation the fields were last cleared for
  std::string confirming_;
  // a reveal authenticated while the window wasn't focused
  std::optional<bool> revealWhenFocused_;
  bool authenticating_ = false;
  sigc::connection countdown_;
  sigc::connection copiedReset_;

  // the three steps, one shown at a time
  Gtk::Box* mainPage_ = nullptr;
  Gtk::Box* explainPage_ = nullptr;
  Gtk::Box* confirmPage_ = nullptr;

  // main
  Gtk::Box* busyBanner_ = nullptr;
  Gtk::Box* retryBanner_ = nullptr;
  Gtk::Label* retryText_ = nullptr;
  Gtk::Button* retryButton_ = nullptr;
  Gtk::Box* saveNowBanner_ = nullptr;
  Gtk::Label* failureLine_ = nullptr;
  Gtk::Label* wordsLabel_ = nullptr;
  Gtk::Button* cover_ = nullptr;
  Gtk::Image* coverIcon_ = nullptr;
  Gtk::Box* revealed_ = nullptr;
  Gtk::Grid* wordsGrid_ = nullptr;
  Gtk::Label* countdownLabel_ = nullptr;
  Gtk::Button* copyButton_ = nullptr;
  Gtk::Box* savedGroup_ = nullptr;
  Gtk::Button* keepButton_ = nullptr;
  Gtk::Button* deleteButton_ = nullptr;
  Gtk::Box* mainDisclaimer_ = nullptr;
  Gtk::Button* closeButton_ = nullptr;

  // what deleting means
  Gtk::Label* explainTitle_ = nullptr;
  Gtk::Box* bullets_ = nullptr;
  Gtk::Box* top200Box_ = nullptr;
  Gtk::Box* explainDisclaimer_ = nullptr;

  // the two words
  Gtk::Label* confirmTitle_ = nullptr;
  Gtk::Label* confirmBody_ = nullptr;
  Gtk::Label* confirmPrompt_ = nullptr;
  std::array<Gtk::Label*, 2> wordTitles_{};
  std::array<Gtk::Entry*, 2> wordFields_{};
  std::array<Gtk::Label*, 2> wordWrong_{};
  Gtk::Label* rejectedLine_ = nullptr;
  Gtk::Label* confirmFailure_ = nullptr;
  Gtk::Box* confirmDisclaimer_ = nullptr;
  Gtk::Button* confirmButton_ = nullptr;
  Gtk::Label* confirmButtonLabel_ = nullptr;
  Gtk::Image* confirmButtonIcon_ = nullptr;
  Gtk::Spinner* confirmSpinner_ = nullptr;
  Gtk::Button* confirmCancel_ = nullptr;
};

}  // namespace urnw
