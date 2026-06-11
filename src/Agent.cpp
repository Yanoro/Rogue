#include "Agent.h"
#include "AI.h"
#include "Components.h"
#include "flecs.h"

Agent::Agent(flecs::entity entity, std::shared_ptr<AI> ai)
    : entity(entity), ai(std::move(ai)) {
  chatWindow = std::make_unique<AIChatWindow>(this->ai);
  entity.set<WindowOnClick>({false, WindowType::AIChatWindowType});
}


