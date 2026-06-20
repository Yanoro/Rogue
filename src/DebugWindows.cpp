#include "DebugWindows.h"
#include "Game.h"
#include "imgui.h"
#include "Components.h"
#include "DebugLog.h"
#include "DebugWindowState.h"
#include "Defaults.h"
#include "MapReloader.h"
#include "DrawAsciiDebug.h"
#include "EntityInfoWindow.h"
#include <vector>
#include <string>
#include <algorithm>

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
  ImGui::Begin("Debug Console", &isOpen, ImGuiWindowFlags_AlwaysAutoResize);
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
      game->playerEntity.set<ActiveWindow>({std::make_shared<EntityInfoWindow>(game->playerEntity, game->map.get())});
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

  bool showFont = game->fontSelectionWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Font Selection", &showFont)) {
    if (showFont) {
      game->fontSelectionWindowEntity.set<ActiveWindow>({std::make_shared<FontSelectionWindow>(game)});
    } else {
      game->fontSelectionWindowEntity.remove<ActiveWindow>();
    }
  }

  ImGui::Separator();
  ImGui::Text("All debug windows can be closed by clicking the X button.");

  ImGui::End();
}

TileInfoWindow::TileInfoWindow(Game* game) : game(game) {}

void TileInfoWindow::Draw() {
  bool isOpen = true;
  ImGui::Begin("Tile Info", &isOpen);
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
  ImGui::Begin("A*", &isOpen);
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
  ImGui::Begin("Entity Overview", &isOpen);
  if (!isOpen) {
    game->entityOverviewWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  static ImGuiTextFilter filter;
  filter.Draw("Filter");

  ImGui::Separator();

  if (ImGui::BeginTable("EntitiesTable", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable |
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
  ImGui::Begin("Debug Log", &isOpen, ImGuiWindowFlags_AlwaysAutoResize);
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

  if (ImGui::BeginChild("##LogContent", ImVec2(500, 300), ImGuiChildFlags_Borders)) {
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
  ImGui::Begin("Map Reload", &isOpen, ImGuiWindowFlags_AlwaysAutoResize);
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

    if (ImGui::BeginListBox("##MapList", ImVec2(-1, 200))) {
      for (size_t i = 0; i < mapList.size(); i++) {
        bool isSelected = (selectedMapIndex == (int)i);
        if (ImGui::Selectable(mapList[i].c_str(), isSelected)) {
          selectedMapIndex = i;
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
  ImGui::Begin("DrawAscii Debug", &isOpen, ImGuiWindowFlags_AlwaysAutoResize);
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
  ImGui::Begin("Font Selection", &isOpen, ImGuiWindowFlags_AlwaysAutoResize);
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

    if (ImGui::Combo("##FontList", &game->selectedFontIndex, items.data(), items.size())) {
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
