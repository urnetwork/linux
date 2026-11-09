// The egress witness's kernel log: the packets that stop a session can be named.
//
// Leg 3 of Tunnel::VerifyEgressWitness stops a session when urnw_probe_tun, the
// count of this daemon's marked packets that left through its own tun, is not
// zero. The count never said which packets those were, so a user whose session
// stopped could not say where they were going, and neither could we. The rule
// that feeds the counter now also logs, rate-limited and tagged, after the
// counter, and the stop says how to collect the lines. The counter must count
// exactly what it counted before: the rule without the log is pinned byte for
// byte, and the log may only follow it.
//
// Tunnel.cpp needs Linux headers, so its call sites are read as source.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cctype>
#include <string>

#include "Tunnel.hpp"
#include "TunnelPolicy.hpp"
#include "WiringSource.hpp"

using namespace urnw::testing::wiring;

namespace {

void ExpectText(const char* what, const std::string& expected, const std::string& actual) {
  if (expected != actual) {
    UR_FAIL(std::string(what) + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

// What BuildNftRuleset wrote for urnw_probe_tun before the log existed.
constexpr const char* kCountedRule =
    "meta mark 0x55524e57 oifname \"urnet0\" counter name \"urnw_probe_tun\"";

}  // namespace

// The rule with the log is the counted rule unchanged, then the limit, then the
// log: nft runs a rule's statements in order and `limit` ends the rule when it
// is over its rate, so whatever comes after it is rate-limited and the counter
// before it is not.
UR_TEST(EgressWitnessLog_TheTunRuleLogsAfterItsUnchangedCounter) {
  ExpectText("the rule without the log", kCountedRule,
             urnw::ProbeTunRule(urnw::kEgressMark, "urnet0", urnw::kNftProbeTunCounter, false));
  const std::string logged =
      urnw::ProbeTunRule(urnw::kEgressMark, "urnet0", urnw::kNftProbeTunCounter, true);
  ExpectText("the rule with the log",
             std::string(kCountedRule) +
                 " limit rate 6/minute burst 6 packets log prefix \"urnetwork-egress-witness: \" "
                 "level warn",
             logged);
  UR_EXPECT_TRUE_MSG("the counted rule is the logged rule's start",
                     logged.rfind(kCountedRule, 0) == 0);
  UR_EXPECT_TRUE_MSG("the counter comes before the limit, and the limit before the log",
                     Before(logged, "counter name", " limit rate ") &&
                         Before(logged, " limit rate ", " log prefix "));
  UR_EXPECT_EQ(static_cast<size_t>(1), CountOf(logged, "counter"));
  UR_EXPECT_EQ(static_cast<size_t>(1), CountOf(logged, " log "));
  // No verdict: urnw_probe still decides the fate of no packet.
  for (const char* verdict :
       {" accept", " drop", " reject", " queue", " jump", " goto", " return"}) {
    UR_EXPECT_TRUE_MSG(verdict, !Contains(logged, verdict));
  }
}

// The tag goes into a quoted nft string, a grep pattern and a 127-byte nft log
// prefix (with ": "). The witness's aim guard matches the tun's name and the
// mark anywhere in urnw_probe, so the tag must contain neither.
UR_TEST(EgressWitnessLog_TheTagIsPlainAndCannotAimTheWitness) {
  const std::string tag = urnw::kWitnessLogTag;
  UR_EXPECT_TRUE(!tag.empty() && tag.size() + 2 <= 127);
  for (const char c : tag) {
    UR_EXPECT_TRUE_MSG(tag, std::islower(static_cast<unsigned char>(c)) != 0 || c == '-');
  }
  UR_EXPECT_FALSE(Contains(tag, urnw::TunnelConfig().name));
  UR_EXPECT_FALSE(Contains(tag, urnw::MarkText(urnw::kEgressMark)));
  UR_EXPECT_FALSE(Contains(tag, std::to_string(urnw::kEgressMark)));
}

// Every ruleset with a tun logs unless NetFilter::Apply had to drop the log.
UR_TEST(EgressWitnessLog_BuildNftRulesetLogsThroughTheOneTunRule) {
  UR_EXPECT_TRUE_MSG("the log is on by default", urnw::FilterConfig().witness_log);
  const std::string build = FunctionBody(ReadCode("Tunnel.cpp"),
                                         "std::string BuildNftRuleset(const FilterConfig& cfg) {");
  if (build.empty()) {
    UR_FAIL("could not read BuildNftRuleset in Tunnel.cpp");
    return;
  }
  const std::string call =
      "line(\"\\t\\t\" + ProbeTunRule(cfg.mark, cfg.tun_name, kNftProbeTunCounter, "
      "cfg.witness_log));";
  UR_EXPECT_TRUE_MSG(call, Contains(build, call));
  UR_EXPECT_EQ(static_cast<size_t>(1), CountOf(build, "ProbeTunRule("));
  UR_EXPECT_TRUE_MSG("the rule is in urnw_probe",
                     Before(build, "kNftProbeChainName", "ProbeTunRule(") &&
                         Before(build, "ProbeTunRule(", "kNftOutChainName"));
  // The counter's declaration and that rule: nothing else feeds it.
  UR_EXPECT_EQ(static_cast<size_t>(2), CountOf(build, "kNftProbeTunCounter"));
  // No other rule logs, the physical leg's included.
  UR_EXPECT_FALSE(Contains(build, "log prefix"));
  UR_EXPECT_FALSE(Contains(build, "kWitnessLogTag"));
}

// A kernel without nf_log or nft_limit refuses the whole transaction. The log
// is dropped first, before the per-app exclusion, and never costs the floor.
// Only a ruleset with a tun carries it, so only that one is retried for it.
UR_TEST(EgressWitnessLog_ARefusedLoadRetriesWithoutTheLogFirst) {
  const std::string tunnel = ReadCode("Tunnel.cpp");
  const std::string logs =
      FunctionBody(tunnel, "bool RulesetLogsWitness(const FilterConfig& cfg) {");
  UR_EXPECT_TRUE_MSG(logs,
                     Contains(logs, "return cfg.witness_log && DeriveFilter(cfg).tun_named;"));
  UR_EXPECT_TRUE(Contains(FunctionBody(tunnel, "std::string BuildNftRuleset("),
                          "if (d.tun_named) {\n    line(std::string(\"\\tchain \") + "
                          "kNftProbeChainName + \" {\");"));
  const std::string apply = FunctionBody(
      tunnel, "bool NetFilter::Apply(const FilterConfig& requested, std::string* error) {");
  if (apply.empty()) {
    UR_FAIL("could not read NetFilter::Apply in Tunnel.cpp");
    return;
  }
  const char* const order[] = {
      "if (!ApplyScript(BuildNftRuleset(cfg), ToString(cfg.state), error)) {",
      "if (RulesetLogsWitness(cfg)) {",
      "cfg.witness_log = false;",
      "loaded = ApplyScript(BuildNftRuleset(cfg), ToString(cfg.state), error);",
      "if (!loaded) {",
      "if (cfg.exclude_cgroups.empty()) return false;",
      "cfg.exclude_cgroups.clear();",
      "if (!ApplyScript(BuildNftRuleset(cfg), ToString(cfg.state), error)) {",
      "if (error) error->clear();",
      "applied_ = cfg;",
  };
  size_t at = 0;
  for (const char* step : order) {
    const size_t next = apply.find(step, at);
    UR_EXPECT_TRUE_MSG(step, next != std::string::npos);
    if (next == std::string::npos) return;
    at = next + 1;
  }
  // The journal says whether the log is in force.
  UR_EXPECT_TRUE(Contains(apply, "excluded=%d witness_log=%d)\\n\""));
  UR_EXPECT_TRUE(Contains(apply, "RulesetLogsWitness(cfg) ? 1 : 0);"));
}

// The stop names the command while the log is in force, read from the same
// listing as the counter, and the app shows the stop's text as the daemon wrote it.
UR_TEST(EgressWitnessLog_TheStopSaysHowToCollectTheLines) {
  ExpectText("WitnessLogCommand", "sudo journalctl -k | grep urnetwork-egress-witness",
             urnw::WitnessLogCommand());
  const std::string hint = urnw::WitnessLogHint();
  UR_EXPECT_TRUE_MSG("the hint continues the reason", hint.rfind(". ", 0) == 0);
  UR_EXPECT_TRUE(Contains(hint, "`sudo journalctl -k | grep urnetwork-egress-witness`"));
  const std::string refusal = urnw::EgressRefusalMessage(
      "24 packet(s) this daemon sent have left through this daemon's own tunnel (urnet0)" + hint,
      "/usr/lib/urnetwork/urnetworkd");
  UR_EXPECT_TRUE_MSG(refusal,
                     Contains(refusal, "destination address and port. `/usr/lib/urnetwork/"
                                       "urnetworkd --diagnose` needs no root"));

  const std::string tunnel = ReadCode("Tunnel.cpp");
  const std::string read = FunctionBody(tunnel, "bool ReadMarkCounters(");
  UR_EXPECT_TRUE_MSG("ReadMarkCounters is read", !read.empty());
  UR_EXPECT_TRUE(Contains(read, "c.logged = body.find(kWitnessLogTag) != std::string::npos;"));
  UR_EXPECT_TRUE(Before(read, "ParseNamedCounter(listing, kNftProbeTunCounter", "c.logged = "));
  UR_EXPECT_TRUE(Before(read, "c.logged = ", "*out = c;"));

  const std::string witness = FunctionBody(tunnel, "bool Tunnel::VerifyEgressWitness(");
  const char* const legThree[] = {
      "if (counters.tun != 0) {",
      "MainRoutesThroughTun(ip, \"-6\")",
      "if (counters.logged) message += WitnessLogHint();",
      "return fail(kCodeEgressUnprotected, message);",
  };
  size_t at = 0;
  for (const char* step : legThree) {
    const size_t next = witness.find(step, at);
    UR_EXPECT_TRUE_MSG(step, next != std::string::npos);
    if (next == std::string::npos) return;
    at = next + 1;
  }
  UR_EXPECT_EQ(static_cast<size_t>(1), CountOf(witness, "WitnessLogHint()"));
  UR_EXPECT_TRUE(Contains(witness, "err->message = EgressRefusalMessage(message, SelfPath());"));

  // The re-check's stop carries the witness's message, and the window shows a
  // stop that is not the dead-tunnel failsafe's in the daemon's own words.
  const std::string check = FunctionBody(ReadCode("daemon/TunnelHost.cpp"),
                                         "bool TunnelHost::CheckEgressWitnessLocked() {");
  UR_EXPECT_TRUE(Contains(check, "\"carrying your traffic. (\" + witnessError.message + \")\","));
  UR_EXPECT_TRUE(Contains(ReadCode("MainWindow.cpp"), ": Glib::ustring(status->error);"));
}
