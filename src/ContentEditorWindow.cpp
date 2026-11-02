#include "ContentEditorWindow.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ArtRequestsPanel.h"
#include "TileArtQueue.hpp"
#include "ContentKinds.h"
#include "DebugLog.h"
#include "DesignerWindow.h"
#include "Game.h"
#include "Settings.h"
#include "SpriteDraw.hpp"
#include "SpritePicker.hpp"
#include "SpritePaths.hpp"
#include "RigRender.hpp"
#include "SpriteKinds.hpp"
#include "imgui.h"

namespace {

// A key is a colour field when it says so. Case-insensitive on purpose: the documents
// in the tree are written `characterColor`, but a hand-authored one may say `colour`
// or `CHARACTER_COLOR`, and a picker that only recognised one spelling would leave the
// raw RGB array in place -- which looks like the feature not working.
bool KeyMentionsColour(const std::string &key) {
  std::string lowered = key;
  for (char &c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lowered.find("color") != std::string::npos ||
         lowered.find("colour") != std::string::npos;
}

bool KeyIsPalette(const std::string &key) {
  std::string lowered = key;
  for (char &c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lowered == "palette";
}

// [r,g,b] or [r,g,b,a]. The length is the whole test: a two-element array is a
// coordinate, and a five-element one is not a colour whatever it is called.
bool IsColourArray(const nlohmann::json &value) {
  if (!value.is_array() || (value.size() != 3 && value.size() != 4)) return false;
  for (const nlohmann::json &channel : value) {
    if (!channel.is_number()) return false;
  }
  return true;
}

// The wheel. Channels are stored 0..255 in the documents and 0..1 in ImGui, so the
// conversion happens on both sides of the widget rather than being hidden in it.
bool DrawColourValue(nlohmann::json &value) {
  float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  for (size_t i = 0; i < value.size(); ++i) {
    const int channel = value[i].get<int>();
    rgba[i] = std::clamp(channel, 0, 255) / 255.0f;
  }

  bool changed = false;
  if (value.size() == 4) {
    changed = ImGui::ColorEdit4(
        "##colour", rgba,
        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf |
            ImGuiColorEditFlags_DisplayRGB);
  } else {
    changed = ImGui::ColorEdit3("##colour", rgba, ImGuiColorEditFlags_DisplayRGB);
  }

  if (changed) {
    for (size_t i = 0; i < value.size(); ++i) {
      const int channel =
          static_cast<int>(std::lround(rgba[i] * 255.0f));
      value[i] = std::clamp(channel, 0, 255);
    }
  }
  return changed;
}

// One value of the document, edited in place. Returns true when it changed.
//
// Recursive over the JSON rather than a type per registry: the form has to be able to
// draw a document it has never seen, which is the whole point of the editor. Arrays get
// add/remove because they are how the grammar expresses most things (components, art,
// palette); objects get add/remove of keys for the same reason.
bool DrawJsonValue(nlohmann::json &value, const std::string &key,
                   bool inPalette) {
  bool changed = false;

  // A colour is recognised before the array branch, because it *is* an array of
  // numbers and would otherwise be drawn as four spinners.
  if ((inPalette || KeyMentionsColour(key)) && IsColourArray(value)) {
    return DrawColourValue(value);
  }

  if (value.is_boolean()) {
    bool checked = value.get<bool>();
    if (ImGui::Checkbox("##bool", &checked)) {
      value = checked;
      changed = true;
    }
  } else if (value.is_number_integer()) {
    long long number = value.get<long long>();
    if (ImGui::InputScalar("##int", ImGuiDataType_S64, &number)) {
      value = number;
      changed = true;
    }
  } else if (value.is_number_unsigned()) {
    unsigned long long number = value.get<unsigned long long>();
    if (ImGui::InputScalar("##uint", ImGuiDataType_U64, &number)) {
      value = number;
      changed = true;
    }
  } else if (value.is_number_float()) {
    double number = value.get<double>();
    if (ImGui::InputScalar("##float", ImGuiDataType_Double, &number, nullptr, nullptr,
                           "%.3f")) {
      value = number;
      changed = true;
    }
  } else if (value.is_string()) {
    // A fixed buffer because that is what ImGui edits; the document is a person's file,
    // not a stream, and a string longer than this in a tile or object definition is a
    // mistake rather than a limitation worth streaming for.
    char buffer[2048];
    std::snprintf(buffer, sizeof(buffer), "%s", value.get<std::string>().c_str());
    if (ImGui::InputText("##string", buffer, sizeof(buffer))) {
      value = std::string(buffer);
      changed = true;
    }
  } else if (value.is_array()) {
    ImGui::PushID("array");
    if (ImGui::TreeNodeEx("##items", ImGuiTreeNodeFlags_DefaultOpen, "%zu item(s)",
                          value.size())) {
      for (size_t i = 0; i < value.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%zu", i);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-60.0f);
        changed |= DrawJsonValue(value[i], key, inPalette);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
          value.erase(value.begin() + static_cast<nlohmann::json::difference_type>(i));
          changed = true;
          ImGui::PopID();
          break;
        }
        ImGui::PopID();
      }
      // Duplicating the last entry is the useful default: a list of palette colours or
      // art lines is extended by copying a neighbour and editing it, not by typing from
      // nothing.
      if (ImGui::SmallButton("add")) {
        value.push_back(value.empty() ? nlohmann::json("") : value.back());
        changed = true;
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  } else if (value.is_object()) {
    ImGui::PushID("object");
    char label[96];
    std::snprintf(label, sizeof(label), "{%zu key(s)}", value.size());
    if (ImGui::TreeNodeEx("##keys", ImGuiTreeNodeFlags_DefaultOpen, "%s", label)) {
      for (auto &entry : value.items()) {
        ImGui::PushID(entry.key().c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(entry.key().c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        changed |= DrawJsonValue(entry.value(), entry.key(),
                                   inPalette || KeyIsPalette(entry.key()));
        ImGui::PopID();
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  } else {
    ImGui::TextDisabled("(%s)", value.type_name());
  }
  return changed;
}

// The sprite kind for a content kind. Tiles and objects are the only ones with art, and
// ContentKinds::Kind::hasSprites is what gates the pane, so this is total for callers that
// have checked.
SpriteKinds::Kind SpriteKindOf(const ContentKinds::Kind &kind) {
  if (kind.id == "objects") return SpriteKinds::Kind::Object;
  if (kind.id == "actors") return SpriteKinds::Kind::Actor;
  return SpriteKinds::Kind::Tile;
}

}  // namespace

ContentEditorWindow::ContentEditorWindow(Game *game, flecs::entity owner)
    : game(game) {
  (void)owner;  // Windows are not ECS components yet; see Window.h.
  ReloadDocument();
}

ContentKinds::World ContentEditorWindow::World_() {
  return ContentKinds::World{
      game->tileRegistry, game->objectFactory, game->actorSpriteRegistry,
      game->rigRegistry, game->itemRegistry, game->skillRegistry,
      game->npcTemplateRegistry, game->brainTemplateRegistry,
      game->debugLog.get(), [this]() { game->RefreshPlayerSkillBar(); }};
}

const ContentKinds::Kind *ContentEditorWindow::Kind() const {
  return ContentKinds::For(kindId);
}

void ContentEditorWindow::ReloadDocument() {
  document = nlohmann::json::object();
  dirty = false;
  const ContentKinds::Kind *kind = Kind();
  if (kind == nullptr || game == nullptr || entryId.empty()) return;
  document = kind->read(World_(), entryId);
}

void ContentEditorWindow::Save() {
  const ContentKinds::Kind *kind = Kind();
  if (kind == nullptr || game == nullptr || entryId.empty()) return;

  const std::string directory =
      (std::filesystem::path(Settings().content.editor) / kind->id).string();
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    status = "could not create " + directory + ": " + ec.message();
    statusIsError = true;
    return;
  }

  // A fixed identity is supplied here rather than shown in the form. The working
  // copy stays clean (so the form never grows an id field); the copy that is
  // validated and written carries the selected entry's id.
  nlohmann::json saveDocument = document;
  if (kind->fixedIdKey != nullptr) {
    saveDocument[kind->fixedIdKey] = entryId;
  }
  if (kind->prepare != nullptr) {
    saveDocument = kind->prepare(std::move(saveDocument));
  }

  const std::string stem = kind->fileName(saveDocument);
  if (stem.empty()) {
    status = "the document has no name, so there is no file to save it under";
    statusIsError = true;
    return;
  }

  std::vector<std::string> warnings;
  std::string error;
  if (!kind->apply(World_(), saveDocument, &error, &warnings)) {
    // Refused before the file is written: a document the registry rejects must not be
    // left on disk looking authoritative, or the next launch loads it and fails the same
    // way with no one to explain it.
    status = "refused: " + error;
    statusIsError = true;
    return;
  }

  std::string written;
  if (!ContentKinds::WriteOverrideFile(*kind, Settings().content.editor,
                                       saveDocument, &written, &error)) {
    status = error;
    statusIsError = true;
    return;
  }
  const std::filesystem::path path(written);

  dirty = false;
  // Skills may have been normalized by the apply (a type default filled in, an
  // activation added), so show the live document. Other kinds keep the form on
  // the authored copy, as before.
  if (kind->id == "skills") {
    ReloadDocument();
  }
  status = "saved " + path.string() +
           (warnings.empty() ? "" : " (" + std::to_string(warnings.size()) +
                                     " warning(s) in the debug log)");
  statusIsError = false;
  if (game->debugLog != nullptr) {
    for (const std::string &warning : warnings) {
      game->debugLog->LogWarning("Content editor: " + entryId + ": " + warning);
    }
    game->debugLog->LogInfo("Content editor: saved " + kind->id + "/" + stem);
  }
}

void ContentEditorWindow::DrawKindAndList() {
  if (game == nullptr) {
    ImGui::TextWrapped("The window has no game to read from.");
    return;
  }

  const ContentKinds::Kind *kind = Kind();
  if (kind == nullptr) return;

  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##contentFilter", "filter by name...", filterBuffer,
                           sizeof(filterBuffer));

  const std::vector<ContentKinds::Entry> entries = kind->list(World_());
  std::string lowered(filterBuffer);
  for (char &c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  ImGui::BeginChild("content_list", ImVec2(0, 0), true);
  for (const ContentKinds::Entry &entry : entries) {
    if (!lowered.empty()) {
      std::string haystack = entry.label + " " + entry.id;
      for (char &c : haystack) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (haystack.find(lowered) == std::string::npos) continue;
    }

    const bool shown = entry.id == entryId;
    ImGui::PushID(entry.id.c_str());
    if (ImGui::Selectable(entry.label.c_str(), shown)) {
      entryId = entry.id;
      ReloadDocument();
      status.clear();
      statusIsError = false;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s\nfrom: %s", entry.id.c_str(), entry.origin.c_str());
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
}

namespace {

// A 14x14 trash can, drawn rather than typed: the window uses ImGui's default font, which
// has no icon glyphs, so a Unicode bin would render as a missing-glyph box.
// Whether two image files hold the same picture. Path comparison is not enough: after a
// variant is activated, `<id>.png` is a *different file* holding the same bytes, and
// archiving it again would add a duplicate row every time the same sprite is chosen.
bool SameContents(const std::filesystem::path &a, const std::filesystem::path &b) {
  std::error_code ec;
  if (std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec) || ec) {
    return false;
  }
  std::ifstream left(a, std::ios::binary);
  std::ifstream right(b, std::ios::binary);
  if (!left.is_open() || !right.is_open()) return false;
  return std::equal(std::istreambuf_iterator<char>(left),
                    std::istreambuf_iterator<char>(),
                    std::istreambuf_iterator<char>(right));
}

// The marker records which sprite is chosen, so the list can mark the active row. Its
// presence alone means "generated wins"; the name is only for display.
std::string ChosenVariantName(const std::string &markerPath) {
  std::ifstream in(markerPath);
  if (!in.is_open()) return std::string();
  nlohmann::json marker;
  try {
    in >> marker;
  } catch (const std::exception &) {
    return std::string();
  }
  return marker.value("sprite", std::string());
}

}  // namespace

std::vector<std::filesystem::path> ContentEditorWindow::SpriteVariants(
    const std::string &conventionId) const {
  std::vector<std::filesystem::path> variants;
  const std::string active = GeneratedSpritePath(conventionId);
  if (active.empty()) return variants;

  const std::filesystem::path path(active);
  const std::string stem = path.stem().string() + ".v";
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(path.parent_path(), ec)) {
    if (entry.path().extension() != ".png") continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind(stem, 0) != 0) continue;
    // `<stem>.v<N>.png` only: a name that merely starts with the stem is a different
    // asset whose name happens to share a prefix.
    const std::string tail = name.substr(stem.size());
    const std::string digits = tail.substr(0, tail.size() - 4);
    if (digits.empty()) continue;
    bool allDigits = true;
    for (const char c : digits) {
      if (!std::isdigit(static_cast<unsigned char>(c))) allDigits = false;
    }
    if (allDigits) variants.push_back(entry.path());
  }
  std::sort(variants.begin(), variants.end());
  return variants;
}

void ContentEditorWindow::ArchiveActiveSprite(const std::string &conventionId) {
  const std::string active = GeneratedSpritePath(conventionId);
  if (active.empty()) return;

  std::error_code ec;
  if (!std::filesystem::exists(active, ec)) return;

  // The next free index, so regenerating twice keeps both.
  int next = 1;
  for (const std::filesystem::path &variant : SpriteVariants(conventionId)) {
    const std::string name = variant.stem().string();
    const size_t dot = name.rfind(".v");
    if (dot == std::string::npos) continue;
    next = std::max(next, std::atoi(name.c_str() + dot + 2) + 1);
  }

  const std::filesystem::path path(active);
  const std::string stem = path.stem().string() + ".v" + std::to_string(next);
  std::filesystem::copy_file(active, path.parent_path() / (stem + ".png"), ec);
  // The DSL document travels with the image: a variant whose document was lost could be
  // made active but never recompiled, which is half a variant.
  const std::string document = GeneratedSpriteDocumentPath(conventionId);
  if (!document.empty() && std::filesystem::exists(document, ec)) {
    std::filesystem::copy_file(document, path.parent_path() / (stem + ".sprite"), ec);
  }
}

void ContentEditorWindow::ActivateVariant(const std::string &conventionId,
                                          const std::filesystem::path &variant) {
  const std::string active = GeneratedSpritePath(conventionId);
  if (active.empty() || !std::filesystem::exists(variant)) return;

  std::error_code ec;
  // Never destroy what is currently showing. `<id>.png` is the newest generation until a
  // *new* regeneration archives it, so activating an older variant used to overwrite the
  // newest one -- leaving the "current generation" row pointing at the file that had just
  // been replaced, which made choosing it again a no-op. Archiving first keeps both ends
  // of the back-and-forth listable.
  bool alreadyListed = std::filesystem::equivalent(active, variant, ec) && !ec;
  ec.clear();
  // Same picture counts as already listed, wherever it currently lives: the file behind
  // the active path is a copy of one of these variants after every activation, so
  // archiving on path comparison alone duplicated the variant on every click.
  if (!alreadyListed && std::filesystem::exists(active, ec) &&
      SameContents(active, variant)) {
    alreadyListed = true;
  }
  ec.clear();
  if (!alreadyListed && std::filesystem::exists(active, ec)) {
    for (const std::filesystem::path &existing : SpriteVariants(conventionId)) {
      if (SameContents(active, existing)) {
        alreadyListed = true;   // this picture is already in the archive
        break;
      }
    }
  }
  ec.clear();
  if (!alreadyListed && std::filesystem::exists(active, ec)) {
    ArchiveActiveSprite(conventionId);
  }
  ec.clear();

  std::filesystem::copy_file(variant, active,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    status = "could not activate " + variant.filename().string() + ": " + ec.message();
    statusIsError = true;
    return;
  }

  // The document next to it, when there is one, so a later recompile rebuilds this
  // variant rather than the one that was there before.
  const std::filesystem::path document = variant.parent_path() /
                                          (variant.stem().string() + ".sprite");
  if (std::filesystem::exists(document, ec)) {
    const std::string target = GeneratedSpriteDocumentPath(conventionId);
    if (!target.empty()) {
      std::filesystem::copy_file(document, target,
                                 std::filesystem::copy_options::overwrite_existing, ec);
    }
  }

  // The marker is what makes the generated copy win over the pack's art. Written before
  // the refresh, because the refresh resolves the sprite again and would otherwise still
  // read the pack-first order.
  const std::string markerPath = GeneratedSpriteMarkerPath(conventionId);
  if (!markerPath.empty()) {
    std::ofstream marker(markerPath);
    if (marker.is_open()) {
      nlohmann::json choice = nlohmann::json::object();
      choice["sprite"] = variant.filename().string();
      marker << choice.dump(2) << "\n";
    }
  }

  // The world memoises textures by path, so the new file would keep drawing the old image
  // until the memo is dropped. This is the same refresh the hot-swap uses.
  RefreshSelectedAsset();
  status = "activated " + variant.filename().string();
  statusIsError = false;
}

void ContentEditorWindow::UseOriginalSprite(const std::string &conventionId) {
  // Removing the marker is the whole action: with nothing preferring the generated copy,
  // resolution falls back to the pack's art, which was never touched.
  const std::string markerPath = GeneratedSpriteMarkerPath(conventionId);
  if (!markerPath.empty()) {
    std::error_code ec;
    std::filesystem::remove(markerPath, ec);
  }
  RefreshSelectedAsset();
  status = "using the original artwork";
  statusIsError = false;
}

void ContentEditorWindow::DeleteVariant(const std::string &conventionId,
                                        const std::filesystem::path &variant) {
  std::error_code ec;
  if (!std::filesystem::exists(variant, ec)) {
    status = "that sprite is already gone";
    statusIsError = true;
    return;
  }

  const std::string active = GeneratedSpritePath(conventionId);
  const bool isActive =
      !active.empty() && std::filesystem::equivalent(active, variant, ec) && !ec;
  ec.clear();

  const std::string filename = variant.filename().string();
  std::filesystem::remove(variant, ec);
  if (ec) {
    status = "could not delete " + filename + ": " + ec.message();
    statusIsError = true;
    return;
  }
  // The document next to it, when the names line up: a sprite without its document can
  // never be recompiled, and this one is not coming back.
  const std::filesystem::path document =
      variant.parent_path() / (variant.stem().string() + ".sprite");
  std::filesystem::remove(document, ec);

  // If the chosen sprite was this one, drop the choice with it. Leaving the marker behind
  // would keep the list claiming "generated, in use" for a file that no longer exists, and
  // resolution would fall back to the pack while the UI said otherwise.
  const std::string markerPath = GeneratedSpriteMarkerPath(conventionId);
  const bool wasChosen = isActive || ChosenVariantName(markerPath) == filename;
  if (wasChosen && !markerPath.empty()) {
    std::filesystem::remove(markerPath, ec);
    RefreshSelectedAsset();
  }
  status = "deleted " + filename + (wasChosen ? " (back to the original)" : "");
  statusIsError = false;
}

void ContentEditorWindow::RefreshSelectedAsset() {
  const ContentKinds::Kind *kind = Kind();
  if (kind == nullptr) return;
  // The three kinds do not share a convention id: a tile is `tile_<name>`, an object and
  // an actor are the bare id. Refreshing an actor through the tile path looked for
  // `tile_<name>`, found nothing, and left the real texture cached -- which is why the
  // large preview kept drawing the sprite that was showing before the switch.
  if (kind->id == "objects") {
    game->RefreshObjectVisuals(entryId);
  } else if (kind->id == "actors") {
    game->ForgetActorSprite(entryId);
  } else {
    game->RefreshTileVisuals(entryId);
  }
}

void ContentEditorWindow::DrawSpritePane() {
  const ContentKinds::Kind *kind = Kind();
  if (kind == nullptr || game == nullptr || !kind->hasSprites) return;

  const std::string convention = kind->spriteId(entryId);

  // The document's own `sprite` override wins, exactly as it does in the world, so the
  // preview cannot show an authored path the renderer would ignore.
  const std::string explicitSprite =
      document.contains("sprite") && document["sprite"].is_string()
          ? document["sprite"].get<std::string>()
          : std::string();

  const Texture2D *texture = ResolveSpriteTexture(
      explicitSprite, convention, game->resourceManager, nullptr);

  const float size = 128.0f;
  if (texture != nullptr) {
    const SpriteRect fit = SpriteFitRect(*texture, ImGui::GetCursorScreenPos().x,
                                         ImGui::GetCursorScreenPos().y, size, size);
    ImGui::Dummy(ImVec2(size, size));
    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(static_cast<intptr_t>(texture->id)),
        ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height));
  } else {
    ImGui::Dummy(ImVec2(size, size));
    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                        IM_COL32(120, 120, 120, 255), 0.0f, 0, 2.0f);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(ImGui::GetItemRectMin().x + 6.0f, ImGui::GetItemRectMin().y + 6.0f),
        IM_COL32(200, 200, 200, 255), "no sprite");
  }

  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextDisabled("%s", convention.empty() ? "(no convention id)"
                                               : convention.c_str());
  // The fact that decides what Regenerate will do, stated before the click rather than
  // returned afterwards as an error from the queue.
  const SpriteDSLDocument document = FindSpriteDSLDocument(convention);
  if (!document.found) {
    ImGui::TextDisabled("no .sprite document yet (Regenerate asks for one)");
  } else if (document.inArtPack) {
    ImGui::TextDisabled("document: %s (hand-authored, never overwritten)",
                        document.path.c_str());
  } else {
    ImGui::TextDisabled("document: %s", document.path.c_str());
  }
  const TileArtQueue &queue = game->TileArtQueueState();
  const std::string unavailable = queue.UnavailableReason();
  if (unavailable.empty()) {
    ImGui::TextDisabled("art generation on (%d/%d requests)", queue.RequestsUsed(),
                        queue.GetConfig().maxRequestsPerSession);
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "art generation off");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", unavailable.c_str());
  }

