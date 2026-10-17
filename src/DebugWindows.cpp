#include "DebugWindows.h"
#include "Game.h"
#include "AgentBrain.h"
#include "AsciiArtDraw.hpp"
#include "SpriteDraw.hpp"
#include "SpritePaths.hpp"
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
#include "VisionChatWindow.h"
#include "SystemOneWindow.h"
#include "AgentContextWindow.h"
#include "BlueprintExpansion.hpp"
#include "BlueprintPreview.hpp"
#include "DesignerChatWindow.h"
#include "TileDesignerWindow.h"
#include <algorithm>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

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

  // The task board opens a new row rather than joining the one above, per the
  // rule stated below: the rows above already fill a narrow Debug Console and
  // ImGui clips past the right edge, which silently makes a checkbox
  // unclickable. The Designer and Tile Designer pair continues on this row.
  bool showTaskBoard = game->taskBoardWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Task Board", &showTaskBoard)) {
    if (showTaskBoard) {
      game->taskBoardWindowEntity.set<ActiveWindow>(
          {std::make_shared<TaskBoardWindow>(game)});
    } else {
      game->taskBoardWindowEntity.remove<ActiveWindow>();
    }
  }
  ImGui::SameLine();

  // Its own row for the same reason the rows above have one: the row already
  // fills a narrow Debug Console and ImGui clips anything past the right edge,
  // which silently makes a checkbox unclickable.
  bool showSystemOne = game->systemOneWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("System 1", &showSystemOne)) {
    if (showSystemOne) {
      game->systemOneWindowEntity.set<ActiveWindow>(
          {std::make_shared<SystemOneWindow>(game, game->systemOneRecorder.get())});
    } else {
      game->systemOneWindowEntity.remove<ActiveWindow>();
    }
  }

  ImGui::SameLine();
  bool showDesigner = game->designerWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Designer", &showDesigner)) {
    if (showDesigner && game->designerWindow) {
      // The instance is owned by Game, not by the component, so this only shows
      // or hides it: the conversation survives a toggle. See Game.h.
      game->designerWindowEntity.set<ActiveWindow>({game->designerWindow});
    } else {
      game->designerWindowEntity.remove<ActiveWindow>();
    }
  }
  // The tile designer shares the row: it shares the Designer's backend and is
  // used with it (invent a material, then design with it), so they belong where
  // they would be looked for together. The rows below are already at the width
  // limit, which is why this is the one that joins rather than starts a row.
  ImGui::SameLine();
  bool showTileDesigner = game->tileDesignerWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Tile Designer", &showTileDesigner)) {
    if (showTileDesigner && game->tileDesignerWindow) {
      // Owned by Game, not by the component, so this only shows or hides it: the
      // conversation survives a toggle. See Game.h.
      game->tileDesignerWindowEntity.set<ActiveWindow>({game->tileDesignerWindow});
    } else {
      game->tileDesignerWindowEntity.remove<ActiveWindow>();
    }
  }

  bool showBlueprint = game->blueprintWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Blueprints", &showBlueprint)) {
    if (showBlueprint) {
      game->blueprintWindowEntity.set<ActiveWindow>({std::make_shared<BlueprintWindow>(game)});
    } else {
      game->blueprintWindowEntity.remove<ActiveWindow>();
      // Leaving the ghost armed with no window to deselect it from would be a
      // trap, so the placement mode goes with the window.
      game->selectedBlueprintId.clear();
    }
  }

  // Deliberately *not* SameLine: the Font Selection / Map Editor / AI Menu / NPC
  // Menu row already fills a narrow Debug Console, and ImGui clips (and makes
  // unclickable) anything past the right edge, so appending to that row hid this
  // checkbox entirely. Its own row fits at any window width.
  bool showVisionChat = game->visionChatWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Vision Chat", &showVisionChat)) {
    // The instance itself is owned by Game, not by the component, so this only
    // shows or hides it: the transcript survives a toggle. See Game.h.
    if (showVisionChat && game->visionChatWindow) {
      game->visionChatWindowEntity.set<ActiveWindow>({game->visionChatWindow});
    } else {
      game->visionChatWindowEntity.remove<ActiveWindow>();
    }
    game->debugWindowState->SetShowVisionChatWindow(showVisionChat);
  }

  // Its own row, for the reason stated twice above: this row would otherwise be
  // past the right edge of a narrow Debug Console, which makes a checkbox
  // unclickable rather than merely clipped.
  bool showVisionDebug = game->visionDebugWindowEntity.has<ActiveWindow>();
  if (ImGui::Checkbox("Vision Debug", &showVisionDebug)) {
    if (showVisionDebug) {
      game->visionDebugWindowEntity.set<ActiveWindow>(
          {std::make_shared<VisionDebugWindow>(game)});
    } else {
      game->visionDebugWindowEntity.remove<ActiveWindow>();
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

namespace {

// A small coloured plate showing what the tile actually looks like: the DrawAscii
// background as a filled square with the glyph centred on it in the character
// colour, so the window shows the tile's look and not just its name.
void DrawGlyphSwatch(const DrawAscii &ascii, float size) {
  ImDrawList *drawList = ImGui::GetWindowDrawList();
  const ImVec2 topLeft = ImGui::GetCursorScreenPos();
  const ImVec2 bottomRight(topLeft.x + size, topLeft.y + size);

  drawList->AddRectFilled(
      topLeft, bottomRight,
      IM_COL32(ascii.backgroundColor.r, ascii.backgroundColor.g,
               ascii.backgroundColor.b, ascii.backgroundColor.a));
  drawList->AddRect(topLeft, bottomRight, IM_COL32(140, 140, 140, 255));

  const char glyph[2] = {ascii.ch, '\0'};
  const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
  drawList->AddText(ImVec2(topLeft.x + (size - glyphSize.x) * 0.5f,
                           topLeft.y + (size - glyphSize.y) * 0.5f),
                    IM_COL32(ascii.characterColor.r, ascii.characterColor.g,
                             ascii.characterColor.b, ascii.characterColor.a),
                    glyph);

  // Reserve the space the plate was drawn into so the layout flows beside and
  // below it instead of drawing on top of it.
  ImGui::Dummy(ImVec2(size, size));
}

// One summary line for an entity standing on the clicked tile: what it is, plus
// the per-instance state that is interesting at a glance. This is a summary, not
// an inspector -- the full view stays in EntityInfoWindow.
void DrawTileOccupant(flecs::entity occupant, flecs::entity playerEntity) {
  const DisplayName *displayName = occupant.get<DisplayName>();
  const ItemType *itemType = occupant.get<ItemType>();
  const bool isPlayer = occupant == playerEntity;

  std::string label = displayName != nullptr ? displayName->name : "";
  if (label.empty()) {
    label = isPlayer ? "Player"
                     : (occupant.has<CharacterTag>() ? "Character" : "Entity");
  }

  const char *kind = "Entity";
  if (isPlayer) {
    kind = "Player";
  } else if (occupant.has<CharacterTag>()) {
    kind = "Character";
  } else if (itemType != nullptr) {
    kind = "Object";
  }

  ImGui::Bullet();
  ImGui::Text("%s", label.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", kind);

  if (itemType != nullptr) {
    ImGui::SameLine();
    ImGui::TextDisabled("· %s", itemType->id.c_str());
  }
  if (occupant.has<MapAuthored>()) {
    ImGui::SameLine();
    ImGui::TextDisabled("· authored");
  }
  if (const Harvestable *harvestable = occupant.get<Harvestable>()) {
    ImGui::SameLine();
    ImGui::TextDisabled("· harvest %d left", harvestable->amountRemaining);
  }
  if (const Evolvable *evolvable = occupant.get<Evolvable>()) {
    if (evolvable->isActive) {
      ImGui::SameLine();
      ImGui::TextDisabled("· grows into %s",
                          evolvable->nextStageTemplate.c_str());
    }
  }
  if (const Storage *storage = occupant.get<Storage>()) {
    ImGui::SameLine();
    ImGui::TextDisabled("· storage cap %d", storage->capacity);
  }
  if (occupant.has<Portable>()) {
    ImGui::SameLine();
    ImGui::TextDisabled("· portable");
  }
}

} // namespace

TileInfoWindow::TileInfoWindow(Game* game) : game(game) {}

void TileInfoWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(460, 560), ImGuiCond_FirstUseEver);
  ImGui::Begin("Tile Info", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->tileInfoWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  if (!game->hasClicked || game->map == nullptr) {
    ImGui::TextUnformatted("Click anywhere on the map to inspect.");
    ImGui::End();
    return;
  }

  Map *map = game->map.get();
  const GamePosition pos = game->lastClickedPos;
  const ScreenPosition clickedScreenPos =
      map->GameCoordsToScreenCoords(pos.x, pos.y);

  ImGui::Text("Map Coordinates: (%d, %d)", pos.x, pos.y);
  ImGui::Text("Screen Position: (%.0f, %.0f)", clickedScreenPos.x,
              clickedScreenPos.y);
  ImGui::Text("Map Size: %d x %d tiles", map->GetWidth(), map->GetHeight());
  ImGui::SameLine();
  ImGui::TextDisabled("(%d x %d px each)", map->GetTileWidth(),
                      map->GetTileHeight());

  ImGui::Separator();

  Tile *terrain = map->GetTile(pos.x, pos.y);
  if (terrain == nullptr) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                       "Outside the map: there is no tile here.");
    ImGui::End();
    return;
  }

  flecs::entity tileEntity =
      game->validTileSelected ? game->selectedTile : flecs::entity::null();
  const bool hasTileEntity = tileEntity.is_alive();

  // The tile entity's DrawAscii is the live look -- the map editor rewrites it
  // when a tile is painted -- so it wins over the authored copy on the Tile.
  const DrawAscii *glyph = terrain->ascii;
  if (hasTileEntity) {
    if (const DrawAscii *entityGlyph = tileEntity.get<DrawAscii>()) {
      glyph = entityGlyph;
    }
  }

  ImGui::TextUnformatted("Terrain");
  if (glyph != nullptr) {
    DrawGlyphSwatch(*glyph, 52.0f);
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  ImGui::Text("%s", terrain->name.c_str());
  if (terrain->blocksTile) {
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Blocks Path: YES");
  } else {
    ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.45f, 1.0f), "Blocks Path: NO");
  }
  ImGui::Text("Collision Hitbox: %zu x %zu px", terrain->hitbox.width,
              terrain->hitbox.height);
  if (terrain->sprite.empty()) {
    ImGui::TextDisabled("Sprite: none (convention path or glyph)");
  } else {
    ImGui::Text("Sprite: %s", terrain->sprite.c_str());
  }
  ImGui::EndGroup();

  ImGui::Separator();
  ImGui::TextUnformatted("Tile Entity");
  if (hasTileEntity) {
    ImGui::Text("Entity ID: %lu", tileEntity.id());
    if (const ScreenPosition *screenPos = tileEntity.get<ScreenPosition>()) {
      ImGui::Text("Screen Position: (%.0f, %.0f)", screenPos->x, screenPos->y);
    }
    if (const Sprite *sprite = tileEntity.get<Sprite>()) {
      ImGui::Text("Attached Sprite: %s", sprite->path.c_str());
    } else {
      ImGui::TextDisabled("Attached Sprite: none (drawn from the glyph)");
    }
  } else {
    ImGui::TextDisabled("No tile entity found at this position.");
  }

  ImGui::Separator();
  if (Location *location = map->GetLocation(pos)) {
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Location: %s",
                       location->name.c_str());
    if (!location->description.empty()) {
      ImGui::TextWrapped("%s", location->description.c_str());
    } else {
      ImGui::TextDisabled("(no description)");
    }
    ImGui::Text("Area: (%d, %d), %d x %d tiles", location->pos.x,
                location->pos.y, location->width, location->height);
  } else {
    ImGui::TextDisabled("Not inside any named location.");
  }

  ImGui::Separator();

  // Everything standing on the clicked tile. Tile entities are excluded for free:
  // they carry a ScreenPosition but no GamePosition, and only the latter is
  // queried here.
  std::vector<flecs::entity> occupants;
  game->ecs.filter<const GamePosition>().each(
      [&pos, &occupants](flecs::entity e, const GamePosition &p) {
        if (p.x == pos.x && p.y == pos.y) {
          occupants.push_back(e);
        }
      });

  ImGui::Text("Entities on This Tile: %zu", occupants.size());
  if (occupants.empty()) {
    ImGui::TextDisabled("Nothing is standing here.");
  } else {
    for (flecs::entity occupant : occupants) {
      DrawTileOccupant(occupant, game->playerEntity);
    }
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

// The permanent-deletion control shared by both palettes. A helper rather than a
// lambda at each call site so the two rows cannot drift apart in colour, size or
// id scheme -- the id has to be unique per content, and it is built from the kind
// and the name for that reason.
//
// Returns true on the frame it is pressed. It never deletes anything itself: the
// caller turns the press into a plan, and the confirmation is what commits.
//
// Deliberately the darkest, most alarming control in the window and styled apart
// from the tile plates, because the thing next to it (selecting a tile to paint)
// is harmless and this one is not reversible.
static bool DrawDeleteContentButton(const char *kind, const std::string &name) {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.06f, 0.06f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.52f, 0.10f, 0.10f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.64f, 0.14f, 0.14f, 1.0f));
  const bool pressed =
      ImGui::Button((std::string("X##deletefiles_") + kind + "_" + name).c_str(),
                    ImVec2(22.0f, 32.0f));
  ImGui::PopStyleColor(3);
  return pressed;
}

