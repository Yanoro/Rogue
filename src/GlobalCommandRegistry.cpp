#include "GlobalCommandRegistry.h"

#include "StringUtils.hpp"

#include <cctype>

std::vector<GlobalCommand> GlobalCommandRegistry::commands;

namespace {

// Turns whatever the model wrote as a HELP argument into a bare verb:
// "MOVE_TO", "move_to", "MOVE_TO $TARGET" and "[MOVE_TO $TARGET]]" all reduce to
// "MOVE_TO". Copying the advertised line verbatim is a natural mistake, so it
// resolves rather than being reported as an unknown command.
std::string NormalizeToVerb(const std::string &raw) {
  std::string text = StringUtils::Trim(raw);
  while (!text.empty() && text.front() == '[') {
    text.erase(text.begin());
  }
  size_t end = 0;
  while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
         text[end] != ']') {
    ++end;
  }
  return text.substr(0, end);
}

} // namespace

void GlobalCommandRegistry::Register(const GlobalCommand& command) {
    commands.push_back(command);
}

const std::vector<GlobalCommand>& GlobalCommandRegistry::GetCommands() {
    return commands;
}

void GlobalCommandRegistry::Clear() {
    commands.clear();
}

std::string GlobalCommandRegistry::VerbOf(const GlobalCommand& command) {
    const size_t open = command.format.find('[');
    if (open == std::string::npos) {
        return "";
    }
    size_t end = open + 1;
    while (end < command.format.size() &&
           !std::isspace(static_cast<unsigned char>(command.format[end])) &&
           command.format[end] != ']') {
        ++end;
    }
    return StringUtils::ToUpper(command.format.substr(open + 1, end - open - 1));
}

const GlobalCommand* GlobalCommandRegistry::FindByVerb(const std::string& verb) {
    const std::string wanted = NormalizeToVerb(verb);
    if (wanted.empty()) {
        return nullptr;
    }
    for (const auto& command : commands) {
        // The catch-all has no verb of its own: it matches whatever [A-Z_]+ the
        // model emits, so it can never be the answer to "what is X?".
        if (command.format.empty()) {
            continue;
        }
        if (StringUtils::EqualsIgnoreCase(VerbOf(command), wanted)) {
            return &command;
        }
    }
    return nullptr;
}

std::string GlobalCommandRegistry::BuildVisibleCommandList() {
    // Format-then-description, indented under the command, matching the style the
    // interaction registry uses so the model reads both the same way.
    std::string list;
    for (const auto& command : commands) {
        if (command.hidden || command.format.empty()) {
            continue;
        }
        list += command.format + "\n";
        if (!command.rule.empty()) {
            list += "  - " + command.rule + "\n";
        }
    }

    // The spawn prompt supplies its own following blank line, and the [HELP]
    // reply appends its own closing hint; neither wants a dangling newline here.
    if (!list.empty() && list.back() == '\n') {
        list.pop_back();
    }
    return list;
}
