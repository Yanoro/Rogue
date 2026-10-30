#!/usr/bin/env python3
"""Generate the navigation test maps for `[NAVIGATE]`.

The suite exists to isolate one layer of the navigation pipeline at a time
(docs/llm-s1-navigation-design.md, docs/navigation-subagent-design.md): an
unreadable failure on the quest map cannot say whether vision, place
derivation, the compass, A* or the strategist was at fault.

What this writes
---------------
    maps/Map_Nav_<Name>.json          the map, in the loader's own schema
    maps/nav_suite/<Name>.test.json   the scenario spec: goal, goal region,
                                      spawn, pre-steps and what to score
    maps/nav_suite/README.md          the run protocol (written separately)

The specs are deliberately *not* in `maps/`: MapReloader lists every `.json`
there that looks like a map, and a spec with no geometry would be skipped but
clutter the list.

Schema facts this generator must respect (they are load-bearing; see Map.cpp
and docs/agent-exploration-task.md):

  * `positions` is COLUMN-major: index = x * height + y.
  * `tilePalette` entries are catalog names from data/tiles/*.json `"name"`,
    not file stems ("stone floor", not "stone_floor_tile").
  * `objects[].type` is the template key, which is the file stem in
    data/objects ("wooden_wall"), not the display name.
  * `placeSeeds[]` are authored *labels*: a label attaches only once the
    anchor's ground has been observed, so a seed is not a hint.
  * Blocking is two separate things: a tile with `blocksTile` (water), and an
    object carrying `Obstacle` (trees, walls, stalls, signposts). `BlocksVision`
    is independent, so a fence stops movement and not sight.

Usage
-----
    python3 tools/gen_nav_maps.py            # write every map + spec
    python3 tools/gen_nav_maps.py --only fork,lake
    python3 tools/gen_nav_maps.py --check    # validate what is on disk
    python3 tools/gen_nav_maps.py --list

The generator validates geometry and reachability, not model behaviour. A run
is still the owner's to judge.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from collections import deque
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MAPS_DIR = REPO_ROOT / "maps"
SUITE_DIR = MAPS_DIR / "nav_suite"
TILES_DIR = REPO_ROOT / "data" / "tiles"
OBJECTS_DIR = REPO_ROOT / "data" / "objects"

# Fixed order so a regenerated map diffs only where the world changed.
PALETTE_ORDER = [
    "grass",
    "path",
    "stone floor",
    "wooden floor",
    "tilled soil",
    "packed earth",
    "water",
]

EIGHT_WAY = [
    (1, 0), (-1, 0), (0, 1), (0, -1),
    (1, 1), (1, -1), (-1, 1), (-1, -1),
]

# One-line inventory entries for the suite README. The long `purpose` is kept in
# the spec; this is the table cell a reader scans.
HEADLINES = {
    "threshold": "Open ground: the compass only exists inside a place.",
    "fork": "Long road, T junction, contradictory signposts, quarry dead end.",
    "hub": "Five arms, four slots: does the cap hide the goal arm?",
    "lake": "Visible across water, reachable the long way round.",
    "lake_sealed": "Visible across water, unreachable.",
    "blind_start": "Cold start in a hut; keyhole view of a road beyond the compass radius.",
    "maze": "One-tile corridors and long A* legs under load.",
    "pinch": "A zero-width diagonal gate: A*'s only route, impossible for a body.",
    "decoy": "Two hamlets; only the far one has a market and a workshop.",
    "names": "Two identical orchards and two roads: identity and naming.",
    "streets": "Two-tile stubs: runs shorter than the minimum are dropped.",
}


# ---------------------------------------------------------------------------
# Catalogs
# ---------------------------------------------------------------------------


class Catalog:
    """The tile and object vocabulary the game will resolve the map against."""

    def __init__(self) -> None:
        self.tiles: dict[str, dict] = {}
        self.objects: dict[str, dict] = {}
        for path in sorted(TILES_DIR.glob("*.json")):
            data = json.loads(path.read_text())
            name = data.get("name")
            if name:
                self.tiles[name] = data
        for path in sorted(OBJECTS_DIR.glob("*.json")):
            data = json.loads(path.read_text())
            self.objects[path.stem] = data

    def tile_blocks(self, name: str) -> bool:
        return bool(self.tiles.get(name, {}).get("blocksTile", False))

    def object_components(self, template: str) -> list[str]:
        return list(self.objects.get(template, {}).get("components", []))

    def object_blocks(self, template: str) -> bool:
        return "Obstacle" in self.object_components(template)

    def object_blocks_vision(self, template: str) -> bool:
        return "BlocksVision" in self.object_components(template)


CATALOG = Catalog()


# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------


class MapBuilder:
    def __init__(self, name: str, width: int, height: int,
                 default_tile: str = "grass") -> None:
        self.name = name
        self.width = width
        self.height = height
        self.grid = [[default_tile for _ in range(height)] for _ in range(width)]
        self.objects: list[tuple[str, int, int]] = []
        self.object_cells: dict[tuple[int, int], str] = {}
        self.seeds: list[dict] = []
        self.npcs: list[dict] = []

        # Scenario spec.
        self.scenario = ""
        self.purpose = ""
        self.goal = ""
        self.goal_region: tuple[int, int, int, int] | None = None
        self.pre_steps: list[str] = []
        self.expected: dict = {}
        self.notes: list[str] = []
        self.allow_unreachable = False

    # -- terrain --------------------------------------------------------

    def in_bounds(self, x: int, y: int) -> bool:
        return 0 <= x < self.width and 0 <= y < self.height

    def tile(self, x: int, y: int, name: str) -> None:
        assert self.in_bounds(x, y), f"{self.name}: tile out of bounds {(x, y)}"
        assert name in CATALOG.tiles, f"{self.name}: unknown tile {name!r}"
        self.grid[x][y] = name

    def rect(self, x0: int, y0: int, x1: int, y1: int, name: str) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            for y in range(min(y0, y1), max(y0, y1) + 1):
                self.tile(x, y, name)

    def hline(self, y: int, x0: int, x1: int, name: str) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.tile(x, y, name)

    def vline(self, x: int, y0: int, y1: int, name: str) -> None:
        for y in range(min(y0, y1), max(y0, y1) + 1):
            self.tile(x, y, name)

    # -- objects --------------------------------------------------------

    def obj(self, template: str, x: int, y: int) -> None:
        assert template in CATALOG.objects, (
            f"{self.name}: unknown object template {template!r}"
        )
        assert self.in_bounds(x, y), f"{self.name}: object out of bounds {(x, y)}"
        assert (x, y) not in self.object_cells, (
            f"{self.name}: two objects on {(x, y)}: "
            f"{self.object_cells[(x, y)]} and {template}"
        )
        ground = self.grid[x][y]
        assert not CATALOG.tile_blocks(ground), (
            f"{self.name}: object {template} on blocking ground {ground} at {(x, y)}"
        )
        self.objects.append((template, x, y))
        self.object_cells[(x, y)] = template

    def box(self, x0: int, y0: int, x1: int, y1: int,
            door: tuple[int, int] | None = None,
            floor: str = "wooden floor",
            wall: str = "wooden_wall") -> None:
        """A one-cell-walled building with a door cell in its perimeter."""
        self.rect(x0, y0, x1, y1, floor)
        perimeter = (
            [(x, y0) for x in range(x0, x1 + 1)]
            + [(x, y1) for x in range(x0, x1 + 1)]
            + [(x0, y) for y in range(y0 + 1, y1)]
            + [(x1, y) for y in range(y0 + 1, y1)]
        )
        for x, y in perimeter:
            if door is not None and (x, y) == door:
                continue
            self.obj(wall, x, y)
        if door is not None:
            self.obj("door", door[0], door[1])

    def scatter(self, template: str, density: float, seed: int,
                only_on: str = "grass",
                keep_clear: list[tuple[int, int, int, int]] | None = None,
                extra_clear: set[tuple[int, int]] | None = None) -> int:
        """Deterministic scatter, used for forest and undergrowth.

        Only ever overwrites empty cells of `only_on`, so a road, a floor or an
        existing object is never covered. `keep_clear` is inclusive rects.
        """
        rng = random.Random(seed)
        blocked = set(extra_clear or ())
        for x0, y0, x1, y1 in keep_clear or []:
            for x in range(x0, x1 + 1):
                for y in range(y0, y1 + 1):
                    blocked.add((x, y))
        placed = 0
        for x in range(self.width):
            for y in range(self.height):
                if self.grid[x][y] != only_on:
                    continue
                if (x, y) in blocked or (x, y) in self.object_cells:
                    continue
                if rng.random() < density:
                    self.obj(template, x, y)
                    placed += 1
        return placed

    # -- scenario -------------------------------------------------------

    def seed(self, label: str, x: int, y: int, radius: int) -> None:
        assert self.in_bounds(x, y), f"{self.name}: seed out of bounds {(x, y)}"
        self.seeds.append(
            {"label": label, "position": [x, y], "radius": int(radius)}
        )

    def npc(self, name: str, x: int, y: int, background: str,
            terminal: list[str]) -> None:
        assert self.in_bounds(x, y), f"{self.name}: npc out of bounds {(x, y)}"
        assert (x, y) not in self.object_cells, (
            f"{self.name}: npc on object {self.object_cells[(x, y)]} at {(x, y)}"
        )
        assert not CATALOG.tile_blocks(self.grid[x][y]), (
            f"{self.name}: npc on blocking ground at {(x, y)}"
        )
        self.npcs.append(
            {
                "name": name,
                "position": [x, y],
                "background": background,
                "goals": {"terminal": list(terminal)},
            }
        )

    # -- geometry queries ----------------------------------------------

    def walkable(self, x: int, y: int) -> bool:
        if not self.in_bounds(x, y):
            return False
        if CATALOG.tile_blocks(self.grid[x][y]):
            return False
        template = self.object_cells.get((x, y))
        if template is not None and CATALOG.object_blocks(template):
            return False
        return True

    def bfs_steps(self, start: tuple[int, int]) -> dict[tuple[int, int], int]:
        """8-way distances, matching AStar's neighbour rule (destination only)."""
        sx, sy = start
        assert self.walkable(sx, sy), f"{self.name}: bfs start not walkable {start}"
        dist = {(sx, sy): 0}
        queue = deque([(sx, sy)])
        while queue:
            x, y = queue.popleft()
            for dx, dy in EIGHT_WAY:
                nx, ny = x + dx, y + dy
                if (nx, ny) in dist or not self.walkable(nx, ny):
                    continue
                dist[(nx, ny)] = dist[(x, y)] + 1
                queue.append((nx, ny))
        return dist

    def goal_cells(self) -> list[tuple[int, int]]:
        assert self.goal_region is not None, f"{self.name}: no goal region"
        x0, y0, x1, y1 = self.goal_region
        return [
            (x, y)
            for x in range(x0, x1 + 1)
            for y in range(y0, y1 + 1)
            if self.walkable(x, y)
        ]

    # -- output ---------------------------------------------------------

    def palette(self) -> list[str]:
        used = {self.grid[x][y] for x in range(self.width)
                for y in range(self.height)}
        unknown = used - set(PALETTE_ORDER)
        assert not unknown, f"{self.name}: tiles outside PALETTE_ORDER: {unknown}"
        return [name for name in PALETTE_ORDER if name in used]

    def build_json(self) -> dict:
        palette = self.palette()
        index = {name: i for i, name in enumerate(palette)}
        positions = [
            index[self.grid[x][y]]
            for x in range(self.width)
            for y in range(self.height)
        ]
        return {
            "mapWidth": self.width,
            "mapHeight": self.height,
            "tileWidth": 32,
            "tileHeight": 32,
            "tilePalette": palette,
            "positions": positions,
            "objects": [
                {"type": template, "x": x, "y": y}
                for template, x, y in self.objects
            ],
            "placeSeeds": self.seeds,
            "npcs": self.npcs,
        }

    def check(self) -> tuple[int, int, int]:
        """Reachability summary: (optimal steps, reachable cells, goal cells)."""
        assert self.npcs, f"{self.name}: no npc"
        spawn = tuple(self.npcs[0]["position"])
        dist = self.bfs_steps(spawn)
        goals = self.goal_cells()
        assert goals, f"{self.name}: goal region has no walkable cell"
        reachable_goals = [dist[cell] for cell in goals if cell in dist]
        if not reachable_goals:
            assert self.allow_unreachable, (
                f"{self.name}: goal region unreachable from {spawn}"
            )
            return -1, len(dist), len(goals)
        return min(reachable_goals), len(dist), len(goals)

    def write(self) -> dict:
        optimal, reachable, goal_cells = self.check()
        map_json = self.build_json()
        map_path = MAPS_DIR / f"Map_Nav_{self.name}.json"
        map_path.write_text(json.dumps(map_json, indent=2) + "\n")

        spec = {
            "map": map_path.name,
            "scenario": self.scenario,
            "headline": HEADLINES.get(self.scenario, self.purpose),
            "purpose": self.purpose,
            "spawn": self.npcs[0]["position"],
            "goal": self.goal,
            "goalRegion": None if self.goal_region is None else {
                "x": [self.goal_region[0], self.goal_region[2]],
                "y": [self.goal_region[1], self.goal_region[3]],
            },
            "preSteps": self.pre_steps,
            "expected": self.expected,
            "metrics": {
                "optimalSteps": optimal,
                "reachableCells": reachable,
                "goalCellsWalkable": goal_cells,
                "objects": len(self.objects),
                "reachable": optimal >= 0,
            },
            "run": f"./build/AIRogue {map_path.relative_to(REPO_ROOT)}",
            "notes": self.notes,
        }
        spec_path = SUITE_DIR / f"{self.name}.test.json"
        spec_path.write_text(json.dumps(spec, indent=2) + "\n")
        return {
            "name": self.name,
            "size": f"{self.width}x{self.height}",
            "objects": len(self.objects),
            "optimal": optimal,
            "reachable": optimal >= 0,
            "map": map_path,
            "spec": spec_path,
        }


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------