void MapEditorWindow::ApplyConfirmedDelete() {
  if (!confirmedDeletePlan.has_value()) {
    return;
  }
  // Copied out and cleared first: everything below reads only this copy, so a
  // failure part-way through cannot leave the window pointing at a half-applied
  // plan.
  const ContentPlan plan = std::move(*confirmedDeletePlan);
  confirmedDeletePlan.reset();

  const ContentDeleteResult result = DeleteContentFiles(plan.files);

  // The registries are made to agree with the disk *after* the files are gone.
  // Without this the palette would keep offering content that no longer exists
  // and Map::CreateTile would keep resolving a name nothing defines.
  const bool wasTile = plan.kind == ContentKind::Tile;
  if (wasTile) {
    game->tileRegistry.RemoveDefinition(plan.name);
    if (game->editorSelection.type == EditorSelectionType::Tile &&
        game->editorSelection.name == plan.name) {
      game->editorSelection.type = EditorSelectionType::None;
      game->editorSelection.name.clear();
    }
  } else {
    game->objectFactory.RemoveTemplate(plan.name);
    if (game->editorSelection.type == EditorSelectionType::ObjectTemplate &&
        game->editorSelection.name == plan.name) {
      game->editorSelection.type = EditorSelectionType::None;
      game->editorSelection.name.clear();
    }
  }

  lastContentDeleteMessage = "Deleted " + std::to_string(result.deleted.size()) +
                             " file(s) for " + ContentKindLabel(plan.kind) +
                             " '" + plan.name + "'";
  if (!result.failed.empty()) {
    lastContentDeleteMessage +=
        " -- " + std::to_string(result.failed.size()) + " could not be removed";
  }
  // The distinction the whole feature turns on, stated in the result as well as
  // in the dialog: the map itself was not touched.
  lastContentDeleteMessage +=
      plan.maps.empty()
          ? ". The open map is unchanged."
          : ". The open map is unchanged; " +
                std::to_string(plan.maps.size()) +
                " map file(s) on disk still name it and will lose it on reload.";

  if (game->debugLog) {
    for (const std::string &path : result.deleted) {
      game->debugLog->LogInfo("Map editor: deleted content FILE " + path + " (" +
                              ContentKindLabel(plan.kind) + " '" + plan.name +
                              "'; the map itself was not changed)");
    }
    for (const std::string &failure : result.failed) {
      game->debugLog->LogError("Map editor: could not delete content file " +
                               failure);
    }
  }
}

void MapEditorWindow::DrawDeleteConfirmation() {
  if (!pendingDeletePlan.has_value()) {
    return;
  }
  ContentPlan &plan = *pendingDeletePlan;

  ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

  bool stayOpen = true;
  if (!ImGui::Begin("Delete content files?", &stayOpen,
                    ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    // Closing the window with its X is a cancel, and nothing was deleted.
    pendingDeletePlan.reset();
    return;
  }

  // A red frame, matching the blueprint window's delete dialog, so a destructive
  // question never reads as an ordinary one.
  const ImVec2 min = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  ImGui::GetWindowDrawList()->AddRect(min, ImVec2(min.x + size.x, min.y + size.y),
                                      IM_COL32(255, 120, 90, 255), 0.0f, 0, 2.0f);

  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.6f, 1.0f));
  ImGui::Text("Delete %s '%s' from disk?", ContentKindLabel(plan.kind),
              plan.name.c_str());
  ImGui::PopStyleColor();
  ImGui::TextDisabled(
      "This removes the content itself, not the tile or object on the map.");
  ImGui::Separator();

  // --- What will be deleted -------------------------------------------------
  ImGui::Text("Files to remove (%zu):", plan.RemovableCount());
  if (plan.files.empty()) {
    ImGui::TextDisabled("  (no file could be named for this content)");
  }
  for (const ContentFile &file : plan.files) {
    if (file.exists) {
      ImGui::BulletText("%s  (%.1f KB)", file.path.c_str(),
                        static_cast<double>(file.bytes) / 1024.0);
    } else {
      ImGui::BulletText("%s", file.path.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("-- not on disk");
    }
  }
  if (!plan.HasAnythingToRemove()) {
    ImGui::TextColored(
        ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
        "There is no file on disk behind this content, so there is nothing to "
        "delete. A map may have declared it inline.");
  }

  ImGui::Separator();

  // --- Where it is referenced ----------------------------------------------
  if (plan.maps.empty()) {
    ImGui::TextDisabled("No map file on disk references it.");
  } else {
    ImGui::Text("%zu map file(s) reference it:", plan.maps.size());
    if (ImGui::BeginChild("##AffectedMaps", ImVec2(0, 140), ImGuiChildFlags_Borders)) {
      for (const ContentReference &reference : plan.maps) {
        std::string joined;
        for (size_t i = 0; i < reference.details.size(); ++i) {
          joined += (i == 0 ? "" : ", ") + reference.details[i];
        }
        ImGui::BulletText("%s", reference.mapName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", joined.c_str());
      }
    }
    ImGui::EndChild();
  }
  if (plan.kind == ContentKind::Tile) {
    ImGui::Text("On the open map, live (unsaved edits included): %zu cell(s)",
                plan.liveCells);
  } else {
    ImGui::Text("On the open map, live (unsaved edits included): %zu object(s)",
                plan.liveObjects);
  }

  ImGui::Separator();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
  ImGui::TextWrapped(
      "This erases files. There is no undo and no trash. The open map is NOT "
      "changed now -- its cells and objects keep drawing until the map is "
      "reloaded, and then the deleted content is gone from it too.");
  ImGui::PopStyleColor();
  ImGui::TextWrapped(
      "To take an object or tile off this map but keep the files, use the "
      "[x] Remove Object/Tile (trash) button instead.");
  ImGui::Separator();

  // The two outcomes are recorded and applied after `plan` is no longer used,
  // rather than acted on inside the button branches. `plan` is a reference into
  // `pendingDeletePlan`, and both branches reset that optional -- acting where the
  // press happens would leave the remainder of this function reading a destroyed
  // plan.
  bool confirm = false;
  bool cancel = false;

  const std::string confirmLabel =
      "Delete " + std::to_string(plan.RemovableCount()) + " file(s)";
  if (plan.HasAnythingToRemove()) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.70f, 0.16f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.20f, 0.20f, 1.0f));
    confirm = ImGui::Button(confirmLabel.c_str(), ImVec2(180, 0));
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
  }
  cancel = ImGui::Button("Cancel", ImVec2(120, 0));

  ImGui::End();

  if (confirm) {
    // Copied first, then the source cleared: ApplyConfirmedDelete runs at the top
    // of the next frame, when nothing is still iterating the palette the deletion
    // rebuilds.
    confirmedDeletePlan = plan;
  }
  if (confirm || cancel || !stayOpen) {
    pendingDeletePlan.reset();
  }
}

// --- Map Editor palette ordering -------------------------------------------

