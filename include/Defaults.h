#pragma once

#include <string>
#include "imgui.h"

inline const ImVec4 DEFAULT_COLOR_SYSTEM_PREFIX = ImVec4(0.3f, 0.6f, 1.0f, 1.0f); // Light Blue
inline const ImVec4 DEFAULT_COLOR_SYSTEM_TEXT = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // White
inline const ImVec4 DEFAULT_COLOR_YOU_PREFIX = ImVec4(0.4f, 0.9f, 0.4f, 1.0f); // Light Green
inline const ImVec4 DEFAULT_COLOR_YOU_TEXT = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // White
inline const ImVec4 DEFAULT_COLOR_OTHER_PREFIX = ImVec4(0.2f, 0.8f, 1.0f, 1.0f); // Cyan
inline const ImVec4 DEFAULT_COLOR_OTHER_TEXT = ImVec4(1.0f, 0.7f, 0.9f, 1.0f); // Light Pink
inline const ImVec4 DEFAULT_COLOR_THOUGHT = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); // Gray
inline const ImVec4 DEFAULT_COLOR_COMMAND = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); // Gold
enum class GameCameraMode {
  // Allows movement with the arrow keys
  FreeRoamMode,
  // Automatically follows the player around the world
  FollowMode,
};

inline const std::string DEFAULT_PLAYER_ENTITY_NAME = "playerCharacter";
constexpr float DEFAULT_MAXSPEED = 100.0f;
constexpr float DEFAULT_WAYPOINT_ACCEL = 100.0f;
constexpr float DEFAULT_PLAYER_FRICTION = 30.0f;
constexpr float DEFAULT_ENTITY_FRICTION = 10.0f;
constexpr float DEFAULT_VELOCITY = 50.0f;
constexpr float DEFAULT_MINIMUM_VEL_FOR_WALL_REPEL = 5.0f;
constexpr float DEFAULT_WALL_REPEL_FORCE = 0.1f;
constexpr float DEFAULT_MINIMUM_WAYPOINT_SPEED = 10.0f;
constexpr float DEFAULT_MINIMUM_WAYPOINT_ACCEL = 100.0f;
constexpr float DEFAULT_MINIMUM_WAYPOINT_DISTANCE= 24.0f;


constexpr float DEFAULT_MINIMUM_SPEED_FOR_SLOWING_RADIUS = 10.0f;
constexpr int DEFAULT_OBJECT_SEARCH_RADIUS = 10;

constexpr int DEFAULT_INPUT_MOVEMENT = 10;
constexpr float DEFAULT_INPUT_ZOOM = 1.0f;
constexpr float DEFAULT_MINIMUM_INPUT_ZOOM = 1.0f;
constexpr float DEFAULT_MAXIMUM_INPUT_ZOOM = 4.0f;
constexpr GameCameraMode DEFAULT_STARTING_CAMERA_MODE = GameCameraMode::FollowMode;
constexpr double DEFAULT_DOUBLE_CLICK_TIME = 0.3;

constexpr const char *DEFAULT_FONT_PATH = "./fonts/oldschool/otb - Bm (linux bitmap)/BmPlus_IBM_CGA.otb";
constexpr size_t DEFAULT_FONTSIZE = 32;
constexpr size_t DEFAULT_PLAYER_HITBOX_WIDTH = 16;
constexpr size_t DEFAULT_PLAYER_HITBOX_HEIGHT = 16; 
constexpr size_t DEFAULT_PLAYER_VISUAL_WIDTH = 16;
constexpr size_t DEFAULT_PLAYER_VISUAL_HEIGHT = 16;

constexpr size_t DEFAULT_ENTITY_HITBOX_WIDTH = 16;
constexpr size_t DEFAULT_ENTITY_HITBOX_HEIGHT = 16; 
constexpr size_t DEFAULT_ENTITY_VISUAL_WIDTH = 16;
constexpr size_t DEFAULT_ENTITY_VISUAL_HEIGHT = 16; 