def map_threshold() -> MapBuilder:
    b = MapBuilder("Threshold", 24, 24)
    b.scenario = "threshold"
    b.purpose = (
        "Open ground between the character and the road. The compass is only "
        "built when the character stands in a place, so this isolates the "
        "distant-place fallback and the action list the model actually sees."
    )
    b.hline(12, 2, 21, "path")
    for x, y in [(3, 3), (4, 3), (5, 3), (3, 4), (5, 4),
                 (4, 5), (3, 6), (4, 6), (5, 6)]:
        b.obj("apple_tree", x, y)
    for x, y in [(19, 19), (20, 19), (19, 20), (20, 20)]:
        b.obj("rock_face", x, y)
    b.npc(
        "Wren", 10, 8,
        "You are Wren, a traveller resting on open grass. You want to find "
        "your way back to the old road before dark.",
        ["Find the road"],
    )
    b.goal = "find the road"
    b.goal_region = (2, 12, 21, 12)
    b.pre_steps = ["[LOOK around]", "[NAVIGATE find the road]"]
    b.expected = {
        "maxLegs": 2,
        "surveysAllowed": 1,
        "note": "The road is four tiles away and the character is in no place, "
                "so the only moves offered should be distant places (pN).",
    }
    b.notes = [
        "If the list has no here:* line, that is the documented compass rule, "
        "not a failure: BuildCandidates skips the compass when PlaceQuery::At "
        "returns nothing.",
        "Score: reached a road cell in at most two legs, and no survey after "
        "the opening LOOK around.",
    ]
    return b


