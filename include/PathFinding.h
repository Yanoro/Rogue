#pragma once
#include <vector>
#include "Components.h"

class Map;

std::vector<GamePosition> AStar(Map *map, const GamePosition &startPos, const GamePosition &endPos);
