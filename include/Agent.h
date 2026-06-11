#pragma once

#include "AI.h"
#include "AIChatWindow.h"
#include <flecs.h>
#include <memory>
#include <utility>

class Agent {
public: 
  Agent(flecs::entity entity, std::shared_ptr<AI> ai);

private:
  flecs::entity entity;
  std::shared_ptr<AI> ai;
  std::unique_ptr<AIChatWindow> chatWindow;
};

