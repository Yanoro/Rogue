#pragma once

// Task-list state helpers: the tolerant wire-format parser behind the hidden
// [SET_TASKS] verb, the spawn-time seeding of authored terminal goals, and the
// horizon/tier label conversions the digest and the debug board share.
//
// See docs/task-system-design.md sections 4, 5, 7, 9 and 12.
//
// This header is deliberately free of behaviour: it turns text into a TaskList
// and back, and nothing here schedules, validates or checks anything against
// the world. The engine-written fields (facts, ruleWarning) are filled in by the
// checking path; the parser only carries the engine's own bookkeeping -- ids and
// timestamps -- across a whole-list replacement.

#include "Components.h"
#include "Defaults.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// The engine's tick for task bookkeeping. flecs' frame counter, so it is
// monotonic for the life of the world and needs no state of its own.
//
// By const reference: a flecs::world is a light handle but is deliberately not
// copyable, so taking one by value does not compile.
inline int CurrentTaskTick(const flecs::world &world) {
  const flecs::world_info_t *info = world.get_info();
  return info ? static_cast<int>(info->frame_count_total) : 0;
}

// Whitespace-trimmed, because model output is not: every field of every row
// arrives padded, and a stray newline inside a row must not become part of a
// description.
inline std::string TrimTaskField(const std::string &text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  auto isSpace = [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  };
  while (begin < end && isSpace(text[begin])) ++begin;
  while (end > begin && isSpace(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

// Horizon labels as the wire format spells them. "longterm" is accepted with
// or without the separator because both are common model output; the digest
// writes the canonical "LongTerm".
inline bool ParseTaskHorizon(const std::string &text, GoalHorizon &out) {
  const std::string lowered = [&text] {
    std::string s = text;
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }();

  if (lowered == "terminal") {
    out = GoalHorizon::Terminal;
    return true;
  }
  if (lowered == "longterm" || lowered == "long-term" || lowered == "long_term" ||
      lowered == "long term") {
    out = GoalHorizon::LongTerm;
    return true;
  }
  if (lowered == "short") {
    out = GoalHorizon::Short;
    return true;
  }
  return false;
}

// Tier labels as the wire format spells them.
inline bool ParseTaskPriority(const std::string &text, TaskPriority &out) {
  const std::string lowered = [&text] {
    std::string s = text;
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }();

  if (lowered == "critical") {
    out = TaskPriority::Critical;
    return true;
  }
  if (lowered == "high") {
    out = TaskPriority::High;
    return true;
  }
  if (lowered == "normal") {
    out = TaskPriority::Normal;
    return true;
  }
  if (lowered == "low") {
    out = TaskPriority::Low;
    return true;
  }
  if (lowered == "background") {
    out = TaskPriority::Background;
    return true;
  }
  return false;
}

inline const char *TaskHorizonLabel(GoalHorizon horizon) {
  switch (horizon) {
    case GoalHorizon::Terminal: return "Terminal";
    case GoalHorizon::LongTerm: return "LongTerm";
    case GoalHorizon::Short: return "Short";
  }
  return "Short";
}

// Shown to the agent exactly as it set it -- the engine never rewrites a tier,
// it only annotates a violation (section 6).
inline const char *TaskPriorityLabel(TaskPriority tier) {
  switch (tier) {
    case TaskPriority::Critical: return "CRITICAL";
    case TaskPriority::High: return "HIGH";
    case TaskPriority::Normal: return "NORMAL";
    case TaskPriority::Low: return "LOW";
    case TaskPriority::Background: return "BACKGROUND";
  }
  return "NORMAL";
}

// Sorting rank, 0 = highest. Used to order the digest and to decide which
// entries overflow DEFAULT_TASK_LIST_MAX. This never changes a stored tier: it
// is a read-only ordering, not an adjustment (section 6).
inline int TaskPriorityRank(TaskPriority tier) {
  switch (tier) {
    case TaskPriority::Critical: return 0;
    case TaskPriority::High: return 1;
    case TaskPriority::Normal: return 2;
    case TaskPriority::Low: return 3;
    case TaskPriority::Background: return 4;
  }
  return 2;
}

inline int ParseTaskInt(const std::string &text) {
  const std::string trimmed = TrimTaskField(text);
  if (trimmed.empty()) return -1;
  try {
    return std::stoi(trimmed);
  } catch (...) {
    return -1;
  }
}

// Rows are ';'-separated, per the documented format. A newline also ends a row,
// so a model that forgets the semicolons still parses instead of collapsing
// every entry into one malformed line.
inline std::vector<std::string> SplitTaskRows(const std::string &payload) {
  std::vector<std::string> rows;
  std::string current;
  for (char c : payload) {
    if (c == ';' || c == '\n') {
      rows.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  rows.push_back(current);
  return rows;
}

inline std::vector<std::string> SplitTaskFields(const std::string &row) {
  std::vector<std::string> fields;
  std::string current;
  for (char c : row) {
    if (c == '|') {
      fields.push_back(TrimTaskField(current));
      current.clear();
    } else {
      current += c;
    }
  }
  fields.push_back(TrimTaskField(current));
  return fields;
}

// Seeds the authored life goals into a fresh TaskList.
//
// Terminal goals are authored data, not model output (section 5): they are
// stamped once at spawn, presented read-only, and the [SET_TASKS] verb cannot
// produce or replace one. They are given the ids 0..n-1, which callers may rely
// on: it is what keeps the terminal range clear of every id [SET_TASKS] hands
// out, so a parent reference is never ambiguous.
inline void SeedTerminalGoals(const std::vector<std::string> &authored, int tick,
                              TaskList &list) {
  for (const std::string &description : authored) {
    const std::string trimmed = TrimTaskField(description);
    if (trimmed.empty()) continue;
    AgentGoal goal;
    goal.description = trimmed;
    goal.horizon = GoalHorizon::Terminal;
    goal.parent = -1;
    goal.tier = TaskPriority::Normal;
    goal.createdTick = tick;
    goal.lastAdvancedTick = tick;
    goal.id = static_cast<int>(list.goals.size());
    list.goals.push_back(std::move(goal));
  }
}

// Applies one [SET_TASKS] payload to `list` as a whole-list replacement.
//
// Returns true when the list was replaced, false when the payload could not be
// read at all -- in which case `list` is untouched and the previous list stands,
// which is what section 9 asks for. Callers report `warnings` back to the agent.
//
// Tolerances, all deliberate because model output is not:
//   * a malformed entry is skipped and the rest of the list still applies;
//   * a `terminal` entry is ignored: the agent cannot re-derive who it is;
//   * ids are re-issued when missing, duplicated or inside the terminal range;
//   * `parent` is a goal *id in the new list* (see the note below), and an
//     unresolvable one becomes -1 rather than deleting the goal;
//   * an empty payload is a legal, idempotent "clear the list".
//
// On `parent`: the design note calls it an index into TaskList::goals, while the
// wire format's own example numbers parents against the entries' ids. Those two
// readings differ only for *non-terminal* goals -- terminal goals are seeded
// first and carry ids 0..n-1, so their index and their id coincide. Resolving by
// id is therefore the reading that satisfies both, and it is what this does.
//
// Terminal goals survive replacement untouched; the agent owns everything below
// them (section 5).
inline bool ParseSetTasksPayload(const std::string &payload, TaskList &list,
                                 int tick, std::vector<std::string> &warnings) {
  auto warn = [&warnings](const std::string &message) {
    warnings.push_back(message);
  };

  struct Row {
    AgentGoal goal;
    int parentRef = -1;
  };
  std::vector<Row> rows;

  for (const std::string &rawRow : SplitTaskRows(payload)) {
    const std::vector<std::string> fields = SplitTaskFields(rawRow);

    bool anyContent = false;
    for (const std::string &field : fields) {
      if (!field.empty()) {
        anyContent = true;
        break;
      }
    }
    // A blank line, or the empty tail after a trailing separator, is not a
    // malformed entry and is not worth a warning.
    if (!anyContent) continue;

    if (fields.size() != 7) {
      warn("skipped a task entry with " + std::to_string(fields.size()) +
           " fields; the format is: id | description | horizon | kind | target "
           "| parent | tier");
      continue;
    }

    const std::string &description = fields[1];
    if (description.empty()) {
      warn("skipped a task entry with no description");
      continue;
    }

    GoalHorizon horizon = GoalHorizon::Short;
    if (!fields[2].empty() && !ParseTaskHorizon(fields[2], horizon)) {
      warn("unknown horizon '" + fields[2] + "' on task '" + description +
           "'; read as short");
      horizon = GoalHorizon::Short;
    }
    if (horizon == GoalHorizon::Terminal) {
      warn("ignored the terminal entry '" + description +
           "'; your life goals are fixed and cannot be set here");
      continue;
    }

    TaskPriority tier = TaskPriority::Normal;
    if (!fields[6].empty() && !ParseTaskPriority(fields[6], tier)) {
      warn("unknown tier '" + fields[6] + "' on task '" + description +
           "'; read as normal");
      tier = TaskPriority::Normal;
    }

    Row row;
    row.goal.description = description;
    row.goal.horizon = horizon;
    // kind and target are kept exactly as authored even on a long-term goal,
    // where the engine will never check them: dropping the agent's own words
    // would be the engine editing intent, and section 7's escape hatch is that
    // an unexpected kind is never a reason to reject or rewrite a task.
    row.goal.kind = fields[3];
    row.goal.target = fields[4];
    row.goal.tier = tier;
    row.goal.id = ParseTaskInt(fields[0]);
    row.parentRef = ParseTaskInt(fields[5]);
    rows.push_back(std::move(row));
  }

  bool payloadHadContent = false;
  for (char c : payload) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) {
      payloadHadContent = true;
      break;
    }
  }
  // Nothing parsed out of a payload that was not empty: the revision failed, so
  // the previous list stays in place rather than being silently cleared.
  if (rows.empty() && payloadHadContent) {
    return false;
  }

  // Engine-owned identity. The terminal range is reserved first so an issued id
  // can never shadow a life goal, and a model-supplied id is honoured only when
  // it is present, unique and clear of that range -- honouring it is what lets a
  // goal keep its age across a revision (section 8 on staleness).
  int nextFreeId = 0;
  for (const AgentGoal &goal : list.goals) {
    if (goal.horizon == GoalHorizon::Terminal) {
      nextFreeId = std::max(nextFreeId, goal.id + 1);
    }
  }
  {
    std::vector<int> usedIds;
    auto isUsed = [&usedIds](int id) {
      return std::find(usedIds.begin(), usedIds.end(), id) != usedIds.end();
    };
    int scan = nextFreeId;
    for (Row &row : rows) {
      const int modelId = row.goal.id;
      if (modelId >= nextFreeId && !isUsed(modelId)) {
        usedIds.push_back(modelId);
        continue;
      }
      while (isUsed(scan)) ++scan;
      row.goal.id = scan;
      usedIds.push_back(scan);
      ++scan;
    }
  }

  // Terminal goals first, then the agent's rows in the order it wrote them:
  // ordering is an agent decision and the engine never reorders (section 6).
  struct Built {
    AgentGoal goal;
    int parentRef = -1;
  };
  std::vector<Built> built;
  for (const AgentGoal &terminal : list.goals) {
    if (terminal.horizon != GoalHorizon::Terminal) continue;
    built.push_back({terminal, -1});
  }
  for (const Row &row : rows) {
    Built entry;
    entry.goal = row.goal;
    entry.parentRef = row.parentRef;
    // Facts and rule warnings are engine observations about the world and are
    // recomputed, never carried across a revision.
    entry.goal.facts.clear();
    entry.goal.ruleWarning.clear();
    entry.goal.createdTick = tick;
    entry.goal.lastAdvancedTick = tick;
    // Age survives a revision when the id does, so staleness keeps meaning
    // something instead of resetting every time the agent re-lists its tasks.
    for (const AgentGoal &old : list.goals) {
      if (old.horizon == GoalHorizon::Terminal) continue;
      if (old.id == entry.goal.id) {
        entry.goal.createdTick = old.createdTick;
        entry.goal.lastAdvancedTick = old.lastAdvancedTick;
        break;
      }
    }
    built.push_back(std::move(entry));
  }

  // The cap. Overflow drops the lowest tier first and, within a tier, the
  // oldest; the agent's ordering is preserved otherwise and life goals are never
  // dropped at all (section 12).
  std::vector<std::size_t> authored;
  for (std::size_t i = 0; i < built.size(); ++i) {
    if (built[i].goal.horizon != GoalHorizon::Terminal) authored.push_back(i);
  }
  const int overflow =
      static_cast<int>(authored.size()) - DEFAULT_TASK_LIST_MAX;
  if (overflow > 0) {
    std::vector<std::size_t> order = authored;
    std::stable_sort(order.begin(), order.end(),
                     [&built](std::size_t a, std::size_t b) {
                       const int rankA = TaskPriorityRank(built[a].goal.tier);
                       const int rankB = TaskPriorityRank(built[b].goal.tier);
                       if (rankA != rankB) return rankA > rankB;
                       return built[a].goal.createdTick <
                              built[b].goal.createdTick;
                     });
    std::vector<bool> dropped(built.size(), false);
    for (int i = 0; i < overflow; ++i) dropped[order[i]] = true;

    warn("dropped " + std::to_string(overflow) + " task(s) over the limit of " +
         std::to_string(DEFAULT_TASK_LIST_MAX) + "; the lowest tiers go first");

    std::vector<Built> kept;
    kept.reserve(built.size() - static_cast<std::size_t>(overflow));
    for (std::size_t i = 0; i < built.size(); ++i) {
      if (!dropped[i]) kept.push_back(std::move(built[i]));
    }
    built = std::move(kept);
  }

  TaskList next;
  next.goals.reserve(built.size());
  for (const Built &entry : built) next.goals.push_back(entry.goal);

  // Parent edges are resolved against the ids in the *new* list, after the cap,
  // so a reference never dangles: an unresolvable parent is reported and left at
  // -1 (an orphan, which section 8 flags rather than forbids).
  for (std::size_t i = 0; i < built.size(); ++i) {
    next.goals[i].parent = -1;
    const int ref = built[i].parentRef;
    if (ref < 0) continue;
    for (std::size_t j = 0; j < next.goals.size(); ++j) {
      if (next.goals[j].id == ref) {
        next.goals[i].parent = static_cast<int>(j);
        break;
      }
    }
    if (next.goals[i].parent < 0) {
      warn("task '" + next.goals[i].description + "' names parent " +
           std::to_string(ref) +
           ", which is not in the list; it is left unparented");
    }
  }

  // activeTaskId says what the agent believes it is doing. It is not part of the
  // wire format, so it survives only while the goal it names still exists.
  next.activeTaskId = -1;
  if (list.activeTaskId >= 0) {
    for (const AgentGoal &goal : next.goals) {
      if (goal.id == list.activeTaskId) {
        next.activeTaskId = list.activeTaskId;
        break;
      }
    }
  }

  list = std::move(next);
  return true;
}

// ---------------------------------------------------------------------------
// Reading: the digest, and the transition log.
//
// Nothing below changes state. The digest is what the agent is shown when it
// wakes (section 9); the transition list is what a human reads afterwards to
// see what changed and when (section 11).
// ---------------------------------------------------------------------------

// Shortened for display only. The digest competes with the command list for
// context, so a `serves:` edge shows enough of its parent to be recognisable
// rather than the whole sentence (section 9).
inline std::string TruncateTaskText(const std::string &text,
                                    std::size_t maxLength = 44) {
  if (text.size() <= maxLength) return text;
  if (maxLength <= 3) return text.substr(0, maxLength);
  return text.substr(0, maxLength - 3) + "...";
}

// The goal this one serves, or "" when it is unparented or its parent no longer
// resolves. Re-read on every render, never cached, so the agent can notice and
// repair its own stale links instead of silently losing the frame (section 9).
inline std::string DescribeTaskParent(const TaskList &list,
                                      const AgentGoal &goal) {
  if (goal.parent < 0 ||
      static_cast<std::size_t>(goal.parent) >= list.goals.size()) {
    return "";
  }
  return TruncateTaskText(
      list.goals[static_cast<std::size_t>(goal.parent)].description);
}

// The facts of one goal as a single line, "(none)" when there are none. Shared by
// the digest, the transition log and the debug windows so all three show the
// same observation in the same words.
inline std::string FormatTaskFacts(const std::vector<std::string> &facts) {
  if (facts.empty()) return "(none)";
  std::string joined;
  for (std::size_t i = 0; i < facts.size(); ++i) {
    if (i > 0) joined += ", ";
    joined += facts[i];
  }
  return joined;
}

// The digest the agent is woken with (section 9).
//
// Four properties this rendering has to keep, all of them load-bearing:
//   * life goals are separated and labelled read-only, so the agent is never
//     invited to rewrite them (section 5);
//   * the agent's tier is shown exactly as the agent set it, with a violation
//     annotated rather than corrected (section 6.1) -- so this only ever prints
//     ruleWarning, it never recomputes a tier;
//   * the `serves` edge is re-read every turn;
//   * it is capped: short tasks render to DEFAULT_TASK_DIGEST_MAX, highest tier
//     first, and the count of what was left out is stated, so a hidden task is
//     never mistaken for a removed one.
//
// The tier ordering is a rendering order only. The stored list keeps the agent's
// own ordering, because reordering is an agent decision (section 6).
//
// `currentActivity` is what the agent is actually doing, as opposed to what it
// says it intends (section 8); it is empty when nothing is running and the line
// is then omitted. `suggestRevision` adds the two-line reminder of the verb,
// which is how the agent learns [SET_TASKS] exists at all -- the verb is hidden
// from the advertised command list. `kindHint` is the engine's vocabulary of task
// kinds, rendered by the caller because the registry is a world resource this
// header must not depend on; it is shown only alongside the reminder, since an
// agent that is not revising its plan has no use for it.
inline std::string RenderTaskDigest(const TaskList &list,
                                    const std::string &currentActivity,
                                    bool suggestRevision,
                                    const std::string &kindHint = std::string()) {
  std::string out;

  std::string lifeGoals;
  for (const AgentGoal &goal : list.goals) {
    if (goal.horizon != GoalHorizon::Terminal) continue;
    lifeGoals += "  [Terminal]  " + goal.description + "\n";
  }
  if (!lifeGoals.empty()) {
    out +=
        "YOUR LIFE GOALS  (fixed -- these are who you are, not something to "
        "rewrite)\n";
    out += lifeGoals;
    out += "\n";
  }

  std::string longTermGoals;
  for (const AgentGoal &goal : list.goals) {
    if (goal.horizon != GoalHorizon::LongTerm) continue;
    longTermGoals += "  [LongTerm]  " + goal.description + "\n";
    const std::string parent = DescribeTaskParent(list, goal);
    if (!parent.empty()) {
      longTermGoals += "              serves: " + parent + "\n";
    }
  }
  if (!longTermGoals.empty()) {
    out += "YOUR CURRENT GOALS\n";
    out += longTermGoals;
    out += "\n";
  }

  std::vector<std::size_t> shortTasks;
  for (std::size_t i = 0; i < list.goals.size(); ++i) {
    if (list.goals[i].horizon == GoalHorizon::Short) shortTasks.push_back(i);
  }
  std::stable_sort(shortTasks.begin(), shortTasks.end(),
                   [&list](std::size_t a, std::size_t b) {
                     const int rankA = TaskPriorityRank(list.goals[a].tier);
                     const int rankB = TaskPriorityRank(list.goals[b].tier);
                     if (rankA != rankB) return rankA < rankB;
                     return list.goals[a].id < list.goals[b].id;
                   });

  std::string tasks;
  const std::size_t shown =
      std::min<std::size_t>(shortTasks.size(), DEFAULT_TASK_DIGEST_MAX);
  for (std::size_t n = 0; n < shown; ++n) {
    const AgentGoal &goal = list.goals[shortTasks[n]];
    tasks += "  " + std::to_string(goal.id) + ". [" +
             TaskPriorityLabel(goal.tier) + "]  " + goal.description;
    if (!goal.facts.empty()) {
      tasks += "  (engine: ";
      for (std::size_t f = 0; f < goal.facts.size(); ++f) {
        if (f > 0) tasks += ", ";
        tasks += goal.facts[f];
      }
      tasks += ")";
    }
    tasks += "\n";
    const std::string parent = DescribeTaskParent(list, goal);
    if (!parent.empty()) tasks += "              serves: " + parent + "\n";
    // Annotated, never corrected: `tier` above is still whatever the agent said.
    if (!goal.ruleWarning.empty()) {
      tasks += "              [!] " + goal.ruleWarning + "\n";
    }
  }
  if (shortTasks.size() > shown) {
    tasks += "  (+" + std::to_string(shortTasks.size() - shown) +
             " more not shown)\n";
  }
  if (!tasks.empty()) {
    out += "YOUR CURRENT TASKS\n";
    out += tasks;
    out += "\n";
  }

  // A violation that is a property of the whole list rather than of one entry
  // (section 4, TaskList::ruleWarning).
  if (!list.ruleWarning.empty()) {
    out += "[!] " + list.ruleWarning + "\n\n";
  }

  if (!currentActivity.empty()) {
    out += "Current activity: " + currentActivity;
    if (list.activeTaskId >= 0) {
      out += " (serving task " + std::to_string(list.activeTaskId) + ")";
    }
    out += "\n\n";
  }

  if (suggestRevision) {
    // Kept to two lines on purpose: this is a reminder, not a specification. The
    // verb is hidden from AVAILABLE COMMANDS, so this is where the agent finds
    // out it exists.
    out +=
        "If your plan has changed, replace the list below the life goals with:\n"
        "  [SET_TASKS id | description | horizon | kind | target | parent | "
        "tier; ...]\n"
        "  (separate entries with ';'; parent is the id it serves, or -1)\n\n";
    // The kinds the engine can actually check. Without this the kind column is a
    // guessing game, and a "find the village" intention has no way to become a
    // task the engine can report on.
    out += kindHint;
  }

  return out;
}

// What changed between two versions of a list, one line per transition.
//
// Transitions, not states (section 11): a per-turn dump of an unchanged list is
// noise that buries the signal. Matching is by goal id, which is why the parser
// honours a model-supplied id -- without that, every revision would read as
// "everything removed, everything added".
inline std::vector<std::string> DescribeTaskTransitions(const TaskList &before,
                                                        const TaskList &after) {
  std::vector<std::string> transitions;

  auto find = [](const TaskList &list, int id) -> const AgentGoal * {
    for (const AgentGoal &goal : list.goals) {
      if (goal.id == id) return &goal;
    }
    return nullptr;
  };

  for (const AgentGoal &goal : after.goals) {
    const AgentGoal *old = find(before, goal.id);
    const std::string id = std::to_string(goal.id);
    if (old == nullptr) {
      transitions.push_back("task " + id + " added: " + goal.description);
      continue;
    }
    if (old->tier != goal.tier) {
      transitions.push_back("task " + id + " re-tiered " +
                            TaskPriorityLabel(old->tier) + " -> " +
                            TaskPriorityLabel(goal.tier) + ": " +
                            goal.description);
    }
    if (old->description != goal.description) {
      transitions.push_back("task " + id + " reworded: " + old->description +
                            " -> " + goal.description);
    }
    if (old->horizon != goal.horizon) {
      transitions.push_back("task " + id + " horizon " +
                            TaskHorizonLabel(old->horizon) + " -> " +
                            TaskHorizonLabel(goal.horizon));
    }
    if (old->parent != goal.parent) {
      transitions.push_back("task " + id + " parent " +
                            std::to_string(old->parent) + " -> " +
                            std::to_string(goal.parent));
    }
    // Facts and violations are what the checking pass writes, so reporting them
    // here is what puts "flagged stale or in violation" into the timestamped
    // transcript section 11 asks for -- without the checking pass having to know
    // a transcript exists.
    if (old->facts != goal.facts) {
      transitions.push_back("task " + id + " engine: " +
                            FormatTaskFacts(old->facts) + " -> " +
                            FormatTaskFacts(goal.facts));
    }
    if (old->ruleWarning != goal.ruleWarning) {
      transitions.push_back("task " + id +
                            (goal.ruleWarning.empty()
                                 ? std::string(" violation cleared")
                                 : " flagged: " + goal.ruleWarning));
    }
  }

  for (const AgentGoal &goal : before.goals) {
    if (find(after, goal.id) == nullptr) {
      transitions.push_back("task " + std::to_string(goal.id) +
                            " removed: " + goal.description);
    }
  }

  if (before.activeTaskId != after.activeTaskId) {
    transitions.push_back("active task " + std::to_string(before.activeTaskId) +
                          " -> " + std::to_string(after.activeTaskId));
  }

  return transitions;
}

// ---------------------------------------------------------------------------
// Debug surface readings (section 11).
//
// Shared by the NPC window's task section and the village board, and pure, so
// the two cannot disagree about what "stale" or "inflated" means -- and so the
// same thresholds stage 5 writes into ruleWarning are the ones the board flags.
// ---------------------------------------------------------------------------

// Frames to seconds. The world runs at ~60 fps, which is the assumption
// DEFAULT_TASK_STALE_TICKS documents ("1800 ~= 30 s at 60 fps").
inline int TaskTicksToSeconds(int ticks) { return ticks > 0 ? ticks / 60 : 0; }

// Compact age for a table cell: "12s", "3m", "1h".
inline std::string FormatTaskAge(int ageTicks) {
  const int seconds = TaskTicksToSeconds(ageTicks);
  if (seconds < 60) return std::to_string(seconds) + "s";
  if (seconds < 3600) return std::to_string(seconds / 60) + "m";
  return std::to_string(seconds / 3600) + "h";
}

// How long a goal has gone without advancing. lastAdvancedTick is written when
// the goal is created and whenever a revision keeps its id, so a goal that is
// genuinely untouched accumulates; createdTick is the fallback for a goal whose
// lastAdvancedTick was never stamped.
inline int TaskAgeTicks(const AgentGoal &goal, int nowTick) {
  const int last =
      goal.lastAdvancedTick > 0 ? goal.lastAdvancedTick : goal.createdTick;
  return nowTick > last ? nowTick - last : 0;
}

// The goal the list is currently serving: the one activeTaskId names, or nullptr.
// Deliberately not "the highest tier task" -- that is what the agent *should* be
// serving, and conflating the two would hide exactly the divergence section 8
// wants visible.
inline const AgentGoal *TaskListActiveGoal(const TaskList &list) {
  if (list.activeTaskId < 0) return nullptr;
  for (const AgentGoal &goal : list.goals) {
    if (goal.id == list.activeTaskId) return &goal;
  }
  return nullptr;
}

// The agent's top intent, for the board to show when nothing is marked active:
// highest tier first, then the lowest id, which is the order the digest renders
// in and the order the cap drops in. Short tasks only -- a long-term goal is a
// frame, and a life goal is never something an agent is "now serving".
inline const AgentGoal *TaskListTopCandidate(const TaskList &list) {
  const AgentGoal *best = nullptr;
  for (const AgentGoal &goal : list.goals) {
    if (goal.horizon != GoalHorizon::Short) continue;
    if (best == nullptr) {
      best = &goal;
      continue;
    }
    const int rank = TaskPriorityRank(goal.tier);
    const int bestRank = TaskPriorityRank(best->tier);
    if (rank < bestRank || (rank == bestRank && goal.id < best->id)) {
      best = &goal;
    }
  }
  return best;
}

// Starvation (section 11): a Critical or High task nobody has touched for longer
// than the stale threshold. Measured from lastAdvancedTick, not createdTick, so a
// task the agent keeps working on never flags.
inline bool IsTaskStarving(const AgentGoal &goal, int nowTick) {
  if (goal.tier != TaskPriority::Critical && goal.tier != TaskPriority::High) {
    return false;
  }
  return TaskAgeTicks(goal, nowTick) > DEFAULT_TASK_STALE_TICKS;
}

// The anti-inflation input (section 6.1). Reporting only: nothing here rewrites a
// tier, and the agent's own label is what both windows display.
inline int CountCriticalTasks(const TaskList &list) {
  int count = 0;
  for (const AgentGoal &goal : list.goals) {
    if (goal.tier == TaskPriority::Critical) ++count;
  }
  return count;
}

inline bool IsTaskListInflated(const TaskList &list) {
  return CountCriticalTasks(list) > DEFAULT_TASK_MAX_CRITICAL;
}

// Whether what the agent says it is doing matches what it is actually doing
// (section 8). Empty when they agree or when nothing is worth reporting; else a
// one-line report. `runningAction` is the live action label, empty when nothing
// is running.
//
// This is the reading the *digest* cannot produce: the brain only asks the agent
// when it is idle, so the prompt's activity line is always empty. Here it is
// sampled from live state, which is the whole point of the debug surface.
inline std::string DescribeTaskDivergence(const TaskList &list,
                                          const std::string &runningAction) {
  // Pure bookkeeping actions are not work, so an action serving nothing while one
  // of them runs is not divergence -- section 8 says as much about InvalidAction
  // and system chatter, and [SET_TASKS] is the same kind of thing.
  const bool isChatter =
      runningAction == "INVALID" || runningAction == "SET_TASKS";
  const bool running = !runningAction.empty() && runningAction != "IDLE" && !isChatter;
  const AgentGoal *active = TaskListActiveGoal(list);

  if (running && active == nullptr) {
    return runningAction + " is running but no task is marked active";
  }
  if (!running && list.activeTaskId >= 0 && active == nullptr) {
    return "active task " + std::to_string(list.activeTaskId) +
           " is not on the list";
  }
  return "";
}