  // Regenerate goes through the same entry point the designer uses: a DSL-source job
  // recompiles the stored document (free), anything else calls the image model for a new
  // one. One button because the pipeline is one pipeline; the cost is the setting's.
  ImGui::BeginDisabled(!unavailable.empty());
  if (ImGui::Button("Regenerate sprite")) {
    // The current image is copied aside first, so "regenerate" is never destructive:
    // whatever was on screen a moment ago is still in the list below.
    ArchiveActiveSprite(convention);
    if (game->designerWindow) {
      // Through the Designer, always: it is the window that owns the document turn. With
      // a document present this just queues the compile; without one it asks the model to
      // write a document, which is the only way "regenerate" can mean anything for an
      // asset that has never had one (the DSL pipeline compiles documents, it cannot
      // invent them). Opening the window is not decoration: its Draw is what pumps the
      // reply, so leaving it closed would queue a request that never lands.
      game->designerWindowEntity.set<ActiveWindow>({game->designerWindow});
      game->designerWindow->AskForSpriteArt(entryId, SpriteKindOf(*kind));
      status = "asked the Designer for this sprite -- it writes the document if there is "
               "none, then compiles it; the job appears in Art requests";
      statusIsError = false;
    } else {
      std::string reason;
      if (game->RecompileSpriteArt(SpriteKindOf(*kind), entryId, &reason)) {
        status = "sprite generation queued; it lands when the queue finishes";
        statusIsError = false;
      } else {
        status = "could not queue a sprite: " + reason;
        statusIsError = true;
      }
    }
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Re-run this asset's art. With art.generation.source = \"dsl\" that recompiles "
        "the stored .sprite document and costs nothing; otherwise it asks the image "
        "model for a new one.");
  }
  ImGui::EndGroup();

  // The archive. One grid, shared with the settings window's actor-sprite picker:
  // which picture is right is a question about the picture, not about the index.
  const std::vector<std::filesystem::path> variants = SpriteVariants(convention);
  ImGui::Separator();
  const std::string markerPath = GeneratedSpriteMarkerPath(convention);
  const bool generatedChosen = PrefersGeneratedSprite(convention);
  const std::string chosen = ChosenVariantName(markerPath);
  const std::string packSprite = ArtPackSpritePath(convention);

  // The original artwork first: every generated sprite is an alternative to it, and
  // choosing it again is how you go back.
  struct Candidate {
    std::string label;
    std::string path;
    bool selected = false;
    // The art pack's files are hand-authored and are not this editor's to delete.
    bool deletable = false;
    std::function<void()> activate;
  };
  std::vector<Candidate> candidates;
  if (!packSprite.empty()) {
    candidates.push_back({"original", packSprite, !generatedChosen, false,
                          [this, convention] { UseOriginalSprite(convention); }});
  }
  for (const std::filesystem::path &variant : variants) {
    const std::string name = variant.filename().string();
    candidates.push_back({name, variant.string(), generatedChosen && chosen == name,
                          true,
                          [this, convention, variant] {
                            ActivateVariant(convention, variant);
                          }});
  }

  std::vector<SpritePicker::Item> items;
  items.reserve(candidates.size());
  for (const Candidate &candidate : candidates) {
    SpritePicker::Item item;
    item.label = candidate.label;
    item.description = candidate.path;
    item.texture = ResolveSpriteTexture(candidate.path, std::string(),
                                        game->resourceManager, nullptr);
    item.selected = candidate.selected;
    items.push_back(std::move(item));
  }

  SpritePicker::Options options;
  options.cell = 52.0f;
  options.rows = 3;
  options.childId = "##sprite-variants";
  // A cell is click-to-use; anything destructive lives in its context menu. Only
  // archived variants are deletable: the pack's original is hand-authored, and the
  // picture in use is shown by the preview above rather than as a cell of its own.
  options.contextMenu = [&](int index) {
    const Candidate &candidate = candidates[index];
    if (!candidate.selected && ImGui::MenuItem("Use")) {
      candidate.activate();
    }
    if (candidate.deletable && ImGui::MenuItem("Delete")) {
      DeleteVariant(convention, std::filesystem::path(candidate.path));
    }
    if (ImGui::MenuItem("Copy path")) {
      ImGui::SetClipboardText(candidate.path.c_str());
    }
  };
  const int clicked = SpritePicker::Draw(items, options);
  if (clicked >= 0) {
    candidates[clicked].activate();
  }
  if (variants.empty()) {
    ImGui::TextDisabled(
        "No archived sprites yet. Regenerating copies the current one here first.");
  }
}

