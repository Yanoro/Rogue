#include "Game.h"
#include <algorithm>
#include <cmath>

const unsigned int DIAGONAL_COST = 14;
const unsigned int ORTHOGONAL_COST = 10; 

struct Node {
  GamePosition position;
  unsigned int fCost;
  Node *parent;
};

std::vector<GamePosition> GetNeighbours(GamePosition p) {
  static const int dx[] = {-1,  0,  1, -1, 1, -1, 0, 1};
  static const int dy[] = {-1, -1, -1,  0, 0,  1, 1, 1};

  std::vector<GamePosition> neighbors;
  for(int i = 0; i < 8; ++i) {
    neighbors.push_back({p.x + dx[i], p.y + dy[i]});
  }
  return neighbors;
}

bool isDiagonal(const GamePosition &pos1, const GamePosition &pos2) {
  return abs(pos1.x - pos2.x) == 1 && abs(pos1.y - pos2.y);
}

unsigned int GetPathLength(const Node *currNode) {
  if (currNode->parent == nullptr) { return 0; }
  unsigned int cost = isDiagonal(currNode->position, currNode->parent->position) ? DIAGONAL_COST : ORTHOGONAL_COST;
  return cost + GetPathLength(currNode->parent);
}

Node *nodeHasPosition(const std::vector<Node*> &nodeVector, const GamePosition &pos) {
  for (Node* currNode : nodeVector) {
    if (currNode->position == pos) { return currNode; }
  }
  return nullptr;
}

unsigned int OctileDistance(const GamePosition &pos1, const GamePosition &pos2) {
  int dx = abs(pos1.x - pos2.x);
  int dy = abs(pos1.y - pos2.y);
  return (ORTHOGONAL_COST * abs(dx - dy)) + (DIAGONAL_COST * std::min(dx, dy));
}

unsigned int getFCost(const Node &node, const GamePosition &endPos) {
  unsigned int hCost = OctileDistance(node.position, endPos);
  unsigned int gCost = GetPathLength(&node);

  return hCost + gCost;
} 

//TODO: Currently calling GetPathLength is way inneficient (Quadratic), it's better if i
// had stored gCost on Node  

GamePosition AStar(flecs::world &ecs, const GamePosition &startPos, const GamePosition &endPos) {
  if (startPos == endPos) { return startPos; }

  std::vector<Node*> openNodes;
  std::vector<Node*> closedNodes;

  Node *startNode = new Node(startPos, 0, nullptr);
  Node *currNode = nullptr;

  openNodes.push_back(startNode);

  while (true) {

    auto it = std::min_element(openNodes.begin(), openNodes.end(), 
                               [](const Node *nodeA, const Node *nodeB) 
                               { return nodeA->fCost < nodeB->fCost; });

    if (it == openNodes.end()) break; 

    currNode = *it;
    openNodes.erase(it);
    closedNodes.push_back(currNode);

    if (currNode->position == endPos) { break; }

    std::vector<GamePosition> possiblePositions = GetNeighbours(currNode->position);
    std::vector<GamePosition> neighbourPositions;

    ecs.filter_builder<Tile, GamePosition>()
      .without<BlocksTile>()
      .build()
      .each([&possiblePositions, &closedNodes, &neighbourPositions](const Tile &t, const GamePosition &pos) {
        if (std::ranges::find(possiblePositions, pos) != possiblePositions.end() and
          nodeHasPosition(closedNodes, pos) == nullptr) {
          neighbourPositions.push_back(pos);
        }
      });

    for (const auto &neighbourPos : neighbourPositions) {
      Node *neighbourNode = nodeHasPosition(openNodes, neighbourPos);
      Node *newNode = new Node(neighbourPos, 0, currNode); 

      newNode->fCost = getFCost(*newNode, endPos); 
      if (neighbourNode == nullptr) {
        openNodes.push_back(newNode);
      }
      else {
        unsigned int newDistance = GetPathLength(newNode);
        unsigned int oldDistance = GetPathLength(neighbourNode);
        if (newDistance < oldDistance) {
          neighbourNode->parent = currNode;
          neighbourNode->fCost = newNode->fCost;
        }
      }
    }
  }

  GamePosition resPos;
  while (currNode->parent->parent != nullptr) { currNode = currNode->parent; }
  resPos = currNode->position;

  for (auto node : openNodes) { delete node; }
  for (auto node : closedNodes) { delete node; } 

  return resPos;
}
