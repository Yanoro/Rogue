#pragma once

#include "raylib-cpp.hpp"

enum class CameraFixMode {
  Normal,
  Centered,       // Center the camera on the map
  BirdseyeView,   // Show the entire map from above
};

class CameraFix {
public:
  static void ApplyFix(raylib::Camera2D &camera, CameraFixMode mode,
                       int mapWidthPx, int mapHeightPx, int screenWidth,
                       int screenHeight);

  static CameraFixMode GetCurrentMode() { return currentMode; }
  static void SetCurrentMode(CameraFixMode mode) { currentMode = mode; }

private:
  static CameraFixMode currentMode;
};
