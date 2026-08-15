#pragma once
//
// TouchGestures.h — gestures shared by the orthographic-globe activities
// (Globe, Moon, Planetarium). ListLayout.h owns the list gestures; this owns
// the ones that act on a rendered sphere.
//
// Why a drag and not a tap: the reticle is FIXED at the centre of the disc and
// the body rotates underneath it, so there is no cursor to place. Dragging the
// surface is the direct manipulation the D-pad was approximating in SPIN_DEG
// steps. Inverting the orthographic projection to fly a tapped point to the
// reticle would need the view basis and would break down near the limb, where
// a pixel spans many degrees; a drag has no such singularity.
//
// On button-only builds MappedInputManager's touch calls are constexpr false,
// so every function here folds away to nothing.
//
#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"

// Apply a flick as a spin. The delta is scaled so one disc radius of travel is
// 90 degrees -- the same span the limb represents -- which makes the gesture
// feel one-to-one with the surface regardless of zoom or panel size.
//
// Sign convention matches the D-pad handlers: dragging left rolls the body west
// under the reticle, exactly as pressing Right does.
//
// Returns true if the view moved; the caller should refresh whatever it derives
// from the view (Globe re-resolves the country) and requestUpdate().
inline bool spinDragToView(const MappedInputManager& mappedInput, const int discRadius, float& viewLat,
                           float& viewLon, const float latLimit = 85.0f) {
  if (discRadius <= 0) return false;

  MappedInputManager::SwipeDir dir = MappedInputManager::SwipeDir::None;
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!mappedInput.wasSwipeWithPoints(dir, sx, sy, ex, ey)) return false;

  const float degPerPx = 90.0f / static_cast<float>(discRadius);
  viewLon -= static_cast<float>(ex - sx) * degPerPx;
  viewLat += static_cast<float>(ey - sy) * degPerPx;

  // Longitude wraps; latitude clamps, because past the pole the view basis
  // flips and the body appears to jump.
  while (viewLon > 180.0f) viewLon -= 360.0f;
  while (viewLon < -180.0f) viewLon += 360.0f;
  viewLat = std::min(std::max(viewLat, -latLimit), latLimit);
  return true;
}

// Hold anywhere: fires ONCE per contact.
//
// The hold-Back / hold-Confirm paths these activities use cannot fire on a
// device with no physical button: a tap on the on-screen hint synthesises a
// press and a release with no held state between them, so isPressed() never
// reports the hold. This uses the SDK long-press classifier instead.
//
// isScreenTouchLongPress() is a PREDICATE, not an event -- it stays true on
// every loop iteration once the contact passes the threshold. Calling an action
// straight off it runs that action at loop rate for as long as the finger is
// down, which for a cycling action (font size, a toggle) means the result is
// whatever it happened to land on when you lifted. The latch below fires on the
// first qualifying frame and re-arms only once the contact ends.
//
// The latch is a single static: only one activity loop runs at a time, and it
// re-arms on release, so a hold that switches activities cannot leak into the
// next one. Do not call this twice in one frame.
inline bool wasLongPressGesture(MappedInputManager& mappedInput, const unsigned long thresholdMs) {
  static bool firedThisContact = false;

  int x = 0, y = 0;
  if (!mappedInput.isScreenTouchHeld(x, y)) {
    firedThisContact = false;  // finger up: re-arm
    return false;
  }
  if (firedThisContact) return false;
  if (!mappedInput.isScreenTouchLongPress(x, y, thresholdMs)) return false;

  firedThisContact = true;
  mappedInput.suppressNextTouchTap();
  return true;
}

// Tap a row in a plain fixed-pitch menu. Globe, Moon and Planetarium predate
// ListLayout and lay their menus out as top + i * rowPitch, so this mirrors that
// arithmetic rather than measuring. Pass the SAME numbers drawMenu() uses.
//
// Returns the tapped row index, or -1 for no tap / a tap outside the rows.
inline int simpleMenuTap(const MappedInputManager& mappedInput, const int top, const int rowPitch,
                         const int count) {
  if (rowPitch <= 0) return -1;
  int tx = 0, ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return -1;
  if (ty < top) return -1;
  const int row = (ty - top) / rowPitch;
  return (row >= 0 && row < count) ? row : -1;
}

// A tap on the CONTENT, ignoring the button-hint strip along the bottom.
//
// wasScreenTapped() fires anywhere on the panel, hints included. An activity
// that acts on a bare tap and returns therefore swallows taps on its own Back
// and Confirm hints -- which, on a panel with no physical Back, means no way
// out of the activity at all. Anything that treats a tap as "do something to
// what I am looking at" wants this instead.
inline bool wasContentTapped(const MappedInputManager& mappedInput, const GfxRenderer& renderer, int& x,
                             int& y) {
  if (!mappedInput.wasScreenTapped(x, y)) return false;
  const auto& m = UITheme::getInstance().getMetrics();
  const int hintsTop = renderer.getScreenHeight() - m.buttonHintsHeight - m.verticalSpacing;
  return y < hintsTop;
}
