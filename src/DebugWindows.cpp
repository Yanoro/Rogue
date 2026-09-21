#include "DebugWindows.h"
#include "Game.h"
#include "AgentBrain.h"
#include "ObjectFactory.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "Components.h"
#include "DebugLog.h"
#include "DebugWindowState.h"
#include "Defaults.h"
#include "MapReloader.h"
#include "DrawAsciiDebug.h"
#include "EntityInfoWindow.h"
#include "AgentContextWindow.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>

// Helper function to format JSON with indentation for pretty printing
static std::string PrettyPrintJson(const char *json) {
  if (!json) return "";

  std::string result;
  int indent = 0;
  bool inString = false;
  bool escapeNext = false;

  for (size_t i = 0; i < strlen(json); ++i) {
    char c = json[i];

    if (escapeNext) {
      result += c;
      escapeNext = false;
      continue;
    }

    if (c == '\\' && inString) {
      result += c;
      escapeNext = true;
      continue;
    }

    if (c == '"') {
      inString = !inString;
      result += c;
      continue;
    }

    if (inString) {
      result += c;
      continue;
    }

    switch (c) {
    case '{':
    case '[':
      result += c;
      result += '\n';
      indent++;
      for (int j = 0; j < indent * 2; ++j) result += ' ';
      break;
    case '}':
    case ']':
      result += '\n';
      indent--;
      for (int j = 0; j < indent * 2; ++j) result += ' ';
      result += c;
      break;
    case ',':
      result += c;
      result += '\n';
      for (int j = 0; j < indent * 2; ++j) result += ' ';
      break;
    case ':':
      result += c;
      result += ' ';
      break;
    case ' ':
    case '\n':
    case '\t':
      // Skip whitespace
      break;
    default:
      result += c;
    }
  }

  return result;
}

DebugConsoleWindow::DebugConsoleWindow(Game* game) : game(game) {}

void DebugConsoleWindow::Draw() {
  bool isOpen = true;
  // Resizable: give it a starting size instead of AlwaysAutoResize.
  ImGui::SetNextWindowSize(ImVec2(620, 320), ImGuiCond_FirstUseEver);
  ImGui::Begin("Debug Console", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->debugConsoleWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  ImGui::Text("Debug Windows");
  ImGui::Separator();

  bool showEntityInfo = game->playerEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Entity Info Window", &showEntityInfo)) {
    if (showEntityInfo) {
      game->playerEntity.set<ActiveWindow>({std::make_shared<EntityInfoWindow>(game->playerEntity)});
    } else {
      game->playerEntity.remove<ActiveWindow>();
    }
  }
  ImGui::SameLine();

  bool showTileInfo = game->tileInfoWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Tile Info Window", &showTileInfo)) {
    if (showTileInfo) {
      game->tileInfoWindowEntity.set<ActiveWindow>({std::make_shared<TileInfoWindow>(game)});
    } else {
      game->tileInfoWindowEntity.remove<ActiveWindow>();
    }
  }

  bool showAStar = game->astarWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("A* Window", &showAStar)) {
    if (showAStar) {
      game->astarWindowEntity.set<ActiveWindow>({std::make_shared<AStarWindow>(game)});
    } else {
      game->astarWindowEntity.remove<ActiveWindow>();
    }
  }
  ImGui::SameLine();

  bool showOverview = game->entityOverviewWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Entity Overview", &showOverview)) {
    if (showOverview) {
      game->entityOverviewWindowEntity.set<ActiveWindow>({std::make_shared<EntityOverviewWindow>(game)});
    } else {
      game->entityOverviewWindowEntity.remove<ActiveWindow>();
    }
  }

  bool showLog = game->debugLogWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Debug Log Window", &showLog)) {
    if (showLog) {
      game->debugLogWindowEntity.set<ActiveWindow>({std::make_shared<DebugLogWindow>(game)});
    } else {
      game->debugLogWindowEntity.remove<ActiveWindow>();
    }
  }
  ImGui::SameLine();

  bool showMap = game->mapReloadWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Map Reload Window", &showMap)) {
    if (showMap) {
      game->mapReloadWindowEntity.set<ActiveWindow>({std::make_shared<MapReloadWindow>(game)});
    } else {
      game->mapReloadWindowEntity.remove<ActiveWindow>();
    }
  }

  bool showAscii = game->drawAsciiToggleWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("DrawAscii Toggle", &showAscii)) {
    if (showAscii) {
      game->drawAsciiToggleWindowEntity.set<ActiveWindow>({std::make_shared<DrawAsciiDebugWindow>(game)});
    } else {
      game->drawAsciiToggleWindowEntity.remove<ActiveWindow>();
    }
  }
  ImGui::SameLine();

  bool showLocations = game->debugWindowState->GetShowLocations();
  if (ImGui::Checkbox("Show Locations", &showLocations)) {
    game->debugWindowState->SetShowLocations(showLocations);
  }

  bool showFont = game->fontSelectionWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Font Selection", &showFont)) {
    if (showFont) {
      game->fontSelectionWindowEntity.set<ActiveWindow>({std::make_shared<FontSelectionWindow>(game)});
    } else {
      game->fontSelectionWindowEntity.remove<ActiveWindow>();
    }
  }
  ImGui::SameLine();

  bool showMapEditor = game->mapEditorWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Map Editor", &showMapEditor)) {
    if (showMapEditor) {
      game->mapEditorWindowEntity.set<ActiveWindow>({std::make_shared<MapEditorWindow>(game)});
    } else {
      game->mapEditorWindowEntity.remove<ActiveWindow>();
    }
  }

  ImGui::SameLine();

  bool showAIMenu = game->aiMenuWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("AI Menu", &showAIMenu)) {
    if (showAIMenu) {
      game->aiMenuWindowEntity.set<ActiveWindow>({std::make_shared<AIMenuWindow>(game)});
    } else {
      game->aiMenuWindowEntity.remove<ActiveWindow>();
    }
  }

  ImGui::SameLine();

  bool showNPCMenu = game->npcMenuWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("NPC Menu", &showNPCMenu)) {
    if (showNPCMenu) {
      game->npcMenuWindowEntity.set<ActiveWindow>({std::make_shared<NPCMenuWindow>(game)});
    } else {
      game->npcMenuWindowEntity.remove<ActiveWindow>();
    }
  }

  ImGui::Separator();
  
  bool stopAllAI = game->debugWindowState->GetStopAllAI();
  if (ImGui::Checkbox("Stop All AI Agents", &stopAllAI)) {
    game->debugWindowState->SetStopAllAI(stopAllAI);
    game->ecs.filter<AgentBrainWrapper>().each([&](flecs::entity, AgentBrainWrapper& wrapper) {
      if (wrapper.agBrain) {
        wrapper.agBrain->isStopped = stopAllAI;
      }
    });
  }

  ImGui::Separator();
  ImGui::Text("All debug windows can be closed by clicking the X button.");

  ImGui::End();
}

