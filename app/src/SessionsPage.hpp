// SessionsPage — Account -> Sessions (REVOKE-UI-FINAL.md): the account's
// signed-in sessions, reached from the Account pane's Sessions row and left
// through its own "‹ Account", like the Refer and earn page. One pane:
//
//   header  Sessions, progress while a refresh runs, refresh, "‹ Account"
//   notice  "Couldn't refresh. Showing the last list." over a list kept
//   body    one row per device sign-in, in the controller's order (current
//           first): the country circle with the device logo, three lines (the
//           device and version, with "This session"; the place and last use;
//           the sign-in date, method and short ID, with a copy button for the
//           full ID) and a flat destructive Sign out; Show more while there
//           are more. Then one group per kind of client (hosted proxies, API
//           clients, paired devices, other clients): a heading button with
//           its count and a chevron that opens and closes it, and while open
//           its rows (the same row, its group's label in place of the device)
//           and Show more. Or the screen's state line (progress, empty,
//           failed with Try again, unsupported, sign-in required)
//   bottom  Sign out all other sessions, what Last used means, and the note
//           on older sign-ins while coverage is partial
//
// Everything is the shared controller's (sdk ClientSessionViewController,
// opened on the account's Api in this process): the list and its layout into
// sections and pages, the 30 s polling while visible, the operations and
// their retries, the refusals.
// SessionsBinding.hpp carries the lifecycle and SessionsPresentation.hpp every
// decision; this file is the GTK. The controller's listener runs on an sdk
// thread and only posts to the main loop, where the post is dropped unless the
// page is alive and the controller it came from is still the one attached
// (alive_, and the binding's generation).
//
// Every sign-out is confirmed first in a modal whose default is Cancel (as
// AccountPage's ConfirmRemoveAuth). Signing out this app's own session is
// followed by the app's normal logout, which the Api reports
// (SdkHost::AdoptSpaceApiLocked) whether or not this page is still on screen.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "PaneKit.hpp"
#include "SdkHost.hpp"
#include "SessionsBinding.hpp"
#include "SessionsPresentation.hpp"

namespace urnw {

class GlyphDot;

class SessionsPage : public Gtk::Box {
 public:
  explicit SessionsPage(SdkHost& host);
  ~SessionsPage() override;

  // Nav-select and auth change: the controller on the account's Api (one per
  // Api: a replaced Api gets a new one) and its snapshot rendered. Signed out,
  // the no-session state, and no request.
  void Load();
  // The window presenting or hiding to the tray (MainWindow's
  // reconcilePresentation): the controller's foreground.
  void SetPresentationActive(bool active);
  // A sign-out: the controller is released, and the departed account's rows
  // and any open confirmation go with it.
  void ResetForSignOut();
  // --preview-ui: no controller; the page settles on its no-session state.
  void SetPreviewMode(bool on);
  void ShowPreviewState();

  std::function<void()> on_back;  // the header's "‹ Account"
  // The window's one-modal-at-a-time gate, both edges reported.
  std::function<bool()> sheet_open;
  std::function<void(bool open)> on_sheet_open_changed;
  std::function<void(const Glib::ustring& message, bool error)> on_snackbar;
  // This app's own session's sign-out was confirmed, just before it is sent.
  // The app signs out once it is done (REVOKE-UI-FINAL.md §4): the Api reports
  // it as a sign-out, or, while this device's client is of another session,
  // as the account's sign-in needed, which the window then follows with its
  // sign-out as well (MainWindow::OnAccountSignInRequired).
  std::function<void()> on_own_session_sign_out;

 private:
  // One session's row. Its callbacks name the session by ID, never by place.
  struct RowWidgets {
    std::string sessionId;
    Gtk::Widget* root = nullptr;
    GlyphDot* dot = nullptr;
    Gtk::Label* title = nullptr;
    Gtk::Label* tag = nullptr;  // "This session"
    Gtk::Label* lastUse = nullptr;
    Gtk::Label* signedIn = nullptr;
    Gtk::Box* status = nullptr;  // "Signing out…" or the failure, under the lines
    Gtk::Spinner* spinner = nullptr;
    Gtk::Label* statusText = nullptr;
    Gtk::Button* signOut = nullptr;
  };

