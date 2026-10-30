#include "InteractionRegistry.h"

std::vector<InteractionHandler> InteractionRegistry::handlers;

void InteractionRegistry::Register(const InteractionHandler& handler) {
    InteractionHandler stored = handler;
    if (stored.key.empty()) {
        stored.key = NormalizeInteractionKey(stored.name);
    }
    handlers.push_back(std::move(stored));
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

std::vector<std::string> InteractionRegistry::GetAvailableInteractionKeys(
    flecs::entity target) {
    std::vector<std::string> available;
    if (!target.is_alive()) return available;
    available.reserve(handlers.size());
    for (const auto& handler : handlers) {
        if (handler.canInteract(target)) {
            available.push_back(handler.key.empty()
                                    ? NormalizeInteractionKey(handler.name)
                                    : handler.key);
        }
    }
    return available;
}

const InteractionHandler *InteractionRegistry::FindInteraction(
    const std::string &key) {
    for (const auto& handler : handlers) {
        const std::string &candidate =
            handler.key.empty() ? handler.name : handler.key;
        if (candidate == key) return &handler;
    }
    return nullptr;
}

bool InteractionRegistry::IsNoArgumentInteraction(const std::string &key) {
    const InteractionHandler *handler = FindInteraction(key);
    return handler != nullptr && !handler->requiresArgs;
}

void InteractionRegistry::Clear() {
    handlers.clear();
}

std::vector<ItemInteractionHandler> ItemInteractionRegistry::handlers;

void ItemInteractionRegistry::Register(const ItemInteractionHandler& handler) {
    handlers.push_back(handler);
}

std::vector<ItemInteractionHandler> ItemInteractionRegistry::GetAvailableInteractions(flecs::entity item) {
    std::vector<ItemInteractionHandler> available;
    if (!item.is_alive()) return available;

    for (const auto& handler : handlers) {
        if (handler.canInteract(item)) {
            available.push_back(handler);
        }
    }
    return available;
}

void ItemInteractionRegistry::Clear() {
    handlers.clear();
}