def map_fork() -> MapBuilder:
    b = MapBuilder("Fork", 64, 64)
    b.scenario = "fork"
    b.purpose = (
        "A long road with a T junction, two contradictory signposts and a "
        "quarry dead end. Isolates run following, junction splitting, the "
        "already-walked marker and told knowledge carried in the goal."
    )
    # Village ground before the road, so the road crosses the square.
    b.rect(47, 15, 56, 25, "stone floor")
    b.hline(50, 4, 30, "path")
    b.vline(30, 20, 50, "path")
    b.vline(30, 51, 58, "path")
    b.hline(20, 30, 52, "path")

    b.box(46, 14, 49, 17, door=(47, 17))
    b.box(52, 22, 55, 25, door=(53, 22))
    for x, y in [(50, 18), (51, 18), (52, 18)]:
        b.obj("market_stall", x, y)
    b.obj("mill", 54, 18)
    b.obj("oven", 55, 19)
    b.obj("chest", 55, 17)
    b.obj("water_trough", 48, 21)

    b.obj("signpost", 31, 49)
    b.obj("signpost_old", 29, 49)

    for x, y in [(28, 56), (29, 56), (31, 56), (32, 56),
                 (28, 58), (29, 58), (31, 58), (32, 58)]:
        b.obj("rock_face", x, y)
    b.obj("iron_vein", 29, 57)
    b.obj("iron_vein", 31, 57)
    b.obj("rock_face", 30, 59)

    b.scatter(
        "apple_tree", 0.28, seed=50,
        keep_clear=[(44, 12, 58, 27), (3, 47, 8, 53), (27, 55, 33, 61)],
    )
    b.seed("Village", 52, 20, 6)
    b.npcs.append({
        "name": "Wren",
        "position": [5, 50],
        "background": (
            "You are Wren, a woodcutter who lives at the end of an old road in "
            "the forest. You have heard there is a village with a market "
            "somewhere beyond the trees, and you have never been there."
        ),
        "goals": {"terminal": ["Find the village"]},
    })
    b.goal = "find a village with a market; the signpost said north"
    b.goal_region = (47, 15, 56, 25)
    b.pre_steps = ["[LOOK around]",
                   "[NAVIGATE find a village with a market; the signpost said north]"]
    b.expected = {
        "maxLegs": 20,
        "surveysAllowed": 4,
        "note": "Legs are capped by compassRadius, so a 25-tile road is four "
                "or five legs. The junction must not walk the west stretch "
                "again once it has been covered.",
    }
    b.notes = [
        "Second act on the same map: after arriving, [NAVIGATE return to the "
        "clearing] checks whether a second episode can use the first one's "
        "places. Route and dead ends are per-episode; the places are not.",
        "The two signposts disagree on purpose (signpost.json vs "
        "signpost_old.json). The goal text carries the one the main model read.",
        "Score: first or second junction choice is north, not the quarry; no "
        "west/east ping-pong after the T; the arrived cell is inside the "
        "square.",
    ]
    return b


