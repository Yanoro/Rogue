#include "SettingsWindow.h"

#include "Components.h"
#include "Game.h"
#include "Settings.h"
#include "SpriteDraw.hpp"
#include "SpritePicker.hpp"
#include "SpritePaths.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return text;
}

// Every leaf path the override file carries, so a row can say "override" without
// touching the disk once per row per frame.
void CollectPaths(const nlohmann::json &node, const std::string &prefix,
                  std::set<std::string> &out) {
  if (!node.is_object()) {
    out.insert(prefix);
    return;
  }
  for (auto it = node.begin(); it != node.end(); ++it) {
    const std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
    CollectPaths(it.value(), path, out);
  }
}

// The model blocks a profile can drive. Longest prefix wins, so
// navigation.single.model is not mistaken for `model`.
const char *const kModelBlocks[] = {"model",
                                    "designer.model",
                                    "designer.rewriter.model",
                                    "navigation.single.model",
                                    "system1.model",
                                    "vision"};

std::string BlockOf(const std::string &path) {
  std::string best;
  for (const char *block : kModelBlocks) {
    const std::string prefix = std::string(block) + ".";
    if (path.rfind(prefix, 0) == 0 && std::string(block).size() > best.size()) {
      best = block;
    }
  }
  return best;
}

// Whether a profile seeds this field. Vision is the exception: `enabled` belongs
// to the window's own switch, not to the backend definition.
bool ProfileGoverns(const SettingsSchema::FieldSpec &field,
                    const std::string &block) {
  if (block.empty() || field.path == block + ".profile") {
    return false;
  }
  if (block == "vision") {
    return field.path == "vision.model" || field.path == "vision.endpoint" ||
           field.path == "vision.keepAlive" ||
           field.path == "vision.temperature";
  }
  return field.path.rfind(block + ".", 0) == 0;
}

// True when the block's consumer ignores it because it shares the main backend.
bool BlockInactive(const std::string &block, const GameSettings &settings) {
  if (block == "designer.model") {
    return settings.designer.useMainModel;
  }
  if (block == "system1.model") {
    return settings.system1.useMainModel;
  }
  if (block == "navigation.single.model") {
    return settings.navigation.single.useMainModel;
  }
  return false;
}

std::string DefaultLabel(const SettingsSchema::FieldSpec &field) {
  const nlohmann::json value = SettingsSchema::DefaultJson(field);
  return value.is_null() ? "*absent*" : value.dump();
}

void ReadOverridePaths(const std::string &settingsPath, std::set<std::string> &out) {
  out.clear();
  const std::string path = SettingsOverrideFilePath(settingsPath);
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }
  try {
    nlohmann::json root;
    file >> root;
    CollectPaths(root, "", out);
  } catch (const std::exception &) {
    out.clear();
  }
}

}  // namespace

SettingsWindow::SettingsWindow(Game *game) : game(game) {
  if (!SettingsSchema::Sections().empty()) {
    selectedSection = SettingsSchema::Sections().front().key;
  }
}

void SettingsWindow::Report(bool ok, const std::string &message) {
  statusOk = ok;
  status = message;
}

bool SettingsWindow::Matches(const SettingsSchema::FieldSpec &field) const {
  if (showRestartOnly && field.apply != SettingsSchema::Apply::Restart) {
    return false;
  }
  if (showOverriddenOnly &&
      overrides.find(field.path) == overrides.end()) {
    return false;
  }
  if (filter.empty()) {
    return true;
  }
  const std::string needle = Lower(filter);
  return Lower(field.path).find(needle) != std::string::npos ||
         Lower(field.doc).find(needle) != std::string::npos;
}

void SettingsWindow::Commit(const SettingsSchema::FieldSpec &field,
                            const nlohmann::json &value) {
  std::string message;
  const bool ok =
      ApplySettingValue(SettingsFilePath(), field.path, value, message);
  Report(ok, message);
  overrideCacheDirty = true;
}

void SettingsWindow::CommitProfiles() {
  std::string message;
  const bool ok =
      ApplyProfilesValue(SettingsFilePath(), Settings().system1.profiles, message);
  Report(ok, message);
  overrideCacheDirty = true;
}

