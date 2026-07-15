#pragma once
//
// FallbackMoment.h — where and when the Almanac's sky lives if the clock has
// never been set.
//
// The X4 loses the time on deep sleep. Rather than a dead "time not set"
// screen, the sky modules fall back to one fixed moment and place:
//
//     45.3114 N, 75.9046 W — September 23rd, 2023, 4:28 PM (EDT, UTC-4)
//
// The choice of moment is the author's own. Modules using it say so on
// screen ("clock not set"), so a fallback sky is never mistaken for tonight.
//
#include <ctime>

#include "TimeSource.h"

namespace fallback_moment {
constexpr double LAT = 45.3114;
constexpr double LON = -75.9046;

inline time_t utc() {
  // 16:28 EDT = 20:28 UTC
  return TimeSource::epochFromUtcParts(2023, 9, 23, 20, 28);
}
}  // namespace fallback_moment
