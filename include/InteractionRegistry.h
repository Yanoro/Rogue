#pragma once

#include <flecs.h>
#include <string>
#include <vector>
#include <functional>
#include <cctype>

struct InteractionHandler {
    std::string name;
    // Stable, lower-case key used by the navigation grammar ("examine",
    // "harvest"). The display name stays exactly as the AI-facing command is
    // written; the key is what the prompt's action grammar uses.
    std::string key;
    // True when execute() needs arguments the simple <verb>:<objectId> grammar
    // cannot express (craft recipe, store item/count, ...). Navigation offers
    // only no-argument interactions for now.
    bool requiresArgs = false;
    std::function<bool(flecs::entity)> canInteract;
    std::function<std::string(flecs::entity actor, flecs::entity target, std::string args)> execute;
    // AI-facing text shown when an agent arrives at the object (MoveToEntityAction).
    // Always write an explanation of what the command does, followed by examples of
    // how to use it, e.g. "Gather the resources this object holds. Example: [HARVEST]".
    std::string description;
};

// Lower-case, underscore-separated form of an interaction display name. Keeps
// letters and digits; collapses other characters to one underscore. An empty
// result becomes "interaction" so a key is never empty.
inline std::string NormalizeInteractionKey(const std::string& name) {
    std::string key;
    key.reserve(name.size());
    bool lastUnderscore = false;
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0) {
            key += static_cast<char>(std::tolower(uc));
            lastUnderscore = false;
        } else if (!key.empty() && !lastUnderscore) {
            key += '_';
            lastUnderscore = true;
        }
    }
    while (!key.empty() && key.back() == '_') {
        key.pop_back();
    }
    return key.empty() ? std::string("interaction") : key;
}

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
    // Keys only, without copying handlers/descriptions. The spatial-memory
    // observer calls this per visible object per sample, so this is the cheap
    // path for a snapshot that only needs the stable verb set.
    static std::vector<std::string> GetAvailableInteractionKeys(flecs::entity target);
    // Registry lookup for the navigation grammar. Returns null when no verb has
    // this stable key. The pointer is valid for the process lifetime (handlers are
    // registered during ECS init and not removed in normal play).
    static const InteractionHandler *FindInteraction(const std::string &key);
    // True only for a known handler whose execute() takes no arguments. The
    // Stage 2 navigator refuses argument-bearing verbs such as craft/store.
    static bool IsNoArgumentInteraction(const std::string &key);
    static void Clear();

    template <typename Component>
    static void RegisterComponentInteraction(const std::string& name,
                                             const std::string& description,
                                             std::function<std::string(flecs::entity actor, flecs::entity target, std::string args)> executeFunc,
                                             bool requiresArgs = false) {
        InteractionHandler handler;
        handler.name = name;
        handler.key = NormalizeInteractionKey(name);
        handler.requiresArgs = requiresArgs;
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