void SettingsWindow::Draw() {
  bool show = game->settingsWindowEntity.has<ActiveWindow>();
  ImGui::SetNextWindowSize(ImVec2(980, 700), ImGuiCond_FirstUseEver);
  ImGui::Begin("Settings", &show);
  if (!show) {
    game->settingsWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  if (overrideCacheDirty) {
    ReadOverridePaths(SettingsFilePath(), overrides);
    explicitBlockKeys.clear();
    for (const char *block : kModelBlocks) {
      const int count = CountExplicitBlockKeys(SettingsFilePath(), block);
      if (count > 0) {
        explicitBlockKeys[block] = count;
      }
    }
    overrideCacheDirty = false;
  }

  ImGui::Text("Settings file: %s", SettingsFilePath().c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("(writes go to %s)",
                      SettingsOverrideFilePath(SettingsFilePath()).c_str());
  ImGui::TextDisabled(
      "Edits apply to the running game. A row tagged restart is read once at "
      "startup, so only a restart makes it real.");

  ImGui::SetNextItemWidth(320);
  ImGui::InputTextWithHint("##filter", "filter by key or description",
                           &filter);
  ImGui::SameLine();
  ImGui::Checkbox("Only restart", &showRestartOnly);
  ImGui::SameLine();
  ImGui::Checkbox("Only overridden", &showOverriddenOnly);
  ImGui::SameLine();
  ImGui::Checkbox("Show secrets", &showSecrets);

  if (ImGui::Button("Reload from disk")) {
    const bool ok = LoadGameSettings(SettingsFilePath());
    Report(ok, ok ? "Settings reloaded from disk."
                  : "Settings reloaded; some values were corrected (see stderr).");
    overrideCacheDirty = true;
    RefreshPlayerActorSprite();
  }
  ImGui::SameLine();
  if (ImGui::Button("Write reference")) {
    std::string message;
    const bool ok = WriteSettingsReference("SETTINGS.md", message);
    Report(ok, message);
  }
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.20f, 0.20f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.24f, 0.24f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.28f, 0.28f, 1.0f));
  if (ImGui::Button("Restart game")) {
    // main() sees this, shuts the game down and re-executes the binary with the
    // same arguments. Nothing needs saving here: every edit was written to the
    // override file as it was made.
    game->requestRestart();
  }
  ImGui::PopStyleColor(3);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Relaunch the game with the same arguments, so values tagged restart "
        "apply.\nThe current run -- map, NPCs, conversations -- is discarded. "
        "Settings are already saved.");
  }

  if (!status.empty()) {
    const ImVec4 colour = statusOk ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                                   : ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Separator();

  ImGui::BeginChild("##sections", ImVec2(260, 0), ImGuiChildFlags_Borders);
  int index = 0;
  while (index < static_cast<int>(SettingsSchema::Sections().size())) {
    DrawTree(index);
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("##fields", ImVec2(0, 0), ImGuiChildFlags_Borders);
  const bool browsing =
      filter.empty() && !showRestartOnly && !showOverriddenOnly;
  if (!browsing) {
    ImGui::TextDisabled("Matches for \"%s\"", filter.c_str());
    ImGui::Separator();
    for (const SettingsSchema::SectionSpec &section :
         SettingsSchema::Sections()) {
      bool headerWritten = false;
      for (const SettingsSchema::FieldSpec &field :
           SettingsSchema::Fields()) {
        if (field.section != section.key || !Matches(field)) {
          continue;
        }
        if (!headerWritten) {
          ImGui::TextDisabled("%s", section.title.c_str());
          headerWritten = true;
        }
        DrawField(field);
      }
    }
  } else {
    DrawFieldsFor(selectedSection);
  }
  ImGui::EndChild();

  ImGui::End();
}

void SettingsWindow::DrawTree(int &index) {
  const auto &sections = SettingsSchema::Sections();
  const std::string &key = sections[index].key;
  const int count = static_cast<int>(sections.size());

  bool hasChild = index + 1 < count &&
                  sections[index + 1].key.rfind(key + ".", 0) == 0;
  if (!hasChild) {
    if (ImGui::Selectable(key.c_str(), selectedSection == key)) {
      selectedSection = key;
    }
    ++index;
    return;
  }

  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_DefaultOpen |
                             ImGuiTreeNodeFlags_SpanAvailWidth;
  if (selectedSection == key) {
    flags |= ImGuiTreeNodeFlags_Selected;
  }
  const bool open = ImGui::TreeNodeEx(key.c_str(), flags);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    selectedSection = key;
  }
  ++index;
  if (open) {
    while (index < count && sections[index].key.rfind(key + ".", 0) == 0) {
      DrawTree(index);
    }
    ImGui::TreePop();
  } else {
    while (index < count && sections[index].key.rfind(key + ".", 0) == 0) {
      ++index;
    }
  }
}

void SettingsWindow::DrawFieldsFor(const std::string &sectionKey) {
  if (sectionKey == "ai.profiles") {
    const SettingsSchema::SectionSpec *section =
        SettingsSchema::FindAISection();
    if (section) {
      ImGui::TextWrapped("%s", section->doc.c_str());
      ImGui::Separator();
    }
    DrawAIProfiles();
    return;
  }
  if (sectionKey == SettingsSchema::Profiles().section) {
    const SettingsSchema::SectionSpec *section =
        SettingsSchema::FindSection(sectionKey);
    if (section) {
      ImGui::TextWrapped("%s", section->doc.c_str());
      ImGui::Separator();
    }
    DrawProfiles();
    return;
  }
  const SettingsSchema::SectionSpec *section =
      SettingsSchema::FindSection(sectionKey);
  if (!section) {
    ImGui::TextDisabled("Select a section.");
    return;
  }
  ImGui::TextDisabled("`%s`", section->title.c_str());
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextWrapped("%s", section->doc.c_str());
  ImGui::PopTextWrapPos();
  ImGui::Separator();

  for (const SettingsSchema::FieldSpec &field : SettingsSchema::Fields()) {
    if (field.section != sectionKey) {
      continue;
    }
    DrawField(field);
  }
}