void ContentEditorWindow::DrawItemPreviewPane() {
  if (entryId.empty() || game == nullptr) return;
  const ItemDef *item = game->itemRegistry.Get(entryId);
  if (item == nullptr) {
    ImGui::TextDisabled(
        "This item is not in the live registry yet -- save it to see the preview.");
    return;
  }
  ImGui::Separator();
  if (item->armor > 0.0f) {
    ImGui::Text("%s  |  slot %s  |  tier %.0f  |  armour %.0f", item->id.c_str(),
                item->slot.c_str(), static_cast<double>(item->tier),
                static_cast<double>(item->armor));
  } else {
    ImGui::Text("%s  |  slot %s  |  tier %.0f", item->id.c_str(),
                item->slot.c_str(), static_cast<double>(item->tier));
  }
  if (item->damage > 0.0f) {
    ImGui::TextDisabled("damage %.0f | swing %.2fs | durability %d",
                        static_cast<double>(item->damage),
                        static_cast<double>(item->swingTime), item->durability);
  }
  if (!item->tags.empty()) {
    std::string tags;
    for (const std::string &tag : item->tags) {
      if (!tags.empty()) tags += ", ";
      tags += tag;
    }
    ImGui::TextDisabled("tags: %s", tags.c_str());
  }

  if (item->wornSprite.empty()) {
    ImGui::TextDisabled(
        "No worn art: this item has no layer on a rig, so there is nothing to draw "
        "over a body.");
    ImGui::Separator();
    return;
  }

  // Regeneration lands asynchronously. Forget the memo whenever the file's write time
  // changes, or the plate keeps drawing the old image (and a remembered "missing" mark
  // outlives the art entirely).
  {
    std::string resolved;
    if (SpriteResolves(std::string(), item->wornSprite, &resolved) &&
        !resolved.empty()) {
      std::error_code ec;
      const auto stamp = std::filesystem::last_write_time(resolved, ec);
      if (!ec) {
        const auto found = itemArtStamps.find(item->wornSprite);
        if (found == itemArtStamps.end() || found->second != stamp) {
          itemArtStamps[item->wornSprite] = stamp;
          game->ForgetSpriteMemo(std::string(), item->wornSprite);
        }
      }
    }
  }

  // The worn layer on its own. The rig composition below answers "does it fit the
  // body"; this answers "is the layer itself right", which is the thing to judge before
  // regenerating it.
  ImGui::Separator();
  const float plateSize = 128.0f;
  const ImVec2 plate = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(plateSize, plateSize));
  ImDrawList *plateDraw = ImGui::GetWindowDrawList();
  plateDraw->AddRectFilled(plate, ImVec2(plate.x + plateSize, plate.y + plateSize),
                           IM_COL32(26, 26, 30, 255));
  if (const Texture2D *texture =
          RigRender::ResolveRigSprite(item->wornSprite, game->resourceManager)) {
    SetTextureFilter(*texture, TEXTURE_FILTER_POINT);
    const SpriteRect fit =
        SpriteFitRect(*texture, plate.x, plate.y, plateSize, plateSize);
    if (fit.width > 0.0f && fit.height > 0.0f) {
      plateDraw->AddImage(
          static_cast<ImTextureID>(static_cast<intptr_t>(texture->id)),
          ImVec2(fit.x, fit.y), ImVec2(fit.x + fit.width, fit.y + fit.height));
    }
  } else {
    plateDraw->AddText(ImVec2(plate.x + 8.0f, plate.y + 8.0f),
                       IM_COL32(200, 200, 200, 255), "no art");
  }
  plateDraw->AddRect(plate, ImVec2(plate.x + plateSize, plate.y + plateSize),
                     IM_COL32(90, 90, 96, 255));
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextDisabled("worn sprite: %s", item->wornSprite.c_str());
  const SpriteDSLDocument document = FindSpriteDSLDocument(item->wornSprite);
  if (!document.found) {
    ImGui::TextDisabled("no .sprite document yet (Regenerate asks for one)");
  } else if (document.inArtPack) {
    ImGui::TextDisabled("document: %s (hand-authored, never overwritten)",
                        document.path.c_str());
  } else {
    ImGui::TextDisabled("document: %s", document.path.c_str());
  }
  const TileArtQueue &queue = game->TileArtQueueState();
  const std::string unavailable = queue.UnavailableReason();
  if (unavailable.empty()) {
    ImGui::TextDisabled("art generation on (%d/%d requests)", queue.RequestsUsed(),
                        queue.GetConfig().maxRequestsPerSession);
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "art generation off");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", unavailable.c_str());
  }

  // The same path a sprite's Regenerate takes: copy the current image aside, then ask the
  // Designer (which recompiles a stored document for free under the DSL source, or asks
  // the image model when there is none).
  ImGui::BeginDisabled(!unavailable.empty());
  if (ImGui::Button("Regenerate worn art")) {
    ArchiveActiveSprite(item->wornSprite);
    if (game->designerWindow) {
      game->designerWindowEntity.set<ActiveWindow>({game->designerWindow});
      game->designerWindow->AskForSpriteArt(item->wornSprite, SpriteKinds::Kind::Actor);
      itemArtMessage =
          "asked the Designer for the worn art -- it writes the document if there is "
          "none, then compiles it; the job appears in Art requests";
      itemArtIsError = false;
    } else {
      std::string reason;
      if (game->RecompileSpriteArt(SpriteKinds::Kind::Actor, item->wornSprite,
                                   &reason)) {
        itemArtMessage = "worn art generation queued; it lands when the queue finishes";
        itemArtIsError = false;
      } else {
        itemArtMessage = "could not queue the worn art: " + reason;
        itemArtIsError = true;
      }
    }
    game->ForgetSpriteMemo(std::string(), item->wornSprite);
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Re-run this item's worn layer. With art.generation.source = \"dsl\" that "
        "recompiles the stored .sprite document and costs nothing; otherwise it asks "
        "the image model for a new one. The current image is kept as a variant.");
  }
  if (!itemArtMessage.empty()) {
    if (itemArtIsError) {
      ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.5f, 1.0f), "%s",
                         itemArtMessage.c_str());
    } else {
      ImGui::TextDisabled("%s", itemArtMessage.c_str());
    }
  }
  ImGui::EndGroup();
  ImGui::Separator();

  const auto rigs = game->rigRegistry.GetAll();
  if (rigs.empty()) {
    ImGui::TextDisabled("No rig is loaded, so there is no body to show the item on.");
    ImGui::Separator();
    return;
  }
  int selected = 0;
  std::vector<const char *> names;
  for (size_t i = 0; i < rigs.size(); ++i) {
    names.push_back(rigs[i]->name.c_str());
    if (rigs[i]->id == itemPreviewRig) selected = static_cast<int>(i);
  }
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::Combo("##item_preview_rig", &selected, names.data(),
                   static_cast<int>(names.size()))) {
    itemPreviewRig = rigs[static_cast<size_t>(selected)]->id;
  }
  const ActorRig *rig = game->rigRegistry.Get(itemPreviewRig);
  if (rig != nullptr) {
    std::vector<RigRender::WornLayer> worn;
    RigRender::WornLayer layer;
    layer.label = item->id;
    layer.texture =
        RigRender::ResolveRigSprite(item->wornSprite, game->resourceManager);
    layer.z = item->wornZ;
    layer.hides = item->wornHides;
    worn.push_back(std::move(layer));
    RigRender::Draw(*rig, game->resourceManager, worn, 200.0f);
    if (!item->wornHides.empty()) {
      std::string hides;
      for (const std::string &part : item->wornHides) {
        if (!hides.empty()) hides += ", ";
        hides += part;
      }
      ImGui::TextDisabled("hides: %s", hides.c_str());
    }
  }
  ImGui::Separator();
}

