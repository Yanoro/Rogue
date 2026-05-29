#include "Command.h"
#include "Game.h"

FunctionCommand::FunctionCommand(std::function<void(flecs::entity)> a, std::function<void(flecs::entity)> u) : action(a), undoAction(u) {}

void FunctionCommand::execute(flecs::entity e) {
  action(e);
}

void FunctionCommand::undo(flecs::entity e) {
  undoAction(e);
} 

MoveCommand::MoveCommand(int x, int y) : dx(x), dy(y) {}

void MoveCommand::execute(flecs::entity e) {
  auto p = e.get_mut<GamePosition>();
  p->x += dx;
  p->y += dy;
}

void MoveCommand::undo(flecs::entity e) {
  auto p = e.get_mut<GamePosition>();
  p->x -= dx;
  p->y -= dy;
}

ChangeVelocityCommand::ChangeVelocityCommand(float x, float y) : dx(x), dy(y) {}

void ChangeVelocityCommand::execute(flecs::entity e) {
  if (!e.has<Velocity>()) {
    e.set<Velocity>(Velocity{0.0f, 0.0f});
  }
  auto v = e.get_mut<Velocity>();
  v->x += dx;
  v->y += dy;
}

void ChangeVelocityCommand::undo(flecs::entity e) {
  if (e.has<Velocity>()) {
    auto v = e.get_mut<Velocity>();
    v->x -= dx;
    v->y -= dy;
  }
}
