// SPDX-License-Identifier: MPL-2.0
#include "WorldTopology.hpp"

#include <cstddef>

#include <nlohmann/json.hpp>

namespace urnw {
namespace {

using nlohmann::json;

// Concatenates the referenced arcs into one closed ring. A negative index i
// references arc ~i reversed. After orientation, each arc's first point equals
// the previous arc's last point, so the duplicate is dropped when stitching.
std::vector<float> StitchRing(const json& arcIndexes,
                              const std::vector<std::vector<float>>& arcs) {
  size_t pointCount = 0;
  for (const auto& element : arcIndexes) {
    const int index = element.get<int>();
    pointCount += arcs.at(static_cast<size_t>(index >= 0 ? index : ~index)).size() / 2;
  }
  pointCount -= arcIndexes.size() - 1;

  std::vector<float> ring(pointCount * 2);
  size_t write = 0;
  for (size_t k = 0; k < arcIndexes.size(); ++k) {
    const int index = arcIndexes[k].get<int>();
    const bool skipSharedEndpoint = 0 < k;
    if (index >= 0) {
      const std::vector<float>& arc = arcs.at(static_cast<size_t>(index));
      const size_t from = skipSharedEndpoint ? 1 : 0;
      for (size_t p = from; p < arc.size() / 2; ++p) {
        ring[write++] = arc[2 * p];
        ring[write++] = arc[2 * p + 1];
      }
    } else {
      const std::vector<float>& arc = arcs.at(static_cast<size_t>(~index));
      const size_t last = arc.size() / 2 - 1;
      // reversed: start one point in when the shared endpoint is already written
      size_t p = skipSharedEndpoint ? last - 1 : last;
      while (true) {
        ring[write++] = arc[2 * p];
        ring[write++] = arc[2 * p + 1];
        if (p == 0) break;
        --p;
      }
    }
  }
  return ring;
}

}  // namespace

bool WorldTopology::Decode(const std::string& jsonText, WorldTopology& out) {
  json root = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded() || !root.is_object()) return false;

  try {
    const json& transform = root.at("transform");
    const json& scale = transform.at("scale");
    const json& translate = transform.at("translate");
    const double scaleX = scale.at(0).get<double>();
    const double scaleY = scale.at(1).get<double>();
    const double translateX = translate.at(0).get<double>();
    const double translateY = translate.at(1).get<double>();

    // Decode every arc once. Each arc is a list of [x, y] integer points where
    // the first point is absolute (quantized) and every later point is a delta;
    // the running sums dequantize to degrees as lon = x * scale[0] +
    // translate[0], lat = y * scale[1] + translate[1]. Packed as
    // [lon0, lat0, lon1, lat1, ...].
    const json& arcsJson = root.at("arcs");
    std::vector<std::vector<float>> arcs;
    arcs.reserve(arcsJson.size());
    for (const auto& arcJson : arcsJson) {
      std::vector<float> points(arcJson.size() * 2);
      long long x = 0;
      long long y = 0;
      for (size_t j = 0; j < arcJson.size(); ++j) {
        const json& point = arcJson[j];
        x += point.at(0).get<long long>();
        y += point.at(1).get<long long>();
        points[2 * j] = static_cast<float>(x * scaleX + translateX);
        points[2 * j + 1] = static_cast<float>(y * scaleY + translateY);
      }
      arcs.push_back(std::move(points));
    }

    const json& geometries = root.at("objects").at("countries").at("geometries");
    std::vector<CountryShape> countries;
    countries.reserve(geometries.size());
    for (const auto& geometry : geometries) {
      CountryShape shape;
      const auto id = geometry.find("id");
      if (id != geometry.end()) {
        // ids are strings in this asset, but tolerate numbers
        shape.isoNumeric = id->is_string() ? id->get<std::string>() : id->dump();
      }
      const json& arcIndexes = geometry.at("arcs");
      const std::string type = geometry.at("type").get<std::string>();
      if (type == "Polygon") {
        // a Polygon is a list of rings, each a list of arc indexes
        for (const auto& ring : arcIndexes) shape.rings.push_back(StitchRing(ring, arcs));
      } else if (type == "MultiPolygon") {
        // a MultiPolygon is a list of polygons
        for (const auto& polygon : arcIndexes) {
          for (const auto& ring : polygon) shape.rings.push_back(StitchRing(ring, arcs));
        }
      } else {
        return false;  // unsupported geometry type
      }
      countries.push_back(std::move(shape));
    }
    out.countries = std::move(countries);
    return true;
  } catch (const json::exception&) {
    return false;
  } catch (const std::out_of_range&) {
    return false;
  }
}

}  // namespace urnw