void ContentEditorWindow::DrawRigPreviewPane() {
  if (entryId.empty() || game == nullptr) return;
  const ActorRig *rig = game->rigRegistry.Get(entryId);
  if (rig == nullptr) {
    ImGui::TextDisabled(
        "This rig is not in the live registry yet -- save it to see the preview.");
    return;
  }

  // A full outfit on a first look: the point of the pane is to show what layering does,
  // and an empty one would show only that the parts compose.
  if (rigPreviewEntry != entryId) {
    rigPreviewEntry = entryId;
    rigPreviewWorn.clear();
    for (const ItemDef *item : game->itemRegistry.GetAll()) {
      if (!item->wornSprite.empty()) rigPreviewWorn.insert(item->id);
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled(
      "rig %s  |  %dx%d canvas  |  %zu part(s)  |  %zu worn item(s) available",
      rig->id.c_str(), rig->width, rig->height, rig->parts.size(),
      game->itemRegistry.GetAll().size());

  std::vector<RigRender::WornLayer> worn;
  int column = 0;
  for (const ItemDef *item : game->itemRegistry.GetAll()) {
    if (item->wornSprite.empty()) continue;
    bool on = rigPreviewWorn.count(item->id) > 0;
    if (ImGui::Checkbox(item->id.c_str(), &on)) {
      if (on) {
        rigPreviewWorn.insert(item->id);
      } else {
        rigPreviewWorn.erase(item->id);
      }
    }
    if (++column % 3 != 0) ImGui::SameLine();
    if (on) {
      RigRender::WornLayer layer;
      layer.label = item->id;
      layer.texture =
          RigRender::ResolveRigSprite(item->wornSprite, game->resourceManager);
      layer.z = item->wornZ;
      layer.hides = item->wornHides;
      worn.push_back(std::move(layer));
    }
  }
  ImGui::NewLine();

  RigRender::Draw(*rig, game->resourceManager, worn, 224.0f);
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextDisabled("Draw order (back to front):");
  std::vector<std::string> hidden;
  int missingArt = 0;
  for (const RigRender::Layer &layer :
       RigRender::Order(*rig, game->resourceManager, worn, &hidden)) {
    if (layer.texture == nullptr) ++missingArt;
    ImGui::TextDisabled("%4d  %s%s%s", layer.z, layer.label.c_str(),
                        layer.worn ? "  (worn)" : "",
                        layer.texture == nullptr ? "  -- MISSING ART" : "");
  }
  for (const std::string &part : hidden) {
    ImGui::TextDisabled("hidden: %s", part.c_str());
  }
  if (missingArt > 0) {
    ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.5f, 1.0f),
                       "%d layer(s) have no sprite file.", missingArt);
    ImGui::TextDisabled(
        "Looked for the pack/generated file first, then data/sprites/<sprite>.png.");
  }
  ImGui::EndGroup();
  ImGui::Separator();
}

