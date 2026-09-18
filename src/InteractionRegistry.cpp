#include "InteractionRegistry.h"

std::vector<InteractionHandler> InteractionRegistry::handlers;

void InteractionRegistry::Register(const InteractionHandler& handler) {
    handlers.push_back(handler);
}

std::vector<InteractionHandler> InteractionRegistry::GetAvailableInteractions(flecs::entity target) {
    std::vector<InteractionHandler> available;
    if (!target.is_alive()) return available;

    for (const auto& handler : handlers) {
        if (handler.canInteract(target)) {
            available.push_back(handler);
        }
    }
    return available;
}

void InteractionRegistry::Clear() {
    handlers.clear();
}