def map_hub() -> MapBuilder:
    b = MapBuilder("Hub", 32, 32)
    b.scenario = "hub"
    b.purpose = (
        "A crossroads with five arms of unequal length. maxDirections=4 must "
        "drop one arm; the shortest is the one that leads to the goal, so the "
        "test is whether the distant-place list catches what the cap hid."
    )
    b.hline(16, 6, 26, "path")
    b.vline(16, 6, 26, "path")
    for step in range(1, 8):
        b.tile(16 + step, 16 - step, "path")

    # N: orchard, walkable grass on the sector line, trees around it.
    b.obj("apple_tree", 14, 5)
    b.obj("apple_tree", 16, 5)
    b.obj("apple_tree", 18, 5)
    b.obj("apple_tree", 14, 7)
    b.obj("apple_tree", 18, 7)

    # E: field.
    b.rect(26, 14, 29, 18, "tilled soil")

    # S: water.
    b.rect(14, 27, 18, 30, "water")

    # W: mine.
    b.obj("iron_vein", 4, 15)
    b.obj("iron_vein", 4, 17)
    b.obj("iron_vein", 6, 14)
    b.obj("rock_face", 5, 16)
    b.obj("rock_face", 7, 15)

    # NE: market and workshop, the goal. Reached by the diagonal arm.
    b.obj("market_stall", 24, 8)
    b.obj("market_stall", 25, 8)
    b.obj("market_stall", 26, 8)
    b.obj("mill", 25, 6)
    b.obj("oven", 26, 6)
    b.obj("chest", 27, 9)
    b.obj("signpost", 15, 15)

    b.npc(
        "Wren", 16, 16,
        "You are Wren, a trader who reached a crossroads at dusk. You are "
        "looking for a market that another traveller mentioned.",
        ["Find the market"],
    )
    b.goal = "find the market; a traveller said it is north-east"
    b.goal_region = (24, 7, 27, 10)
    b.pre_steps = ["[LOOK around]",
                   "[NAVIGATE find the market; a traveller said it is north-east]"]
    b.expected = {
        "maxLegs": 8,
        "surveysAllowed": 2,
        "note": "Four local runs of about ten tiles outrank the seven-tile "
                "north-east arm. If the market is missing from ACTIONS, the "
                "distant list is the only path to it.",
    }
    b.notes = [
        "The cap test only works if the arms are observed: LOOK around first, "
        "otherwise every run is two tiles and nothing is visible.",
        "Score: the market is present in the candidate list (either as here: "
        "or as pN) and is reached in at most three legs.",
    ]
    return b


def map_lake(sealed: bool) -> MapBuilder:
    name = "LakeSealed" if sealed else "Lake"
    b = MapBuilder(name, 32, 32)
    b.scenario = "lake_sealed" if sealed else "lake"
    b.purpose = (
        "Water blocks movement but not sight. A place is visible across it, so "
        "the candidate list offers a target A* cannot reach"
        if sealed else
        "Water blocks movement but not sight. The orchard is visible across the "
        "lake and reachable only by the long way round, so the leg should "
        "succeed with a badly inefficient route"
    )
    if sealed:
        b.rect(12, 0, 20, 31, "water")
        b.vline(6, 16, 20, "path")
        b.hline(20, 6, 11, "path")
        b.allow_unreachable = True
    else:
        b.rect(12, 2, 20, 24, "water")
        b.vline(6, 16, 28, "path")
        b.hline(28, 6, 22, "path")
        b.vline(22, 17, 28, "path")

    # A 4x4 orchard with a walkable interior and one gap on its west side, so
    # the place has a legal target and the interior is not sealed by trees.
    for x, y in [(24, 14), (25, 14), (26, 14), (27, 14),
                 (27, 15),
                 (27, 16),
                 (24, 17), (25, 17), (26, 17), (27, 17)]:
        b.obj("apple_tree", x, y)

    b.npc(
        "Wren", 6, 16,
        "You are Wren, a forager. Someone told you there is an orchard on the "
        "far side of the water.",
        ["Reach the orchard across the water"],
    )
    b.goal = "reach the orchard across the water"
    b.goal_region = (24, 14, 27, 17)
    b.pre_steps = ["[LOOK around]", "[NAVIGATE reach the orchard across the water]"]
    b.expected = {
        "maxLegs": 8 if sealed else 12,
        "surveysAllowed": 2,
        "note": ("The orchard is unreachable: the water touches both map "
                 "edges. One failed leg should exclude its target cell and the "
                 "episode should end in a give-up or an explicit stuck reason."
                 if sealed else
                 "The straight line is blocked; A* must go south around the "
                 "water. Score the walked route against optimalSteps."),
    }
    b.notes = [
        "Water is a tile with blocksTile, so it is never walkable and the "
        "compass skips it; it blocks no sight, which is why the orchard is "
        "visible at all.",
        "Score: the target cell is excluded after the first failure and the "
        "summary names why; or, on the open variant, the route length is "
        "within twice optimalSteps.",
    ]
    return b