TileInfoWindow::TileInfoWindow(Game* game) : game(game) {}

void TileInfoWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(360, 240), ImGuiCond_FirstUseEver);
  ImGui::Begin("Tile Info", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->tileInfoWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  if (game->hasClicked) {
    ImGui::Text("Map Coordinates: (%d, %d)", game->lastClickedPos.x, game->lastClickedPos.y);

    if (Location *loc = game->map->GetLocation(game->lastClickedPos)) {
      ImGui::Separator();
      ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Location: %s", loc->name.c_str());
      ImGui::TextWrapped("%s", loc->description.c_str());
    }

    ImGui::Separator();

    if (game->validTileSelected && game->selectedTile.is_alive()) {
      ImGui::Text("Tile Entity ID: %lu", game->selectedTile.id());

      if (const ScreenPosition *sPos = game->selectedTile.get<ScreenPosition>()) {
        ImGui::Text("Screen Position: (%.2f, %.2f)", sPos->x, sPos->y);
      }

      if (game->selectedTile.has<BlocksTile>()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Blocks Path: YES");
      } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Blocks Path: NO");
      }
    } else {
      ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "No valid tile entity at this position.");
    }
  } else {
    ImGui::Text("Click anywhere on the map to inspect.");
  }

  ImGui::End();
}

AStarWindow::AStarWindow(Game* game) : game(game) {}

void AStarWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
  ImGui::Begin("A*", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->astarWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  if (game->isSelectingAStarPath) {
    if (ImGui::Button("Cancel Selection")) {
      game->isSelectingAStarPath = false;
      game->astarClickCount = 0;
    }
    ImGui::Text("Click %d/2 on map", game->astarClickCount + 1);
  } else if (game->isSettingPlayerTarget) {
    if (ImGui::Button("Cancel Player Target")) {
      game->isSettingPlayerTarget = false;
    }
    ImGui::Text("Click on map to set target");
  } else {
    if (ImGui::Button("Select Path")) {
      game->isSelectingAStarPath = true;
      game->astarClickCount = 0;
      game->astarStartPos = {0, 0};
      game->astarEndPos = {0, 0};
      game->astarPath.clear();
    }
    if (ImGui::Button("Set Player Target")) {
      game->isSettingPlayerTarget = true;
    }
  }

  ImGui::End();
}

