#pragma once
#include <flecs.h>
#include <functional>
#include <algorithm>

struct GamePosition;

class Command {
public:
  virtual ~Command() {}
  virtual void execute(flecs::entity e) = 0;
  virtual void undo(flecs::entity e) = 0;
};

class FunctionCommand : public Command {
public:
  FunctionCommand(std::function<void(flecs::entity)> a,
                  std::function<void(flecs::entity)> u);

  void execute(flecs::entity e) override;
  void undo(flecs::entity e) override;

private:
  std::function<void(flecs::entity)> action, undoAction;
};

class MoveCommand : public Command {
public:
  MoveCommand(int x, int y);

  void execute(flecs::entity e) override;
  void undo(flecs::entity e) override;

private:
  int dx, dy;
};

class ChangeVelocityCommand : public Command {
public:
  ChangeVelocityCommand(float x, float y);

  void execute(flecs::entity e) override;
  void undo(flecs::entity e) override;

private:
  float dx, dy;
};
