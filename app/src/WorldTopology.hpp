// The world map decoded from quantized TopoJSON (assets/world-110m.json, the
// same Natural Earth 110m file the android app ships). A port of the android
// WorldTopology.kt.
//
// TopoJSON stores shared borders once as delta-encoded quantized integer arcs;
// polygons reference arcs by index, with a negative index i meaning arc ~i
// traversed in reverse. See https://github.com/topojson/topojson.
//
// Only `objects.countries` is decoded; `land` and `bbox` are ignored.
//
// Toolkit-independent pure C++ (nlohmann_json only) so it builds and unit-tests
// standalone -- see tests/geometry_test.cpp.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

namespace urnw {

// One country from the world topology: its ISO-3166-1 numeric id (zero padded,
// e.g. "840" is the USA) and its outline rings in lon/lat degrees. Each ring is
// a packed vector of [lon0, lat0, lon1, lat1, ...] and is closed (first point
// equals last point). MultiPolygon countries contribute all of their rings,
// flattened.
struct CountryShape {
  std::string isoNumeric;
  std::vector<std::vector<float>> rings;
};

struct WorldTopology {
  std::vector<CountryShape> countries;

  // Decodes the TopoJSON document. Returns false (leaving `out` untouched) on
  // malformed input or an unsupported geometry type, so a missing/corrupt asset
  // degrades to a globe with no land rather than a crash.
  static bool Decode(const std::string& json, WorldTopology& out);
};

}  // namespace urnw
