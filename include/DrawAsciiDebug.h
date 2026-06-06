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

  // Set the state
  static void SetShowOuterRectangles(bool value) {
    showOuterRectangles = value;
  }
  static void SetShowInnerRectangles(bool value) {
    showInnerRectangles = value;
  }

private:
  static bool showOuterRectangles;
  static bool showInnerRectangles;
};
