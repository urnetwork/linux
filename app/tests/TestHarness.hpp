// A tiny assertion harness for the app's pure-logic unit tests (the globe
// projection math, the TopoJSON decoder and the location-override state
// machine). Deliberately dependency-free: these tests must build and run with
// nothing but a C++17 compiler, so they are runnable on a developer machine
// that cannot build GTK or link the SDK.
//
// Tests self-register with UR_TEST(name) { ... } and are run by tests/main.cpp.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace urnw {
namespace testing {

struct TestCase {
  const char* name;
  void (*fn)();
};

// The registry. Defined in tests/main.cpp.
std::vector<TestCase>& Registry();
// Failures recorded by the currently running case. Cleared per case.
std::vector<std::string>& CurrentFailures();

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back(TestCase{name, fn}); }
};

inline void Fail(const char* file, int line, const std::string& message) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), ":%d: ", line);
  CurrentFailures().push_back(std::string(file) + buf + message);
}

inline std::string Str(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

}  // namespace testing
}  // namespace urnw

#define UR_TEST_CAT2(a, b) a##b
#define UR_TEST_CAT(a, b) UR_TEST_CAT2(a, b)

// Defines and registers a test case.
#define UR_TEST(name)                                                             \
  static void name();                                                             \
  static ::urnw::testing::Registrar UR_TEST_CAT(g_reg_, name)(#name, &name);      \
  static void name()

#define UR_FAIL(message) ::urnw::testing::Fail(__FILE__, __LINE__, (message))

#define UR_EXPECT_TRUE(cond)                                    \
  do {                                                          \
    if (!(cond)) UR_FAIL("expected true: " #cond);              \
  } while (0)

#define UR_EXPECT_FALSE(cond)                                   \
  do {                                                          \
    if ((cond)) UR_FAIL("expected false: " #cond);              \
  } while (0)

#define UR_EXPECT_EQ(expected, actual)                                                       \
  do {                                                                                       \
    const auto ur_e_ = (expected);                                                            \
    const auto ur_a_ = (actual);                                                              \
    if (!(ur_e_ == ur_a_))                                                                    \
      UR_FAIL(std::string(#actual) + ": expected " + ::urnw::testing::Str(double(ur_e_)) +    \
              ", got " + ::urnw::testing::Str(double(ur_a_)));                                \
  } while (0)

// Floating point comparison with an absolute tolerance (the android tests'
// assertEquals(expected, actual, delta)).
#define UR_EXPECT_NEAR(expected, actual, tolerance)                                          \
  do {                                                                                       \
    const double ur_e_ = double(expected);                                                    \
    const double ur_a_ = double(actual);                                                      \
    const double ur_t_ = double(tolerance);                                                   \
    if (!(std::fabs(ur_e_ - ur_a_) <= ur_t_))                                                 \
      UR_FAIL(std::string(#actual) + ": expected " + ::urnw::testing::Str(ur_e_) + " +/- " +  \
              ::urnw::testing::Str(ur_t_) + ", got " + ::urnw::testing::Str(ur_a_));          \
  } while (0)

// Same, with a caller-supplied context string (the android assertEquals(message, ...)).
#define UR_EXPECT_NEAR_MSG(message, expected, actual, tolerance)                             \
  do {                                                                                       \
    const double ur_e_ = double(expected);                                                    \
    const double ur_a_ = double(actual);                                                      \
    const double ur_t_ = double(tolerance);                                                   \
    if (!(std::fabs(ur_e_ - ur_a_) <= ur_t_))                                                 \
      UR_FAIL(std::string(message) + ": " + #actual + ": expected " +                         \
              ::urnw::testing::Str(ur_e_) + " +/- " + ::urnw::testing::Str(ur_t_) + ", got " + \
              ::urnw::testing::Str(ur_a_));                                                    \
  } while (0)

#define UR_EXPECT_TRUE_MSG(message, cond)                                    \
  do {                                                                       \
    if (!(cond)) UR_FAIL(std::string(message) + ": expected true: " #cond);  \
  } while (0)
