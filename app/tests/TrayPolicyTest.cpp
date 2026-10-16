// The window and the tray (TrayPolicy.hpp): closing hides to the tray only
// while a tray has taken the icon and minimizes otherwise, and a hidden window
// with no tray shows again, minimized, once the tray's grace has passed. The
// first hide to the tray says so once ever. The tray and the window need gio
// and gtkmm, so the wiring cases read Tray.cpp, main.cpp and MainWindow.cpp
// with the comments blanked.
// SPDX-License-Identifier: MPL-2.0
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "TestHarness.hpp"
#include "TrayPolicy.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

using urnw::tray_policy::CloseAction;

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadTrayPolicySource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  bool inString = false;
  for (size_t at = 0; at < text.size(); ++at) {
    const char c = text[at];
    if (inString) {
      if (c == '\\') {
        ++at;
      } else if (c == '"' || c == '\n') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
      while (at < text.size() && text[at] != '\n') text[at++] = ' ';
    }
  }
  return text;
}

std::string TrayPolicyBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The text from `from` to the first `to` after it, or nothing.
std::string TrayPolicySpan(const std::string& text, const std::string& from, const std::string& to) {
  const size_t start = text.find(from);
  if (start == std::string::npos) return std::string();
  const size_t end = text.find(to, start + from.size());
  return end == std::string::npos ? std::string() : text.substr(start, end - start);
}

bool TrayPolicyHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool TrayPolicyInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

}  // namespace

UR_TEST(TrayPolicy_CloseHidesOnlyToATray) {
  UR_EXPECT_TRUE(urnw::tray_policy::OnClose(/*trayAvailable=*/true) == CloseAction::Hide);
  UR_EXPECT_TRUE(urnw::tray_policy::OnClose(/*trayAvailable=*/false) == CloseAction::Minimize);
}

// Hidden with no tray is the one state with no way back.
UR_TEST(TrayPolicy_OnlyAHiddenWindowWithNoTraySurfaces) {
  UR_EXPECT_TRUE(urnw::tray_policy::MustSurface(/*windowVisible=*/false, /*trayAvailable=*/false));
  UR_EXPECT_FALSE(urnw::tray_policy::MustSurface(false, true));
  UR_EXPECT_FALSE(urnw::tray_policy::MustSurface(true, false));
  UR_EXPECT_FALSE(urnw::tray_policy::MustSurface(true, true));
  // long enough for a tray that starts after the autostart entries or restarts
  UR_EXPECT_TRUE(urnw::tray_policy::kNoTrayGraceMillis >= 3000);
}

