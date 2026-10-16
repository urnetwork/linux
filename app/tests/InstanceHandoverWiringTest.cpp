// The call sites of the handover (InstanceHandover.hpp): a launch is decided
// before GApplication registers and before anything touches the storage or the
// log directory, the instance serves launches only once its window and tray
// exist, every exit refuses launches before it stops anything, and autostart
// reaches the GUI as the launcher's flag. The logic is pure and runs in
// InstanceHandoverTest.cpp; main.cpp and SingleInstance.cpp need gtkmm and
// gio, so this reads their sources with the comments blanked, and the
// packaging files with their # comments blanked, so prose cannot satisfy a
// contract.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "InstanceHandover.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadHandoverFile(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// A C++ source with every // comment blanked; string literals are kept.
std::string ReadHandoverSource(const std::string& relative) {
  std::string text = ReadHandoverFile(relative);
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

// A shell script or desktop entry with every line that starts with # blanked.
std::string ReadHandoverConfig(const std::string& relative) {
  std::istringstream in(ReadHandoverFile(relative));
  std::string out;
  for (std::string line; std::getline(in, line);) {
    const size_t first = line.find_first_not_of(" \t");
    out += (first != std::string::npos && line[first] == '#') ? std::string() : line;
    out += '\n';
  }
  return out;
}

std::string HandoverBetween(const std::string& text, const std::string& start,
                            const std::string& end) {
  const size_t from = text.find(start);
  if (from == std::string::npos) return std::string();
  const size_t to = text.find(end, from + start.size());
  return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

bool HandoverHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Each needle occurs after the one before it.
bool HandoverInOrder(const std::string& text, const std::vector<std::string>& needles) {
  size_t from = 0;
  for (const std::string& needle : needles) {
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) return false;
    from = at + needle.size();
  }
  return true;
}

std::string MainBody() {
  return HandoverBetween(ReadHandoverSource("main.cpp"), "int main(int argc, char** argv) {",
                         "\n}\n");
}

}  // namespace

// The launch is decided before GApplication registers and before the SDK host
// is made or the storage and log directories are touched; a launch handed over
// exits there, and one that met an instance still closing exits with a
// failure. The option GApplication would refuse is registered, the launcher's
// flag is read once and unset, and nothing holds the application before run():
// a hold there kept every GApplication remote running forever.
UR_TEST(InstanceHandoverWiring_ALaunchIsDecidedBeforeGApplicationAndAHandoverExits) {
  const std::string main = MainBody();
  UR_EXPECT_TRUE(!main.empty());
  UR_EXPECT_TRUE(HandoverInOrder(
      main, {"Gtk::Application::create(urnw::instance::kBusName,",
             "app->add_main_option_entry(Gio::Application::OptionType::BOOL, \"autostart\"",
             "Glib::OptionEntry::Flags::HIDDEN);", "urnw::instance::ParseArguments(",
             "TakeAutostartFlag());", "if (arguments.handover) {",
             "urnw::instance::LaunchFor(G_APPLICATION(app->gobj()), arguments);",
             "urnw::instance::LaunchOnSessionBus(launch);",
             "case urnw::instance::Outcome::HandedOver:", "return 0;",
             "case urnw::instance::Outcome::StillClosing:",
             "case urnw::instance::Outcome::GaveUp:", "return 1;",
             "auto host = std::make_shared<urnw::SdkHost>();",
             "EnsureDir(Glib::get_user_data_dir(), \"urnetwork\");",
             "app->signal_startup().connect(", "const int status = app->run(argc, argv);"}));
  const std::string beforeStartup = main.substr(0, main.find("app->signal_startup().connect("));
  UR_EXPECT_TRUE(!HandoverHas(beforeStartup, "app->hold();"));
  UR_EXPECT_TRUE(!HandoverHas(beforeStartup, "host->Initialize("));
  const std::string flag = HandoverBetween(ReadHandoverSource("main.cpp"),
                                           "bool TakeAutostartFlag() {", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(
      flag, {"urnw::instance::IsAutostartFlag(g_getenv(urnw::instance::kAutostartEnvironment));",
             "g_unsetenv(urnw::instance::kAutostartEnvironment);", "return autostart;"}));
}

// The instance holds the application and serves launches only once its window
// and tray exist; a start that fails begins to exit, so what it held is
// refused rather than lost.
UR_TEST(InstanceHandoverWiring_TheInstanceOpensOnlyOnceItsWindowAndTrayExist) {
  const std::string startup =
      HandoverBetween(MainBody(), "app->signal_startup().connect([&] {", "\n  });\n");
  UR_EXPECT_TRUE(HandoverInOrder(
      startup, {"if (!host->Initialize(storageDir, logDir)) {", "urnw::instance::BeginExiting();",
                "app->quit();", "return;", "window = std::make_shared<urnw::MainWindow>(*host);",
                "app->add_window(*window);", "tray = std::make_shared<urnw::Tray>();",
                "app->hold();", "urnw::instance::OpenLaunches(G_APPLICATION(app->gobj()), serve);"}));
}