// Case-insensitive, because a catalog's names and keys are lowercase today but
// there is no reason a future one has to be; an exact tie is broken by the raw
// strings so the order stays deterministic.
static int ContentNameCompare(const std::string &a, const std::string &b) {
  const size_t shared = std::min(a.size(), b.size());
  for (size_t i = 0; i < shared; ++i) {
    const unsigned char ca = static_cast<unsigned char>(
        std::tolower(static_cast<unsigned char>(a[i])));
    const unsigned char cb = static_cast<unsigned char>(
        std::tolower(static_cast<unsigned char>(b[i])));
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

static bool ContentNameLess(const std::string &a, const std::string &b) {
  const int compare = ContentNameCompare(a, b);
  return compare != 0 ? compare < 0 : a < b;
}

static bool HasContentTime(const std::filesystem::file_time_type &time) {
  return time != std::filesystem::file_time_type{};
}

static size_t ContentUsage(const std::unordered_map<std::string, size_t> &usage,
                           const std::string &key) {
  const auto it = usage.find(key);
  return it == usage.end() ? 0 : it->second;
}

static const char *ContentSortDirectionLabel(ContentSortMetric metric,
                                             bool descending) {
  switch (metric) {
  case ContentSortMetric::Name:
    return descending ? "Z to A" : "A to Z";
  case ContentSortMetric::Date:
    return descending ? "Newest first" : "Oldest first";
  case ContentSortMetric::Usage:
    return descending ? "Most used first" : "Least used first";
  case ContentSortMetric::Source:
    return descending ? "Generated first" : "Hand-authored first";
  }
  return "";
}

static const char *ContentSortDirectionTooltip(ContentSortMetric metric,
                                               bool descending) {
  switch (metric) {
  case ContentSortMetric::Name:
    return "Alphabetical order. Click to reverse it.";
  case ContentSortMetric::Date:
    return descending
               ? "The most recently written source file first. Click for oldest "
                 "first."
               : "The oldest source file first. Click for newest first.";
  case ContentSortMetric::Usage:
    return descending
               ? "The most-used content on this map first. Click for least used."
               : "The least-used content on this map first. Click for most used.";
  case ContentSortMetric::Source:
    return descending
               ? "Generated/designer content first, then the hand-authored "
                 "catalog. Click to reverse."
               : "Hand-authored catalog first, then generated/designer content. "
                 "Click to reverse.";
  }
  return "";
}

// The one sort row, used by both palettes. `id` keeps the combo and button ids
// unique across the two lists; the visible direction label changes with the
// metric, but the ## suffix keeps the button's id stable across that change.
static void DrawContentSortControls(const char *id, ContentSortMetric &metric,
                                    bool &descending) {
  static const char *kMetricLabels[] = {"Name", "Date", "Usage on this map",
                                        "Source"};

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Sort");
  ImGui::SameLine();

  int selected = static_cast<int>(metric);
  const std::string comboId = std::string("##sortMetric_") + id;
  ImGui::SetNextItemWidth(170.0f);
  if (ImGui::Combo(comboId.c_str(), &selected, kMetricLabels,
                   IM_ARRAYSIZE(kMetricLabels))) {
    metric = static_cast<ContentSortMetric>(selected);
  }

  ImGui::SameLine();
  const std::string directionLabel =
      std::string(ContentSortDirectionLabel(metric, descending)) +
      "##sortDirection_" + id;
  if (ImGui::Button(directionLabel.c_str())) {
    descending = !descending;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", ContentSortDirectionTooltip(metric, descending));
  }
}

static void SortTileCatalog(
    std::vector<const TileDef *> &catalog, ContentSortMetric metric,
    bool descending, const std::unordered_map<std::string, size_t> &usage,
    const TileRegistry &registry) {
  std::stable_sort(catalog.begin(), catalog.end(),
                   [&](const TileDef *a, const TileDef *b) {
                     switch (metric) {
                     case ContentSortMetric::Name: {
                       const int compare = ContentNameCompare(a->name, b->name);
                       if (compare != 0) {
                         return descending ? compare > 0 : compare < 0;
                       }
                       break;
                     }
                     case ContentSortMetric::Date: {
                       const bool hasA = HasContentTime(a->sourceTime);
                       const bool hasB = HasContentTime(b->sourceTime);
                       // A definition with no recorded date sorts last whichever
                       // way the list is pointing; putting it first when
                       // descending would read as "the newest thing".
                       if (hasA != hasB) return hasA;
                       if (a->sourceTime != b->sourceTime) {
                         return descending ? a->sourceTime > b->sourceTime
                                           : a->sourceTime < b->sourceTime;
                       }
                       break;
                     }
                     case ContentSortMetric::Usage: {
                       const size_t usedA = ContentUsage(usage, a->name);
                       const size_t usedB = ContentUsage(usage, b->name);
                       if (usedA != usedB) {
                         return descending ? usedA > usedB : usedA < usedB;
                       }
                       break;
                     }
                     case ContentSortMetric::Source: {
                       const bool runtimeA = registry.IsRuntimeAdded(a->name);
                       const bool runtimeB = registry.IsRuntimeAdded(b->name);
                       if (runtimeA != runtimeB) {
                         return descending ? runtimeA > runtimeB
                                           : runtimeA < runtimeB;
                       }
                       break;
                     }
                     }
                     // Ties, and the final ordering within a Source group, are
                     // alphabetical: a metric that leaves a group unordered is
                     // not a sort, it is a shuffle.
                     return ContentNameLess(a->name, b->name);
                   });
}

static void SortObjectTemplates(
    std::vector<const std::pair<const std::string, nlohmann::json> *> &entries,
    ContentSortMetric metric, bool descending,
    const std::unordered_map<std::string, size_t> &usage,
    const ObjectFactory &factory) {
  std::stable_sort(
      entries.begin(), entries.end(),
      [&](const std::pair<const std::string, nlohmann::json> *a,
          const std::pair<const std::string, nlohmann::json> *b) {
        switch (metric) {
        case ContentSortMetric::Name: {
          const int compare = ContentNameCompare(a->first, b->first);
          if (compare != 0) {
            return descending ? compare > 0 : compare < 0;
          }
          break;
        }
        case ContentSortMetric::Date: {
          const std::filesystem::file_time_type timeA =
              factory.GetTemplateTime(a->first);
          const std::filesystem::file_time_type timeB =
              factory.GetTemplateTime(b->first);
          const bool hasA = HasContentTime(timeA);
          const bool hasB = HasContentTime(timeB);
          if (hasA != hasB) return hasA;
          if (timeA != timeB) {
            return descending ? timeA > timeB : timeA < timeB;
          }
          break;
        }
        case ContentSortMetric::Usage: {
          const size_t usedA = ContentUsage(usage, a->first);
          const size_t usedB = ContentUsage(usage, b->first);
          if (usedA != usedB) {
            return descending ? usedA > usedB : usedA < usedB;
          }
          break;
        }
        case ContentSortMetric::Source: {
          const bool runtimeA = factory.IsRuntimeAdded(a->first);
          const bool runtimeB = factory.IsRuntimeAdded(b->first);
          if (runtimeA != runtimeB) {
            return descending ? runtimeA > runtimeB : runtimeA < runtimeB;
          }
          break;
        }
        }
        return ContentNameLess(a->first, b->first);
      });
}

void MapEditorWindow::Draw() {
  // A deletion confirmed last frame is applied before anything else this frame.
  // It has to be here rather than where the button was pressed: this window has
  // just iterated the tile catalog, and the deletion frees definitions that
  // iteration was holding pointers into.
  ApplyConfirmedDelete();

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

  // Trash button: toggles the removal mode, which takes the object on a clicked
  // tile and, when nothing is placed there, the tile itself. The armed-looking
  // style is pushed and popped around the button using the same value, because
  // the button itself flips the mode: reading the (now flipped) state when
  // deciding whether to pop would unbalance the style stack on the very click
  // that arms it.
  ImGui::SameLine();
  const bool removeObjectLit = game->removeObjectMode;
  if (removeObjectLit) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.25f, 0.25f, 1.0f));
  }
  // The visible label is followed by ##MapEditor, so the ImGui id -- and with it
  // the button's remembered state -- is unchanged by the rename.
  if (ImGui::Button("[x] Remove Object/Tile##MapEditor")) {
    game->removeObjectMode = !game->removeObjectMode;
  }
  if (removeObjectLit) {
    ImGui::PopStyleColor(3);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Trash: while lit, clicking a tile deletes the object "
                      "placed on it; on a tile with no object it deletes the "
                      "tile itself, leaving an empty cell. Both are what the "
                      "map file records on save.");
  }

  if (game->removeLocationMode) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                       "Click a location on the map to remove it.");
  }
  if (game->removeObjectMode) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                       "Trash armed: click a tile to delete the object on it, "
                       "or the tile when it holds no object.");
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
  if (!game->lastRemovedTileName.empty()) {
    ImGui::Text("Last removed tile: %s",
                game->lastRemovedTileName.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##LastRemovedTile")) {
      game->lastRemovedTileName.clear();
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
    // Every tile the world's catalog knows, not just the ones this map already
    // contains.
    //
    // The palette used to be `map->GetUniqueTiles()`, which is the map file's own
    // palette -- the very array "positions" indexes into, so it is the map's index
    // table and not its vocabulary. Listing only that was honest but made creation
    // invisible: a tile the Tile Designer invents goes into the catalog
    // (TileRegistry), never into a map, so it could not be selected, and it could
    // only join the map by being painted -- with the tile you cannot select. The
    // loop was closed, and the only way out was editing a map file by hand.
    //
    // The catalog is now the list and the map is an annotation: a count on the
    // tiles that are already here. Selecting one that is not is fine -- Game::
    // PaintTile creates it on demand from the catalog (Map::CreateTile), and the
    // map only starts "using" it once a cell is actually painted.
    std::vector<const TileDef *> catalog = game->tileRegistry.GetAll();

    // How many cells of the current map draw each tile, by name. Duplicate names
    // are possible on purpose -- two palette entries with the same name and
    // different overrides are two distinct tiles (see the palette work in
    // Map.cpp) -- so this is keyed by name and the count is the sum, which is what
    // a reader of the palette wants to know anyway.
    std::unordered_map<std::string, size_t> cellsInUse;
    if (game->map != nullptr) {
      for (int y = 0; y < game->map->GetHeight(); ++y) {
        for (int x = 0; x < game->map->GetWidth(); ++x) {
          if (const Tile *cell = game->map->GetTile(x, y)) {
            ++cellsInUse[cell->name];
          }
        }
      }
    }

    // The same count for object templates, by key, so the permanent-deletion
    // confirmation can say how much of the *open* map it is about to affect. Only
    // MapAuthored entities count: those are the map's own content, which is
    // exactly what a map file would reference.
    std::unordered_map<std::string, size_t> objectsInUse;
    game->ecs.each([&objectsInUse](flecs::entity, const MapAuthored &authored) {
      ++objectsInUse[authored.templateKey];
    });

    // The map files a deletion would affect. Built only when a Delete button is
    // actually pressed -- it re-reads the maps directory, which is not something
    // to do on every frame the window is open.
    const auto mapPathsForScan = [this]() {
      std::vector<std::string> paths;
      if (!game->mapReloader) {
        return paths;
      }
      game->mapReloader->RefreshMapList();
      const std::vector<std::string> &list = game->mapReloader->GetMapList();
      paths.reserve(list.size());
      for (size_t i = 0; i < list.size(); ++i) {
        paths.push_back(game->mapReloader->GetMapPath(i));
      }
      return paths;
    };

    // The one line that states the difference the two delete-ish controls have,
    // where both are actually on screen. The trash is a mode up top that edits the
    // map; the X below erases files. Without this, "I deleted it and it is still
    // there / it came back" is the predictable confusion.
    ImGui::TextDisabled(
        "Click a swatch to paint with it.  The X beside a swatch deletes that "
        "content's FILES (data + sprite) from disk.");
    ImGui::SameLine();
    ImGui::TextDisabled("(The trash button above only edits this map.)");

    ImGui::Text("Tiles");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu on this map, %zu in the catalog)", cellsInUse.size(),
                        catalog.size());

    // The sort is read from and written back to DebugWindowState every frame
    // rather than held in this window: the window is rebuilt on every toggle, so
    // a member here would forget the choice. The catalog is sorted after the
    // controls so a change is visible in the same frame it is made.
    ContentSortMetric tileSortMetric =
        game->debugWindowState->GetTileSortMetric();
    bool tileSortDescending =
        game->debugWindowState->GetTileSortDescending();
    DrawContentSortControls("tiles", tileSortMetric, tileSortDescending);
    game->debugWindowState->SetTileSortMetric(tileSortMetric);
    game->debugWindowState->SetTileSortDescending(tileSortDescending);
    SortTileCatalog(catalog, tileSortMetric, tileSortDescending, cellsInUse,
                    game->tileRegistry);

    // A filter, because the catalog is the whole world's vocabulary: an art pack
    // plus every agent-created tile. At today's size the table scrolled; at a few
    // hundred tiles it would be unusable without one.
    //
    // A `static` buffer living in this window's lifetime, which is the same
    // lifetime as the window instance: unlike the Designer and Vision windows this
    // one is rebuilt on every toggle (see Game.h), so the filter resets when the
    // Map Editor is closed. That is acceptable for a search box and is why this is
    // not a member.
    static char tileFilter[64] = {};
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##tileFilter", "filter tiles by name...",
                             tileFilter, sizeof(tileFilter));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##tileFilter")) {
      tileFilter[0] = '\0';
    }

    // Lowercased once, not per tile: this runs every frame over the whole catalog.
    std::string filter(tileFilter);
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    ImGui::Separator();
    // Sixteen columns is eight palettes of two: the tile button, then its Delete
    // control. The pair is deliberate -- the delete button has to sit next to what
    // it deletes, and it has to be in its own column so it cannot inherit the
    // tile's own plate colours from the style stack.
    if (ImGui::BeginTable("TilesTable", 16)) {
      for (const TileDef *definition : catalog) {
        if (!filter.empty()) {
          std::string name = definition->name;
          std::transform(name.begin(), name.end(), name.begin(),
                         [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                         });
          if (name.find(filter) == std::string::npos) continue;
        }

        ImGui::TableNextColumn();

        // The plate and glyph come from the *definition*, which is what the world
        // would draw for a tile this map has not got yet -- a map's own entry can
        // override either, but only for tiles it already has.
        const Color bg = definition->hasBackgroundColor ? definition->backgroundColor
                                                        : Color{0, 0, 0, 0};
        const Color fg =
            definition->hasCharacterColor ? definition->characterColor
                                          : Color{255, 255, 255, 255};
        const char glyph =
            definition->hasCharacter ? definition->character : '?';

        const auto used = cellsInUse.find(definition->name);
        const size_t cellCount = used == cellsInUse.end() ? 0 : used->second;

        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(bg.r / 255.0f, bg.g / 255.0f,
                                     bg.b / 255.0f, bg.a / 255.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(fg.r / 255.0f, fg.g / 255.0f,
                                     fg.b / 255.0f, fg.a / 255.0f));

        const bool isSelected =
            (game->editorSelection.type == EditorSelectionType::Tile &&
             game->editorSelection.name == definition->name);
        if (isSelected) {
          ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 0, 1));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        }

        // The tile's texture, resolved through the very same helper the world uses
        // -- an explicit "sprite" key, then the art pack, then the generated layer,
        // by TileSpriteId -- so the palette cannot offer a look the map would not
        // draw. This is also what makes a generated sprite appear here: the path is
        // where the generator writes, and ResourceManager memoises only failures,
        // so the newest sprite is found on the next frame.
        const Texture2D *sprite = ResolveSpriteTexture(
            definition->sprite, TileSpriteId(definition->name),
            game->resourceManager, game->debugLog.get());

        const std::string label =
            sprite != nullptr
                ? ("##" + definition->name)
                : (std::string(1, glyph) + "##" + definition->name);
        if (ImGui::Button(label.c_str(), ImVec2(32, 32))) {
          game->editorSelection.type = EditorSelectionType::Tile;
          game->editorSelection.name = definition->name;
        }
        // Drawn over the button's own plate, so the tile's background colour still
        // shows through any transparency in the texture.
        if (sprite != nullptr) {
          DrawSpriteImGui(*sprite, ImGui::GetItemRectMin(),
                          ImGui::GetItemRectSize());
        }

        // The map-membership badge: a count in the corner for tiles this map
        // already draws, nothing for tiles it does not. Deliberately quiet -- the
        // palette is for choosing a tile, and the count is a secondary fact, but a
        // count of zero is exactly the case that used to be impossible to select.
        if (cellCount > 0) {
          ImGui::GetWindowDrawList()->AddText(
              ImVec2(ImGui::GetItemRectMax().x - 16.0f,
                     ImGui::GetItemRectMin().y + 1.0f),
              IM_COL32(255, 255, 255, 200), std::to_string(cellCount).c_str());
        }

        ImGui::PopStyleColor(2);
        if (isSelected) {
          ImGui::PopStyleColor();
          ImGui::PopStyleVar();
        }
        if (ImGui::IsItemHovered()) {
          if (cellCount > 0) {
            ImGui::SetTooltip("%s\non this map: %zu cell(s)%s",
                              definition->name.c_str(), cellCount,
                              definition->blocksTile ? "\nblocks movement" : "");
          } else {
            ImGui::SetTooltip(
                "%s\nnot on this map yet -- painting a cell adds it%s",
                definition->name.c_str(),
                definition->blocksTile ? "\nblocks movement" : "");
          }
        }

        // Permanent deletion. Nothing like the trash button above: that one edits
        // the map, this one erases files. It is drawn in its own column and in its
        // own red so the two cannot be mistaken for each other at a glance, and it
        // never deletes on the click -- it opens the confirmation, which is where
        // the files and the affected maps are named.
        ImGui::TableNextColumn();
        if (DrawDeleteContentButton("tile", definition->name)) {
          pendingDeletePlan = PlanTileRemoval(
              definition->name, definition->sourcePath, definition->sprite,
              cellCount, mapPathsForScan());
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "Delete the FILES behind tile '%s':\n%s\nand its sprite art.\n"
              "Every map that uses it is affected. Cannot be undone.\n"
              "(The trash button above only removes it from this map.)",
              definition->name.c_str(),
              definition->sourcePath.empty()
                  ? "(declared inline by a map -- no catalog file)"
                  : definition->sourcePath.c_str());
        }
      }
      ImGui::EndTable();
    }

    ImGui::Text("Object Templates");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu)", game->objectFactory.GetTemplates().size());

    // Pointers into the factory's map, sorted by the chosen metric. The map is
    // not mutated while this window draws -- permanent deletion is deferred to
    // the top of the next frame -- so the pointers stay valid for this frame.
    std::vector<const std::pair<const std::string, nlohmann::json> *>
        objectEntries;
    objectEntries.reserve(game->objectFactory.GetTemplates().size());
    for (const auto &pair : game->objectFactory.GetTemplates()) {
      objectEntries.push_back(&pair);
    }

    // Same state-ownership rule as the tile palette: the choice lives in
    // DebugWindowState because this window does not survive a toggle.
    ContentSortMetric objectSortMetric =
        game->debugWindowState->GetObjectSortMetric();
    bool objectSortDescending =
        game->debugWindowState->GetObjectSortDescending();
    DrawContentSortControls("objects", objectSortMetric,
                            objectSortDescending);
    game->debugWindowState->SetObjectSortMetric(objectSortMetric);
    game->debugWindowState->SetObjectSortDescending(objectSortDescending);
    SortObjectTemplates(objectEntries, objectSortMetric, objectSortDescending,
                        objectsInUse, game->objectFactory);

    ImGui::Separator();
    if (ImGui::BeginTable("ObjectsTable", 16)) {
      for (const auto *entry : objectEntries) {
        const auto &pair = *entry;
        ImGui::TableNextColumn();
        
        char ch = '?';
        ImVec4 bgColor(0,0,0,0);
        ImVec4 fgColor(1,1,1,1);
        
        if (pair.second.contains("character")) {
           std::string charStr = pair.second["character"];
           if (!charStr.empty()) ch = charStr[0];
        }

        // Parsed once, here, because the art preview's fallback colour is the
        // template's characterColor and the ImGui text style wants the same value.
        Color characterColor = {255, 255, 255, 255};
        if (pair.second.contains("characterColor")) {
           characterColor = ParseAsciiColor(pair.second["characterColor"], characterColor);
           fgColor = ImVec4(characterColor.r/255.0f, characterColor.g/255.0f, characterColor.b/255.0f, characterColor.a/255.0f);
        }
        if (pair.second.contains("backgroundColor")) {
           const Color backgroundColor =
               ParseAsciiColor(pair.second["backgroundColor"], Color{0, 0, 0, 0});
           bgColor = ImVec4(backgroundColor.r/255.0f, backgroundColor.g/255.0f, backgroundColor.b/255.0f, backgroundColor.a/255.0f);
        }

        // Authored art is previewed through the same layout the world renderer
        // uses, so the palette button shows what placing the object will actually
        // look like. A template whose art is unusable falls back to its glyph
        // here; ObjectFactory already reported why at load time.
        AsciiArt art;
        std::string artWarning;
        const bool hasArt = ParseAsciiArt(pair.second, art, artWarning);

        // A raster sprite wins over that art, resolved through the very same
        // helper the world uses -- an explicit "sprite" key, then
        // data/sprites/<templateKey>.png. Sharing the resolver is the point: the
        // palette cannot promise a look the map would not deliver.
        std::string spriteKey;
        if (pair.second.contains("sprite") && pair.second["sprite"].is_string()) {
          spriteKey = pair.second["sprite"].get<std::string>();
        }
        const Texture2D *sprite = ResolveSpriteTexture(
            spriteKey, pair.first, game->resourceManager, game->debugLog.get());
        const bool hasSprite = sprite != nullptr;

        // The plate is the glyph/art backdrop, and it follows the same rule as the
        // world renderer (Game::DrawEntityVisual): drawn when there is no sprite,
        // skipped when a sprite covers the cell. This palette's promise is "what
        // placing the object will look like", so a plate under a cut-out sprite
        // would make the object look opaque here right up until it was placed.
        //
        // There is no map tile behind a palette button, so what shows through a
        // sprite's transparent pixels is the window. That is the honest equivalent --
        // it is *a* background with no claim about which one -- where the world shows
        // the tile the object is standing on.
        ImGui::PushStyleColor(
            ImGuiCol_Button, hasSprite ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f) : bgColor);
        ImGui::PushStyleColor(ImGuiCol_Text, fgColor);
        
        bool isSelected = (game->editorSelection.type == EditorSelectionType::ObjectTemplate && game->editorSelection.name == pair.first);
        if (isSelected) {
          ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 0, 1));
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        }

        // Both image paths draw over an empty label: the label is the button's click
        // target and its text would show through a sprite that has the same glyph, so
        // only the plain-glyph case carries one.
        const std::string label =
            (hasSprite || hasArt) ? ("##" + pair.first)
                                  : (std::string(1, ch) + "##" + pair.first);
        if (ImGui::Button(label.c_str(), ImVec2(32, 32))) {
          game->editorSelection.type = EditorSelectionType::ObjectTemplate;
          game->editorSelection.name = pair.first;
        }
        if (hasSprite) {
          DrawSpriteImGui(*sprite, ImGui::GetItemRectMin(),
                          ImGui::GetItemRectSize());
        } else if (hasArt) {
          DrawAsciiArtImGui(art, characterColor, ImGui::GetItemRectMin(),
                            ImGui::GetItemRectSize());
        }
        
        ImGui::PopStyleColor(2);
        if (isSelected) {
          ImGui::PopStyleColor();
          ImGui::PopStyleVar();
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", pair.first.c_str());
        }

        // The same permanent-deletion control as the tile palette, on the same
        // terms: its own column, its own red, and a confirmation that names the
        // files and every map that references the template before anything goes.
        ImGui::TableNextColumn();
        if (DrawDeleteContentButton("object", pair.first)) {
          const auto liveObjects = objectsInUse.find(pair.first);
          pendingDeletePlan = PlanObjectRemoval(
              pair.first, game->objectFactory.GetDirectory(), spriteKey,
              liveObjects == objectsInUse.end() ? 0 : liveObjects->second,
              mapPathsForScan());
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "Delete the FILES behind object '%s':\n%s/%s.json\nand its "
              "sprite art.\nEvery map that uses it is affected. Cannot be "
              "undone.\n(The trash button above only removes it from this map.)",
              pair.first.c_str(),
              game->objectFactory.GetDirectory().empty()
                  ? "(no object directory)"
                  : game->objectFactory.GetDirectory().c_str(),
              pair.first.c_str());
        }
      }
      ImGui::EndTable();
    }

    if (!lastContentDeleteMessage.empty()) {
      ImGui::Separator();
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.6f, 1.0f), "%s",
                         lastContentDeleteMessage.c_str());
    }
  }
  ImGui::EndChild();

  ImGui::End();

  // The file-deletion confirmation. Drawn as its own window, like the location
  // prompt below, rather than as an ImGui popup: it is opened from inside the
  // palette's child window and confirmed here, and a popup's id is part of the
  // id stack it was opened in, which is not the same stack at this point.
  DrawDeleteConfirmation();

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

