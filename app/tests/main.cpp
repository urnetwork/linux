// Unit-test runner for the app's pure-logic modules. Builds and runs with a
// bare C++17 compiler -- no GTK, no libadwaita, no SDK -- so the projection
// math, the TopoJSON decoder and the location-override state machine stay
// verifiable on any developer machine.
//
//   meson test -C build            (registered in meson.build)
//   or compile tests/*.cpp + src/{GlobeGeometry,WorldTopology,
//   LocationOverrideState}.cpp directly.
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace urnw {
namespace testing {

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

std::vector<std::string>& CurrentFailures() {
  static std::vector<std::string> failures;
  return failures;
}

}  // namespace testing
}  // namespace urnw

int main() {
  int passed = 0;
  int failed = 0;
  for (const auto& test : urnw::testing::Registry()) {
    urnw::testing::CurrentFailures().clear();
    test.fn();
    if (urnw::testing::CurrentFailures().empty()) {
      ++passed;
      std::printf("ok    %s\n", test.name);
    } else {
      ++failed;
      std::printf("FAIL  %s\n", test.name);
      for (const auto& failure : urnw::testing::CurrentFailures()) {
        std::printf("        %s\n", failure.c_str());
      }
    }
  }
  std::printf("\n%d passed, %d failed, %d total\n", passed, failed, passed + failed);
  return failed == 0 ? 0 : 1;
}
