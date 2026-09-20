#pragma once

#include <flecs.h>
#include <string>
#include <vector>
#include <functional>

struct InteractionHandler {
    std::string name;
    std::function<bool(flecs::entity)> canInteract;
    std::function<std::string(flecs::entity actor, flecs::entity target)> execute;
};

class InteractionRegistry {
public:
    static void Register(const InteractionHandler& handler);
    static std::vector<InteractionHandler> GetAvailableInteractions(flecs::entity target);
    static void Clear();

    template <typename Component>
    static void RegisterComponentInteraction(const std::string& name, std::function<std::string(flecs::entity actor, flecs::entity target)> executeFunc) {
        Register({
            name,
            [](flecs::entity target) { return target.has<Component>(); },
            executeFunc
        });
    }

private:
    static std::vector<InteractionHandler> handlers;
};

struct ItemInteractionHandler {
    std::string name;
    std::function<bool(flecs::entity)> canInteract;
    std::function<std::string(flecs::entity actor, flecs::entity item)> execute;
};

class ItemInteractionRegistry {
public:
    static void Register(const ItemInteractionHandler& handler);
    static std::vector<ItemInteractionHandler> GetAvailableInteractions(flecs::entity item);
    static void Clear();

    template <typename Component>
    static void RegisterComponentInteraction(const std::string& name, std::function<std::string(flecs::entity actor, flecs::entity item)> executeFunc) {
        Register({
            name,
            [](flecs::entity item) { return item.has<Component>(); },
            executeFunc
        });
    }

private:
    static std::vector<ItemInteractionHandler> handlers;
};
