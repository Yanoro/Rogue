#pragma once

class DrawAsciiDebug {
public:
  // Toggle the visibility of outer rectangles (tile boundaries)
  static void ToggleOuterRectangles() {
    showOuterRectangles = !showOuterRectangles;
  }

  // Toggle the visibility of inner rectangles (text boundaries)
  static void ToggleInnerRectangles() {
    showInnerRectangles = !showInnerRectangles;
  }

  // Get the current state
  static bool GetShowOuterRectangles() { return showOuterRectangles; }
  static bool GetShowInnerRectangles() { return showInnerRectangles; }
  static bool GetShowHitboxes() { return showHitboxes; }
  static bool GetShowPickboxes() { return showPickboxes; }
  static bool GetShowCapsules() { return showCapsules; }

  // Set the state
  static void SetShowOuterRectangles(bool value) {
    showOuterRectangles = value;
  }
  static void SetShowInnerRectangles(bool value) {
    showInnerRectangles = value;
  }
  static void SetShowHitboxes(bool value) { showHitboxes = value; }
  static void SetShowPickboxes(bool value) { showPickboxes = value; }
  static void SetShowCapsules(bool value) { showCapsules = value; }

private:
  static bool showOuterRectangles;
  static bool showInnerRectangles;
  static bool showHitboxes;
  static bool showPickboxes;
  static bool showCapsules;
};
