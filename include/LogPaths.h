#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// Per-run log layout.
//
// Every game session gets one directory:
//
//   logs/<run-start>/
//     npcs/<Agent>/
//     designer/
//     system1/
//     tile-designer/
//     navigator/
//
// The session name is the existing run-start timestamp, so the NPC transcripts,
// system 1 recorder and navigator logs of one run sort together and cannot be
// confused with another run.
namespace LogPaths {

// The run's session directory. Created lazily, together with its five standard
// subdirectories, on the first log write.
const std::filesystem::path &SessionRoot();

// One standard subdirectory of the session (created on demand).
std::filesystem::path Subdir(const std::string &name);

// A stable, filesystem-safe folder name for one agent. Duplicate display names
// get the entity id appended so two "Wren" entities cannot share transcript
// files.
std::string AgentKey(const std::string &name, std::uint64_t entityId);

// npcs/<Agent>/ (created).
std::filesystem::path NpcDir(const std::string &name, std::uint64_t entityId);

// npcs/<Agent>/<file>.
std::filesystem::path NpcFile(const std::string &name, std::uint64_t entityId,
                              const std::string &file);

// navigator/<Agent>.txt.
std::filesystem::path NavigatorFile(const std::string &name,
                                    std::uint64_t entityId);

}  // namespace LogPaths