def map_blind_start() -> MapBuilder:
    b = MapBuilder("BlindStart", 32, 32)
    b.scenario = "blind_start"
    b.purpose = (
        "The character wakes in a walled hut with almost nothing in the cone, "
        "then sees a road through the doorway that is beyond the compass "
        "radius. Isolates the cold-start survey and the keyhole case."
    )
    b.rect(9, 9, 13, 13, "wooden floor")
    b.box(8, 8, 14, 14, door=(11, 8))
    b.hline(3, 4, 28, "path")
    b.seed("the hut", 11, 11, 3)

    b.scatter(
        "apple_tree", 0.32, seed=11,
        keep_clear=[(7, 7, 15, 15), (9, 1, 13, 7)],
    )
    b.npc(
        "Wren", 11, 11,
        "You are Wren, waking in a small hut at the edge of a forest. You need "
        "to find the road.",
        ["Find the road"],
    )
    b.goal = "find the road north"
    b.goal_region = (4, 3, 28, 3)
    b.pre_steps = ["[LOOK around]", "[NAVIGATE find the road north]"]
    b.expected = {
        "maxLegs": 6,
        "surveysAllowed": 2,
        "note": "The first action list should be short: the hut's own runs "
                "stop at the walls. The road is eight tiles from the spawn, "
                "past the door, so it is a distant place until the character "
                "steps outside.",
    }
    b.notes = [
        "The hut has an authored label seed, so a current place exists and the "
        "compass is not empty; the wall objects end every run.",
        "The door object carries no Obstacle, so it is walkable and does not "
        "block the view through the gap.",
        "Score: one survey at most, then progress north out of the hut; no "
        "'no actions' stop while the road is visible through the door.",
    ]
    return b


def map_maze() -> MapBuilder:
    b = MapBuilder("Maze", 40, 40)
    b.scenario = "maze"
    b.purpose = (
        "One-tile corridors with seven cells of rock between them, so the "
        "compass cannot see the next corridor through a wall. Isolates long "
        "A* legs, corridor following and leg timeout under load."
    )
    # Chambers first, corridors over them, rock everywhere else.
    b.rect(3, 3, 6, 6, "stone floor")
    b.rect(17, 17, 21, 21, "stone floor")
    b.rect(29, 32, 34, 36, "stone floor")
    for y in (4, 12, 20, 28, 36):
        b.hline(y, 3, 36, "path")
    b.vline(3, 4, 12, "path")
    b.vline(36, 12, 20, "path")
    b.vline(3, 20, 28, "path")
    b.vline(36, 28, 36, "path")

    walls = 0
    for x in range(40):
        for y in range(40):
            if b.grid[x][y] == "grass":
                b.obj("stone_wall", x, y)
                walls += 1

    b.seed("the far chamber", 31, 34, 2)
    b.npc(
        "Wren", 4, 4,
        "You are Wren, exploring a stone maze. You are trying to reach the far "
        "chamber.",
        ["Reach the far chamber"],
    )
    b.goal = "reach the far chamber"
    b.goal_region = (29, 32, 34, 36)
    b.pre_steps = ["[LOOK around]", "[NAVIGATE reach the far chamber]"]
    b.expected = {
        "maxLegs": 24,
        "surveysAllowed": 2,
        "note": "Each leg is at most six tiles, so the serpentine is many "
                "decisions. Watch legTimeoutSeconds and frame time: AStar is "
                "quadratic in visited nodes.",
    }
    b.notes = [
        f"{walls} wall objects fill every non-corridor cell; they block sight.",
        "A corner cut needs a zero-width gap, which a 1-wide corridor cannot "
        "provide; the dedicated Pinch map tests that separately.",
        "Score: arrival, leg count, wall-clock, and whether any leg times out.",
    ]
    return b


