// SPDX-License-Identifier: MPL-2.0
#include "LocationOverrideState.hpp"

#include <iomanip>
#include <locale>
#include <sstream>

namespace urnw {

LocationOverrideStatus ResolveLocationOverrideStatus(bool enabled, bool geoclueInstalled,
                                                     bool staticSourceEnabled,
                                                     bool writerAvailable, bool tunnelUp,
                                                     bool hasTarget, bool orphaned) {
  if (orphaned) {
    return LocationOverrideStatus::Orphaned;
  }
  if (!enabled) {
    return LocationOverrideStatus::Disabled;
  }
  if (!geoclueInstalled) {
    return LocationOverrideStatus::NeedsGeoClue;
  }
  if (!staticSourceEnabled) {
    return LocationOverrideStatus::NeedsStaticSource;
  }
  if (!writerAvailable) {
    return LocationOverrideStatus::NeedsPrivilege;
  }
  return (tunnelUp && hasTarget) ? LocationOverrideStatus::Active
                                 : LocationOverrideStatus::Eligible;
}

// ---- /etc/geolocation file contents -----------------------------------------

const char* const kGeolocationMarker =
    "# Written by URnetwork -- device location synced to the connected provider.";

namespace {

// C-locale fixed-point formatting. std::to_string and printf %f both honour
// the global locale, where a comma decimal separator (fr_FR, de_DE, ...)
// would make GeoClue reject the line; an explicitly classic-imbued stream
// cannot.
std::string FormatFixed(double value, int precision) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

}  // namespace

std::string RenderGeolocationFileContents(double lat, double lon, double altitudeMeters,
                                          double accuracyMeters) {
  std::string contents = kGeolocationMarker;
  contents +=
      "\n"
      "# Remove this file (or turn the setting off in URnetwork) to restore the\n"
      "# real location. Format: latitude, longitude, altitude (m), accuracy (m).\n";
  contents += FormatFixed(lat, 6) + "\n";
  contents += FormatFixed(lon, 6) + "\n";
  contents += FormatFixed(altitudeMeters, 1) + "\n";
  contents += FormatFixed(accuracyMeters, 1) + "\n";
  return contents;
}

bool GeolocationContentsAreOurs(const std::string& contents) {
  return contents.compare(0, std::string(kGeolocationMarker).size(), kGeolocationMarker) == 0;
}

}  // namespace urnw
