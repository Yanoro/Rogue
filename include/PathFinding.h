#pragma once
#include <vector>
#include "Components.h"

class Map;

// Shortest walkable route from startPos to endPos, excluding the start cell and
// including the end cell. Empty when startPos == endPos, when the destination is
// not walkable, or when no route exists.
//
// Walkability follows Map::IsBlocked, plus one rule about diagonals: every cell
// is solid, so a diagonal step also requires both cells sharing its corner to be
// free. A route therefore never cuts a corner -- neither between two objects
// that touch corner to corner, nor past a single wall the actor is walking
// alongside.
std::vector<GamePosition> AStar(Map *map, const GamePosition &startPos, const GamePosition &endPos);
