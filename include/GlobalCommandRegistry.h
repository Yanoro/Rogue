#pragma once

#include <flecs.h>
#include <string>
#include <vector>
#include <functional>
#include <regex>
#include <memory>

class AgentAction;

// Long-form help for one command, printed by [HELP <COMMAND>].
//
// This is the *long* description. The short one is GlobalCommand::rule, which is
// what the bare [HELP] list and the NPC spawn prompt print.
//
// `description` and `examples` are mandatory: a command that cannot say what it
// does and show at least one worked call is not documented well enough to
// advertise. `usage` and `notes` are optional and are omitted from the output
// when empty, so a simple verb does not have to invent filler for them.
struct CommandHelp {
    // Prose: what the command does, and what the agent should expect back when
    // it succeeds. Distinct from GlobalCommand::rule -- this is the thorough
    // version, not the one-line summary.
    std::string description;

    // Worked examples, complete with brackets, e.g. "[MOVE_TO Pietro]".
    std::vector<std::string> examples;

    // One line per placeholder, explaining what it accepts. Optional: no-argument
    // verbs do not need it.
    std::vector<std::string> usage;

    // Optional caveats: preconditions, failure modes, gotchas.
    std::vector<std::string> notes;
};

struct GlobalCommand {
    std::string format;
    std::string rule;
    std::regex pattern;
    std::function<std::function<std::unique_ptr<AgentAction>(flecs::entity)>(const std::smatch&)> createAction;
    bool hidden = false;
    CommandHelp help;
};

class GlobalCommandRegistry {
public:
    static void Register(const GlobalCommand& command);
    static const std::vector<GlobalCommand>& GetCommands();
    static void Clear();

    // Uppercase verb inside the format's brackets: "MOVE_TO" for
    // "[MOVE_TO $TARGET]". Empty when the format has no verb to name, which is
    // how the catch-all [GENERIC_INTERACT] is skipped by every lookup below.
    static std::string VerbOf(const GlobalCommand& command);

    // Case-insensitive lookup by verb. The argument is normalised first, so
    // "MOVE_TO", "move_to", "MOVE_TO $TARGET" and "[MOVE_TO $TARGET]" all resolve
    // to the same entry. Hidden commands are deliberately findable -- an agent
    // that already knows a contextual verb can still ask what it does -- but the
    // empty-format catch-all never matches. Returns nullptr when nothing does.
    static const GlobalCommand* FindByVerb(const std::string& verb);

    // Every advertised command as "format" followed by its short description,
    // one entry per visible command in registration order, with no trailing
    // newline. Shared by the NPC spawn prompt and the bare [HELP] reply so the
    // two listings cannot drift apart.
    static std::string BuildVisibleCommandList();

private:
    static std::vector<GlobalCommand> commands;
};