// What an NPC is doing right now, split so the table can style the verb
// differently from its subject. `context` is always safe to display: it is
// empty when the action has no interesting subject.
struct NPCActionLabel {
  std::string name;
  std::string context;
};

// Builds the action description shown by the NPC menu. Prefers the timed action
// entity referenced by Busy (e.g. harvesting), then falls back to the brain's
// current polymorphic action. `registry` may be null; when it resolves the
// recipe the craft label can name what is being made.
static NPCActionLabel GetNPCActionLabel(const flecs::entity &npc,
                                        const RecipeRegistry *registry = nullptr) {
  NPCActionLabel label;

  if (const Busy *busy = npc.get<Busy>()) {
    if (busy->actionEntity.is_alive()) {
      // A craft's subject is the recipe, not the station it happens at, so that
      // branch fills the context itself and the generic ActionTarget fallback
      // below has to leave it alone.
      bool contextFromRecipe = false;
      if (busy->actionEntity.has<HarvestAction>()) {
        label.name = "HARVEST";
      } else if (const CraftAction *craft = busy->actionEntity.get<CraftAction>()) {
        label.name = "CRAFT";
        // A batch craft reads as "3x Bread" in the subject slot below; the
        // recipe id is the fallback when the registry cannot resolve it.
        const CraftRecipe *recipe =
            registry ? registry->Get(craft->recipeId) : nullptr;
        const std::string craftName = recipe ? recipe->name : craft->recipeId;
        if (craft->count > 1) {
          label.context = std::to_string(craft->count) + "x " + craftName;
        } else {
          label.context = craftName;
        }
        contextFromRecipe = true;
      } else {
        label.name = "BUSY";
      }

      // Timed actions carry their subject as a real entity, which is the only
      // place the target name survives: the interaction itself has already
      // returned and its result string was consumed by the agent.
      if (!contextFromRecipe) {
        if (const ActionTarget *target = busy->actionEntity.get<ActionTarget>()) {
          if (target->target.is_alive() && target->target.has<DisplayName>()) {
            label.context = target->target.get<DisplayName>()->name;
          }
        }
      }
      if (const ActionTimer *timer = busy->actionEntity.get<ActionTimer>()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs left", timer->timeRemaining);
        if (!label.context.empty()) {
          label.context += " - ";
        }
        label.context += buf;
      }
      return label;
    }
  }

  if (const AgentBrainWrapper *wrapper = npc.get<AgentBrainWrapper>()) {
    if (wrapper->agBrain) {
      if (AgentAction *current = wrapper->agBrain->getCurrentAction()) {
        label.name = current->getActionName();
        label.context = current->getActionContext();
      }
    }
  }

  if (label.name.empty()) {
    label.name = "IDLE";
    return label;
  }

  // MOVE_TO keeps its remaining route on the entity, so the label can say how
  // much walking is left, not just where the NPC is headed.
  if (label.name == "MOVE_TO") {
    if (const MOVE_THROUGH_PATH_ACTION *path = npc.get<MOVE_THROUGH_PATH_ACTION>()) {
      if (!path->path.empty()) {
        label.context += " (" + std::to_string(path->path.size()) + " tiles)";
      }
    }
  }

  return label;
}

