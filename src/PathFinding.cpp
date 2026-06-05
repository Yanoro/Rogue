#include "PathFinding.h"
#include "Game.h"
#include "Map.h"
#include <algorithm>
#include <cmath>

const unsigned int DIAGONAL_COST = 14;
const unsigned int ORTHOGONAL_COST = 10;

struct Node {
  GamePosition position;
  unsigned int fCost;
  unsigned int gCost;
  Node *parent;
};

std::vector<GamePosition> GetNeighbours(GamePosition p) {
  static const int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};

  std::vector<GamePosition> neighbors;
  for (int i = 0; i < 8; ++i) {
    neighbors.push_back({p.x + dx[i], p.y + dy[i]});
  }
  return neighbors;
}

bool isDiagonal(const GamePosition &pos1, const GamePosition &pos2) {
  return abs(pos1.x - pos2.x) == 1 && abs(pos1.y - pos2.y);
}

unsigned int GetPathLength(const Node *currNode) {
  if (currNode->parent == nullptr) {
    return 0;
  }
  unsigned int cost = isDiagonal(currNode->position, currNode->parent->position)
                          ? DIAGONAL_COST
                          : ORTHOGONAL_COST;
  return cost + GetPathLength(currNode->parent);
}

Node *nodeHasPosition(const std::vector<Node *> &nodeVector,
                      const GamePosition &pos) {
  for (Node *currNode : nodeVector) {
    if (currNode->position == pos) {
      return currNode;
    }
  }
  return nullptr;
}

unsigned int OctileDistance(const GamePosition &pos1,
                            const GamePosition &pos2) {
  int dx = abs(pos1.x - pos2.x);
  int dy = abs(pos1.y - pos2.y);
  return (ORTHOGONAL_COST * abs(dx - dy)) + (DIAGONAL_COST * std::min(dx, dy));
}

unsigned int getFCost(const Node &node, const GamePosition &endPos) {
  unsigned int hCost = OctileDistance(node.position, endPos);
  unsigned int gCost = node.gCost;

  return hCost + gCost;
}

// TODO: Use priority_heap to make the min_element search faster
// The ECS lookup in the middle of the code is really slow, placing it before the loop
// will make it much faster

std::vector<GamePosition> AStar(Map *map, const GamePosition &startPos,
                                const GamePosition &endPos) {
  if (startPos == endPos) {
    return {};
  }

  std::vector<Node *> openNodes;
  std::vector<Node *> closedNodes;

  Node *startNode = new Node(startPos, 0, 0, nullptr);
  Node *currNode = nullptr;

  openNodes.push_back(startNode);

  std::vector<GamePosition> path;

  while (true) {

    auto it = std::min_element(openNodes.begin(), openNodes.end(),
                               [](const Node *nodeA, const Node *nodeB) {
                                 return nodeA->fCost < nodeB->fCost;
                               });

    // Exhausted all nodes, path doesn't exist
    if (it == openNodes.end())
      break;

    currNode = *it;
    openNodes.erase(it);
    closedNodes.push_back(currNode);

    if (currNode->position == endPos) {
      while (currNode->parent != nullptr) {
        path.push_back(currNode->position);
        currNode = currNode->parent;
      }
      path.push_back(currNode->position);
      std::reverse(path.begin(), path.end());
      break;
    }

    std::vector<GamePosition> possiblePositions =
        GetNeighbours(currNode->position);
    std::vector<GamePosition> neighbourPositions;

    for (const GamePosition& pos : possiblePositions) {
      if (map->IsInBounds(pos.x, pos.y)) {
        Tile* t = map->GetTile(pos.x, pos.y);
        if (t && !t->blocksTile && nodeHasPosition(closedNodes, pos) == nullptr) {
          neighbourPositions.push_back(pos);
        }
      }
    }

    for (const auto &neighbourPos : neighbourPositions) {
      Node *neighbourNode = nodeHasPosition(openNodes, neighbourPos);
      unsigned int newFCost = 0;
      unsigned int newGCost =
          currNode->gCost + OctileDistance(currNode->position, neighbourPos);
      if (neighbourNode == nullptr) {
        Node *newNode = new Node(neighbourPos, newFCost, newGCost, currNode);
        newNode->fCost = getFCost(*newNode, endPos);
        openNodes.push_back(newNode);
      } else if (newGCost < neighbourNode->gCost) {
        neighbourNode->parent = currNode;
        neighbourNode->gCost = newGCost;
        neighbourNode->fCost = getFCost(*neighbourNode, endPos);
      }
    }
  }

  for (auto node : openNodes) {
    delete node;
  }
  for (auto node : closedNodes) {
    delete node;
  }
  return path;
}
