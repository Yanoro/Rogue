#pragma once

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "DebugLog.h"
#include "ObjectFactory.h"
#include "TileRegistry.h"

// The blueprint designer's system prompt, assembled from the instruction document and
// the live world.
//
// A header of inline functions rather than a method on Game, for the same reason
// TileDesigner::BuildPromptForKind is one: the window and the headless
// `--designer-prompt building` mode must not be able to disagree about what the model was
// told. When this lived inside Game, the CLI could not reach it at all, and the only way
// to see a building prompt was to read a self-test log.
//
// The document is data (designer.promptFile); the palette below it is generated, because
// it has to match the registries exactly. A hand-written list drifts the moment a tile is
// added, and the failure is quiet: the designer asks for a material that does not exist
// and every design is refused.
namespace BlueprintPrompt {

// `%MATERIALS%` when the document has one, appended under a heading when it does not --
// a document that forgets the placeholder still gets the list rather than silently
// losing its vocabulary.
inline constexpr const char *kMaterialsPlaceholder = "%MATERIALS%";

inline std::string Build(const std::string &promptFile, const TileRegistry &tiles,
                         const ObjectFactory &objects, DebugLog *log) {
  std::string prompt;
  if (!promptFile.empty()) {
    std::ifstream file(promptFile);
    if (file.is_open()) {
      std::stringstream buffer;
      buffer << file.rdbuf();
      prompt = buffer.str();
    } else {
      const std::string message =
          "Designer: could not read the prompt file " + promptFile +
          "; the designer will run without its instructions.\n";
      if (log != nullptr) log->LogWarning(message);
      else std::cerr << message;
    }
  }

  std::string palette = "Tiles, for `paint` -- write the name exactly as shown:\n";
  if (tiles.Count() == 0) {
    palette += "  (none: no tile catalog is loaded, so no design can be built)\n";
  } else {
    for (const TileDef *tile : tiles.GetAll()) {
      palette += "  - " + tile->name +
                 (tile->blocksTile ? "  [blocks movement]" : "") + "\n";
    }
  }

  palette += "\nObjects, for `place` -- each one costs a real item:\n";
  const auto &templates = objects.GetTemplates();
  if (templates.empty()) {
    palette += "  (none)\n";
  } else {
    std::vector<std::string> names;
    names.reserve(templates.size());
    for (const auto &entry : templates) {
      names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    for (const std::string &name : names) {
      palette += "  - " + name + "\n";
    }
  }

  const size_t at = prompt.find(kMaterialsPlaceholder);
  if (at != std::string::npos) {
    prompt.replace(at, std::string(kMaterialsPlaceholder).size(), palette);
  } else if (!prompt.empty()) {
    prompt += "\n\n## Available materials (generated from this world's content)\n\n" +
              palette;
  }
  return prompt;
}

}  // namespace BlueprintPrompt