NPCMenuWindow::NPCMenuWindow(Game* game) : game(game) {}

namespace {

// The colour a tier is drawn in, so the board and the NPC section agree. The tier
// is never changed by the engine, so this is presentation only -- an inflated
// CRITICAL still reads as CRITICAL, it just reads as wrong.
ImVec4 TaskTierColor(TaskPriority tier) {
  switch (tier) {
    case TaskPriority::Critical: return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
    case TaskPriority::High: return ImVec4(1.0f, 0.7f, 0.2f, 1.0f);
    case TaskPriority::Normal: return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    case TaskPriority::Low: return ImVec4(0.55f, 0.65f, 0.75f, 1.0f);
    case TaskPriority::Background: return ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
  }
  return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
}

// One goal, as the NPC window's task section shows it: what it is, what it
// serves, how old it is, and what the engine observed about it. `nowTick` drives
// the age and the stale flag.
void DrawAgentGoalRow(const TaskList& list, const AgentGoal& goal, int nowTick,
                      bool isActive) {
  ImGui::TableNextRow();

  ImGui::TableNextColumn();
  if (isActive) {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "> %d", goal.id);
  } else {
    ImGui::Text("%d", goal.id);
  }

  ImGui::TableNextColumn();
  ImGui::TextUnformatted(TaskHorizonLabel(goal.horizon));

  ImGui::TableNextColumn();
  // The agent's own label, verbatim. Violations are annotated in the next column
  // rather than corrected here (section 6.1).
  ImGui::TextColored(TaskTierColor(goal.tier), "%s", TaskPriorityLabel(goal.tier));

  ImGui::TableNextColumn();
  ImGui::TextUnformatted(goal.description.c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", goal.description.c_str());
  }
  if (!goal.kind.empty() || !goal.target.empty()) {
    ImGui::TextDisabled("    kind=%s target=%s", goal.kind.c_str(),
                        goal.target.c_str());
  }

  ImGui::TableNextColumn();
  const std::string parent = DescribeTaskParent(list, goal);
  if (parent.empty()) {
    ImGui::TextDisabled("-");
  } else {
    ImGui::TextDisabled("%s", parent.c_str());
  }

  ImGui::TableNextColumn();
  const int ageTicks = TaskAgeTicks(goal, nowTick);
  if (IsTaskStarving(goal, nowTick)) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s (stale)",
                       FormatTaskAge(ageTicks).c_str());
  } else {
    ImGui::TextUnformatted(FormatTaskAge(ageTicks).c_str());
  }

  ImGui::TableNextColumn();
  bool wroteSomething = false;
  for (const std::string& fact : goal.facts) {
    if (wroteSomething) ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", fact.c_str());
    wroteSomething = true;
  }
  if (!goal.ruleWarning.empty()) {
    if (wroteSomething) ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "! %s",
                       goal.ruleWarning.c_str());
    wroteSomething = true;
  }
  // An orphaned short task is legal but usually means the agent lost the thread
  // (section 8).
  if (goal.horizon == GoalHorizon::Short && goal.parent < 0) {
    if (wroteSomething) ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextDisabled("orphan");
  }
}

// The selected NPC's whole list, laid out the way the digest lays it out so the
// human and the model are looking at the same thing (section 11). Everything the
// board cannot fit lives here: the life goals, the frame each task sits in, the
// age of every entry, and the divergence from the live action.
void DrawNPCTaskSection(flecs::entity npc, const TaskList& list,
                        const std::string& runningAction) {
  const int nowTick = CurrentTaskTick(npc.world());

  if (IsTaskListInflated(list)) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                       "! %d tasks marked CRITICAL; at most %d is allowed",
                       CountCriticalTasks(list), DEFAULT_TASK_MAX_CRITICAL);
  }

  const std::string divergence = DescribeTaskDivergence(list, runningAction);
  if (!divergence.empty()) {
    // The single most useful signal in the system (section 8): the agent says one
    // thing and is doing another. The engine cannot prevent it -- actions still
    // schedule themselves -- so it makes it visible.
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "DIVERGENCE: %s",
                       divergence.c_str());
  } else if (const AgentGoal* active = TaskListActiveGoal(list)) {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "active: task %d -- %s",
                       active->id, active->description.c_str());
  } else {
    ImGui::TextDisabled("active: none");
  }

  if (!list.ruleWarning.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "! %s",
                       list.ruleWarning.c_str());
  }

  if (ImGui::BeginTable("NPCTaskTable", 7,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 34.0f);
    ImGui::TableSetupColumn("Horizon", ImGuiTableColumnFlags_WidthFixed, 76.0f);
    ImGui::TableSetupColumn("Tier", ImGuiTableColumnFlags_WidthFixed, 84.0f);
    ImGui::TableSetupColumn("Description");
    ImGui::TableSetupColumn("Serves", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 76.0f);
    ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthFixed, 190.0f);
    ImGui::TableHeadersRow();

    // Terminal first, then long-term, then short, which is the stored order and
    // the order the digest presents.
    for (int horizonIndex = 0; horizonIndex < 3; ++horizonIndex) {
      const GoalHorizon horizon =
          horizonIndex == 0 ? GoalHorizon::Terminal
                            : (horizonIndex == 1 ? GoalHorizon::LongTerm
                                                 : GoalHorizon::Short);
      for (const AgentGoal& goal : list.goals) {
        if (goal.horizon != horizon) continue;
        DrawAgentGoalRow(list, goal, nowTick,
                         list.activeTaskId >= 0 && goal.id == list.activeTaskId);
      }
    }

    ImGui::EndTable();
  }
}

}  // namespace