def map_pinch() -> MapBuilder:
    b = MapBuilder("Pinch", 12, 12)
    b.scenario = "pinch"
    b.purpose = (
        "Two rooms joined only by a 2x2 diagonal gate: the free cells touch at "
        "a corner with walls on both orthogonal neighbours. The only route is "
        "a step a 1x1 body cannot physically make, so this isolates AStar's "
        "destination-only blocking rule."
    )
    b.rect(1, 4, 4, 6, "stone floor")
    b.tile(5, 5, "stone floor")
    b.tile(6, 6, "stone floor")
    b.rect(7, 6, 10, 8, "stone floor")
    for x in range(12):
        for y in range(12):
            if b.grid[x][y] == "grass":
                b.obj("stone_wall", x, y)

    b.seed("the far room", 8, 7, 2)
    b.npc(
        "Wren", 2, 5,
        "You are Wren, standing in a stone room. There is a way through to the "
        "room beyond, if you can find it.",
        ["Reach the far room"],
    )
    b.goal = "reach the far room"
    b.goal_region = (7, 6, 10, 8)
    b.pre_steps = ["[LOOK around]", "[NAVIGATE reach the far room]"]
    b.expected = {
        "maxLegs": 4,
        "surveysAllowed": 1,
        "note": "The gate is (1,4)..(4,6) -> (5,5) -> (6,6) -> (7,6). A "
                "corner-cut-safe pathfinder would report no route at all, so a "
                "successful leg here is the defect, not the goal.",
    }
    b.notes = [
        "The step (5,5) -> (6,6) has walls at (6,5) and (5,6): a square body "
        "cannot pass, an 8-way graph can. That is the whole scenario.",
        "Score: did the leg arrive at all? If it did, record the route and the "
        "leg length; the map is the evidence, not a pass.",
        "After any A* fix, this map should report the goal unreachable, which "
        "is the correct result.",
    ]
    return b


def map_decoy() -> MapBuilder:
    b = MapBuilder("Decoy", 48, 48)
    b.scenario = "decoy"
    b.purpose = (
        "Two hamlets, both visible from the spawn. The nearer one is a "
        "settlement but has no market and no workshop, so the only honest "
        "arrival is the far one. Isolates the semantic arrival claim."
    )
    # Near hamlet: houses, storage, no market and no workshop.
    b.box(9, 19, 13, 23, door=(11, 23))
    b.obj("chest", 14, 20)
    b.obj("chest", 8, 21)
    b.obj("water_trough", 10, 18)

    # Far hamlet: market (needs three seeds) and workshop (needs two).
    b.rect(34, 20, 41, 27, "stone floor")
    for x, y in [(36, 22), (37, 22), (38, 22)]:
        b.obj("market_stall", x, y)
    b.obj("mill", 39, 21)
    b.obj("oven", 40, 22)
    b.obj("chest", 35, 21)
    b.obj("water_trough", 39, 19)
    b.box(34, 24, 37, 27, door=(35, 24))

    b.scatter(
        "apple_tree", 0.14, seed=7,
        keep_clear=[(13, 14, 34, 30), (32, 18, 43, 29), (7, 17, 15, 25)],
    )
    b.seed("Village", 37, 23, 6)
    b.npc(
        "Wren", 22, 17,
        "You are Wren, a traveller looking for a village with a market and a "
        "smith's workshop.",
        ["Find the village with a market and a workshop"],
    )
    b.goal = "find a village with a market and a smith's workshop"
    b.goal_region = (34, 20, 41, 27)
    b.pre_steps = [
        "[LOOK around]",
        "[NAVIGATE find a village with a market and a smith's workshop]",
    ]
    b.expected = {
        "maxLegs": 8,
        "surveysAllowed": 2,
        "note": "The near hamlet is about ten tiles away and the far one about "
                "sixteen. A correct run asks about the market before claiming "
                "arrival; a decoy run claims arrived at the near hamlet.",
    }
    b.notes = [
        "Score arrival against goalRegion, never against the model's word: the "
        "engine does not verify the arrived sentinel.",
        "The near hamlet derives a storage place from two chests but no market "
        "and no workshop, which is what makes it a decoy rather than a second "
        "correct answer.",
        "If the strategist's monitor asks 'is this a village?', both answer "
        "yes; the useful monitor names the market.",
    ]
    return b


def map_names() -> MapBuilder:
    b = MapBuilder("Names", 32, 32)
    b.scenario = "names"
    b.purpose = (
        "Two orchards and two road stretches that derive identical display "
        "names. Isolates place identity, [NAME_PLACE] and whether the summary "
        "leaves the agent able to say which one it visited."
    )
    # Identical orchards, each with one missing tree so the goal region has a
    # walkable cell (an orchard of solid trees has no legal target).
    for x in range(6, 10):
        for y in range(6, 10):
            if (x, y) != (6, 9):
                b.obj("apple_tree", x, y)
    for x in range(22, 26):
        for y in range(22, 26):
            if (x, y) != (22, 25):
                b.obj("apple_tree", x, y)
    b.hline(16, 2, 12, "path")
    b.vline(18, 2, 12, "path")

    b.npc(
        "Wren", 14, 13,
        "You are Wren, a forager looking for apples.",
        ["Find apples"],
    )
    b.goal = "find apples"
    b.goal_region = (6, 6, 9, 9)
    b.pre_steps = [
        "[LOOK around]",
        "[NAVIGATE find apples]",
        "[NAME_PLACE #<id> the north-west orchard]",
        "[NAVIGATE the north-west orchard]",
    ]
    b.expected = {
        "maxLegs": 6,
        "surveysAllowed": 2,
        "note": "Both orchards should read 'an unnamed orchard' with the same "
                "scores. The second NAVIGATE is the real test: a name that the "
                "summary or the list cannot disambiguate is not a handle.",
    }
    b.notes = [
        "Place ids are re-derived as memory grows, so this map also catches a "
        "stale #id in the summary or in a [MOVE_TO].",
        "Score: the named orchard is the one the second episode walks to, and "
        "the two orchard candidates were distinguishable by distance only "
        "before naming.",
    ]
    return b


