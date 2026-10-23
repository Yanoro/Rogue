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
// The default field is all-round: 360 degrees at 22 tiles, about 1,500 cells.
// That deliberately removes the forward cone as an independent variable while
// navigation is being evaluated. The forward-cone variant -- 90 degrees at 22
// tiles plus the 2-tile all-round radar, about 390 cells -- is still available
// through the `vision` settings. These are starting guesses; the Vision Debug
// window edits them live precisely because they are guesses.
constexpr float DEFAULT_VISION_RANGE = 22.0f;
constexpr float DEFAULT_VISION_ANGLE_DEGREES = 360.0f;
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

// ---------------------------------------------------------------------------
// Navigation by System 1 (docs/navigation-subagent-design.md).
//
// Defaults for `system1.navigation`: how many actions the state shows, how the
// local compass is bounded, and when a choice is confident enough to act on.
// ---------------------------------------------------------------------------
// One line per action in the request: a local compass move or a remembered place.
// A decision model's input budget is short, and a candidate list longer than this
// turns a choice into a reading comprehension task.
constexpr int DEFAULT_NAVIGATION_MAX_CANDIDATES = 8;
// Local compass moves offered before the distant place list. In a wide place the
// eight sectors would otherwise crowd out the places that are the actual goal.
constexpr int DEFAULT_NAVIGATION_MAX_DIRECTIONS = 4;
// How far the local compass looks for the first place in a sector. A short radius
// keeps "north" a local move; a place farther than this belongs in the distant
// list, where its own distance is shown.
constexpr int DEFAULT_NAVIGATION_COMPASS_RADIUS = 6;
// A run of the current place shorter than this is a sidestep, not a direction:
// one tile into the edge of the room is not "continue north". Two tiles is the
// smallest move that is visibly a continuation.
constexpr int DEFAULT_NAVIGATION_MIN_RUN = 2;
// Places listed in the completion summary, in the same form [PLACES] prints.
// Enough to orient the main model, not a second full listing.
constexpr int DEFAULT_NAVIGATION_NEARBY_PLACES = 5;
// Answer::probabilities[choice] below this is "not sure enough to walk".
//
// 0.3, not 0.5: a choice model spreading its answer over eight or ten actions
// rarely gives the top one half the mass. In the first real Jev run the model's
// successful `here:south-east` choices were 0.37/0.28 with a 0.13/0.19 runner-up;
// the 0.5 floor rejected both, surveyed, and stopped with "turned a full circle"
// while the character stood still beside the road. The margin below still rejects
// a genuinely split answer (0.28 vs 0.19); the absolute floor only has to reject
// the weak tail.
constexpr float DEFAULT_NAVIGATION_MIN_PROBABILITY = 0.3f;
// The margin over the runner-up. A 0.51/0.49 split is a coin flip even though the
// winner clears the probability floor.
constexpr float DEFAULT_NAVIGATION_MIN_MARGIN = 0.15f;
// Survey recoveries per episode. One per unsure decision is enforced separately;
// this is a cheap recovery budget, not a navigation stop: a failed survey still
// hands to the strategist.
constexpr int DEFAULT_NAVIGATION_MAX_SURVEYS = 4;
// Wall-clock ceiling for the whole episode, including decision latency and
// walking. A resource/hang guard, not a navigation decision: the strategist and
// pilot decide when to stop; this only prevents an unbounded episode.
constexpr float DEFAULT_NAVIGATION_MAX_SECONDS = 600.0f;
// One leg may not monopolise the episode. MoveToEntityAction has no timeout of
// its own; a navigation leg that cannot finish should hand back, not hang.
constexpr float DEFAULT_NAVIGATION_LEG_TIMEOUT_SECONDS = 60.0f;

// ---------------------------------------------------------------------------
// LLM strategist + S1 pilot navigation (docs/llm-s1-navigation-design.md).
// The S1 pilot keeps the system1.navigation defaults above; these are the outer
// strategist layer.
// ---------------------------------------------------------------------------
// The hybrid is the default. A strategist failure falls back to the S1-only
// navigation rather than stranding the character.
constexpr bool DEFAULT_NAVIGATION_STRATEGIST_ENABLED = true;
constexpr bool DEFAULT_NAVIGATION_STRATEGIST_FALLBACK = true;
// How many strategist calls one episode may spend (start + re-plans). A cost
// guard, not a navigation-decision cap: the strategist's own give-up is the
// normal stop.
constexpr int DEFAULT_NAVIGATION_MAX_STRATEGIES = 12;
// Monitor questions per strategist plan. The S1 is a small model; a long list
// degrades its local choice.
constexpr int DEFAULT_NAVIGATION_MAX_MONITORS = 3;
// A yes/no Score answer must be at least this far from 0.5 (confidence =
// 2*|p-0.5|) to be actionable. Below it the monitor is unknown and hands back.
constexpr float DEFAULT_NAVIGATION_MONITOR_MIN_CONFIDENCE = 0.6f;
// Standard-LLM strategist call timeout. The S1 is not blocked on it; the action
// abandons the call and falls back.
constexpr float DEFAULT_NAVIGATION_STRATEGIST_TIMEOUT_SECONDS = 30.0f;
// Observation bounds for the strategist's brief.
constexpr int DEFAULT_NAVIGATION_OBSERVATION_GRID_RADIUS = 8;
constexpr int DEFAULT_NAVIGATION_MAX_GRAPH_NODES = 24;
constexpr int DEFAULT_NAVIGATION_MAX_GRAPH_EDGES = 48;
constexpr int DEFAULT_NAVIGATION_OBSERVATION_MAX_TOKENS = 2000;

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