EntityOverviewWindow::EntityOverviewWindow(Game* game) : game(game) {}

void EntityOverviewWindow::Draw() {
  struct OpenComponentWindow {
    flecs::entity entity;
    flecs::id id;
  };
  static std::vector<OpenComponentWindow> openComponentWindows;

  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(760, 460), ImGuiCond_FirstUseEver);
  ImGui::Begin("Entity Overview", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->entityOverviewWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  static ImGuiTextFilter filter;
  filter.Draw("Filter");

  ImGui::Separator();

  // ScrollY keeps the header visible and lets the table follow the window size;
  // without it the list clips at the window edge.
  if (ImGui::BeginTable("EntitiesTable", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Position");
    ImGui::TableSetupColumn("Components");
    ImGui::TableHeadersRow();

    game->ecs.filter_builder().term(flecs::Any).build().each([&](flecs::entity e) {
      std::string path = e.path().c_str() ? e.path().c_str() : "";
      if (path.find("::flecs") == 0) {
        return;
      }

      std::string name = e.name().c_str() ? e.name().c_str() : "Unnamed";
      std::string idStr = std::to_string(e.id());
      std::string searchString = name + " " + idStr;

      if (filter.PassFilter(searchString.c_str())) {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Text("%lu", e.id());

        ImGui::TableNextColumn();
        ImGui::Text("%s", name.c_str());

        ImGui::TableNextColumn();
        if (const GamePosition *pos = e.get<GamePosition>()) {
          ImGui::Text("(%d, %d)", pos->x, pos->y);
        } else if (const ScreenPosition *sPos = e.get<ScreenPosition>()) {
          ImGui::Text("Screen(%.1f, %.1f)", sPos->x, sPos->y);
        } else {
          ImGui::Text("-");
        }

        ImGui::TableNextColumn();

        e.each([&](flecs::id id) {
          std::string label = std::string(id.str().c_str()) + "##" +
                              std::to_string(e.id()) + "_" +
                              std::to_string(id.raw_id());
          if (ImGui::SmallButton(label.c_str())) {
            openComponentWindows.push_back({e, id});
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ID: %lu", id.raw_id());
          }
          ImGui::SameLine();
        });
        ImGui::Text(" ");
      }
    });

    ImGui::EndTable();
  }

  ImGui::End();

  for (auto it = openComponentWindows.begin();
       it != openComponentWindows.end();) {
    bool open = true;
    std::string winName =
        "Component: " + std::string(it->id.str().c_str()) + " (" +
        std::string(it->entity.name().c_str() ? it->entity.name().c_str()
                                              : "Unnamed") +
        ")##" + std::to_string(it->entity.id()) + "_" +
        std::to_string(it->id.raw_id());

    ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(winName.c_str(), &open)) {
      const void *ptr = it->entity.get(it->id.raw_id());
      if (ptr) {
        char *json = ecs_ptr_to_json(game->ecs.c_ptr(), it->id.raw_id(), ptr);
        if (json) {
          std::string prettyJson = PrettyPrintJson(json);
          ImGui::TextWrapped("%s", prettyJson.c_str());
          ecs_os_free(json);
        } else {
          ImGui::TextDisabled("No JSON representation available.");
        }
      } else {
        ImGui::TextDisabled("No data for this component (might be a tag).");
      }
    }
    ImGui::End();

    if (!open) {
      it = openComponentWindows.erase(it);
    } else {
      ++it;
    }
  }
}

DebugLogWindow::DebugLogWindow(Game* game) : game(game) {}

void DebugLogWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_FirstUseEver);
  ImGui::Begin("Debug Log", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->debugLogWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  ImGui::Text("Debug Log (Latest %zu entries)", game->debugLog->GetMaxEntries());
  ImGui::Separator();

  if (ImGui::Button("Clear Log")) {
    game->debugLog->Clear();
  }

  // Fills whatever is left of the window so resizing grows the log view rather
  // than leaving a fixed 500x300 hole in it.
  if (ImGui::BeginChild("##LogContent", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    const auto &entries = game->debugLog->GetEntries();
    for (const auto &entry : entries) {
      ImVec4 color;
      switch (entry.level) {
      case DebugLog::LogLevel::INFO:
        color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        break;
      case DebugLog::LogLevel::WARNING:
        color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
        break;
      case DebugLog::LogLevel::ERROR:
        color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
        break;
      case DebugLog::LogLevel::DEBUG:
        color = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
        break;
      }
      ImGui::TextColored(color, "[%s] %s", entry.timestamp.c_str(), entry.message.c_str());
    }

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetItemRectSize().y) {
      ImGui::SetScrollHereY(1.0f);
    }
  }
  ImGui::EndChild();
  ImGui::End();
}

MapReloadWindow::MapReloadWindow(Game* game) : game(game) {}

void MapReloadWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(420, 460), ImGuiCond_FirstUseEver);
  ImGui::Begin("Map Reload", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->mapReloadWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  ImGui::Text("Available Maps");
  ImGui::Separator();

  if (ImGui::Button("Refresh Map List")) {
    game->mapReloader->RefreshMapList();
    game->debugLog->LogInfo("Map list refreshed");
  }

  ImGui::Separator();

  const auto &mapList = game->mapReloader->GetMapList();
  if (mapList.empty()) {
    ImGui::TextDisabled("No maps found in: %s", game->mapReloader->GetDirectory().c_str());
  } else {
    static int selectedMapIndex = 0;

    // Sized to fill the window so the list grows and shrinks with it.
    if (ImGui::BeginListBox("##MapList", ImVec2(-1, -1))) {
      for (size_t i = 0; i < mapList.size(); i++) {
        bool isSelected = (selectedMapIndex == (int)i);
        if (ImGui::Selectable(mapList[i].c_str(), isSelected)) {
          selectedMapIndex = i;
          std::string mapPath = game->mapReloader->GetMapPath(i);
          game->LoadMap(mapPath);
          game->debugLog->LogInfo("Loaded map: " + mapList[i]);
        }
        if (isSelected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndListBox();
    }

    if (selectedMapIndex >= 0 && selectedMapIndex < (int)mapList.size()) {
      std::string mapPath = game->mapReloader->GetMapPath(selectedMapIndex);
      ImGui::Text("Selected: %s", mapList[selectedMapIndex].c_str());
      ImGui::Text("Path: %s", mapPath.c_str());
    }
  }

  ImGui::End();
}

DrawAsciiDebugWindow::DrawAsciiDebugWindow(Game* game) : game(game) {}

void DrawAsciiDebugWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(340, 220), ImGuiCond_FirstUseEver);
  ImGui::Begin("DrawAscii Debug", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->drawAsciiToggleWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  ImGui::Text("DrawAscii Rectangle Toggles");
  ImGui::Separator();

  bool showOuter = DrawAsciiDebug::GetShowOuterRectangles();
  if (ImGui::Checkbox("Show Outer Rectangles (RED)", &showOuter)) {
    DrawAsciiDebug::SetShowOuterRectangles(showOuter);
    game->debugLog->LogInfo(showOuter ? "Outer rectangles enabled" : "Outer rectangles disabled");
  }

  bool showInner = DrawAsciiDebug::GetShowInnerRectangles();
  if (ImGui::Checkbox("Show Inner Rectangles (BLUE)", &showInner)) {
    DrawAsciiDebug::SetShowInnerRectangles(showInner);
    game->debugLog->LogInfo(showInner ? "Inner rectangles enabled" : "Inner rectangles disabled");
  }

  ImGui::Separator();
  ImGui::Text("Red: Tile boundaries");
  ImGui::Text("Blue: Text boundaries");

  ImGui::End();
}

FontSelectionWindow::FontSelectionWindow(Game* game) : game(game) {}

void FontSelectionWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
  ImGui::Begin("Font Selection", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->fontSelectionWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  ImGui::Text("Available Fonts");
  ImGui::Separator();

  if (!game->availableFontPaths.empty()) {
    static std::vector<std::string> fontDisplayNames;
    
    if (fontDisplayNames.empty()) {
      for (const auto &path : game->availableFontPaths) {
        size_t lastSlash = path.find_last_of("/\\");
        std::string filename = (lastSlash == std::string::npos) 
                               ? path 
                               : path.substr(lastSlash + 1);
        fontDisplayNames.push_back(filename);
      }
    }

    std::vector<const char *> items;
    for (const auto &name : fontDisplayNames) {
      items.push_back(name.c_str());
    }

    bool fontChanged = false;

    ImGui::PushItemWidth(150);
    if (ImGui::Combo("##FontList", &game->selectedFontIndex, items.data(), items.size())) {
      fontChanged = true;
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Next")) {
      if (!items.empty()) {
        game->selectedFontIndex = (game->selectedFontIndex + 1) % items.size();
        fontChanged = true;
      }
    }

    if (fontChanged) {
      if (game->selectedFontIndex >= 0 && game->selectedFontIndex < (int)game->availableFontPaths.size()) {
        if (game->gameFont.glyphCount > 0) {
          UnloadFont(game->gameFont);
        }
        
        const std::string &fontPath = game->availableFontPaths[game->selectedFontIndex];
        game->gameFont = LoadFontEx(fontPath.c_str(), DEFAULT_FONTSIZE, NULL, 0);
        SetTextureFilter(game->gameFont.texture, TEXTURE_FILTER_POINT);
        
        game->debugLog->LogInfo("Font loaded: " + fontPath);
      }
    }

    ImGui::Separator();

    if (game->selectedFontIndex >= 0 && game->selectedFontIndex < (int)fontDisplayNames.size()) {
      ImGui::Text("Preview:");
      ImGui::Text("%s", "The quick brown fox jumps over the lazy dog.");
      ImGui::Text("%s", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
      ImGui::Text("%s", "abcdefghijklmnopqrstuvwxyz");
      ImGui::Text("%s", "0123456789 !@#$^&*()");

      ImGui::Separator();
      if (ImGui::Button("Set to default")) {
        game->debugWindowState->SetDefaultFontPath(game->availableFontPaths[game->selectedFontIndex]);
        game->debugWindowState->SaveState("./debug_windows_state.json");
        if (game->debugLog) {
          game->debugLog->LogInfo("Default font set to: " + game->availableFontPaths[game->selectedFontIndex]);
        }
      }
    }
  } else {
    ImGui::TextDisabled("No fonts found in ./fonts directory");
  }

  ImGui::End();
}

MapEditorWindow::MapEditorWindow(Game* game) : game(game) {}

void MapEditorWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
  ImGui::Begin("Map Editor", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->mapEditorWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }
  
  ImGui::Text("Selected: %s", game->editorSelection.type != EditorSelectionType::None ? game->editorSelection.name.c_str() : "None");
  if (game->editorSelection.type != EditorSelectionType::None) {
    ImGui::SameLine();
    if (ImGui::Button("Clear Selection")) {
      game->editorSelection.type = EditorSelectionType::None;
      game->editorSelection.name = "";
    }
  }

  ImGui::Separator();
  ImGui::Text("Locations");
  if (ImGui::Checkbox("Create Location##MapEditor", &game->createLocationMode)) {
    if (!game->createLocationMode) {
      game->isDraggingLocation = false;
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Arm this, then press and drag on the map to mark the "
                      "tiles of a new location. Releasing asks for a name.");
  }
  if (game->createLocationMode) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                       "Drag on the map to mark the new location's tiles.");
  }

  ImGui::Checkbox("Remove Location##MapEditor", &game->removeLocationMode);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Arm this, then click a location on the map to delete "
                      "it. Removals are not written to the map file.");
  }

  // Trash button: toggles the object-removal mode. The armed-looking style is
  // pushed and popped around the button using the same value, because the button
  // itself flips the mode: reading the (now flipped) state when deciding whether
  // to pop would unbalance the style stack on the very click that arms it.
  ImGui::SameLine();
  const bool removeObjectLit = game->removeObjectMode;
  if (removeObjectLit) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.25f, 0.25f, 1.0f));
  }
  if (ImGui::Button("[x] Remove Object##MapEditor")) {
    game->removeObjectMode = !game->removeObjectMode;
  }
  if (removeObjectLit) {
    ImGui::PopStyleColor(3);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Trash: while lit, clicking a tile deletes the object "
                      "placed on it (and what the map file would record).");
  }

  if (game->removeLocationMode) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                       "Click a location on the map to remove it.");
  }
  if (game->removeObjectMode) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                       "Trash armed: click a tile to delete the object on it.");
  }

  if (!game->lastRemovedLocationName.empty()) {
    ImGui::Text("Last removed location: %s",
                game->lastRemovedLocationName.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##LastRemovedLocation")) {
      game->lastRemovedLocationName.clear();
    }
  }
  if (!game->lastRemovedObjectName.empty()) {
    ImGui::Text("Last removed object: %s",
                game->lastRemovedObjectName.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##LastRemovedObject")) {
      game->lastRemovedObjectName.clear();
    }
  }
  ImGui::Separator();

  ImGui::Text("Map File");
  if (ImGui::Button("Save Map to File")) {
    if (game->SaveMapToFile()) {
      game->lastMapSaveMessage = "Saved.";
    } else {
      game->lastMapSaveMessage = "Save failed - see the debug log.";
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Rewrite the current map file with the editor's tile "
                      "layout, locations and placed objects.");
  }
  if (!game->mapFilePath.empty()) {
    ImGui::TextDisabled("Target: %s", game->mapFilePath.c_str());
  }
  if (!game->lastMapSaveMessage.empty()) {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                       game->lastMapSaveMessage.c_str());
  }
  ImGui::Separator();

  // The palette and template tables grow with the map's content, so they live in
  // a scroll region that fills the window. Without this a resized window just
  // clips them, and the content would force the window taller every frame.
  if (ImGui::BeginChild("##MapEditorContent", ImVec2(0, 0))) {
    ImGui::Text("Map Tiles");
    ImGui::Separator();
    if (ImGui::BeginTable("TilesTable", 8)) {
      for (const auto& tile : game->map->GetUniqueTiles()) {
        ImGui::TableNextColumn();
        ImVec4 bgColor(tile->ascii->backgroundColor.r / 255.0f, tile->ascii->backgroundColor.g / 255.0f, tile->ascii->backgroundColor.b / 255.0f, tile->ascii->backgroundColor.a / 255.0f);
        ImVec4 fgColor(tile->ascii->characterColor.r / 255.0f, tile->ascii->characterColor.g / 255.0f, tile->ascii->characterColor.b / 255.0f, tile->ascii->characterColor.a / 255.0f);
        
        ImGui::PushStyleColor(ImGuiCol_Button, bgColor);
        ImGui::PushStyleColor(ImGuiCol_Text, fgColor);
        
        bool isSelected = (game->editorSelection.type == EditorSelectionType::Tile && game->editorSelection.name == tile->name);
        if (isSelected) {
          ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 0, 1));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        }

        std::string label = std::string(1, tile->ascii->ch) + "##" + tile->name;
        if (ImGui::Button(label.c_str(), ImVec2(32, 32))) {
          game->editorSelection.type = EditorSelectionType::Tile;
          game->editorSelection.name = tile->name;
        }
        
        ImGui::PopStyleColor(2);
        if (isSelected) {
          ImGui::PopStyleColor();
          ImGui::PopStyleVar();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", tile->name.c_str());
        }
      }
      ImGui::EndTable();
    }
  
    ImGui::Text("Object Templates");
    ImGui::Separator();
    if (ImGui::BeginTable("ObjectsTable", 8)) {
      for (const auto& pair : game->objectFactory.GetTemplates()) {
        ImGui::TableNextColumn();
        
        char ch = '?';
        ImVec4 bgColor(0,0,0,0);
        ImVec4 fgColor(1,1,1,1);
        
        if (pair.second.contains("character")) {
           std::string charStr = pair.second["character"];
           if (!charStr.empty()) ch = charStr[0];
        }
        
        if (pair.second.contains("characterColor")) {
           auto& c = pair.second["characterColor"];
           if (c.size() >= 3) {
              fgColor = ImVec4(c[0].get<float>()/255.0f, c[1].get<float>()/255.0f, c[2].get<float>()/255.0f, c.size() > 3 ? c[3].get<float>()/255.0f : 1.0f);
           }
        }
        if (pair.second.contains("backgroundColor")) {
           auto& c = pair.second["backgroundColor"];
           if (c.size() >= 3) {
              bgColor = ImVec4(c[0].get<float>()/255.0f, c[1].get<float>()/255.0f, c[2].get<float>()/255.0f, c.size() > 3 ? c[3].get<float>()/255.0f : 1.0f);
           }
        }
        
        ImGui::PushStyleColor(ImGuiCol_Button, bgColor);
        ImGui::PushStyleColor(ImGuiCol_Text, fgColor);
        
        bool isSelected = (game->editorSelection.type == EditorSelectionType::ObjectTemplate && game->editorSelection.name == pair.first);
        if (isSelected) {
          ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 0, 1));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        }

        std::string label = std::string(1, ch) + "##" + pair.first;
        if (ImGui::Button(label.c_str(), ImVec2(32, 32))) {
          game->editorSelection.type = EditorSelectionType::ObjectTemplate;
          game->editorSelection.name = pair.first;
        }
        
        ImGui::PopStyleColor(2);
        if (isSelected) {
          ImGui::PopStyleColor();
          ImGui::PopStyleVar();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", pair.first.c_str());
        }
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();

  ImGui::End();

  // The naming prompt only exists while a rectangle is waiting to be named.
  if (game->pendingLocationWantsFocus || game->pendingLocationHasFocus) {
    LocationNamingWindow(game).Draw();
  }
}