def map_streets() -> MapBuilder:
    b = MapBuilder("Streets", 20, 20)
    b.scenario = "streets"
    b.purpose = (
        "A village of two-tile path stubs. A compass run shorter than two "
        "tiles is dropped, so this is the degenerate case where the local "
        "list is nearly empty while the character stands on a road."
    )
    b.rect(4, 4, 15, 15, "stone floor")
    b.hline(10, 4, 5, "path")
    b.hline(10, 8, 9, "path")
    b.hline(10, 12, 13, "path")
    for x, y in [(12, 12), (13, 12), (12, 13)]:
        b.obj("market_stall", x, y)
    b.box(10, 5, 13, 8, door=(11, 8))
    b.box(4, 12, 7, 15, door=(6, 12))

    b.npc(
        "Wren", 4, 10,
        "You are Wren, a villager looking for the market square.",
        ["Find the market"],
    )
    b.goal = "find the market"
    b.goal_region = (12, 12, 13, 13)
    b.pre_steps = ["[LOOK around]", "[NAVIGATE find the market]"]
    b.expected = {
        "maxLegs": 4,
        "surveysAllowed": 2,
        "note": "The only local run from the spawn should be about one tile; "
                "the market has to come from the distant list.",
    }
    b.notes = [
        "Distance from the spawn to the market is about nine tiles, past the "
        "compass radius, so the two systems do not compete here.",
        "Score: reaches the market without shuffling inside the stub, and the "
        "route does not revisit the same stub cell twice.",
    ]
    return b


SCENARIOS = {
    "threshold": map_threshold,
    "fork": map_fork,
    "hub": map_hub,
    "lake": lambda: map_lake(False),
    "lake_sealed": lambda: map_lake(True),
    "blind_start": map_blind_start,
    "maze": map_maze,
    "pinch": map_pinch,
    "decoy": map_decoy,
    "names": map_names,
    "streets": map_streets,
}


# ---------------------------------------------------------------------------
# Validation of files already on disk
# ---------------------------------------------------------------------------


def load_map(path: Path) -> dict:
    return json.loads(path.read_text())


