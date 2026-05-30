#include "raylib-cpp.hpp"
#include <vector>
#include <cmath>
#include <stdlib.h>
#include <algorithm>
#include <iostream>
#include <thread>
#include <chrono>

struct GamePosition {
    int x, y;
    
    bool operator==(const GamePosition& other) const {
        return x == other.x && y == other.y;
    }
};

struct Node {
  GamePosition position;
  unsigned int fCost;
  unsigned int gCost;
  Node *parent;
  
  Node() : position{0,0}, fCost(0), gCost(0), parent(nullptr) {}
  Node(GamePosition p, unsigned int f, unsigned int g, Node* par) : position(p), fCost(f), gCost(g), parent(par) {}
  
  // Helper to clone a node deeply enough for snapshots (parent is just a reference, that's okay for now)
  Node* clone() const {
      return new Node(position, fCost, gCost, parent);
  }
};

struct Tile {};
struct BlocksTile {};

// --- Mocking Flecs for Visualization ---
struct MockWorld {
    int width = 20;
    int height = 20;
    std::vector<bool> blocked;

    MockWorld() {
        blocked.resize(width * height, false);
    }
    
    void Reset() {
        std::fill(blocked.begin(), blocked.end(), false);
    }
    
    void AddWall(int x, int y) {
        if(x >= 0 && x < width && y >= 0 && y < height) blocked[y * width + x] = true;
    }
    
    void AddRect(int x, int y, int w, int h) {
        for(int i=0; i<w; i++) {
            for(int j=0; j<h; j++) {
                AddWall(x+i, y+j);
            }
        }
    }

    bool isBlocked(int x, int y) {
        if(x < 0 || x >= width || y < 0 || y >= height) return true;
        return blocked[y * width + x];
    }

    // Mock Filter Builder Chain
    struct Filter {
        MockWorld* world;
        bool excludeBlocked;

        template<typename Func>
        void each(Func f) {
            for(int y=0; y<world->height; y++) {
                for(int x=0; x<world->width; x++) {
                    if (excludeBlocked && world->isBlocked(x, y)) continue;
                    f(Tile{}, GamePosition{x, y});
                }
            }
        }
    };

    struct FilterBuilder {
        MockWorld* world;
        bool excludeBlocked = false;

        template<typename T>
        FilterBuilder& without() {
            excludeBlocked = true; 
            return *this;
        }

        Filter build() {
            return Filter{world, excludeBlocked};
        }
    };

    template<typename... T>
    FilterBuilder filter_builder() {
        return FilterBuilder{this};
    }
};

MockWorld* g_world = nullptr;

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

const unsigned int DIAGONAL_COST = 14;
const unsigned int ORTHOGONAL_COST = 10;

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
  unsigned int gCost = node.gCost;

  return hCost + gCost;
}

// --- Snapshot System for Time Travel ---
struct AStarSnapshot {
    std::vector<Node> openNodes;   // Storing VALUES to keep independent state
    std::vector<Node> closedNodes; // Storing VALUES
    Node currNode;
    bool hasCurrent;
    
    // Helper to populate from pointers
    void capture(const std::vector<Node*>& open, const std::vector<Node*>& closed, Node* current) {
        openNodes.clear();
        for(auto n : open) openNodes.push_back(*n);
        
        closedNodes.clear();
        for(auto n : closed) closedNodes.push_back(*n);
        
        if (current) {
            currNode = *current;
            hasCurrent = true;
        } else {
            hasCurrent = false;
        }
    }
};

std::vector<AStarSnapshot> g_history;
int g_currentStep = 0;
bool g_simulationComplete = false;
GamePosition g_startPos;
GamePosition g_endPos;

std::vector<Node*> g_allocatedNodes;