LocationNamingWindow::LocationNamingWindow(Game* game) : game(game) {}

void LocationNamingWindow::Draw() {
  if (!game->pendingLocationWantsFocus && !game->pendingLocationHasFocus) {
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(440, 240), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

  bool show = true;
  ImGui::Begin("New Location", &show, ImGuiWindowFlags_None);

  PendingLocation &pending = game->pendingLocation;
  const int markedWidth = pending.bottomRight.x - pending.topLeft.x + 1;
  const int markedHeight = pending.bottomRight.y - pending.topLeft.y + 1;

  ImGui::Text("Marked area: %d x %d tiles at (%d, %d)", markedWidth,
              markedHeight, pending.topLeft.x, pending.topLeft.y);
  ImGui::Separator();

  if (game->pendingLocationWantsFocus) {
    ImGui::SetKeyboardFocusHere();
    game->pendingLocationWantsFocus = false;
    game->pendingLocationHasFocus = true;
  }

  ImGui::Text("Name");
  ImGui::SetNextItemWidth(-1.0f);
  bool submitted = ImGui::InputText("##LocationName", &pending.name,
                                    ImGuiInputTextFlags_EnterReturnsTrue);

  ImGui::Text("Description (optional)");
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText("##LocationDescription", &pending.description);

  const bool nameIsEmpty = pending.name.empty();

  // The button is always drawn; the Enter key is handled separately below so an
  // Enter consumed by the text field is never read as a button activation.
  const bool createClicked = ImGui::Button("Create Location");
  ImGui::SameLine();
  const bool cancel = ImGui::Button("Cancel");

  const bool confirm = !nameIsEmpty && (createClicked || submitted);

  if (nameIsEmpty) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                       "A name is required.");
  }

  if (confirm) {
    game->map->AddLocation(pending.name, pending.description,
                           pending.topLeft, markedWidth, markedHeight);
    if (game->debugLog) {
      game->debugLog->LogInfo("Map editor: created location '" + pending.name +
                              "' (" + std::to_string(markedWidth) + "x" +
                              std::to_string(markedHeight) + " tiles at " +
                              std::to_string(pending.topLeft.x) + "," +
                              std::to_string(pending.topLeft.y) +
                              ") (in memory only)");
    }

    game->pendingLocation = PendingLocation{};
    game->pendingLocationHasFocus = false;
  } else if (cancel || !show) {
    game->pendingLocation = PendingLocation{};
    game->pendingLocationHasFocus = false;
  }

  ImGui::End();
}

