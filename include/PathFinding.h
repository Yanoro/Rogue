#pragma once
#include <vector>
#include <flecs.h>
#include "Components.h"

std::vector<GamePosition> AStar(flecs::world &ecs, const GamePosition &startPos, const GamePosition &endPos);
