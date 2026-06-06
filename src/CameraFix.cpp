#include "CameraFix.h"
#include <algorithm>

CameraFixMode CameraFix::currentMode = CameraFixMode::Normal;

void CameraFix::ApplyFix(raylib::Camera2D &camera, CameraFixMode mode,
                         int mapWidthPx, int mapHeightPx, int screenWidth,
                         int screenHeight) {
  currentMode = mode;

  switch (mode) {
  case CameraFixMode::Centered: {
    // Center the camera on the map
    camera.offset = {screenWidth / 2.0f, screenHeight / 2.0f};
    camera.target = {mapWidthPx / 2.0f, mapHeightPx / 2.0f};
    camera.zoom = 1.0f;
    break;
  }
  case CameraFixMode::BirdseyeView: {
    // Show the entire map from above
    float zoomX = static_cast<float>(screenWidth) / mapWidthPx;
    float zoomY = static_cast<float>(screenHeight) / mapHeightPx;
    float zoom = std::min(zoomX, zoomY);

    camera.offset = {screenWidth / 2.0f, screenHeight / 2.0f};
    camera.target = {mapWidthPx / 2.0f, mapHeightPx / 2.0f};
    camera.zoom = std::max(0.1f, zoom * 0.95f); // 95% to add some padding
    break;
  }
  case CameraFixMode::Normal:
  default:
    // Don't change anything, let normal camera behavior take over
    break;
  }
}