AIMenuWindow::AIMenuWindow(Game* game) : game(game) {}

void AIMenuWindow::Draw() {
  bool show = game->aiMenuWindowEntity.has<ActiveWindow>();
  if (!show)
    return;
    
  ImGui::SetNextWindowSize(ImVec2(460, 280), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("AI Menu", &show, ImGuiWindowFlags_None)) {
    const AIBackend* backend = game->ecs.get<AIBackend>();
    if (backend && backend->ptr) {
      ImGui::Text("AI Backend: %s", backend->ptr->getAIName().c_str());
      ImGui::Text("Provider: %s", backend->ptr->getProviderName().c_str());
      ImGui::Text("Model Name: %s", backend->ptr->getModelName().c_str());
      ImGui::Text("Quantization: %s", backend->ptr->getQuantization().c_str());
      ImGui::Separator();
      ImGui::TextUnformatted(backend->ptr->getAdditionalInfo().c_str());
    } else {
      ImGui::Text("No AI initialized.");
    }
  }
  ImGui::End();

  if (!show) {
    game->aiMenuWindowEntity.remove<ActiveWindow>();
    game->debugWindowState->SetShowAIMenuWindow(false);
  } else {
    game->debugWindowState->SetShowAIMenuWindow(true);
  }
}

// Builds a short tag-style description of what an NPC is doing right now.
// Prefers the timed action entity referenced by Busy (e.g. harvesting), then
// falls back to the brain's current polymorphic action.
static std::string GetNPCActionLabel(const flecs::entity &npc) {
  if (const Busy *busy = npc.get<Busy>()) {
    if (busy->actionEntity.is_alive()) {
      std::string label =
          busy->actionEntity.has<HarvestAction>() ? "HARVEST" : "BUSY";
      if (const ActionTimer *timer = busy->actionEntity.get<ActionTimer>()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " (%.1fs)", timer->timeRemaining);
        label += buf;
      }
      return label;
    }
  }

  if (const AgentBrainWrapper *wrapper = npc.get<AgentBrainWrapper>()) {
    if (wrapper->agBrain) {
      if (AgentAction *current = wrapper->agBrain->getCurrentAction()) {
        return current->getActionName();
      }
    }
  }

  return "IDLE";
}