void NPCMenuWindow::Draw() {
  bool show = game->npcMenuWindowEntity.has<ActiveWindow>();
  if (!show)
    return;

  ImGui::SetNextWindowSize(ImVec2(840, 460), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("NPC Menu", &show, ImGuiWindowFlags_None)) {
    static ImGuiTextFilter filter;
    filter.Draw("Filter", 180.0f);

    ImGui::Separator();

    // Applied after the iteration so we do not mutate the ECS while querying it.
    flecs::entity contextToToggle = flecs::entity::null();
    flecs::entity inventoryToOpen = flecs::entity::null();

    // Which row's task list is expanded below the table. Held statically because
    // the window itself is rebuilt on every toggle while the selection is about
    // the agent, not the window; it is validated against is_alive() before use so
    // a deselected or destroyed NPC cannot leave a dangling handle.
    static flecs::entity selectedForTasks = flecs::entity::null();
    flecs::entity taskRowToSelect = flecs::entity::null();

    // The detail pane is only reserved while it has something to show, so an
    // unselected window keeps the whole area for the table.
    const bool hasTaskDetail =
        selectedForTasks.is_alive() && selectedForTasks.has<TaskList>();
    const float detailHeight = hasTaskDetail ? 190.0f : 0.0f;
    const float tableHeight =
        std::max(80.0f, ImGui::GetContentRegionAvail().y - detailHeight -
                            (hasTaskDetail ? ImGui::GetStyle().ItemSpacing.y
                                           : 0.0f));

    if (ImGui::BeginTable("NPCMenuTable", 8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0f, tableHeight))) {
      ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
      ImGui::TableSetupColumn("Name");
      ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed,
                              90.0f);
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed,
                              240.0f);
      ImGui::TableSetupColumn("Tasks", ImGuiTableColumnFlags_WidthFixed,
                              130.0f);
      ImGui::TableSetupColumn("Stop Brain",
                              ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthFixed,
                              130.0f);
      ImGui::TableSetupColumn("Inventory",
                              ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableHeadersRow();

      game->ecs.filter<AgentBrainWrapper>().each(
          [&contextToToggle, &inventoryToOpen, &taskRowToSelect, &selectedForTasks,
           this](flecs::entity npc, AgentBrainWrapper &wrapper) {
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
            const NPCActionLabel action =
                GetNPCActionLabel(npc, &game->recipeRegistry);
            ImVec4 color(0.4f, 1.0f, 0.6f, 1.0f);
            if (action.name == "IDLE") {
              color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            } else if (action.name.find("HARVEST") != std::string::npos ||
                       action.name.find("BUSY") != std::string::npos) {
              color = ImVec4(1.0f, 0.7f, 0.2f, 1.0f);
            }
            // Verb and subject share the cell: the subject is dimmed and allowed
            // to wrap, so a long destination stays readable instead of being
            // clipped at the column edge.
            ImGui::TextColored(color, "%s", action.name.c_str());
            if (!action.context.empty()) {
              ImGui::SameLine(0.0f, 8.0f);
              ImGui::PushStyleColor(
                  ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
              ImGui::TextWrapped("%s", action.context.c_str());
              ImGui::PopStyleColor();
            }
            if (!action.context.empty() && ImGui::IsItemHovered()) {
              ImGui::SetTooltip("%s %s", action.name.c_str(),
                                action.context.c_str());
            }

            // The compact task summary: whether a list exists at all, how many
            // entries, and what it is serving. Clicking selects the row, and the
            // detail appears below the table -- the table cell is far too narrow
            // for life goals, ages and divergence.
            ImGui::TableNextColumn();
            if (const TaskList *tasks = npc.get<TaskList>()) {
              const AgentGoal *serving = TaskListActiveGoal(*tasks);
              if (serving == nullptr) serving = TaskListTopCandidate(*tasks);
              std::string summary =
                  std::to_string(tasks->goals.size()) + " entries";
              if (serving != nullptr) {
                summary += "  ";
                summary += TaskPriorityLabel(serving->tier);
              }
              const bool isSelected = selectedForTasks == npc;
              if (ImGui::Selectable((summary + "##tasks" + idStr).c_str(),
                                    isSelected,
                                    ImGuiSelectableFlags_AllowOverlap)) {
                taskRowToSelect = npc;
              }
              if (serving != nullptr && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n(click for the whole list)",
                                  serving->description.c_str());
              }
            } else {
              ImGui::TextDisabled("(none)");
            }

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

            ImGui::TableNextColumn();
            std::string inventoryLabel = "Open##inventory" + idStr;
            if (ImGui::Button(inventoryLabel.c_str())) {
              inventoryToOpen = npc;
            }
          });

      ImGui::EndTable();
    }

    // Clicking the selected row again collapses it, so there is a way back to the
    // full-height table.
    if (taskRowToSelect.is_alive()) {
      selectedForTasks = (selectedForTasks == taskRowToSelect)
                             ? flecs::entity::null()
                             : taskRowToSelect;
    }

    if (hasTaskDetail && selectedForTasks.is_alive()) {
      ImGui::Separator();
      std::string title = "Tasks";
      if (const DisplayName *displayName = selectedForTasks.get<DisplayName>()) {
        title += " -- " + displayName->name;
      }
      ImGui::TextUnformatted(title.c_str());
      if (ImGui::BeginChild("NPCMenuTaskDetail",
                            ImVec2(0.0f, detailHeight - 24.0f),
                            ImGuiChildFlags_Borders)) {
        if (const TaskList *tasks = selectedForTasks.get<TaskList>()) {
          DrawNPCTaskSection(selectedForTasks, *tasks,
                             GetNPCActionLabel(selectedForTasks,
                                               &game->recipeRegistry)
                                 .name);
        }
      }
      ImGui::EndChild();
    }

    if (contextToToggle.is_alive()) {
      if (contextToToggle.has<ActiveWindow>()) {
        contextToToggle.remove<ActiveWindow>();
      } else {
        contextToToggle.set<ActiveWindow>(
            {std::make_shared<NPCContextWindow>(contextToToggle)});
      }
    }

    // Opens the same read-only held-items window the "See Inventory"
    // context-menu option uses.
    if (inventoryToOpen.is_alive()) {
      game->OpenStorageWindow(inventoryToOpen);
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

// The village-wide task board (docs/task-system-design.md section 11): one row
// per agent, sorted by tier then age.
//
// The question this exists to answer is "why is this agent doing something
// dumb", which is impossible to see from inside one agent's context -- an
// inflated priority list, a task nobody has touched for a minute, or an agent
// whose stated intent and running action disagree are all invisible locally and
// obvious in a column of rows.
TaskBoardWindow::TaskBoardWindow(Game* game) : game(game) {}

void TaskBoardWindow::Draw() {
  bool show = game->taskBoardWindowEntity.has<ActiveWindow>();
  if (!show) return;

  ImGui::SetNextWindowSize(ImVec2(940, 360), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Task Board", &show, ImGuiWindowFlags_None)) {
    const int nowTick = CurrentTaskTick(game->ecs);

    ImGui::TextDisabled(
        "One row per agent. Sorted by tier, then by age. A developer tool: no "
        "agent can read another's list (section 10).");
    ImGui::Separator();

    // Collected first, then sorted, then drawn: an ImGui table cannot be
    // reordered once rows are being emitted, and querying flecs while drawing
    // inside the same iteration invites the usual mutation hazards.
    struct BoardRow {
      flecs::entity npc;
      std::string name;
      std::string lifeGoal;
      std::string serving;
      int servingId = -1;
      TaskPriority tier = TaskPriority::Normal;
      bool hasTier = false;
      int ageTicks = 0;
      std::string flags;
      std::string divergence;
      bool hasList = false;
      int tierRank = 99;
    };
    std::vector<BoardRow> rows;

    game->ecs.filter<AgentBrainWrapper>().each(
        [&rows, nowTick](flecs::entity npc, AgentBrainWrapper &) {
          if (!npc.is_alive()) return;

          BoardRow row;
          row.npc = npc;
          if (const DisplayName *displayName = npc.get<DisplayName>()) {
            row.name = displayName->name;
          } else if (npc.name().c_str() != nullptr) {
            row.name = npc.name().c_str();
          } else {
            row.name = "Unnamed";
          }

          const TaskList *list = npc.get<TaskList>();
          if (list == nullptr) {
            rows.push_back(std::move(row));
            return;
          }
          row.hasList = true;

          // LIFE GOAL: the first authored life goal. A character may have more
          // than one; the board shows the lead one, because the point of the
          // column is to recognise whose row this is, not to enumerate a life.
          for (const AgentGoal &goal : list->goals) {
            if (goal.horizon != GoalHorizon::Terminal) continue;
            row.lifeGoal = TruncateTaskText(goal.description, 28);
            break;
          }

          // NOW SERVING: what the agent says it is serving, falling back to its
          // top intent. The fallback is labelled in FLAGS rather than passed off
          // as an active task.
          const AgentGoal *serving = TaskListActiveGoal(*list);
          if (serving == nullptr) {
            serving = TaskListTopCandidate(*list);
            row.flags += (row.flags.empty() ? "" : " ");
            row.flags += "idle";
          }
          if (serving != nullptr) {
            row.serving = TruncateTaskText(serving->description, 34);
            row.servingId = serving->id;
            row.tier = serving->tier;
            row.hasTier = true;
            row.tierRank = TaskPriorityRank(serving->tier);
            row.ageTicks = TaskAgeTicks(*serving, nowTick);
            if (IsTaskStarving(*serving, nowTick)) {
              row.flags += (row.flags.empty() ? "" : " ");
              row.flags += "stale";
            }
            if (serving->horizon == GoalHorizon::Short && serving->parent < 0) {
              row.flags += (row.flags.empty() ? "" : " ");
              row.flags += "orphan";
            }
            for (const std::string &fact : serving->facts) {
              row.flags += (row.flags.empty() ? "" : " ");
              row.flags += fact;
            }
            if (!serving->ruleWarning.empty()) {
              row.flags += (row.flags.empty() ? "" : " ");
              row.flags += "!";
            }
          }

          // The two derived warnings that are impossible to see in a single
          // context (section 11).
          if (IsTaskListInflated(*list)) {
            row.flags += (row.flags.empty() ? "" : " ");
            row.flags += "inflated(" +
                         std::to_string(CountCriticalTasks(*list)) + "x crit)";
          }

          row.divergence = DescribeTaskDivergence(
              *list, GetNPCActionLabel(npc, nullptr).name);

          rows.push_back(std::move(row));
        });

    // Sorted by tier then age, with unlisted agents last: a row with no list is
    // not "lowest priority", it is a different kind of thing.
    std::stable_sort(rows.begin(), rows.end(),
                     [](const BoardRow &a, const BoardRow &b) {
                       if (a.hasTier != b.hasTier) return a.hasTier;
                       if (a.tierRank != b.tierRank) return a.tierRank < b.tierRank;
                       return a.ageTicks > b.ageTicks;
                     });

    if (ImGui::BeginTable("TaskBoardTable", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("NPC");
      ImGui::TableSetupColumn("Life Goal");
      ImGui::TableSetupColumn("Now Serving");
      ImGui::TableSetupColumn("Tier", ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 60.0f);
      ImGui::TableSetupColumn("Flags & Divergence",
                              ImGuiTableColumnFlags_WidthFixed, 300.0f);
      ImGui::TableHeadersRow();

      for (const BoardRow &row : rows) {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.name.c_str());

        ImGui::TableNextColumn();
        if (row.lifeGoal.empty()) {
          ImGui::TextDisabled("-");
        } else {
          ImGui::TextDisabled("%s", row.lifeGoal.c_str());
        }

        ImGui::TableNextColumn();
        if (!row.hasList) {
          ImGui::TextDisabled("(no task list)");
        } else if (row.serving.empty()) {
          ImGui::TextDisabled("-");
        } else {
          ImGui::Text("%s", row.serving.c_str());
        }

        ImGui::TableNextColumn();
        if (row.hasTier) {
          ImGui::TextColored(TaskTierColor(row.tier), "%s",
                             TaskPriorityLabel(row.tier));
        } else {
          ImGui::TextDisabled("-");
        }

        ImGui::TableNextColumn();
        if (row.hasTier) {
          ImGui::TextUnformatted(FormatTaskAge(row.ageTicks).c_str());
        } else {
          ImGui::TextDisabled("-");
        }

        ImGui::TableNextColumn();
        if (!row.divergence.empty()) {
          ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "DIVERGENCE: %s",
                             row.divergence.c_str());
        } else if (row.flags.empty()) {
          ImGui::TextDisabled("--");
        } else {
          ImGui::TextUnformatted(row.flags.c_str());
        }
      }

      ImGui::EndTable();
    }
  }
  ImGui::End();

  if (!show) {
    game->taskBoardWindowEntity.remove<ActiveWindow>();
    game->debugWindowState->SetShowTaskBoardWindow(false);
  } else {
    game->debugWindowState->SetShowTaskBoardWindow(true);
  }
}

