// The points leaderboard's emoji tag editor (android/POINTSLEADERBOARD.md and
// its amendment). The tag is composed on an EMOJI-ONLY keyboard: the draft is
// a read-only line the keys below append to, a backspace removes one emoji at
// a time (one grapheme, never half a sequence), and there is no entry, so
// nothing but emoji can ever be typed or pasted. A network with no tag starts
// from the SDK's random 1-3 emoji suggestion and the shuffle key re-rolls it;
// a suggestion is only a draft until Save. Every change is validated by the
// SDK exactly the way the server validates it (validateEmojiTag), so the
// counter and the Save button always say what the server would say.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <gtkmm.h>

namespace urnw {

class EmojiTagSheet : public Gtk::Window {
 public:
  // `saver(tag, done)` stores the tag (an empty tag clears it) and calls
  // done(error) ON THE MAIN LOOP; an empty error closes the sheet, anything
  // else is shown under the draft and the sheet stays open.
  using Saver = std::function<void(std::string tag, std::function<void(std::string)> done)>;

  EmojiTagSheet(Gtk::Window& parent, Saver saver);
  ~EmojiTagSheet() override;

  // Opens the editor on `currentTag` (empty: the SDK's suggestion).
  void Open(const std::string& currentTag);

 private:
  void BuildUi();
  void BuildKeyboard(Gtk::Box& into);
  void ShowGroup(size_t index);
  void Append(const std::string& emoji);
  void DropLast();
  void Shuffle();
  // re-validates the draft through the SDK and renders the line, the counter
  // or the error, the keys and the buttons from the verdict
  void ApplyDraft();
  void Submit(const std::string& tag);
  void ApplyResult(const std::string& error);

  Saver saver_;
  std::string currentTag_;
  std::vector<std::string> draft_;  // one element per emoji
  std::string normalized_;          // the SDK's form of a valid draft
  bool saving_ = false;
  bool full_ = false;  // the draft holds the maximum: keys are disabled
  size_t group_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  Gtk::Label* draftLabel_ = nullptr;
  Gtk::Label* supportLabel_ = nullptr;
  Gtk::Button* backspaceButton_ = nullptr;
  Gtk::Button* shuffleButton_ = nullptr;
  Gtk::Box* groupStrip_ = nullptr;
  Gtk::FlowBox* keys_ = nullptr;
  Gtk::Button* clearButton_ = nullptr;
  Gtk::Button* saveButton_ = nullptr;
  Gtk::Spinner* spinner_ = nullptr;
  std::vector<Gtk::ToggleButton*> groupButtons_;
  std::vector<Gtk::Button*> keyButtons_;
};

}  // namespace urnw