NPCMenuWindow::NPCMenuWindow(Game* game) : game(game) {}

void NPCMenuWindow::Draw() {
  bool show = game->npcMenuWindowEntity.has<ActiveWindow>();
  if (!show)
    return;

  ImGui::SetNextWindowSize(ImVec2(720, 440), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("NPC Menu", &show, ImGuiWindowFlags_None)) {
    static ImGuiTextFilter filter;
    filter.Draw("Filter", 180.0f);

    ImGui::Separator();

    // Applied after the iteration so we do not mutate the ECS while querying it.
    flecs::entity contextToToggle = flecs::entity::null();

    if (ImGui::BeginTable("NPCMenuTable", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
      ImGui::TableSetupColumn("Name");
      ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed,
                              90.0f);
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed,
                              130.0f);
      ImGui::TableSetupColumn("Stop Brain",
                              ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthFixed,
                              130.0f);
      ImGui::TableHeadersRow();

      game->ecs.filter<AgentBrainWrapper>().each(
          [&contextToToggle](flecs::entity npc, AgentBrainWrapper &wrapper) {
            if (!npc.is_alive()) {
              return;
            }

            std::string name = "Unnamed";
            if (const DisplayName *displayName = npc.get<DisplayName>()) {
              name = displayName->name;
            } else if (npc.name().c_str()) {
              name = npc.name().c_str();
            }

            std::string idStr = std::to_string(npc.id());
            if (!filter.PassFilter((name + " " + idStr).c_str())) {
              return;
            }

            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%lu", npc.id());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name.c_str());

            ImGui::TableNextColumn();
            if (const GamePosition *pos = npc.get<GamePosition>()) {
              ImGui::Text("(%d, %d)", pos->x, pos->y);
            } else {
              ImGui::TextDisabled("-");
            }

            ImGui::TableNextColumn();
            const std::string action = GetNPCActionLabel(npc);
            ImVec4 color(0.4f, 1.0f, 0.6f, 1.0f);
            if (action == "IDLE") {
              color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            } else if (action.find("HARVEST") != std::string::npos ||
                       action.find("BUSY") != std::string::npos) {
              color = ImVec4(1.0f, 0.7f, 0.2f, 1.0f);
            }
            ImGui::TextColored(color, "%s", action.c_str());

            ImGui::TableNextColumn();
            if (wrapper.agBrain) {
              ImGui::Checkbox(("##stopBrain" + idStr).c_str(),
                              &wrapper.agBrain->isStopped);
            } else {
              ImGui::TextDisabled("-");
            }

            ImGui::TableNextColumn();
            const bool hasContextWindow = npc.has<ActiveWindow>();
            std::string buttonLabel = (hasContextWindow ? "Close Context##"
                                                        : "Open Context##") +
                                      idStr;
            if (ImGui::Button(buttonLabel.c_str())) {
              contextToToggle = npc;
            }
          });

      ImGui::EndTable();
    }

    if (contextToToggle.is_alive()) {
      if (contextToToggle.has<ActiveWindow>()) {
        contextToToggle.remove<ActiveWindow>();
      } else {
        contextToToggle.set<ActiveWindow>(
            {std::make_shared<NPCContextWindow>(contextToToggle)});
      }
    }
  }
  ImGui::End();

  if (!show) {
    game->npcMenuWindowEntity.remove<ActiveWindow>();
    game->debugWindowState->SetShowNPCMenuWindow(false);
  } else {
    game->debugWindowState->SetShowNPCMenuWindow(true);
  }
}