void SettingsWindow::DrawField(const SettingsSchema::FieldSpec &field) {
  GameSettings &settings = MutableSettings();
  if (!field.persisted) {
    ImGui::PushID(field.path.c_str());
    ImGui::TextDisabled("%s", field.Label().c_str());
    ImGui::SameLine(240.0f);
    if (field.type == SettingsSchema::ValueType::String) {
      const std::string &text = *static_cast<const std::string *>(
          field.access(settings));
      ImGui::TextDisabled("%zu characters (produced at load)", text.size());
    } else {
      ImGui::TextDisabled("produced at load");
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", field.doc.c_str());
    }
    ImGui::PopID();
    return;
  }

  if (field.path == "player.actorSprite") {
    DrawPlayerActorSpriteRow(field);
    return;
  }

  // A block that names a profile shows the picker and hides the fields the
  // profile supplies, unless the user asked to see them. The picker row reports
  // any explicit override that the file carries, so nothing is hidden silently.
  const std::string block = BlockOf(field.path);
  const bool isProfileRow = !block.empty() && field.path == block + ".profile";
  std::string profileId;
  if (!block.empty()) {
    if (const SettingsSchema::FieldSpec *reference =
            SettingsSchema::FindField(block + ".profile")) {
      const nlohmann::json value =
          SettingsSchema::CurrentJson(*reference, settings);
      if (value.is_string()) {
        profileId = value.get<std::string>();
      }
    }
  }
  if (!block.empty() && !isProfileRow && !profileId.empty() &&
      revealedBlocks.find(block) == revealedBlocks.end()) {
    return;
  }
  const bool inactive = BlockInactive(block, settings);

  const bool fromCommandLine =
      SettingsSchema::IsCommandLineOverride(field.path);

  ImGui::PushID(field.path.c_str());
  ImGui::TextUnformatted(field.Label().c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s\n\ntype: %s\ndefault: %s\n\n%s%s",
                      field.path.c_str(),
                      SettingsSchema::TypeDescription(field).c_str(),
                      DefaultLabel(field).c_str(), field.doc.c_str(),
                      fromCommandLine
                          ? "\n\nSet by a command-line flag for this run. The file "
                            "does not contain it, so editing it here writes "
                            "settings.local.json for runs that omit the flag."
                          : "");
  }
  ImGui::SameLine(240.0f);

  const bool hasPresence = static_cast<bool>(field.presence);
  bool present = hasPresence ? *field.presence(settings) : true;
  if (hasPresence) {
    bool ticked = present;
    if (ImGui::Checkbox("##present", &ticked)) {
      if (ticked) {
        // A field whose member default is a null JSON (the provider routing
        // object) needs a seed value, or ticking "set" would mean "commit
        // absent" and the editor would stay disabled forever.
        nlohmann::json initial =
            SettingsSchema::CurrentJson(field, settings);
        if (initial.is_null()) {
          initial = nlohmann::json::object();
        }
        Commit(field, initial);
      } else {
        Commit(field, nullptr);
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Unset means \"absent\": the backend's own default "
                        "applies instead of a value written here.");
    }
    ImGui::SameLine();
  }

  ImGui::BeginDisabled(!present || inactive);
  ImGui::SetNextItemWidth(260.0f);
  void *leaf = field.access(settings);

  switch (field.type) {
  case SettingsSchema::ValueType::Bool: {
    bool value = *static_cast<bool *>(leaf);
    if (ImGui::Checkbox("##v", &value)) {
      Commit(field, value);
    }
    break;
  }
  case SettingsSchema::ValueType::Int: {
    int value = *static_cast<int *>(leaf);
    ImGui::InputInt("##v", &value, 1, 10);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      Commit(field, value);
    }
    break;
  }
  case SettingsSchema::ValueType::Int64: {
    long value = *static_cast<long *>(leaf);
    long long widget = value;
    ImGui::InputScalar("##v", ImGuiDataType_S64, &widget);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      Commit(field, static_cast<long long>(widget));
    }
    break;
  }
  case SettingsSchema::ValueType::UInt64: {
    unsigned long value = *static_cast<unsigned long *>(leaf);
    unsigned long long widget = value;
    ImGui::InputScalar("##v", ImGuiDataType_U64, &widget);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      Commit(field, static_cast<unsigned long long>(widget));
    }
    break;
  }
  case SettingsSchema::ValueType::Float: {
    float value = *static_cast<float *>(leaf);
    ImGui::InputFloat("##v", &value, 0.1f, 1.0f, "%.4f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      Commit(field, value);
    }
    break;
  }
  case SettingsSchema::ValueType::Double: {
    double value = *static_cast<double *>(leaf);
    ImGui::InputDouble("##v", &value, 0.1, 1.0, "%.4f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      Commit(field, value);
    }
    break;
  }
  case SettingsSchema::ValueType::String: {
    std::string value = *static_cast<std::string *>(leaf);
    const ImGuiInputTextFlags flags =
        (field.secret && !showSecrets) ? ImGuiInputTextFlags_Password : 0;
    ImGui::InputText("##v", &value, flags);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      Commit(field, value);
    }
    break;
  }
  case SettingsSchema::ValueType::EnumString: {
    std::string value = *static_cast<std::string *>(leaf);
    if (!field.enumSource.empty()) {
      // A dynamic list: the ids of ai.profiles entries of the kind this block
      // expects, plus the empty choice for an inline block.
      const std::string kind = field.enumSource.substr(
          field.enumSource.find(':') == std::string::npos
              ? field.enumSource.size()
              : field.enumSource.find(':') + 1);
      // The choice is applied after the combo closes: selecting one reloads the
      // registry (references resolve at load), which would invalidate the loop.
      std::string chosen;
      bool chooseNone = false;
      bool createNew = false;
      if (ImGui::BeginCombo("##v", value.empty() ? "(no profile)"
                                                 : value.c_str())) {
        if (ImGui::Selectable("(no profile)", value.empty())) {
          chooseNone = true;
        }
        for (const auto &[id, profile] : Settings().ai.profiles) {
          if (profile.kind != kind) {
            continue;
          }
          const std::string label =
              id + (profile.label.empty() || profile.label == id
                        ? ""
                        : "  (" + profile.label + ")");
          if (ImGui::Selectable(label.c_str(), value == id)) {
            chosen = id;
          }
        }
        ImGui::Separator();
        if (ImGui::Selectable("+ New profile...", false)) {
          createNew = true;
        }
        ImGui::EndCombo();
      }
      if (!chosen.empty()) {
        AssignProfile(field, chosen, kind);
      } else if (chooseNone) {
        AssignProfile(field, std::string(), kind);
      } else if (createNew) {
        NewProfileForBlock(field, kind);
      }
      break;
    }
    if (ImGui::BeginCombo("##v", value.c_str())) {
      for (const std::string &option : field.enumValues) {
        const bool selected = (option == value);
        if (ImGui::Selectable(option.c_str(), selected)) {
          Commit(field, option);
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    break;
  }
  case SettingsSchema::ValueType::Json: {
    std::string &buffer = jsonBuffers[field.path];
    if (buffer.empty()) {
      buffer = SettingsSchema::CurrentJson(field, settings).dump(2);
    }
    ImGui::InputTextMultiline("##json", &buffer, ImVec2(-1, 100));
    if (ImGui::Button("Apply JSON")) {
      try {
        Commit(field, nlohmann::json::parse(buffer));
      } catch (const std::exception &error) {
        Report(false, std::string("Invalid JSON: ") + error.what());
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload text")) {
      buffer = SettingsSchema::CurrentJson(field, settings).dump(2);
    }
    break;
  }
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  if (field.apply == SettingsSchema::Apply::Restart) {
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "restart");
    ImGui::SameLine();
  }
  if (fromCommandLine) {
    ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "cli");
    ImGui::SameLine();
  }
  // The file override is masked by the command line, so showing both would be a
  // claim about a value the run is not using.
  const bool overridden =
      !fromCommandLine && overrides.find(field.path) != overrides.end();
  if (overridden) {
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.95f, 1.0f), "override");
    ImGui::SameLine();
  }
  ImGui::BeginDisabled(!overridden);
  if (ImGui::SmallButton("reset")) {
    std::string message;
    const bool ok = ResetSettingValue(SettingsFilePath(), field.path, message);
    Report(ok, message);
    overrideCacheDirty = true;
    jsonBuffers.erase(field.path);
    RefreshPlayerActorSprite();
  }
  ImGui::EndDisabled();

  // Type and default are the two facts a row cannot show through its editor: an
  // int's bounds are not visible in an InputInt, and "absent" is not visible at
  // all. The type is shown always, the default only when the value is not it.
  ImGui::SameLine();
  ImGui::TextDisabled("%s", SettingsSchema::TypeDescription(field).c_str());
  const nlohmann::json current = SettingsSchema::CurrentJson(field, settings);
  const nlohmann::json fallback = SettingsSchema::DefaultJson(field);
  if (current != fallback) {
    ImGui::SameLine();
    ImGui::TextDisabled("(default %s)", DefaultLabel(field).c_str());
  }

  if (isProfileRow) {
    if (!profileId.empty()) {
      const auto found = settings.ai.profiles.find(profileId);
      if (found == settings.ai.profiles.end()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
                           "unknown profile '%s'", profileId.c_str());
      } else {
        ImGui::TextDisabled("  %s / %s", found->second.model.type.c_str(),
                            found->second.model.model.c_str());
        const auto count = explicitBlockKeys.find(block);
        if (count != explicitBlockKeys.end() && count->second > 0) {
          ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                             "  %d explicit field(s) override it",
                             count->second);
        }
      }
      bool reveal = revealedBlocks.find(block) != revealedBlocks.end();
      if (ImGui::Checkbox("override fields", &reveal)) {
        if (reveal) {
          revealedBlocks.insert(block);
        } else {
          revealedBlocks.erase(block);
        }
      }
    }
    if (inactive) {
      ImGui::TextDisabled("inactive: this block shares the main model");
    }
  }

  ImGui::PopID();
}