// Quit refuses launches before it waits for the queued commands and tears
// down (QuitSequence.hpp), and every end of the main loop does too, before
// GtkApplication's own shutdown can run the loop again (connected with after =
// false).
UR_TEST(InstanceHandoverWiring_QuitAndEveryEndOfTheLoopBeginExitingFirst) {
  const std::string main = MainBody();
  UR_EXPECT_TRUE(HandoverHas(main, "tray->on_quit = [&] { endApp("));
  const std::string quit = HandoverBetween(main, "const auto endApp = [&](bool teardown) {", "\n  };\n");
  UR_EXPECT_TRUE(HandoverInOrder(quit, {"if (!quit.Begin(teardown)) return;",
                                        "urnw::instance::BeginExiting();",
                                        "host->WhenCommandsDrained("}));
  const std::string step = HandoverBetween(main, "std::function<void(urnw::QuitSequence::Step)> applyQuitStep =",
                                           "\n      };\n");
  UR_EXPECT_TRUE(HandoverInOrder(step, {"host->ShutdownOffMainLoop(", "app->release();", "app->quit();"}));
  UR_EXPECT_TRUE(HandoverHas(
      main, "app->signal_shutdown().connect([] { urnw::instance::BeginExiting(); }, false);"));
}

// Every activation goes through one serve: links routed, the window shown
// unless the launch is the autostart entry's. The process's own launch is the
// first activation.
UR_TEST(InstanceHandoverWiring_EveryActivationIsServedByItsKind) {
  const std::string main = MainBody();
  UR_EXPECT_TRUE(HandoverInOrder(
      main, {"urnw::instance::Activations activations(arguments.kind);",
             "const auto serve = [&](const urnw::instance::Launch& launch) {",
             "for (const std::string& uri : launch.uris) host->HandleDeepLink(uri);",
             "if (window && urnw::instance::ShowsWindow(launch.kind)) window->present();"}));
  const std::string activate =
      HandoverBetween(main, "app->signal_activate().connect([&] {", "\n  });\n");
  UR_EXPECT_TRUE(HandoverInOrder(activate, {"launch.kind = activations.Next();", "serve(launch);"}));
  UR_EXPECT_TRUE(!HandoverHas(activate, "present("));
  const std::string open = HandoverBetween(main, "app->signal_open().connect(", "\n      });\n");
  UR_EXPECT_TRUE(HandoverInOrder(open, {"launch.kind = urnw::instance::LaunchKind::Link;",
                                        "launch.uris.push_back(f->get_uri());", "serve(launch);"}));
  UR_EXPECT_TRUE(!HandoverHas(open, "present("));
}