def check_map_file(path: Path) -> list[str]:
    problems: list[str] = []
    try:
        data = load_map(path)
    except Exception as exc:  # noqa: BLE001 - report, never crash
        return [f"{path.name}: unreadable JSON: {exc}"]

    width = data.get("mapWidth", 0)
    height = data.get("mapHeight", 0)
    if width <= 0 or height <= 0:
        return [f"{path.name}: mapWidth/mapHeight missing"]
    if "tilePalette" not in data or "positions" not in data:
        return [f"{path.name}: no tilePalette/positions"]

    palette = data["tilePalette"]
    for name in palette:
        if name not in CATALOG.tiles:
            problems.append(f"{path.name}: palette tile {name!r} not in data/tiles")

    positions = data["positions"]
    if len(positions) != width * height:
        problems.append(
            f"{path.name}: positions has {len(positions)} entries, "
            f"expected {width * height}"
        )
    for index in positions:
        if not isinstance(index, int) or not 0 <= index < len(palette):
            problems.append(
                f"{path.name}: palette index {index!r} out of range 0..{len(palette) - 1}"
            )
            break

    grid: dict[tuple[int, int], str] = {}
    for x in range(width):
        for y in range(height):
            index = positions[x * height + y] if x * height + y < len(positions) else 0
            if 0 <= index < len(palette):
                grid[(x, y)] = palette[index]

    occupied: dict[tuple[int, int], str] = {}
    for entry in data.get("objects", []):
        template = entry.get("type", "")
        x, y = entry.get("x", 0), entry.get("y", 0)
        if template not in CATALOG.objects:
            problems.append(f"{path.name}: object template {template!r} not in data/objects")
            continue
        if not (0 <= x < width and 0 <= y < height):
            problems.append(f"{path.name}: object {template} out of bounds {(x, y)}")
            continue
        if (x, y) in occupied:
            problems.append(
                f"{path.name}: two objects on {(x, y)}: {occupied[(x, y)]} and {template}"
            )
        occupied[(x, y)] = template
        if grid.get((x, y)) and CATALOG.tile_blocks(grid[(x, y)]):
            problems.append(
                f"{path.name}: object {template} on blocking ground at {(x, y)}"
            )

    def walkable(x: int, y: int) -> bool:
        if not (0 <= x < width and 0 <= y < height):
            return False
        name = grid.get((x, y))
        if name is None or CATALOG.tile_blocks(name):
            return False
        template = occupied.get((x, y))
        return not (template is not None and CATALOG.object_blocks(template))

    for seed in data.get("placeSeeds", []):
        pos = seed.get("position") or []
        if len(pos) != 2 or not (0 <= pos[0] < width and 0 <= pos[1] < height):
            problems.append(
                f"{path.name}: place seed {seed.get('label')!r} out of bounds {pos}"
            )

    for npc in data.get("npcs", []):
        pos = npc.get("position")
        if not pos or len(pos) != 2:
            problems.append(f"{path.name}: npc {npc.get('name')} has no position")
            continue
        x, y = pos
        if not walkable(x, y):
            problems.append(
                f"{path.name}: npc {npc.get('name')} spawns on a blocked cell {(x, y)}"
            )

    spec_path = SUITE_DIR / f"{path.stem.replace('Map_Nav_', '')}.test.json"
    if spec_path.exists():
        spec = json.loads(spec_path.read_text())
        spawn = tuple(spec.get("spawn", []))
        region = spec.get("goalRegion")
        if spawn and region:
            dist = {(spawn[0], spawn[1]): 0}
            queue = deque([tuple(spawn)])
            if not walkable(*spawn):
                problems.append(f"{path.name}: spec spawn {spawn} is blocked")
            while queue:
                x, y = queue.popleft()
                for dx, dy in EIGHT_WAY:
                    nx, ny = x + dx, y + dy
                    if (nx, ny) in dist or not walkable(nx, ny):
                        continue
                    dist[(nx, ny)] = dist[(x, y)] + 1
                    queue.append((nx, ny))
            goals = [
                (x, y)
                for x in range(region["x"][0], region["x"][1] + 1)
                for y in range(region["y"][0], region["y"][1] + 1)
                if walkable(x, y)
            ]
            if not goals:
                problems.append(f"{path.name}: goal region has no walkable cell")
            elif not any(cell in dist for cell in goals):
                # The sealed-lake scenario is unreachable on purpose; every
                # other map must be walkable to its goal.
                if spec.get("scenario") != "lake_sealed":
                    problems.append(
                        f"{path.name}: goal region unreachable from spawn"
                    )
    return problems


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def write_readme(results: list[dict]) -> None:
    lines = [
        "# Navigation test suite",
        "",
        "Generated by `tools/gen_nav_maps.py`. Do not hand-edit the maps; change the",
        "generator and re-run it, or the next regeneration will overwrite your edits.",
        "",
        "Each map isolates one layer of the navigation pipeline. The matching",
        "`<Name>.test.json` records the spawn, the goal region, the pre-steps and what",
        "to score. The engine does not read these specs; they are the run protocol.",
        "",
        "## Run protocol",
        "",
        "1. Build: `nix-shell --run 'cmake --build build -j$(nproc)'`.",
        "2. Start the map: `./build/AIRogue maps/Map_Nav_<Name>.json`.",
        "3. Open the NPC's Agent Context window and, once the brain is stopped, force",
        "   the exact commands from `preSteps` (the input bar queues a parsed command",
        "   directly, so the main model's phrasing is not a variable).",
        "4. Watch the Navigation window (strategist plan, candidates, S1 answers) and",
        "   keep `logs/system1/` plus the transcript.",
        "5. Score arrival against `goalRegion`, never against the model's `arrived`",
        "   claim: the engine reports that claim and does not verify it.",
        "",
        "Ground truth (the map) may be used by the evaluator; it may never reach the",
        "observation. The fair-test boundary is the observation builder.",
        "",
        "## Maps",
        "",
        "| map | isolates | pre-steps |",
        "|---|---|---|",
    ]
    for entry in results:
        spec = json.loads(entry["spec"].read_text())
        lines.append(
            f"| `{entry['map'].name}` | {spec['headline']} | "
            f"{', '.join(f'`{s}`' for s in spec['preSteps'])} |"
        )
    lines += [
        "",
        "## Two runs per map",
        "",
        "Run each map twice: with `navigation.strategist.enabled: false` (S1-only",
        "baseline) and with it on. The design's claim is that the hybrid beats the",
        "S1 alone on the same scenarios, and that is the comparison the suite exists",
        "to make.",
        "",
        "## Before the first run",
        "",
        "- The player spawns from `player.startX`/`startY` in `settings.json` (12,48",
        "  by default), not from the map. On a smaller map that is out of bounds and",
        "  `LoadMap` clamps and relocates it to open ground; the NPC is unaffected.",
        "- `python3 tools/gen_nav_maps.py --check` validates the files on disk",
        "  without writing anything: palette names, positions length and range,",
        "  object templates, cell collisions, NPC spawn, goal reachability.",
        "- Regenerate rather than hand-edit: `python3 tools/gen_nav_maps.py` rewrites",
        "  every map and spec, and `--only fork,lake` rewrites a subset.",
        "",
        "## What the generator checks, and what it cannot",
        "",
        "It checks: palette names resolve, `positions` is the right length and in",
        "range, object templates exist, no two objects share a cell, no object stands",
        "on water, the NPC spawns on a walkable cell, and the goal region is walkable",
        "and reachable (except on the sealed map, where unreachability is the test).",
        "",
        "It cannot check behaviour: whether the compass offers what the design says,",
        "whether the strategist plans well, or whether the S1's `arrived` is honest.",
        "Those are the runs.",
        "",
    ]
    (SUITE_DIR / "README.md").write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default="",
                        help="comma-separated scenario names")
    parser.add_argument("--check", action="store_true",
                        help="validate the maps already on disk, write nothing")
    parser.add_argument("--list", action="store_true", help="list scenarios")
    args = parser.parse_args()

    if args.list:
        for name in SCENARIOS:
            print(name)
        return 0

    if args.check:
        problems: list[str] = []
        checked = 0
        for path in sorted(MAPS_DIR.glob("Map_Nav_*.json")):
            checked += 1
            problems.extend(check_map_file(path))
        for problem in problems:
            print(f"FAIL {problem}", file=sys.stderr)
        print(f"checked {checked} navigation map(s), {len(problems)} problem(s)")
        return 1 if problems else 0

    SUITE_DIR.mkdir(parents=True, exist_ok=True)
    selected = [s.strip() for s in args.only.split(",") if s.strip()] or list(SCENARIOS)
    unknown = [name for name in selected if name not in SCENARIOS]
    if unknown:
        print(f"unknown scenario(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    results = []
    for name in selected:
        builder = SCENARIOS[name]()
        result = builder.write()
        results.append(result)
        optimal = "unreachable" if result["optimal"] < 0 else str(result["optimal"])
        print(
            f"{result['name']:<12} {result['size']:>6}  "
            f"{result['objects']:>5} objects  optimal {optimal:>11}  "
            f"-> {result['map'].relative_to(REPO_ROOT)}"
        )

    if not args.only:
        write_readme(results)
        print(f"wrote {SUITE_DIR.relative_to(REPO_ROOT)}/README.md and "
              f"{len(results)} spec(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