VisionDebugWindow::VisionDebugWindow(Game* game) : game(game) {}

// Stage 0 of docs/spatial-memory-design.md: the facing marker, and nothing that
// consumes it yet. The counts are the point of the window at this stage -- they
// answer "did every character get a direction", which is the question that
// otherwise turns into a cone bug two stages later.
//
// The checkbox value is written through to DebugWindowState on change only, while
// the window's own visibility is written every frame, matching TaskBoardWindow.
// The two differ because a checkbox written every frame would be harmless but a
// visibility flag read once means a closed window reopens on the next launch.
void VisionDebugWindow::Draw() {
  bool show = game->visionDebugWindowEntity.has<ActiveWindow>();
  if (!show) return;

  ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Vision Debug", &show, ImGuiWindowFlags_None)) {
    ImGui::TextDisabled(
        "Stage 0 -- facing only. The view cone and spatial memory land in later "
        "stages; nothing samples from a facing yet.");
    ImGui::Separator();

    bool markers = game->debugWindowState->GetShowFacingMarkers();
    if (ImGui::Checkbox("Draw facing markers", &markers)) {
      game->debugWindowState->SetShowFacingMarkers(markers);
    }
    ImGui::TextDisabled(
        "Yellow line = facing (%.0f%% of a tile). Held while standing still.",
        DEFAULT_FACING_MARKER_TILE_FRACTION * 100.0f);

    ImGui::Separator();

    // Counted rather than queried: a filter already yields one canary row per
    // entity, and an explicit count keeps the two numbers in the same units even
    // if one of the two queries is ever changed to a narrower one.
    int facingCount = 0;
    game->ecs.filter<Facing>().each(
        [&facingCount](flecs::entity, const Facing &) { ++facingCount; });

    int characterCount = 0;
    int characterWithoutFacing = 0;
    game->ecs.filter<CharacterTag>().each(
        [&](flecs::entity character, const CharacterTag &) {
          ++characterCount;
          if (!character.has<Facing>()) ++characterWithoutFacing;
        });

    ImGui::Text("Characters: %d", characterCount);
    ImGui::Text("With a Facing component: %d", facingCount);
    if (characterWithoutFacing == 0) {
      ImGui::TextDisabled("Every character has a direction.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                         "%d character(s) have no Facing -- they will not be "
                         "sampled in stage 1.",
                         characterWithoutFacing);
    }
  }
  ImGui::End();

  if (!show) {
    game->visionDebugWindowEntity.remove<ActiveWindow>();
    game->debugWindowState->SetShowVisionDebugWindow(false);
  } else {
    game->debugWindowState->SetShowVisionDebugWindow(true);
  }
}

// Lists every blueprint in the registry and arms the placement ghost when one is
// selected. See docs/blueprint-design.md.
//
// The window owns no placement state: Game::selectedBlueprintId is the mode, and
// Game::handleInput performs the click, exactly like the map editor's selection.
// That is what keeps the preview and the commit on one code path -- both go
// through Game::BuildBlueprintPlacement.
namespace {
// Neither pane may be dragged to nothing: a window with an ungrabbable sliver is a
// state the user cannot get out of.
//
// The draggable pane is the *list*, which sits on the left now, so the splitter
// stays directly right of the thing it resizes. The preview keeps a wider floor
// because it holds the canvas and the details.
constexpr float kMinListPaneWidth = 200.0f;
constexpr float kMinPreviewPaneWidth = 300.0f;
}  // namespace

BlueprintWindow::BlueprintWindow(Game* game) : game(game) {}

void BlueprintWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_FirstUseEver);
  ImGui::Begin("Blueprints", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    game->blueprintWindowEntity.remove<ActiveWindow>();
    // The placement mode goes with the window: leaving a ghost armed with no window
    // to deselect it from would be a trap.
    game->selectedBlueprintId.clear();
    ImGui::End();
    return;
  }

  // A delete requested last frame, applied before anything is read this frame.
  // The registry is re-acquired afterwards rather than reused, because deleting
  // reloads it and the old pointer then refers to freed storage.
  if (!pendingDeleteId.empty() && game->debugWindowState) {
    const std::string doomed = pendingDeleteId;
    pendingDeleteId.clear();
    if (auto* res = game->ecs.get<BlueprintRegistryResource>()) {
      if (res->registry != nullptr) {
        const BlueprintRegistry::DeleteResult deleted =
            res->registry->DeleteBlueprint(doomed);
        if (deleted.ok) {
          lastDeleteMessage = "Deleted " + deleted.path;
          if (previewedBlueprintId == doomed) {
            previewedBlueprintId.clear();
            previewedExpansion.reset();
          }
          // The ghost is disarmed if the destroyed design was the armed one:
          // handleInput would drop the mode anyway, but doing it here keeps the
          // window's own state consistent with it.
          if (game->selectedBlueprintId == doomed) {
            game->selectedBlueprintId.clear();
            game->lastBlueprintPlacementMessage.clear();
          }
          if (game->debugLog) {
            game->debugLog->LogInfo("Blueprint window: deleted " + deleted.path);
          }
        } else {
          lastDeleteMessage = "Could not delete: " + deleted.error;
          if (game->debugLog) {
            game->debugLog->LogError("Blueprint window: " + lastDeleteMessage);
          }
        }
      }
    }
  }

  auto* registryRes = game->ecs.get<BlueprintRegistryResource>();
  const BlueprintRegistry* registry =
      registryRes != nullptr ? registryRes->registry : nullptr;

  if (registry == nullptr || registry->Count() == 0) {
    ImGui::TextWrapped(
        "No blueprints loaded. Expected data/blueprints/*.json -- see "
        "docs/blueprint-design.md for the grammar.");
    ImGui::End();
    return;
  }

  if (!layoutLoaded && game->debugWindowState) {
    paneWidth = game->debugWindowState->GetBlueprintPaneWidth();
    zoom = game->debugWindowState->GetBlueprintZoom();
    layoutLoaded = true;
  }

  const std::vector<const Blueprint*> all = registry->GetAll();

  ImGui::Text("Selected for placement: %s",
              game->selectedBlueprintId.empty()
                  ? "(none - use the Select button)"
                  : game->selectedBlueprintId.c_str());

  if (ImGui::Checkbox("Persist painted terrain to map file",
                      &game->blueprintPersistTerrain)) {
    // Nothing to do: the flag is read when a placement commits. Objects are always
    // marked MapAuthored, so this only governs the tiles.
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Objects placed are always marked as authored map content and survive a "
        "save.\nPainted tiles only survive if this is on, because terrain lives "
        "in the map's\npositions array. Leave it off for throwaway experiments.");
  }

  ImGui::Separator();

  // Defensive: an id that no longer resolves (deleted, or the registry reloaded)
  // drops back to previewing the armed design, then to the first in the list.
  // Without this the pane could sit empty while designs exist.
  if (!previewedBlueprintId.empty() && registry->Get(previewedBlueprintId) == nullptr) {
    previewedBlueprintId.clear();
  }
  if (previewedBlueprintId.empty()) {
    previewedBlueprintId = !game->selectedBlueprintId.empty() &&
                                   registry->Get(game->selectedBlueprintId) != nullptr
                               ? game->selectedBlueprintId
                               : (all.empty() ? std::string() : all.front()->id);
  }

  // The previewed design and its expansion are resolved together, by this lambda,
  // so they cannot disagree.
  //
  // They did, and it crashed: the pair was resolved before the list ran, the list
  // then changed the id and reset the expansion, and the details below read
  // `previewedExpansion->cells` on an emptied optional while `previewed` still
  // pointed at a design. Recomputing only one of the two is the bug; recomputing
  // both from the id is the fix.
  //
  // Cached because the pane redraws every frame and rasterising a shape set per
  // frame would be wasted work, and rebuilt whenever the id changes -- checked
  // rather than assumed, so any path that changes the id is covered.
  std::string expansionFor;
  const auto resolve = [&]() {
    const Blueprint* blueprint =
        previewedBlueprintId.empty() ? nullptr : registry->Get(previewedBlueprintId);
    if (blueprint == nullptr) {
      expansionFor.clear();
      previewedExpansion.reset();
      return static_cast<const Blueprint*>(nullptr);
    }
    if (expansionFor != previewedBlueprintId || !previewedExpansion.has_value()) {
      previewedExpansion = BlueprintExpansion::Expand(*blueprint);
      expansionFor = previewedBlueprintId;
    }
    return blueprint;
  };

  // Mutable: re-resolved after the list runs. See below it.
  const Blueprint* previewed = resolve();

  // Two panes only: the list on the left, the preview and the design's details
  // together on the right.
  //
  // They used to be three -- preview, list, and a full-width details strip beneath
  // -- which split one design's information across two places and made the eye
  // travel. Everything about the previewed design now lives in one pane.
  const float available = ImGui::GetContentRegionAvail().x;
  const float maxPaneWidth =
      std::max(kMinListPaneWidth, available - kMinPreviewPaneWidth - 8.0f);
  // `paneWidth` is the list pane. A value persisted when it meant the preview pane
  // is simply the wrong size rather than a broken one; the clamp settles it.
  paneWidth = std::clamp(paneWidth, kMinListPaneWidth, maxPaneWidth);

  ImGui::BeginChild("blueprint_list", ImVec2(paneWidth, 0), true);
  for (const Blueprint* blueprint : all) {
    const bool armed = blueprint->id == game->selectedBlueprintId;
    const bool shown = blueprint->id == previewedBlueprintId;
    // A design that expands to nothing is rejected at load, so anything listed here
    // has cells. The dimensions are shown because they are the quickest way to see
    // that a design parsed into what its author intended.
    const std::string label = blueprint->name + " (" +
                              std::to_string(blueprint->Width()) + "x" +
                              std::to_string(blueprint->Height()) + ")" +
                              "###" + blueprint->id;
    // Clicking a row previews it. It no longer arms anything: that is the Select
    // button's job, which is what makes browsing safe.
    if (ImGui::Selectable(label.c_str(), shown)) {
      // Only the id changes. The preview and its expansion are re-derived from it
      // by `resolve`, so there is nothing here to keep in sync -- and nothing to
      // get wrong.
      previewedBlueprintId = blueprint->id;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s\nid: %s\nsource: %s",
                        armed ? "Armed for placement." : "Click to preview.",
                        blueprint->id.c_str(),
                        blueprint->sourcePath.empty() ? "(runtime)"
                                                      : blueprint->sourcePath.c_str());
    }
  }
  ImGui::EndChild();

  // Re-resolved: the list above is what changes the selection, so the pair read
  // before it may belong to the previous design. Cheap, because `resolve` only
  // re-expands when the id actually changed.
  previewed = resolve();

  // The splitter, built the same way as the Designer window's.
  ImGui::SameLine(0.0f, 0.0f);
  const float paneLeft = ImGui::GetCursorScreenPos().x;
  ImGui::InvisibleButton("blueprint_splitter", ImVec2(8.0f, 0));
  const bool splitterHovered = ImGui::IsItemHovered();
  const bool splitterActive = ImGui::IsItemActive();
  if (splitterHovered || splitterActive) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }
  if (splitterActive) {
    paneWidth = ImGui::GetIO().MousePos.x - paneLeft;
  }
  {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float centerX = (min.x + max.x) * 0.5f;
    const ImU32 color = splitterActive    ? IM_COL32(255, 170, 80, 255)
                        : splitterHovered ? IM_COL32(200, 200, 200, 255)
                                          : IM_COL32(90, 90, 90, 255);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(centerX, min.y),
                                        ImVec2(centerX, max.y), color,
                                        splitterActive ? 3.0f : 1.0f);
  }

  ImGui::SameLine(0.0f, 0.0f);

  // The one pane that holds everything about the previewed design: the drawing,
  // its legend, its details and the buttons that act on it.
  ImGui::BeginChild("blueprint_preview_pane", ImVec2(0, 0), true);
  if (previewed == nullptr) {
    ImGui::TextWrapped(
        "Click a blueprint in the list to preview it. A preview is drawn the way "
        "the map draws it, at the game's own tile size by default.");
  } else {
    ImGui::Text("%s", previewed->name.empty() ? previewed->id.c_str()
                                              : previewed->name.c_str());
    ImGui::TextDisabled("%s | %d x %d | %zu cells", previewed->id.c_str(),
                        previewed->Width(), previewed->Height(),
                        previewedExpansion->cells.size());
    if (!previewed->owner.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("| owner: %s", previewed->owner.c_str());
    }
    if (!previewed->purpose.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("| %s", previewed->purpose.c_str());
    }
    if (!previewed->sourcePath.empty()) {
      ImGui::TextDisabled("file: %s", previewed->sourcePath.c_str());
    }

    BlueprintPreview::DrawZoomControl("##blueprint_zoom", zoom);

    const float canvasHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.45f);
    BlueprintPreview::DrawCanvas("blueprint_preview_canvas", *previewed,
                                 *previewedExpansion, zoom, game->tileRegistry,
                                 game->resourceManager, canvasHeight);
    BlueprintPreview::DrawLegend(*previewedExpansion, game->tileRegistry);
  }

  if (previewed != nullptr) {
    if (!previewedExpansion->errors.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
      for (const std::string& message : previewedExpansion->errors) {
        ImGui::TextWrapped("%s", message.c_str());
      }
      ImGui::PopStyleColor();
    }

    ImGui::TextDisabled("Contents");
    std::map<std::string, int> counts;
    for (const BlueprintExpansion::ExpandedCell& cell : previewedExpansion->cells) {
      if (cell.carve) {
        counts["(carved opening)"]++;
      } else if (!cell.tile.empty()) {
        counts[cell.tile]++;
      } else if (!cell.object.empty()) {
        counts["object: " + cell.object]++;
      }
    }
    std::string contents;
    for (const auto& entry : counts) {
      if (!contents.empty()) contents += ", ";
      contents += entry.first + " x" + std::to_string(entry.second);
    }
    ImGui::TextWrapped("%s", contents.c_str());

    ImGui::Separator();

    // One button that toggles, rather than a Select and a Deselect.
    //
    // Two buttons meant the second was dead half the time (Deselect only applies
    // when this design is the armed one), and neither told you which state you were
    // in. A single control whose label names its action does: it reads "Select"
    // when this design is idle and "Deselect" when it is the one following the
    // cursor, so the label always says what pressing it will do.
    const bool isArmed = previewed->id == game->selectedBlueprintId;
    if (isArmed) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.20f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.26f, 0.55f, 0.26f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.38f, 0.16f, 1.0f));
    }
    const bool togglePressed = ImGui::Button(isArmed ? "Deselect" : "Select");
    if (isArmed) {
      ImGui::PopStyleColor(3);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          isArmed ? "Stop carrying this design; the world preview goes away."
                  : "Carry this design under the cursor, so a left click on the map "
                    "builds it.");
    }
    if (togglePressed) {
      if (isArmed) {
        game->selectedBlueprintId.clear();
      } else {
        game->selectedBlueprintId = previewed->id;
      }
      game->lastBlueprintPlacementMessage.clear();
    }

    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
      ImGui::OpenPopup("Delete blueprint?");
    }
    // Copied out of `previewed` and `previewedExpansion` up front. The delete
    // reloads the registry, which frees the objects both point into, so nothing
    // below may read through them again -- this is the shape of the crash this
    // window had once already.
    const std::string deleteId = previewed->id;
    const std::string deleteSource = previewed->sourcePath;
    const std::string deleteName = previewed->name;
    const size_t deleteCells = previewedExpansion->cells.size();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Deletes %s from %s.\nThis removes the file and cannot be undone.",
          deleteId.c_str(),
          deleteSource.empty() ? "(runtime)" : deleteSource.c_str());
    }

    // Confirmed rather than immediate: this deletes a file from the content
    // directory with no trash and no revision history behind it.
    //
    // The modal's full-screen dimming is suppressed for this one popup, so the
    // window the question is about stays visible while you answer it.
    ImVec4 &modalDim = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg];
    const ImVec4 savedDim = modalDim;
    modalDim = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    const bool modalOpen = ImGui::BeginPopupModal(
        "Delete blueprint?", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (modalOpen) {
      const ImVec2 min = ImGui::GetWindowPos();
      const ImVec2 size = ImGui::GetWindowSize();
      ImGui::GetWindowDrawList()->AddRect(min, ImVec2(min.x + size.x, min.y + size.y),
                                          IM_COL32(255, 120, 90, 255), 0.0f, 0, 2.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.6f, 1.0f));
      ImGui::Text("Delete '%s'?", deleteId.c_str());
      ImGui::PopStyleColor();
      ImGui::TextDisabled("%s (%s, %zu cells)",
                          deleteName.empty() ? deleteId.c_str() : deleteName.c_str(),
                          deleteSource.empty() ? "no file" : deleteSource.c_str(),
                          deleteCells);
      ImGui::Separator();
      if (ImGui::Button("Delete", ImVec2(120, 0))) {
        // Deferred to the next frame. See `pendingDeleteId`: the reload cannot
        // happen while this frame still holds pointers into the old registry.
        pendingDeleteId = deleteId;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    modalDim = savedDim;

    if (!lastDeleteMessage.empty()) {
      ImGui::TextDisabled("%s", lastDeleteMessage.c_str());
    }

    // Site legality, only while the design is armed: it describes where the ghost
    // is, which means nothing for a design that is not following the cursor.
    if (isArmed) {
      const BlueprintPlacement preview =
          game->BuildBlueprintPlacement(*previewed, game->blueprintGhostOrigin);
      ImGui::Separator();
      if (preview.allLegal) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        ImGui::TextWrapped("Site at (%d,%d) is clear to build.",
                           game->blueprintGhostOrigin.x,
                           game->blueprintGhostOrigin.y);
        ImGui::PopStyleColor();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        ImGui::TextWrapped("Site at (%d,%d) is blocked:",
                           game->blueprintGhostOrigin.x,
                           game->blueprintGhostOrigin.y);
        ImGui::PopStyleColor();
        // The first few distinct reasons, deduplicated: a design overlapping a
        // house reports the same reason for every cell it overlaps.
        std::vector<std::string> shown;
        for (const BlueprintGhostCell& cell : preview.cells) {
          if (cell.legal || cell.reason.empty()) continue;
          if (std::find(shown.begin(), shown.end(), cell.reason) != shown.end())
            continue;
          if (shown.size() >= 4) break;
          shown.push_back(cell.reason);
        }
        for (const std::string& reason : shown) {
          ImGui::BulletText("%s", reason.c_str());
        }
      }
      if (!game->lastBlueprintPlacementMessage.empty()) {
        ImGui::TextWrapped("%s", game->lastBlueprintPlacementMessage.c_str());
      }
    }
  }
  ImGui::EndChild();
  ImGui::End();

  game->debugWindowState->SetShowBlueprintWindow(true);
}
