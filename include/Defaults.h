#pragma once

#include <string>
#include "imgui.h"
#include "raylib-cpp.hpp"

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

// How many tiles in each direction [SURROUNDINGS] reports on. The placement
// hint deliberately keeps a smaller radius of its own: its grid is meant to show
// only the ground a single [PLACE] can reach, not everything in sight.
constexpr int DEFAULT_SURROUNDINGS_RADIUS = 10;

// How far from the actor [PLACE] may set an item down, in tiles. A template's
// "placement" block may override this per item; see docs/placement-design.md.
constexpr int DEFAULT_PLACEMENT_MAX_DISTANCE = 1;

// Durability one harvest removes from each tool used for it, when the object
// does not declare a "durabilityCost" of its own. Durability is opt-in per item
// (an item whose definition declares no "durability" never wears out), so this
// only matters for tools that already opted in.
constexpr int DEFAULT_HARVEST_DURABILITY_COST = 1;

constexpr int DEFAULT_INPUT_MOVEMENT = 10;
constexpr float DEFAULT_INPUT_ZOOM = 1.0f;
constexpr float DEFAULT_MINIMUM_INPUT_ZOOM = 1.0f;
constexpr float DEFAULT_MAXIMUM_INPUT_ZOOM = 4.0f;
constexpr GameCameraMode DEFAULT_STARTING_CAMERA_MODE = GameCameraMode::FollowMode;
constexpr double DEFAULT_DOUBLE_CLICK_TIME = 0.3;

constexpr const char *DEFAULT_FONT_PATH = "./fonts/oldschool/otb - Bm (linux bitmap)/BmPlus_IBM_CGA.otb";
constexpr const char *MAPS_DIRECTORY = "./maps";

constexpr size_t DEFAULT_FONTSIZE = 32;
constexpr size_t DEFAULT_PLAYER_HITBOX_WIDTH = 16;
constexpr size_t DEFAULT_PLAYER_HITBOX_HEIGHT = 16; 
constexpr size_t DEFAULT_PLAYER_VISUAL_WIDTH = 16;
constexpr size_t DEFAULT_PLAYER_VISUAL_HEIGHT = 16;

constexpr size_t DEFAULT_ENTITY_HITBOX_WIDTH = 16;
constexpr size_t DEFAULT_ENTITY_HITBOX_HEIGHT = 16; 
constexpr size_t DEFAULT_ENTITY_VISUAL_WIDTH = 16;
constexpr size_t DEFAULT_ENTITY_VISUAL_HEIGHT = 16; 

// Player-only context-menu option that opens the held-items window for a
// Storage container or a character. It is deliberately NOT registered in
// InteractionRegistry: the AI must never see or issue it. Produced by
// Game::DrawGameWindows and consumed by the PendingPlayerInteraction handling in
// GameECS.cpp.
constexpr const char *DEFAULT_SEE_INVENTORY_OPTION = "See Inventory";

// Player-only context-menu option that opens the character status window: name,
// starting context, stats and skills. Like DEFAULT_SEE_INVENTORY_OPTION it is
// deliberately NOT registered in InteractionRegistry, so the AI never sees it in
// its command list and can never issue it. Unlike that option it opens
// immediately rather than after walking over, because inspecting a character is
// not an action on the world.
constexpr const char *DEFAULT_SHOW_STATUS_OPTION = "Show Status";

// Timed-action progress bar, drawn just below the entity that is acting.
constexpr float DEFAULT_ACTION_BAR_HEIGHT = 4.0f;
constexpr float DEFAULT_ACTION_BAR_OFFSET_Y = 4.0f;
constexpr float DEFAULT_ACTION_BAR_MIN_WIDTH = 24.0f;
inline const Color DEFAULT_ACTION_BAR_BACKGROUND = {30, 30, 30, 200};
inline const Color DEFAULT_ACTION_BAR_FILL = {90, 210, 90, 255};

// Floating change numbers: the small "+N" that pops over an entity, drifts up
// and fades (XP gains today, damage once combat exists). These are shared
// defaults every spawn starts from; a caller can still override the per-number
// fields on the FloatingText component itself.
//
// The rise is deliberately small and the lifetime deliberately short: the point
// is a glanceable tick beside the thing it describes, not a second HUD.
constexpr float DEFAULT_FLOATING_TEXT_LIFETIME = 1.0f;
constexpr float DEFAULT_FLOATING_TEXT_RISE = 24.0f;
// Fraction of the lifetime that stays fully opaque. The number is clearest
// while it is near the entity, so it holds its colour and only fades on the way
// out rather than dimming the whole journey.
constexpr float DEFAULT_FLOATING_TEXT_FADE_START = 0.66f;
// Baseline gap between the entity's visual top and the number's first frame.
constexpr float DEFAULT_FLOATING_TEXT_OFFSET_Y = 4.0f;
// Horizontal step used to fan out numbers that spawn on the same target at the
// same moment, so a multi-skill award does not collapse into one number.
constexpr float DEFAULT_FLOATING_TEXT_FAN_SPACING = 12.0f;
constexpr float DEFAULT_FLOATING_TEXT_FONT_SIZE = 16.0f;
// Green for a gain, red for damage: the same reading the chat already gives a
// character's own progress.
inline const Color DEFAULT_FLOATING_TEXT_XP_COLOR = {120, 230, 120, 255};
inline const Color DEFAULT_FLOATING_TEXT_DAMAGE_COLOR = {235, 90, 90, 255};
