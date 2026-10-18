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

// Facing (docs/spatial-memory-design.md section 4.1).
//
// Below this speed a character counts as standing still and keeps whatever
// direction it last had. Not zero on purpose: the steer system decelerates
// through small non-zero velocities, and a threshold of zero would let those
// residual values jitter the facing (and, once stage 1 samples on facing change,
// re-sample the cone for no reason). Compared against Velocity::Length(), whose
// units are the same screen pixels per second the waypoint constants use.
constexpr float DEFAULT_FACING_MIN_SPEED = 5.0f;

// Length of the debug facing marker, as a fraction of one tile. Long enough to
// read at a glance, short enough not to be confused with the vision cone that
// stage 1 will draw at a much greater range.
constexpr float DEFAULT_FACING_MARKER_TILE_FRACTION = 0.6f;

// Vision (docs/spatial-memory-design.md section 4.2). Initial values for
// `Vision`, which carries its own copy of each so a character can override them
// later without a code change.
//
// The cone is 90 degrees at 22 tiles and the radar 2 tiles: about 390 cells in
// total, comparable to the radius-10 square [SURROUNDINGS] already scans. The
// cone is not cheaper than a disc, it is differently shaped -- it reaches further
// ahead and nothing behind. These are starting guesses, chosen from looking at
// the overlay; the Vision Debug window edits them live precisely because they
// are guesses.
constexpr float DEFAULT_VISION_RANGE = 22.0f;
constexpr float DEFAULT_VISION_ANGLE_DEGREES = 90.0f;
constexpr int DEFAULT_VISION_RADAR_RADIUS = 2;

// How far the facing must turn before the cached sweep is recomputed. A smaller
// value samples more often for a finer cone; a larger one lets the cone lag the
// character while turning. 22.5 degrees is one 16th of a turn.
constexpr float DEFAULT_VISION_FACING_RESAMPLE_DEGREES = 22.5f;

// The cache is also rebuilt this often regardless, which is what notices an
// obstacle appearing or disappearing (a tree felled, a barrel moved) without
// tracking every such change. At ~60 fps this is twice a second.
constexpr int DEFAULT_VISION_RESAMPLE_TICKS = 30;

// Spatial memory caps (docs/spatial-memory-design.md section 5).
//
// The cell cap is a memory budget, not a simulation rule. It has to hold a map
// the game actually ships, or memory forgets ground the character just walked and
// a long road breaks into fragments as its far end ages out: 4096 is one 64x64
// map (the village and the place test map), which is the largest world there is
// today. A map larger than this needs a spatial budget rather than a bigger
// number -- that is an open question in the design doc. Overflow drops the cell
// with the lowest age-discounted-by-visits score.
constexpr int DEFAULT_MEMORY_CELLS_MAX = 4096;

// How many ticks of youth one revisit is worth when the cell cap has to drop
// something. Eviction scores a cell as observedTick - visitCount * this and drops
// the lowest, so a road crossed often outlives a corner glimpsed once at the same
// age, while an older cell always loses to one observed now -- otherwise every new
// cell is the cheapest thing in a full store and memory stops growing.
constexpr int DEFAULT_MEMORY_VISIT_BONUS_TICKS = 60;

// What belonging to a place is worth when runs are ordered for a drop. Place
// ground is exempt from dropping, so this only decides listing order -- but it
// keeps the ledger readable: the runs that matter sort to the top.
constexpr long long DEFAULT_MEMORY_PLACE_RANK_BONUS = 6000;

// How many consolidation events the ledger keeps. Enough to see a trend, small
// enough that the dump stays a document rather than a log file.
constexpr int DEFAULT_CONSOLIDATION_LOG_MAX = 32;

// Objects are far sparser than cells, but a walled village is not sparse: the
// village map's 453 structural objects are recorded too, and at 256 the budget was
// consumed by walls, evicting the seeds places are made from. Structural objects
// will get their own cheaper store when wall qualities land (section 3e); until
// then this bound has to accommodate them.
constexpr int DEFAULT_MEMORY_OBJECTS_MAX = 1024;

// How many places one character may hold (docs/spatial-memory-design.md section
// 3.3). Overflow drops the place with the fewest member cells, then the one seen
// longest ago; the cells stay in memory and simply stop being placed.
constexpr int DEFAULT_MEMORY_PLACES_MAX = 24;

// How close (Chebyshev) two seeds must be to land in the same place. Global
// rather than per category, because one region may hold many categories
// (docs/place-derivation-design.md section 5.1): a market beside an orchard is one
// place whose aspect fields overlap. This is the knob if a dense row chains a whole
// district into one place.
constexpr int DEFAULT_PLACE_LINK_RADIUS = 4;

// How far a place a character names may spread when nothing covers the ground it
// points at. Small on purpose: a name is not a claim to a district.
constexpr int DEFAULT_PLACE_NAME_RADIUS = 3;

// How many cells a place's aspect mean samples. The mean is a summary for a
// reader; evaluating every cell against every source was the largest single cost
// of the per-sample derivation pass.
constexpr int DEFAULT_PLACE_MEAN_SAMPLES = 64;

// How many tile cells per category contribute to one place's aspect field. A lake
// or a field is hundreds of cells; every cell as a source would make the per-cell
// evaluation the overlay does quadratic. The sample is evenly spaced and
// deterministic, so the field keeps its shape.
constexpr int DEFAULT_PLACE_TILE_SOURCES_MAX = 48;

// How close (Chebyshev, in cells) a newly observed cell must be to an existing
// cluster of the same surface to join it instead of starting a new place. This is
// what lets one field be one place while keeping two fields apart.
constexpr int DEFAULT_MEMORY_PLACE_CLUSTER_RADIUS = 3;

// Durability one harvest removes from each tool used for it, when the object
// does not declare a "durabilityCost" of its own. Durability is opt-in per item
// (an item whose definition declares no "durability" never wears out), so this
// only matters for tools that already opted in.
constexpr int DEFAULT_HARVEST_DURABILITY_COST = 1;

// Task lists (docs/task-system-design.md section 12). The cap bounds what one
// agent may store, so a model that re-lists its whole life every revision cannot
// grow the component without bound; overflow drops the lowest tier first,
// oldest first. Life goals are authored and are never counted or dropped.
constexpr int DEFAULT_TASK_LIST_MAX = 12;

// How many short tasks the digest renders. Terminal and long-term goals render
// as one line each regardless; this bounds only the tasks, highest tier first.
// The digest competes with the command list for context, so it is deliberately
// small.
constexpr int DEFAULT_TASK_DIGEST_MAX = 5;

// Frames a task may go untouched before it is flagged. The world runs at ~60 fps,
// so this is about thirty seconds: long enough not to flag a task the agent is
// legitimately working through, short enough that a stuck agent is visible while
// you are still watching it.
constexpr int DEFAULT_TASK_STALE_TICKS = 1800;

// The anti-inflation threshold (section 6.1): at most this many Critical entries.
// Used to *report* a violation, never to rewrite a tier.
constexpr int DEFAULT_TASK_MAX_CRITICAL = 1;

// How often the checking pass runs, in frames (~1 s at 60 fps). Each pass is a
// world-wide name query per task, and nothing about it is time-critical: the
// result is only shown to the agent when it wakes, and the debug windows sample
// the same fields live in between.
constexpr int DEFAULT_TASK_VERIFY_INTERVAL_TICKS = 60;

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
