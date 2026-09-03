#pragma once

#include <cstdint>

#include "Ui.hpp"

// The provide indicator (apple/android parity), from the LIVE effective provide
// mode: "●" solid dot = Network tier (also Auto while idle), "◉" dot with an
// outer ring = Public tier (amber while paused — pause stops public only),
// coral = not providing. ProvideMode is a bit set (0 none, 1 network, 2
// friends-and-family, 3 public) — per-case only. One rule shared by the connect
// page's provide row and the earnings page's provide-mode row, so they never
// disagree.
struct ProvideModeGlyph {
  const char* glyph;
  urnw::Rgba color;
};

inline ProvideModeGlyph ProvideModeGlyphFor(int64_t provideMode, bool paused) {
  switch (provideMode) {
    case 3:  // public
      return {"◉", paused ? urnw::kUrAmber : urnw::kUrGreen};
    case 1:  // network (also Auto while idle)
    case 2:  // friends-and-family
      return {"●", urnw::kUrGreen};
    default:
      return {"●", urnw::kUrCoral};
  }
}