  // One client group: its heading, its rows while open and their Show more.
  // Its callbacks name the group by kind, never by place.
  struct GroupWidgets {
    std::string kind;
    Gtk::Box* root = nullptr;
    Gtk::Button* header = nullptr;  // opens and closes the group
    Gtk::Label* heading = nullptr;  // "Hosted proxies (1,204)"
    Gtk::Image* chevron = nullptr;
    Gtk::Box* rowsBox = nullptr;
    std::vector<RowWidgets> rows;
    Gtk::Widget* moreRow = nullptr;
  };

  void BuildPane();
  bool CanCallApi();
  sessions::Words MakeWords() const;
  void EnsureController();
  void ReadSnapshot();
  void Render();
  // Reconciles one section's rows in `box` with `listed` (KeyedReconcile.hpp);
  // `section` is where they are listed (kGroupSignIns or a group's kind).
  void RenderRows(Gtk::Box& box, std::vector<RowWidgets>& rows,
                  const std::vector<sessions::Session>& listed, const std::string& section,
                  const sessions::Words& words, int64_t now);
  void ClearRows(Gtk::Box& box, std::vector<RowWidgets>& rows);
  RowWidgets BuildRow(const std::string& sessionId);
  void UpdateRow(RowWidgets& row, const sessions::Row& text, sessions::ActionView view);
  void RenderGroups(const sessions::Words& words, int64_t now);
  GroupWidgets BuildGroup(const std::string& kind);
  // `listed`: the group's rows while open (none while closed)
  void UpdateGroup(GroupWidgets& group, const sessions::GroupView& view,
                   const std::vector<sessions::Session>& listed, const sessions::Words& words,
                   int64_t now);
  void ClearGroups();
  void ToggleGroup(const std::string& kind);
  void RenderOthers(const sessions::Screen& screen);
  void PressSignOut(const sessions::Target& target);
  // `keyNeedsBackup`: this app's own session leaves an unsaved one-tap key
  // behind, and the confirmation says so
  void ShowConfirmation(const sessions::Confirmation& confirmation, bool keyNeedsBackup);
  void RestoreFocus(const sessions::Target& target);
  void Retry();
  void CopySessionId(const std::string& sessionId);
  void Snack(const Glib::ustring& message, bool error);

  Gtk::Window* RootWindow();
  bool BeginSheet(const char* what);
  void EndSheet();

  SdkHost& host_;
  // marshaled work's liveness: a post that lands after the page is gone does
  // nothing
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  sessions::Binding binding_;
  sessions::SignOutFlow flow_;
  sessions::Snapshot snapshot_;  // the controller's, as last read
  bool previewMode_ = false;
  bool sheetShowing_ = false;  // this page's half of the one-modal gate

  kit::Pane pane_;
  Gtk::Spinner* refreshSpinner_ = nullptr;
  Gtk::Button* refreshButton_ = nullptr;
  Gtk::Widget* noticeRow_ = nullptr;
  Gtk::Box* stateBox_ = nullptr;
  Gtk::Spinner* stateSpinner_ = nullptr;
  Gtk::Label* stateText_ = nullptr;
  Gtk::Button* retryButton_ = nullptr;
  Gtk::Box* rowsBox_ = nullptr;
  std::vector<RowWidgets> rows_;           // the device sign-ins
  Gtk::Widget* signInsMoreRow_ = nullptr;  // their Show more
  Gtk::Box* groupsBox_ = nullptr;
  std::vector<GroupWidgets> groups_;       // in the controller's order
  Gtk::Widget* othersRow_ = nullptr;
  Gtk::Button* othersButton_ = nullptr;
  Gtk::Box* othersStatus_ = nullptr;
  Gtk::Spinner* othersSpinner_ = nullptr;
  Gtk::Label* othersStatusText_ = nullptr;
  Gtk::Widget* helpRow_ = nullptr;
  Gtk::Widget* legacyRow_ = nullptr;
  std::unique_ptr<Gtk::Window> confirmDialog_;
};

}  // namespace urnw
