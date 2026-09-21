#pragma once

#include <flecs.h>
#include <string>
#include <vector>
#include <functional>
#include <regex>
#include <memory>

class AgentAction;

struct GlobalCommand {
    std::string format;
    std::string rule;
    std::regex pattern;
    std::function<std::function<std::unique_ptr<AgentAction>(flecs::entity)>(const std::smatch&)> createAction;
    bool hidden = false;
};

class GlobalCommandRegistry {
public:
    static void Register(const GlobalCommand& command);
    static const std::vector<GlobalCommand>& GetCommands();
    static void Clear();

private:
    static std::vector<GlobalCommand> commands;
};
