#pragma once

#include <flecs.h>
#include <string>
#include <vector>
#include <functional>

struct InteractionHandler {
    std::string name;
    std::function<bool(flecs::entity)> canInteract;
    std::function<std::string(flecs::entity actor, flecs::entity target, std::string args)> execute;
    // AI-facing text shown when an agent arrives at the object (MoveToEntityAction).
    // Always write an explanation of what the command does, followed by examples of
    // how to use it, e.g. "Gather the resources this object holds. Example: [HARVEST]".
    std::string description;
};

// Renders one interaction as an AI-facing bullet, e.g.
//   - [HARVEST]: Gather whatever resources this object still holds. Example: [HARVEST]
// commandName is expected to already be cased the way the AI should type it.
inline std::string FormatInteractionForAI(const std::string& commandName,
                                          const std::string& description) {
    std::string line = "- [" + commandName + "]";
    if (!description.empty()) {
        line += ": " + description;
    }
    line += "\n";
    return line;
}

class InteractionRegistry {
public:
    static void Register(const InteractionHandler& handler);
    static std::vector<InteractionHandler> GetAvailableInteractions(flecs::entity target);
    static void Clear();

    template <typename Component>
    static void RegisterComponentInteraction(const std::string& name,
                                             const std::string& description,
                                             std::function<std::string(flecs::entity actor, flecs::entity target, std::string args)> executeFunc) {
        InteractionHandler handler;
        handler.name = name;
        handler.canInteract = [](flecs::entity target) { return target.has<Component>(); };
        handler.execute = executeFunc;
        handler.description = description;
        Register(handler);
    }

private:
    static std::vector<InteractionHandler> handlers;
};

struct ItemInteractionHandler {
    std::string name;
    std::function<bool(flecs::entity)> canInteract;
    std::function<std::string(flecs::entity actor, flecs::entity item)> execute;
    // See InteractionHandler: AI-facing text shown by [EXAMINE_ITEM].
    std::string description;
};

class ItemInteractionRegistry {
public:
    static void Register(const ItemInteractionHandler& handler);
    static std::vector<ItemInteractionHandler> GetAvailableInteractions(flecs::entity item);
    static void Clear();

    template <typename Component>
    static void RegisterComponentInteraction(const std::string& name,
                                             const std::string& description,
                                             std::function<std::string(flecs::entity actor, flecs::entity item)> executeFunc) {
        ItemInteractionHandler handler;
        handler.name = name;
        handler.canInteract = [](flecs::entity item) { return item.has<Component>(); };
        handler.execute = executeFunc;
        handler.description = description;
        Register(handler);
    }

private:
    static std::vector<ItemInteractionHandler> handlers;
};