// The tray is available only once a watcher took the icon and says a host
// draws it: the registration has a reply, then the watcher's
// IsStatusNotifierHostRegistered is read, and read again whenever a host comes
// or goes. Every new watcher is told again, and a vanished one takes the
// availability with it. A reply after the tray is gone touches nothing.
UR_TEST(TrayPolicyWiring_TheTrayIsAvailableOnlyOnTheWatchersReply) {
  const std::string tray = ReadTrayPolicySource("Tray.cpp");
  const std::string ctor = TrayPolicyBody(tray, "Tray::Tray() {");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      ctor, {"g_bus_own_name(", "OnItemNamed();", "g_bus_watch_name(", "kWatcherName",
             "OnWatcherAppeared();", "OnWatcherVanished();"}));
  const std::string reg = TrayPolicyBody(tray, "void Tray::RegisterWithWatcher() {");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      reg, {"if (!conn_ || !item_named_ || !watcher_present_) return;",
            "\"RegisterStatusNotifierItem\"", "cancellable_, &Tray::OnRegistered, this);"}));
  const std::string reply = TrayPolicyBody(
      tray, "void Tray::OnRegistered(GObject* source, GAsyncResult* result, gpointer self) {");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      reply, {"G_IO_ERROR_CANCELLED", "return;", "static_cast<Tray*>(self)",
              "SetRegistered(false);", "if (!tray->watcher_present_) return;",
              "tray->SetRegistered(true);", "tray->ReadHostRegistered();"}));
  // the host: the property read, its reply, and every signal that it changed
  UR_EXPECT_TRUE(TrayPolicyHas(
      tray, "kHostRegisteredProperty = \"IsStatusNotifierHostRegistered\";"));
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      TrayPolicyBody(tray, "void Tray::ReadHostRegistered() {"),
      {"if (!conn_ || !watcher_present_) return;", "\"org.freedesktop.DBus.Properties\"",
       "\"Get\"", "kWatcherName, kHostRegisteredProperty", "cancellable_",
       "&Tray::OnHostRegisteredRead, this);"}));
  const std::string hostReply = TrayPolicyBody(
      tray, "void Tray::OnHostRegisteredRead(GObject* source, GAsyncResult* result, gpointer self) {");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      hostReply, {"G_IO_ERROR_CANCELLED", "return;", "bool hostRegistered = false;",
                  "G_VARIANT_TYPE_BOOLEAN", "hostRegistered = g_variant_get_boolean(value);",
                  "if (!tray->watcher_present_) return;",
                  "tray->SetHostRegistered(hostRegistered);"}));
  const std::string bus = TrayPolicyBody(tray, "void Tray::OnBusAcquired(GDBusConnection* conn) {");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      bus, {"ReadHostRegistered();", "\"StatusNotifierHostRegistered\"", "onHostSignal",
            "\"StatusNotifierHostUnregistered\"", "onHostSignal", "\"PropertiesChanged\"",
            "kWatcherName, G_DBUS_SIGNAL_FLAGS_NONE, onHostSignal"}));
  UR_EXPECT_TRUE(TrayPolicyInOrder(TrayPolicyBody(tray, "void Tray::OnWatcherAppeared() {"),
                                   {"watcher_present_ = true;", "RegisterWithWatcher();"}));
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      TrayPolicyBody(tray, "void Tray::OnWatcherVanished() {"),
      {"watcher_present_ = false;", "SetRegistered(false);", "SetHostRegistered(false);"}));
  UR_EXPECT_TRUE(TrayPolicyInOrder(TrayPolicyBody(tray, "void Tray::SetRegistered(bool registered) {"),
                                   {"const bool wasAvailable = Available();",
                                    "registered_ = registered;", "NoteAvailability(wasAvailable);"}));
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      TrayPolicyBody(tray, "void Tray::SetHostRegistered(bool hostRegistered) {"),
      {"const bool wasAvailable = Available();", "host_registered_ = hostRegistered;",
       "NoteAvailability(wasAvailable);"}));
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      TrayPolicyBody(tray, "void Tray::NoteAvailability(bool wasAvailable) {"),
      {"const bool available = Available();", "if (available == wasAvailable) return;",
       "on_availability_change(available);"}));
  UR_EXPECT_TRUE(TrayPolicyHas(ReadTrayPolicySource("Tray.hpp"),
                               "bool Available() const { return registered_ && host_registered_; }"));
  UR_EXPECT_TRUE(TrayPolicyInOrder(TrayPolicyBody(tray, "Tray::~Tray() {"),
                                   {"g_cancellable_cancel(cancellable_);", "g_bus_unwatch_name(",
                                    "g_dbus_connection_signal_unsubscribe(conn_, sub);"}));
  // the name's own paths register through the gate, never around it
  UR_EXPECT_TRUE(TrayPolicyInOrder(TrayPolicyBody(tray, "void Tray::OnNameLost(GDBusConnection* conn) {"),
                                   {"service_name_ = unique;", "OnItemNamed();"}));
}

// Close hides only to an available tray, dropping any earlier minimize so the
// tray shows the window again unminimized, the availability's every change and
// the end of startup check that a hidden window can come back, and the grace
// shows it minimized only if it still cannot.
UR_TEST(TrayPolicyWiring_TheWindowHidesOnlyToATrayAndNeverStaysUnreachable) {
  const std::string main = ReadTrayPolicySource("main.cpp");
  const std::string close =
      TrayPolicySpan(main, "window->signal_close_request().connect(", "return true;");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      close, {"urnw::tray_policy::OnClose(tray && tray->Available())", "CloseAction::Hide:",
              "window->set_visible(false);", "window->unminimize();", "CloseAction::Minimize:",
              "window->minimize();"}));
  UR_EXPECT_TRUE(TrayPolicyHas(main, "tray->on_availability_change = [&](bool) { keepReachable(); };"));
  const std::string startup =
      TrayPolicySpan(main, "app->signal_startup().connect(", "app->signal_activate().connect(");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      startup, {"OpenLaunches(G_APPLICATION(app->gobj()), serve);", "keepReachable();"}));
  const std::string keep = TrayPolicySpan(main, "const auto keepReachable = [&] {", "\n  };\n");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      keep, {"noTrayGrace.disconnect();", "if (!unreachable()) return;",
             "Glib::signal_timeout().connect(", "if (unreachable()) {", "window->minimize();",
             "window->set_visible(true);", "urnw::tray_policy::kNoTrayGraceMillis"}));
  UR_EXPECT_TRUE(TrayPolicyHas(
      main, "urnw::tray_policy::MustSurface(window->get_visible(), tray && tray->Available())"));
}