void ContentEditorWindow::DrawDocumentPane() {
  if (entryId.empty()) {
    ImGui::TextWrapped(
        "Pick an entry on the left. The document is shown as a form generated from its "
        "own JSON, so a kind added to the registry table is editable without a new "
        "window.");
    return;
  }

  const ContentKinds::Kind *kind = Kind();
  if (kind == nullptr) return;

  ImGui::TextDisabled("%s / %s", kind->id.c_str(), entryId.c_str());
  ImGui::SameLine();
  if (dirty) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "(edited, not saved)");
  }
  ImGui::Separator();

  DrawSpritePane();
  if (kind->id == "rigs") {
    DrawRigPreviewPane();
  } else if (kind->id == "items") {
    DrawItemPreviewPane();
  }

  // The requests this editor started, and anything else in flight. A button that queues
  // work and then reports it once, in a status line the next click overwrites, is how a
  // user ends up asking "why is nothing happening".
  {
    const TileArtQueue &queue = game->TileArtQueueState();
    const bool anyRequests = !queue.Snapshot().empty();
    if (ImGui::CollapsingHeader(
            "Requests",
            anyRequests ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) {
      ArtRequestsPanel::Draw(queue, entryId);
    }
  }
  ImGui::Separator();

  ImGui::BeginChild("content_document", ImVec2(0, -34.0f), true);
  // One form for the whole document: the fields are whatever the file has, and the
  // registry's own validator is what decides whether the result is usable.
  if (DrawJsonValue(document, std::string(), false)) {
    dirty = true;
  }
  ImGui::EndChild();

  ImGui::BeginDisabled(!dirty);
  if (ImGui::Button("Save")) Save();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Revert")) {
    ReloadDocument();
    status = "reverted to the live definition";
    statusIsError = false;
  }

  if (!status.empty()) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, statusIsError ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f)
                                                       : ImVec4(0.5f, 1.0f, 0.6f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
}

