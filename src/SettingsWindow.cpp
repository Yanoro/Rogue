#include "SettingsWindow.h"

#include "Components.h"
#include "Game.h"
#include "Settings.h"
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

  ImGui::BeginDisabled(!present);
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
      for (const SettingsSchema::CollectionFieldSpec &field :
           SettingsSchema::Profiles().fields) {
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