void SettingsWindow::DrawProfileField(
    const SettingsSchema::CollectionFieldSpec &field,
    GameSettings::SystemOneProfileSection &profile) {
  const std::string bufferKey = profile.id + "|" + field.path;
  ImGui::PushID(field.path.c_str());
  ImGui::TextUnformatted(field.label.c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", field.doc.c_str());
  }
  ImGui::SameLine(170.0f);
  ImGui::SetNextItemWidth(220.0f);

  if (field.presence) {
    bool present = *field.presence(profile);
    if (ImGui::Checkbox("##present", &present)) {
      *field.presence(profile) = present;
      CommitProfiles();
    }
    ImGui::SameLine();
  }

  void *leaf = field.access(profile);

  if (field.path == "profile") {
    // The same picker the model blocks use, restricted to s1. Clearing it forks
    // the profile into the entry's model block, matching the block behaviour.
    std::string current = *static_cast<std::string *>(leaf);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("##profile", current.empty() ? "(no profile)"
                                                       : current.c_str())) {
      if (ImGui::Selectable("(no profile)", current.empty())) {
        if (!current.empty()) {
          const auto found = Settings().ai.profiles.find(current);
          if (found != Settings().ai.profiles.end()) {
            profile.model = found->second.model;
          }
        }
        *static_cast<std::string *>(leaf) = std::string();
        CommitProfiles();
      }
      for (const auto &[id, candidate] : Settings().ai.profiles) {
        if (candidate.kind != "s1") {
          continue;
        }
        if (ImGui::Selectable(id.c_str(), current == id)) {
          *static_cast<std::string *>(leaf) = id;
          CommitProfiles();
        }
      }
      ImGui::EndCombo();
    }
    if (!current.empty()) {
      const auto found = Settings().ai.profiles.find(current);
      if (found == Settings().ai.profiles.end()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
                           "unknown profile '%s'", current.c_str());
      } else {
        ImGui::TextDisabled("  %s / %s", found->second.model.type.c_str(),
                            found->second.model.model.c_str());
      }
      bool reveal = revealedEntries.find(profile.id) != revealedEntries.end();
      if (ImGui::Checkbox("override fields", &reveal)) {
        if (reveal) {
          revealedEntries.insert(profile.id);
        } else {
          revealedEntries.erase(profile.id);
        }
      }
    }
    // The caller opened a disabled scope for the presence flag; this branch
    // returns early, so it has to close it before the ID stack.
    ImGui::EndDisabled();
    ImGui::PopID();
    return;
  }

  switch (field.type) {
  case SettingsSchema::ValueType::Bool: {
    bool value = *static_cast<bool *>(leaf);
    if (ImGui::Checkbox("##v", &value)) {
      *static_cast<bool *>(leaf) = value;
      CommitProfiles();
    }
    break;
  }
  case SettingsSchema::ValueType::Int: {
    int value = *static_cast<int *>(leaf);
    ImGui::InputInt("##v", &value, 1, 10);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      *static_cast<int *>(leaf) = value;
      CommitProfiles();
    }
    break;
  }
  case SettingsSchema::ValueType::Int64: {
    long value = *static_cast<long *>(leaf);
    long long widget = value;
    ImGui::InputScalar("##v", ImGuiDataType_S64, &widget);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      *static_cast<long *>(leaf) = static_cast<long>(widget);
      CommitProfiles();
    }
    break;
  }
  case SettingsSchema::ValueType::UInt64: {
    unsigned long value = *static_cast<unsigned long *>(leaf);
    unsigned long long widget = value;
    ImGui::InputScalar("##v", ImGuiDataType_U64, &widget);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      *static_cast<unsigned long *>(leaf) =
          static_cast<unsigned long>(widget);
      CommitProfiles();
    }
    break;
  }
  case SettingsSchema::ValueType::Float: {
    float value = *static_cast<float *>(leaf);
    ImGui::InputFloat("##v", &value, 0.1f, 1.0f, "%.4f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      *static_cast<float *>(leaf) = value;
      CommitProfiles();
    }
    break;
  }
  case SettingsSchema::ValueType::Double: {
    double value = *static_cast<double *>(leaf);
    ImGui::InputDouble("##v", &value, 0.1, 1.0, "%.4f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      *static_cast<double *>(leaf) = value;
      CommitProfiles();
    }
    break;
  }
  case SettingsSchema::ValueType::String: {
    std::string value = *static_cast<std::string *>(leaf);
    ImGui::InputText("##v", &value);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      *static_cast<std::string *>(leaf) = value;
      CommitProfiles();
    }
    break;
  }
  case SettingsSchema::ValueType::EnumString: {
    std::string value = *static_cast<std::string *>(leaf);
    if (ImGui::BeginCombo("##v", value.c_str())) {
      for (const std::string &option : field.enumValues) {
        const bool selected = (option == value);
        if (ImGui::Selectable(option.c_str(), selected)) {
          *static_cast<std::string *>(leaf) = option;
          CommitProfiles();
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    break;
  }
  case SettingsSchema::ValueType::Json: {
    nlohmann::json &jsonValue = *static_cast<nlohmann::json *>(leaf);
    std::string &buffer = jsonBuffers[bufferKey];
    if (buffer.empty()) {
      buffer = jsonValue.is_null() ? "{}" : jsonValue.dump(2);
    }
    ImGui::InputTextMultiline("##json", &buffer, ImVec2(-1, 70));
    if (ImGui::Button("Apply JSON")) {
      try {
        *static_cast<nlohmann::json *>(leaf) =
            nlohmann::json::parse(buffer);
        CommitProfiles();
      } catch (const std::exception &error) {
        Report(false, std::string("Invalid JSON: ") + error.what());
      }
    }
    break;
  }
  }
  ImGui::PopID();
}

void SettingsWindow::AssignProfile(const SettingsSchema::FieldSpec &field,
                                   const std::string &id,
                                   const std::string &kind) {
  (void)kind;
  GameSettings &settings = MutableSettings();
  const nlohmann::json current =
      SettingsSchema::CurrentJson(field, settings);
  const std::string previous = current.is_string() ? current.get<std::string>() : "";

  if (id.empty() && !previous.empty()) {
    // Fork: copy the profile's effective values into the inline block before the
    // reference is cleared, so the configuration that was working survives the
    // switch and can be tuned from there.
    const std::string block = BlockOf(field.path);
    for (const SettingsSchema::FieldSpec &candidate : SettingsSchema::Fields()) {
      if (!ProfileGoverns(candidate, block)) {
        continue;
      }
      if (candidate.presence && !*candidate.presence(settings)) {
        continue;
      }
      const nlohmann::json value =
          SettingsSchema::CurrentJson(candidate, settings);
      // Never copy a secret into the fork. A hosted block's key is normally not
      // its own: it is inherited from model.apiKey at load, so forking would
      // pin a copy here that outlives a key rotation in settings.local.json.
      // An empty value is skipped for the reason it always was -- it would mask
      // the key the loader supplies. Nothing is lost either way: the fork only
      // adds fields, so a key already written inline keeps its value.
      if (candidate.secret) {
        continue;
      }
      if (value.is_string() && value.get<std::string>().empty()) {
        continue;
      }
      std::string ignored;
      ApplySettingValue(SettingsFilePath(), candidate.path, value, ignored);
    }
  }
  CommitProfileReference(field, id);
  revealedBlocks.erase(BlockOf(field.path));
}

void SettingsWindow::NewProfileForBlock(const SettingsSchema::FieldSpec &field,
                                        const std::string &kind) {
  GameSettings &settings = MutableSettings();
  std::string id = "profile";
  for (int suffix = 2; settings.ai.profiles.count(id) != 0; ++suffix) {
    id = "profile_" + std::to_string(suffix);
  }
  nlohmann::json entry = nlohmann::json::object();
  entry["label"] = id;
  entry["kind"] = kind;
  if (kind == "s1") {
    // A decisions model is defined by its preset far more than by a model id.
    entry["preset"] = "hosted_jev";
  } else if (kind == "vision") {
    entry["model"] = {{"type", "ollama"}, {"model", "moondream"},
                      {"keepAlive", "30m"}};
  } else {
    entry["model"] = {{"type", "ollama"},
                      {"model", "llama3"},
                      {"endpoint", "http://localhost:11434/api/generate"}};
  }
  std::string message;
  bool ok = ApplyAIProfileValue(SettingsFilePath(), id, entry, message);
  if (ok) {
    ok = ApplySettingValue(SettingsFilePath(), field.path, id, message);
  }
  Report(ok, message);
  overrideCacheDirty = true;
  if (ok) {
    LoadGameSettings(SettingsFilePath());
    selectedSection = "ai.profiles";
  }
}

void SettingsWindow::CommitProfileReference(
    const SettingsSchema::FieldSpec &field, const nlohmann::json &value) {
  std::string message;
  const bool ok =
      ApplySettingValue(SettingsFilePath(), field.path, value, message);
  Report(ok, message);
  overrideCacheDirty = true;
  // A reference is resolved by the loader, so the block's seeded values only
  // appear after a reload.
  if (ok) {
    LoadGameSettings(SettingsFilePath());
  }
}

void SettingsWindow::DrawAIProfileField(
    const SettingsSchema::AIProfileFieldSpec &field,
    GameSettings::AIProfileSection &profile, const std::string &profileId) {
  void *leaf = SettingsSchema::AIProfileLeaf(profile, field.path);
  if (leaf == nullptr) {
    return;
  }
  ImGui::PushID(field.path.c_str());
  ImGui::TextUnformatted(field.label.c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s\n\n%s", field.path.c_str(), field.doc.c_str());
  }
  ImGui::SameLine(170.0f);
  ImGui::SetNextItemWidth(260.0f);

  bool *presence = field.presence
                       ? SettingsSchema::AIProfilePresence(profile, field.path)
                       : nullptr;
  bool present = presence ? *presence : true;
  if (presence != nullptr) {
    bool ticked = present;
    if (ImGui::Checkbox("##present", &ticked)) {
      *presence = ticked;
    }
    ImGui::SameLine();
  }
  ImGui::BeginDisabled(!present);

  switch (field.type) {
  case SettingsSchema::ValueType::Bool: {
    bool value = *static_cast<bool *>(leaf);
    if (ImGui::Checkbox("##v", &value)) {
      *static_cast<bool *>(leaf) = value;
    }
    break;
  }
  case SettingsSchema::ValueType::Int: {
    int value = *static_cast<int *>(leaf);
    ImGui::InputInt("##v", &value, 1, 10);
    *static_cast<int *>(leaf) = value;
    break;
  }
  case SettingsSchema::ValueType::Float: {
    float value = *static_cast<float *>(leaf);
    ImGui::InputFloat("##v", &value, 0.1f, 1.0f, "%.4f");
    *static_cast<float *>(leaf) = value;
    break;
  }
  case SettingsSchema::ValueType::Double: {
    double value = *static_cast<double *>(leaf);
    ImGui::InputDouble("##v", &value, 0.1, 1.0, "%.4f");
    *static_cast<double *>(leaf) = value;
    break;
  }
  case SettingsSchema::ValueType::String: {
    std::string value = *static_cast<std::string *>(leaf);
    const bool secret = field.path.size() >= 6 &&
                        field.path.compare(field.path.size() - 6, 6, "apiKey") == 0;
    ImGui::InputText("##v", &value,
                     secret && !showSecrets ? ImGuiInputTextFlags_Password : 0);
    *static_cast<std::string *>(leaf) = value;
    break;
  }
  case SettingsSchema::ValueType::EnumString: {
    std::string value = *static_cast<std::string *>(leaf);
    if (ImGui::BeginCombo("##v", value.c_str())) {
      for (const std::string &option : field.enumValues) {
        if (ImGui::Selectable(option.c_str(), option == value)) {
          *static_cast<std::string *>(leaf) = option;
        }
        if (option == value) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    break;
  }
  case SettingsSchema::ValueType::Json: {
    nlohmann::json &value = *static_cast<nlohmann::json *>(leaf);
    const std::string key = profileId + "|" + field.path;
    std::string &buffer = jsonBuffers[key];
    if (buffer.empty()) {
      buffer = value.is_null() ? "{}" : value.dump(2);
    }
    ImGui::InputTextMultiline("##json", &buffer, ImVec2(-1, 80));
    if (ImGui::Button("Apply JSON")) {
      try {
        value = nlohmann::json::parse(buffer);
      } catch (const std::exception &error) {
        Report(false, std::string("Invalid JSON: ") + error.what());
      }
    }
    break;
  }
  default:
    break;
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}

void SettingsWindow::RefreshPlayerActorSprite() {
  std::string message;
  game->ApplyPlayerActorSprite(Settings().player.actorSprite, message);
}

void SettingsWindow::SelectActorSprite(
    const SettingsSchema::FieldSpec &field, const std::string &id) {
  // Visual first: a sprite the registry names but that has no art must not be
  // persisted, or the next launch would warn about a look nobody can see.
  std::string applied;
  if (!game->ApplyPlayerActorSprite(id, applied)) {
    Report(false, applied);
    return;
  }
  std::string message;
  const bool saved =
      ApplySettingValue(SettingsFilePath(), field.path, id, message);
  if (!applied.empty()) {
    message += (message.empty() ? "" : " ") + applied;
  }
  Report(saved, message);
  overrideCacheDirty = true;
}

void SettingsWindow::DrawPlayerActorSpriteRow(
    const SettingsSchema::FieldSpec &field) {
  GameSettings &settings = MutableSettings();
  const std::string current =
      *static_cast<std::string *>(field.access(settings));

  ImGui::PushID(field.path.c_str());
  ImGui::TextUnformatted(field.Label().c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s\n\n%s", field.path.c_str(), field.doc.c_str());
  }
  ImGui::SameLine(240.0f);
  ImGui::SetNextItemWidth(190.0f);
  ImGui::InputTextWithHint("##filter", "filter sprites", &actorSpriteFilter);
  ImGui::SameLine();
  ImGui::Checkbox("without art", &showActorSpritesWithoutArt);
  ImGui::SameLine();
  ImGui::TextDisabled("%s", current.empty() ? "(race default)" : current.c_str());

  std::vector<SpritePicker::Item> items;
  std::vector<std::string> keys;
  const auto addCell = [&](const std::string &key, const std::string &label,
                           const std::string &description,
                           const std::string &placeholder,
                           const Texture2D *texture) {
    SpritePicker::Item item;
    item.label = label;
    item.description = description;
    item.placeholder = placeholder;
    item.texture = texture;
    item.selected = (current == key);
    items.push_back(std::move(item));
    keys.push_back(key);
  };

  addCell(std::string(), "Race default",
          "The race's own look, and then the ASCII art.", "race", nullptr);

  const std::string needle = Lower(actorSpriteFilter);
  const auto matches = [&needle](const std::string &id, const std::string &name,
                                 const std::string &description) {
    if (needle.empty()) return true;
    return Lower(id).find(needle) != std::string::npos ||
           Lower(name).find(needle) != std::string::npos ||
           Lower(description).find(needle) != std::string::npos;
  };

  int shown = 0;
  for (const ActorSpriteDef *definition : game->actorSpriteRegistry.GetAll()) {
    const std::string name =
        definition->name.empty() ? definition->id : definition->name;
    if (!matches(definition->id, name, definition->description)) continue;
    const Texture2D *texture =
        ResolveSpriteTexture(std::string(), definition->id, game->resourceManager,
                             nullptr);
    if (texture == nullptr && !showActorSpritesWithoutArt) continue;
    addCell(definition->id, name, definition->description, "?", texture);
    ++shown;
  }

  SpritePicker::Options options;
  options.cell = 52.0f;
  options.rows = 3;
  options.childId = "##player-actor-sprites";
  const int clicked = SpritePicker::Draw(items, options);
  if (clicked >= 0) {
    SelectActorSprite(field, keys[clicked]);
  }

  if (shown == 0) {
    // Distinguish "the filter hides everything" from "the catalogue has no art
    // yet", because the second is the state a fresh checkout is in and the fix is
    // somewhere else entirely.
    if (game->actorSpriteRegistry.Count() == 0) {
      ImGui::TextDisabled(
          "No actor sprites are registered (data/actor_sprites).");
    } else if (actorSpriteFilter.empty() && !showActorSpritesWithoutArt) {
      ImGui::TextDisabled(
          "No actor sprite has art yet. Generate them in the Content Editor's "
          "actor sprites, or tick \"without art\".");
    } else {
      ImGui::TextDisabled("No actor sprites match.");
    }
  }
  ImGui::PopID();
}

void SettingsWindow::DrawAIProfiles() {
  GameSettings &settings = MutableSettings();
  std::string removeId;
  // Saving reloads the settings (references are resolved at load), which would
  // rebuild the map being iterated. The request is therefore captured here and
  // applied after the loop.
  std::string saveId;
  nlohmann::json saveEntry;

  for (auto &[id, profile] : settings.ai.profiles) {
    ImGui::PushID(id.c_str());
    const std::string header =
        id + (profile.label.empty() || profile.label == id
                  ? ""
                  : "  (" + profile.label + ")");
    if (ImGui::CollapsingHeader(header.c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextDisabled("kind %s%s", profile.kind.c_str(),
                         profile.preset.empty()
                             ? ""
                             : (", preset " + profile.preset).c_str());
      for (const SettingsSchema::AIProfileFieldSpec &field :
           SettingsSchema::AIProfileFields()) {
        DrawAIProfileField(field, profile, id);
      }
      if (ImGui::Button("Save profile")) {
        saveId = id;
        saveEntry = AIProfileToJson(profile);
      }
      ImGui::SameLine();
      if (ImGui::Button("Remove profile")) {
        removeId = id;
      }
    }
    ImGui::PopID();
  }

  if (!saveId.empty()) {
    std::string message;
    const bool ok = ApplyAIProfileValue(SettingsFilePath(), saveId, saveEntry,
                                        message);
    Report(ok, message);
    overrideCacheDirty = true;
  }

  if (!removeId.empty()) {
    std::string message;
    const bool ok = RemoveAIProfile(SettingsFilePath(), removeId, message);
    Report(ok, message);
    overrideCacheDirty = true;
  }

  ImGui::Separator();
  ImGui::SetNextItemWidth(220.0f);
  ImGui::InputTextWithHint("##newprofile", "new profile id", &newProfileId);
  ImGui::SameLine();
  ImGui::BeginDisabled(newProfileId.empty() ||
                       settings.ai.profiles.count(newProfileId) != 0);
  if (ImGui::Button("Add profile")) {
    GameSettings::AIProfileSection profile;
    profile.kind = "s2";
    std::string message;
    const bool ok = ApplyAIProfileValue(
        SettingsFilePath(), newProfileId, AIProfileToJson(profile), message);
    Report(ok, message);
    overrideCacheDirty = true;
    newProfileId.clear();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled(
      "Ids are the key in ai.profiles; model blocks pick one by name.");
}

void SettingsWindow::DrawProfiles() {
  GameSettings &settings = MutableSettings();
  int removeIndex = -1;

  for (std::size_t i = 0; i < settings.system1.profiles.size(); ++i) {
    GameSettings::SystemOneProfileSection &profile =
        settings.system1.profiles[i];
    ImGui::PushID(static_cast<int>(i));
    const std::string header =
        profile.id + (profile.label.empty() ? "" : " (" + profile.label + ")");
    if (ImGui::CollapsingHeader(header.c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      const bool entryHasProfile = !profile.profile.empty();
      const bool entryRevealed =
          revealedEntries.find(profile.id) != revealedEntries.end();
      for (const SettingsSchema::CollectionFieldSpec &field :
           SettingsSchema::Profiles().fields) {
        // The profile supplies the model block; calibration and timeout stay
        // visible because they are per-entry.
        if (entryHasProfile && !entryRevealed &&
            field.path.rfind("model.", 0) == 0) {
          continue;
        }
        DrawProfileField(field, profile);
      }
      if (ImGui::Button("Remove this profile")) {
        removeIndex = static_cast<int>(i);
      }
    }
    ImGui::PopID();
  }

  if (removeIndex >= 0) {
    settings.system1.profiles.erase(settings.system1.profiles.begin() +
                                    removeIndex);
    CommitProfiles();
  }

  ImGui::Separator();
  if (ImGui::Button("Add profile")) {
    GameSettings::SystemOneProfileSection profile;
    profile.id = "profile_" +
                 std::to_string(settings.system1.profiles.size() + 1);
    settings.system1.profiles.push_back(std::move(profile));
    CommitProfiles();
  }
  ImGui::SameLine();
  ImGui::TextDisabled(
      "The whole list is written to the override file: a merge patch cannot "
      "remove an element of an array.");
}