// The session bus side: the handover object is registered before the claim and
// dropped by a launch that does not start; the claim does not queue; the
// handover goes to the holder's own connection, bounded and never starting a
// service; a wait subscribes before it looks and runs on a context of its own;
// the exiting name is claimed before anything is refused, on a connection the
// process never lets go of; a served launch gets GApplication's before_emit and
// after_emit with its startup notification id; and no link is ever logged.
UR_TEST(InstanceHandoverWiring_TheSessionBusSideKeepsTheContract) {
  const std::string source = ReadHandoverSource("SingleInstance.cpp");
  const std::string launch =
      HandoverBetween(source, "Outcome LaunchOnSessionBus(const Launch& launch) {", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(
      launch, {"g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);",
               "g_dbus_connection_register_object(instance.bus, kObjectPath,",
               "RunLaunch(SessionBus(instance.bus), launch);", "if (outcome != Outcome::Start) {",
               "g_dbus_connection_unregister_object(instance.bus, instance.objectId);"}));
  UR_EXPECT_TRUE(HandoverHas(source, "constexpr guint32 kDoNotQueue = 4;"));
  UR_EXPECT_TRUE(HandoverHas(HandoverBetween(source, "Claim ClaimOn(", "\n}\n"),
                             "g_variant_new(\"(su)\", kBusName, kDoNotQueue)"));
  const std::string handover = HandoverBetween(source, "Handed HandoverOn(", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(handover, {"bus, holder.c_str(), kObjectPath, kInterface, \"Handover\",",
                                            "G_DBUS_CALL_FLAGS_NO_AUTO_START, budgetMillis"}));
  const std::string legacy = HandoverBetween(source, "Handed LegacyHandoverOn(", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(legacy, {"bus, holder.c_str(), kApplicationPath, \"org.freedesktop.Application\",",
                                          "open ? \"Open\" : \"Activate\"",
                                          "G_DBUS_CALL_FLAGS_NO_AUTO_START,", "budgetMillis"}));
  const std::string errors = HandoverBetween(source, "Handed HandedFromError(", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(
      errors, {"G_IO_ERROR_TIMED_OUT", "handed = Handed::TimedOut;", "G_DBUS_ERROR_SERVICE_UNKNOWN",
               "G_DBUS_ERROR_NO_REPLY", "handed = Handed::Gone;", "G_DBUS_ERROR_UNKNOWN_METHOD",
               "G_DBUS_ERROR_UNKNOWN_OBJECT", "handed = Handed::Unsupported;"}));
  const std::string wait = HandoverBetween(source, "Wait AwaitGoneOn(", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(
      wait, {"g_main_context_push_thread_default(context);", "g_dbus_connection_signal_subscribe(",
             "\"NameOwnerChanged\"", "OwnerOn(bus, name);", "g_timeout_source_new(",
             "g_main_context_iteration(context, TRUE);",
             "g_dbus_connection_signal_unsubscribe(bus, subscription);",
             "g_main_context_pop_thread_default(context);"}));
  const std::string exiting = HandoverBetween(source, "void BeginExiting() {", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(
      exiting, {"instance.gate.Close([&instance] {",
                "\"RequestName\", g_variant_new(\"(su)\", kExitingName, 0u)"}));
  UR_EXPECT_TRUE(HandoverHas(source, "static Instance* instance = new Instance();"));
  UR_EXPECT_TRUE(!HandoverHas(source, "g_object_unref(instance.bus)"));
  UR_EXPECT_TRUE(!HandoverHas(source, "g_dbus_connection_close("));
  const std::string open = HandoverBetween(source, "void OpenLaunches(", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(open, {"TheInstance().gate.Open(",
                                        "PlatformData(launch.activationToken)",
                                        "applicationClass->before_emit(application, platform);",
                                        "serve(launch);",
                                        "applicationClass->after_emit(application, platform);"}));
  const std::string launchFor = HandoverBetween(source, "Launch LaunchFor(", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(launchFor, {"g_file_new_for_commandline_arg(path.c_str());",
                                             "g_file_get_uri(file);",
                                             "->add_platform_data(application, &builder);",
                                             "\"activation-token\""}));
  const std::string call = HandoverBetween(source, "void OnHandoverCall(", "\n}\n");
  UR_EXPECT_TRUE(HandoverInOrder(call, {"g_variant_get(parameters, \"(&sas&s)\"",
                                        "TheInstance().gate.Take(std::move(launch),",
                                        "g_dbus_method_invocation_return_value(invocation"}));
  // the one log line of a handover names the kind, never a link or a token
  UR_EXPECT_TRUE(HandoverHas(
      call, "g_message(\"launch: a launch reached this instance (%s)\", ToWire(launch.kind));"));
  for (const char* leak : {"uri.c_str()", "launch.uris[", "activationToken.c_str()"}) {
    UR_EXPECT_TRUE_MSG(leak, !HandoverHas(HandoverBetween(call, "g_message(", ";"), leak));
  }
}

// Autostart: the template asks the launcher for it, the launcher hands the GUI
// the flag the GUI reads (never the option, which an older GUI refuses), and
// the menu entry stays a plain launch.
UR_TEST(InstanceHandoverWiring_AutostartReachesTheGuiAsTheLaunchersFlag) {
  const std::string autostart =
      ReadHandoverConfig("../packaging/autostart/com.bringyour.network.desktop");
  UR_EXPECT_TRUE(HandoverHas(autostart, "\nExec=urnetwork --autostart\n"));
  UR_EXPECT_TRUE(HandoverHas(autostart, "\nTryExec=urnetwork\n"));
  const std::string menu = ReadHandoverConfig("../packaging/com.bringyour.network.desktop");
  UR_EXPECT_TRUE(HandoverHas(menu, "\nExec=urnetwork %u\n"));
  UR_EXPECT_TRUE(!HandoverHas(menu, "autostart"));
  const std::string launcher = ReadHandoverConfig("../packaging/urnetwork-launcher");
  UR_EXPECT_TRUE(HandoverInOrder(
      launcher, {"if [ \"${1:-}\" = \"--autostart\" ]; then", "shift",
                 std::string(urnw::instance::kAutostartEnvironment) + "=1",
                 std::string("export ") + urnw::instance::kAutostartEnvironment, "else",
                 std::string("unset ") + urnw::instance::kAutostartEnvironment, "fi",
                 "candidate=\"\"", "exec \"${candidate}\" \"$@\""}));
  UR_EXPECT_TRUE(std::string(urnw::instance::kAutostartOption) == "--autostart");
}