void RunAStarSimulation(MockWorld &ecs, const GamePosition &startPos, const GamePosition &endPos) {
  g_startPos = startPos;
  g_endPos = endPos;
  g_history.clear();
  
  for (Node* n : g_allocatedNodes) delete n;
  g_allocatedNodes.clear();
  
  if (startPos == endPos) return;

  std::vector<Node*> openNodes;
  std::vector<Node*> closedNodes;
  
  // O(1) lookup grids for performance
  std::vector<bool> closedGrid(ecs.width * ecs.height, false);
  std::vector<Node*> openGrid(ecs.width * ecs.height, nullptr);
  
  Node *startNode = new Node(startPos, 0, 0, nullptr);
  g_allocatedNodes.push_back(startNode);
  Node *currNode = nullptr;

  openNodes.push_back(startNode);
  openGrid[startPos.y * ecs.width + startPos.x] = startNode;

  // Initial Snapshot
  AStarSnapshot initial;
  initial.capture(openNodes, closedNodes, currNode);
  g_history.push_back(initial);

  while (!openNodes.empty()) {
    
    // Using min_element is acceptable here as N is small (visualizer).
    // A real game would use a std::priority_queue.
    auto it = std::min_element(openNodes.begin(), openNodes.end(), 
                               [](const Node *nodeA, const Node *nodeB) 
                               { return nodeA->fCost < nodeB->fCost; });
    
    currNode = *it;
    openNodes.erase(it);
    openGrid[currNode->position.y * ecs.width + currNode->position.x] = nullptr;
    
    closedNodes.push_back(currNode);
    closedGrid[currNode->position.y * ecs.width + currNode->position.x] = true;
    
    // Snapshot at start of processing this node
    AStarSnapshot step;
    step.capture(openNodes, closedNodes, currNode);
    g_history.push_back(step);

    if (currNode->position == endPos) { break; }

    std::vector<GamePosition> possiblePositions = GetNeighbours(currNode->position);

    for (const auto &neighbourPos : possiblePositions) {
        // Bounds check
        if (neighbourPos.x < 0 || neighbourPos.x >= ecs.width || neighbourPos.y < 0 || neighbourPos.y >= ecs.height) continue;
        
        // Blocked check (O(1) instead of ECS query loop)
        if (ecs.isBlocked(neighbourPos.x, neighbourPos.y)) continue;
        
        // Closed list check (O(1))
        if (closedGrid[neighbourPos.y * ecs.width + neighbourPos.x]) continue;

        Node *neighbourNode = openGrid[neighbourPos.y * ecs.width + neighbourPos.x];
        
        unsigned int newGCost = currNode->gCost + OctileDistance(currNode->position, neighbourPos);
        
        if (neighbourNode == nullptr) {
            Node *newNode = new Node(neighbourPos, 0, newGCost, currNode);
            newNode->fCost = getFCost(*newNode, endPos);
            openNodes.push_back(newNode);
            openGrid[neighbourPos.y * ecs.width + neighbourPos.x] = newNode;
            g_allocatedNodes.push_back(newNode);
        } else if (newGCost < neighbourNode->gCost) {
            neighbourNode->parent = currNode;
            neighbourNode->gCost = newGCost;
            neighbourNode->fCost = getFCost(*neighbourNode, endPos);
        }
    }
  }
  
  // Final Snapshot
  AStarSnapshot finalState;
  finalState.capture(openNodes, closedNodes, currNode);
  g_history.push_back(finalState);
  
  g_simulationComplete = true;
}

void DrawAStarState(const AStarSnapshot& state) {
    int tileSize = 30;
    
    // Draw Grid
    for(int y=0; y<g_world->height; y++) {
        for(int x=0; x<g_world->width; x++) {
            Color c = LIGHTGRAY;
            if (g_world->isBlocked(x, y)) c = DARKGRAY;
            DrawRectangle(x*tileSize, y*tileSize, tileSize-1, tileSize-1, c);
        }
    }

    // Draw Parent Links
    auto drawParentLine = [&](const Node& n) {
        if (n.parent) {
            Vector2 startPos = { (float)n.position.x * tileSize + tileSize/2, (float)n.position.y * tileSize + tileSize/2 };
            Vector2 endPos = { (float)n.parent->position.x * tileSize + tileSize/2, (float)n.parent->position.y * tileSize + tileSize/2 };
            DrawLineEx(startPos, endPos, 2.0f, DARKGRAY);
        }
    };

    for(const auto& n : state.closedNodes) drawParentLine(n);
    for(const auto& n : state.openNodes) drawParentLine(n);
    if(state.hasCurrent) drawParentLine(state.currNode);

    // Draw Closed Nodes (Red)
    for(const auto& n : state.closedNodes) {
        DrawRectangle(n.position.x*tileSize, n.position.y*tileSize, tileSize-1, tileSize-1, Fade(RED, 0.5f));
        DrawText(TextFormat("%d", n.fCost), n.position.x*tileSize + 2, n.position.y*tileSize + 2, 10, DARKGRAY);
    }

    // Draw Open Nodes (Green)
    for(const auto& n : state.openNodes) {
        DrawRectangle(n.position.x*tileSize, n.position.y*tileSize, tileSize-1, tileSize-1, Fade(GREEN, 0.5f));
        DrawText(TextFormat("%d", n.fCost), n.position.x*tileSize + 2, n.position.y*tileSize + 2, 10, DARKGRAY);
    }
    
    // Draw Current (Blue)
    if (state.hasCurrent) {
        DrawRectangle(state.currNode.position.x*tileSize, state.currNode.position.y*tileSize, tileSize-1, tileSize-1, BLUE);
        DrawText(TextFormat("%d", state.currNode.fCost), state.currNode.position.x*tileSize + 2, state.currNode.position.y*tileSize + 2, 10, WHITE);
    }
    
    // Draw End (Purple)
    DrawRectangle(g_endPos.x*tileSize, g_endPos.y*tileSize, tileSize-1, tileSize-1, PURPLE);

    DrawText(TextFormat("Step: %d / %d", g_currentStep, (int)g_history.size() - 1), 10, g_world->height * tileSize + 10, 20, BLACK);
    DrawText("Controls: < (Back) | > (Forward)", 10, g_world->height * tileSize + 35, 20, DARKGRAY);
    DrawText("Red: Closed | Green: Open | Blue: Current", 10, g_world->height * tileSize + 60, 10, DARKGRAY);
}

