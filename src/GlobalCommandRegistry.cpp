#include "GlobalCommandRegistry.h"

std::vector<GlobalCommand> GlobalCommandRegistry::commands;

void GlobalCommandRegistry::Register(const GlobalCommand& command) {
    commands.push_back(command);
}

const std::vector<GlobalCommand>& GlobalCommandRegistry::GetCommands() {
    return commands;
}

void GlobalCommandRegistry::Clear() {
    commands.clear();
}