void ContentEditorWindow::Draw() {
  bool isOpen = true;
  ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
  ImGui::Begin("Content Editor", &isOpen, ImGuiWindowFlags_None);
  if (!isOpen) {
    if (game != nullptr) game->contentEditorWindowEntity.remove<ActiveWindow>();
    ImGui::End();
    return;
  }

  if (game == nullptr) {
    ImGui::TextWrapped("The window has no game to read from.");
    ImGui::End();
    return;
  }

  // The kind selector. The list of kinds is the registry table, so a new registry appears
  // here without touching this window.
  ImGui::TextDisabled("Kind:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160.0f);
  if (ImGui::BeginCombo("##contentKind", Kind() != nullptr ? Kind()->label.c_str() : "")) {
    for (const ContentKinds::Kind &candidate : ContentKinds::All()) {
      const bool selected = candidate.id == kindId;
      if (ImGui::Selectable(candidate.label.c_str(), selected)) {
        kindId = candidate.id;
        entryId.clear();
        filterBuffer[0] = '\0';
        status.clear();
        statusIsError = false;
        ReloadDocument();
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("| %s", Settings().content.editor.c_str());
  ImGui::Separator();

  // Open on a real document rather than an empty pane: the list is the table's, so the
  // first entry is a legitimate default and a person who opened the window wants to see
  // something editable.
  if (entryId.empty()) {
    const ContentKinds::Kind *kind = Kind();
    if (kind != nullptr) {
      const std::vector<ContentKinds::Entry> entries = kind->list(World_());
      if (!entries.empty()) {
        entryId = entries.front().id;
        ReloadDocument();
      }
    }
  }

  const float available = ImGui::GetContentRegionAvail().x;
  const float listWidth = std::clamp(available * 0.3f, 220.0f, 360.0f);
  ImGui::BeginChild("content_list_pane", ImVec2(listWidth, 0), false);
  DrawKindAndList();
  ImGui::EndChild();

  ImGui::SameLine(0.0f, 8.0f);
  ImGui::BeginChild("content_document_pane", ImVec2(0, 0), false);
  DrawDocumentPane();
  ImGui::EndChild();

  ImGui::End();
}