#include <functional>

struct TestCase {
    std::string name;
    GamePosition start;
    GamePosition end;
    std::function<void(MockWorld&)> setup;
};

std::vector<TestCase> g_testCases;
int g_currentCaseIndex = 0;

void RunAStarSimulation(MockWorld &ecs, const GamePosition &startPos, const GamePosition &endPos);

void InitTestCases() {
    g_testCases.push_back({
        "0. Infinite Loop Trap", {2, 5}, {18, 5},
        [](MockWorld& w) {
             for(int y=0; y<w.height; y++) w.AddWall(10, y);  // A solid wall blocking the entire path
        }
    });

    g_testCases.push_back({
        "1. Wall Gap", {2, 10}, {18, 10}, 
        [](MockWorld& w) {
            for(int y=0; y<w.height; y++) w.AddWall(10, y);
            w.blocked[10 * w.width + 10] = false; 
        }
    });
    
    g_testCases.push_back({
        "2. Zig Zag", {2, 1}, {18, 18},
        [](MockWorld& w) {
             for(int x=0; x<15; x++) w.AddWall(x, 5);
             for(int x=5; x<20; x++) w.AddWall(x, 10);
             for(int x=0; x<15; x++) w.AddWall(x, 15);
        }
    });

    g_testCases.push_back({
        "3. U-Shape Trap", {7, 5}, {12, 5},
        [](MockWorld& w) {
             // Cup shape
             for(int y=0; y<12; y++) w.AddWall(5, y);  // Left wall
             for(int y=0; y<12; y++) w.AddWall(10, y); // Right wall
             for(int x=5; x<=10; x++) w.AddWall(x, 12); // Bottom wall
        }
    });
    
     g_testCases.push_back({
        "4. Open Field", {2, 2}, {18, 18},
        [](MockWorld& w) { 
            // No walls
        }
    });

    g_testCases.push_back({
        "5. Labyrinth", {0, 0}, {19, 19},
        [](MockWorld& w) {
            for (int x = 1; x < 19; x += 2) {
                bool gapAtTop = ((x / 2) % 2 != 0);
                for (int y = 0; y < w.height; y++) {
                    if (gapAtTop && y == 0) continue;
                    if (!gapAtTop && y == w.height - 1) continue;
                    w.AddWall(x, y);
                }
            }
        }
    });
}

void LoadTestCase(int index) {
    if(index < 0 || index >= (int)g_testCases.size()) return;
    g_currentCaseIndex = index;
    g_world->Reset();
    g_testCases[index].setup(*g_world);
    g_currentStep = 0;
    RunAStarSimulation(*g_world, g_testCases[index].start, g_testCases[index].end);
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    
    raylib::Window window(600, 800, "A* Time Travel Debugger");
    window.SetTargetFPS(60);

    g_world = new MockWorld();
    InitTestCases();
    LoadTestCase(0);

    int frameCounter = 0;
    const int holdDelay = 5; // Advance every 5 frames when held

    while (!window.ShouldClose()) {
        // Handle Input
        bool stepForward = IsKeyPressed(KEY_PERIOD) || IsKeyPressed(KEY_RIGHT);
        bool stepBackward = IsKeyPressed(KEY_COMMA) || IsKeyPressed(KEY_LEFT);
        
        bool holdForward = IsKeyDown(KEY_PERIOD) || IsKeyDown(KEY_RIGHT);
        bool holdBackward = IsKeyDown(KEY_COMMA) || IsKeyDown(KEY_LEFT);

        frameCounter++;
        if (frameCounter >= holdDelay) {
            if (holdForward) stepForward = true;
            if (holdBackward) stepBackward = true;
            frameCounter = 0;
        }

        if (stepForward) {
            if (g_currentStep < (int)g_history.size() - 1) g_currentStep++;
        }
        if (stepBackward) {
            if (g_currentStep > 0) g_currentStep--;
        }
        
        // Reset frame counter if no keys held to make next press immediate
        if (!holdForward && !holdBackward) {
            frameCounter = holdDelay; 
        }
        
        // Case Switching
        if (IsKeyPressed(KEY_ONE)) LoadTestCase(0);
        if (IsKeyPressed(KEY_TWO)) LoadTestCase(1);
        if (IsKeyPressed(KEY_THREE)) LoadTestCase(2);
        if (IsKeyPressed(KEY_FOUR)) LoadTestCase(3);
        if (IsKeyPressed(KEY_FIVE)) LoadTestCase(4);

        BeginDrawing();
        ClearBackground(RAYWHITE);
        
        if (!g_history.empty()) {
            DrawAStarState(g_history[g_currentStep]);
        }
        
        // Draw UI Overlay
        DrawText(TextFormat("Scenario: %s", g_testCases[g_currentCaseIndex].name.c_str()), 10, 700, 20, BLACK);
        DrawText("Press 1-5 to switch scenarios", 10, 730, 20, DARKGRAY);

        EndDrawing();
    }

    delete g_world;
    return 0;
}
