#include "Game.h"
#include <algorithm>
#include <cmath>

struct Node {
  Position position;
  unsigned int fCost;
  Node *parent;
};

std::vector<Position> GetNeighbours(Position p) {
  static const int dx[] = {-1,  0,  1, -1, 1, -1, 0, 1};
  static const int dy[] = {-1, -1, -1,  0, 0,  1, 1, 1};

  std::vector<Position> neighbors;
  for(int i = 0; i < 8; ++i) {
    neighbors.push_back({p.x + dx[i], p.y + dy[i]});
  }
  return neighbors;
}

unsigned int GetPathLength(const Node *currNode) {
  if (currNode->parent == nullptr) { return currNode->fCost; }
  return currNode->fCost + GetPathLength(currNode->parent);
}

Node *nodeHasPosition(std::vector<Node> &nodeVector, const Position &pos) {
  for (Node &currNode : nodeVector) {
    if (samePosition(currNode.position, pos)) { return &currNode; }
  }
  return nullptr;
}

unsigned int manhattanDistance(const Position &pos1, const Position &pos2) {
  return std::max(abs(pos1.x - pos2.x), abs(pos1.y - pos2.y));
}

unsigned int getFCost(const Node &node, const Position &endPos) {
  unsigned int hCost = manhattanDistance(node.position, endPos);
  unsigned int gCost = GetPathLength(&node);

  return hCost + gCost;
}

Position AStar(flecs::world &ecs, const Position &startPos, const Position &endPos) {
  std::vector<Node> openNodes;
  std::vector<Node> closedNodes;
  
  Node startNode(startPos, 0, nullptr);
  Node currNode;

  openNodes.push_back(startNode);
  while (true) {
    auto it = std::min_element(openNodes.begin(), openNodes.end(), 
    [](const Node &nodeA, const Node &nodeB) 
       { return nodeA.fCost < nodeB.fCost; });

    currNode = std::move(*it);
    openNodes.erase(it);
    closedNodes.push_back(currNode);

    if (samePosition(currNode.position, endPos)) { break; }

    std::vector<Position> possiblePositions = GetNeighbours(currNode.position);
    std::vector<Position> neighbourPositions;

    ecs.filter_builder<Tile, Position>()
      .without<BlocksTile>()
      .build()
      .each([&possiblePositions, &closedNodes, &neighbourPositions](const Position &pos) {
        if (std::ranges::find(possiblePositions, pos) != possiblePositions.end() and
            nodeHasPosition(closedNodes, pos) == nullptr) {
        neighbourPositions.push_back(pos);
      }
                                                                  });

    for (const auto &neighbourPos : neighbourPositions) {
      //Node neighbourNode = Node(neighbourPos, getFCost(neighbourPos, startPos, endPos), )
      Node *neighbourNode = nodeHasPosition(openNodes, neighbourPos);
      Node newNode  = Node(neighbourPos, 0, &currNode);
      newNode.fCost = getFCost(newNode, endPos); 
      if (neighbourNode == nullptr) {
        openNodes.push_back(newNode);
      }
      else {
        unsigned int newDistance = GetPathLength(&newNode);
        unsigned int oldDistance = GetPathLength(neighbourNode);
        if (newDistance < oldDistance) {
          neighbourNode->parent = &currNode;
          neighbourNode->fCost = newNode.fCost;
        }
      }
    }
  }

  Node *endNode = &currNode;
  while (endNode->parent != nullptr) { endNode = endNode->parent; }
  return endNode->position;

}