// The first hide to the tray says the app still runs there: only from the
// close's hide branch, once ever, recorded before the send, so neither Quit nor
// a close with no tray (it minimizes) can show it, and nothing shows it twice.
UR_TEST(TrayPolicyWiring_OnlyTheFirstHideToTheTraySaysSo) {
  UR_EXPECT_TRUE(std::string(urnw::tray_policy::kHideNoticeSeenKey) == "onb_tray_balloon_seen");
  const std::string main = ReadTrayPolicySource("main.cpp");
  const std::string close =
      TrayPolicySpan(main, "window->signal_close_request().connect(", "return true;");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      close, {"CloseAction::Hide:", "window->set_visible(false);", "window->NoteHiddenToTray();",
              "break;", "CloseAction::Minimize:"}));
  size_t calls = 0;
  for (size_t at = main.find("NoteHiddenToTray"); at != std::string::npos;
       at = main.find("NoteHiddenToTray", at + 1)) {
    ++calls;
  }
  UR_EXPECT_EQ(1, calls);
  const std::string quit = TrayPolicySpan(main, "tray->on_quit = [&] {", "\n    };\n");
  UR_EXPECT_TRUE(!quit.empty());
  UR_EXPECT_FALSE(TrayPolicyHas(quit, "NoteHiddenToTray"));
  const std::string window = ReadTrayPolicySource("MainWindow.cpp");
  const std::string notice = TrayPolicyBody(window, "void MainWindow::NoteHiddenToTray() {");
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      notice, {"if (prefs::Get<bool>(tray_policy::kHideNoticeSeenKey, false)) return;",
               "auto app = NotifyingApp();", "prefs::Set(tray_policy::kHideNoticeSeenKey, true);",
               "Gio::Notification::create(\"URnetwork\")", "T_(\"onb_tray_balloon_hide\"",
               "app->send_notification(kHideNoticeId, notification);"}));
  // the window was just hidden: gtkmm has removed it from its application
  UR_EXPECT_FALSE(TrayPolicyHas(notice, "get_application()"));
}

// With no status (signed out) the menu offers neither Connect nor Disconnect
// (apple e46555b7); with one, the word follows the session.
UR_TEST(TrayPolicy_NoConnectItemWithoutAStatus) {
  using urnw::tray_policy::ConnectItem;
  using urnw::tray_policy::ConnectItemFor;
  UR_EXPECT_TRUE(ConnectItemFor(/*hasStatus=*/false, /*sessionUp=*/false) == ConnectItem::None);
  UR_EXPECT_TRUE(ConnectItemFor(/*hasStatus=*/false, /*sessionUp=*/true) == ConnectItem::None);
  UR_EXPECT_TRUE(ConnectItemFor(/*hasStatus=*/true, /*sessionUp=*/false) == ConnectItem::Connect);
  UR_EXPECT_TRUE(ConnectItemFor(/*hasStatus=*/true, /*sessionUp=*/true) == ConnectItem::Disconnect);
}

// The menu builds the item, and takes its click, only while it is offered;
// the window tells the tray whether there is a status (signed in), and its
// own toggle does nothing without one. Before, the item was always there, and
// signed out its Connect ran a start that could only be refused.
UR_TEST(TrayPolicyWiring_TheConnectItemNeedsAStatus) {
  const std::string tray = ReadTrayPolicySource("Tray.cpp");
  const std::string layout = TrayPolicySpan(tray, "if (g_strcmp0(method, \"GetLayout\") == 0) {",
                                            "BuildItem(kIdShow,");
  UR_EXPECT_TRUE(TrayPolicyInOrder(layout, {"if (ConnectItemOf(*self) != tray_policy::ConnectItem::None) {",
                                            "BuildItem(kIdConnect,"}));
  UR_EXPECT_TRUE(TrayPolicyHas(tray, "if (id == kIdConnect && ConnectItemOf(*self) != tray_policy::ConnectItem::None &&"));
  UR_EXPECT_TRUE(TrayPolicyInOrder(
      TrayPolicyBody(tray, "void Tray::SetState(bool sessionUp, bool proven, const std::string& status, bool hasStatus) {"),
      {"has_status_ = hasStatus;"}));
  const std::string app = ReadTrayPolicySource("main.cpp");
  UR_EXPECT_TRUE(TrayPolicyHas(app, "tray->SetState(sessionUp, proven, status, hasStatus);"));
  const std::string window = ReadTrayPolicySource("MainWindow.cpp");
  UR_EXPECT_TRUE(TrayPolicyHas(window, "on_tray_state(connected_, proven, status, signedIn_);"));
  UR_EXPECT_TRUE(TrayPolicyInOrder(TrayPolicyBody(window, "void MainWindow::ApplyAuthState(bool loggedIn) {"),
                                   {"signedIn_ = loggedIn;", "readingApplied_ = false;"}));
  UR_EXPECT_TRUE(TrayPolicyInOrder(TrayPolicyBody(window, "void MainWindow::ToggleConnect() {"),
                                   {"if (!signedIn_) return;", "ToggleConnect(connected_);"}));
}
