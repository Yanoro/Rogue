#include "BlueprintRegistry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

#include "BlueprintExpansion.hpp"
#include "DebugLog.h"
#include "ObjectFactory.h"
#include "TileRegistry.h"

namespace {

bool ParsePair(const nlohmann::json& j, int& x, int& y) {
  if (!j.is_array() || j.size() < 2) return false;
  x = j[0];
  y = j[1];
  return true;
}

// A shape is an object with exactly one primitive key (or "of" for the set
// operations). Recursive, because union/intersect/difference nest.
bool ParseShape(const nlohmann::json& j, BlueprintShape& out, std::string& error) {
  if (!j.is_object()) {
    error = "shape must be an object";
    return false;
  }

  if (j.contains("region")) {
    out.kind = "region";
    out.ref = j["region"].get<std::string>();
    return true;
  }

  for (const char* op : {"union", "intersect", "difference"}) {
    if (!j.contains(op)) continue;
    out.kind = op;
    const auto& list = j[op];
    if (!list.is_array() || list.empty()) {
      error = std::string("'") + op + "' must be a non-empty array of shapes";
      return false;
    }
    for (const auto& entry : list) {
      BlueprintShape child;
      if (!ParseShape(entry, child, error)) return false;
      out.of.push_back(std::move(child));
    }
    return true;
  }

  static const char* kPrimitives[] = {
      "point",          "line",           "rect_fill",      "rect_outline",
      "disk_fill",      "disk_outline",   "polygon_fill",   "polygon_outline"};

  const char* found = nullptr;
  for (const char* primitive : kPrimitives) {
    if (j.contains(primitive)) {
      if (found != nullptr) {
        error = "shape declares more than one primitive ('" + std::string(found) +
                "' and '" + primitive + "')";
        return false;
      }
      found = primitive;
    }
  }

  if (found == nullptr) {
    error =
        "shape has no primitive key. Use point, line, rect_fill, rect_outline, "
        "disk_fill, disk_outline, polygon_fill, polygon_outline, region, union, "
        "intersect or difference";
    return false;
  }

  out.kind = found;
  const auto& body = j[found];
  if (!body.is_object()) {
    error = std::string("'") + found + "' must be an object";
    return false;
  }

  if (body.contains("at")) {
    if (!ParsePair(body["at"], out.atX, out.atY)) {
      error = "'at' must be [x, y]";
      return false;
    }
  } else if (found != std::string("polygon_fill") &&
             found != std::string("polygon_outline")) {
    // A polygon is the one primitive that does not need a single anchor.
    error = std::string("'") + found + "' needs 'at'";
    return false;
  }

  if (body.contains("to") && !ParsePair(body["to"], out.toX, out.toY)) {
    error = "'to' must be [x, y]";
    return false;
  }
  if (body.contains("width")) out.width = body["width"].get<int>();
  if (body.contains("height")) out.height = body["height"].get<int>();
  if (body.contains("radius")) out.radius = body["radius"].get<int>();

  if (body.contains("points")) {
    if (!body["points"].is_array()) {
      error = "'points' must be an array of [x, y]";
      return false;
    }
    for (const auto& entry : body["points"]) {
      int x = 0;
      int y = 0;
      if (!ParsePair(entry, x, y)) {
        error = "'points' must be an array of [x, y]";
        return false;
      }
      out.points.push_back({x, y});
    }
  }

  const std::string kind = out.kind;
  if ((kind == "rect_fill" || kind == "rect_outline") && out.width == 0 &&
      out.height == 0 && !body.contains("to")) {
    error = "'" + kind + "' needs 'to', or both 'width' and 'height'";
    return false;
  }
  if ((kind == "disk_fill" || kind == "disk_outline") && out.radius <= 0) {
    error = "'" + kind + "' needs a positive 'radius'";
    return false;
  }
  if ((kind == "polygon_fill" || kind == "polygon_outline") &&
      out.points.size() < 3) {
    error = "'" + kind + "' needs at least three points";
    return false;
  }

  return true;
}

bool ParseOp(const nlohmann::json& j, BlueprintOp& out, std::string& error) {
  if (!j.is_object()) {
    error = "op must be an object";
    return false;
  }

  if (j.contains("paint")) {
    out.kind = "paint";
    out.material = j["paint"].get<std::string>();
    return true;
  }
  if (j.contains("place")) {
    out.kind = "place";
    out.material = j["place"].get<std::string>();
    return true;
  }
  if (j.contains("carve")) {
    out.kind = "carve";
    return true;
  }
  if (j.contains("fill")) {
    out.kind = "fill";
    const auto& body = j["fill"];
    if (!body.is_object()) {
      error = "'fill' must be an object naming what to fill with";
      return false;
    }
    if (body.contains("paint")) {
      out.fillKind = "paint";
      out.fillMaterial = body["paint"].get<std::string>();
      return true;
    }
    if (body.contains("place")) {
      out.fillKind = "place";
      out.fillMaterial = body["place"].get<std::string>();
      return true;
    }
    error = "'fill' must name either 'paint' or 'place'";
    return false;
  }

  error = "op must be one of: paint, place, carve, fill";
  return false;
}

bool ParsePart(const nlohmann::json& j, BlueprintPart& out, std::string& error) {
  if (!j.is_object()) {
    error = "part must be an object";
    return false;
  }

  out.id = j.value("id", std::string());
  if (out.id.empty()) {
    error = "part has no 'id'";
    return false;
  }

  if (j.contains("region")) {
    if (j.contains("shape")) {
      error = "part '" + out.id +
              "' declares both 'region' and 'shape'; they are alternatives";
      return false;
    }
    // "region" names a region directly. The {"ref": "..."} object form is a
    // natural mistake -- it is what an early draft of docs/blueprint-design.md
    // showed -- and nlohmann turns it into an opaque "type must be a string"
    // exception, so it is caught by name here.
    if (!j["region"].is_string()) {
      error = "part '" + out.id + "': 'region' must be the region's name as a " +
              "string, for example \"region\": \"footprint\"" +
              (j["region"].is_object() && j["region"].contains("ref")
                   ? " -- not { \"ref\": ... }"
                   : "");
      return false;
    }
    out.shape.isRegionRef = true;
    out.shape.regionName = j["region"].get<std::string>();
  } else if (j.contains("shape")) {
    if (!ParseShape(j["shape"], out.shape.shape, error)) {
      error = "part '" + out.id + "': " + error;
      return false;
    }
  } else {
    error = "part '" + out.id + "' has neither 'shape' nor 'region'";
    return false;
  }

  if (!j.contains("op")) {
    error = "part '" + out.id + "' has no 'op'";
    return false;
  }
  if (!ParseOp(j["op"], out.op, error)) {
    error = "part '" + out.id + "': " + error;
    return false;
  }

  out.role = j.value("role", std::string());

  if (j.contains("level")) {
    if (j["level"].is_array() && j["level"].size() >= 2) {
      out.level = j["level"][0];
      out.levelTo = j["level"][1];
    } else {
      out.level = j["level"].get<int>();
      out.levelTo = out.level;
    }
  }

  return true;
}

// Serialises one shape back to the schema. The inverse of ParseShape, so a
// design written out and read back is the same design.
//
// Primitives carry their own name, so the fill state survives a round trip
// without a flag -- which is the whole reason the grammar puts "_fill" and
// "_outline" in the primitive name rather than in a field.
nlohmann::json WriteShape(const BlueprintShape &shape) {
  nlohmann::json out = nlohmann::json::object();

  if (shape.kind == "region") {
    out["region"] = shape.ref;
    return out;
  }

  const auto bounds = [&](nlohmann::json &body) {
    body["at"] = nlohmann::json::array({shape.atX, shape.atY});
    if (shape.width != 0 || shape.height != 0) {
      body["width"] = shape.width;
      body["height"] = shape.height;
    } else {
      body["to"] = nlohmann::json::array({shape.toX, shape.toY});
    }
    return body;
  };

  if (shape.kind == "union" || shape.kind == "intersect" ||
      shape.kind == "difference") {
    nlohmann::json list = nlohmann::json::array();
    for (const BlueprintShape &child : shape.of) {
      list.push_back(WriteShape(child));
    }
    out[shape.kind] = std::move(list);
    return out;
  }

  nlohmann::json body = nlohmann::json::object();
  if (shape.kind == "point" || shape.kind == "line") {
    body["at"] = nlohmann::json::array({shape.atX, shape.atY});
    if (shape.kind == "line") {
      body["to"] = nlohmann::json::array({shape.toX, shape.toY});
    }
  } else if (shape.kind == "rect_fill" || shape.kind == "rect_outline") {
    bounds(body);
  } else if (shape.kind == "disk_fill" || shape.kind == "disk_outline") {
    body["at"] = nlohmann::json::array({shape.atX, shape.atY});
    body["radius"] = shape.radius;
  } else if (shape.kind == "polygon_fill" || shape.kind == "polygon_outline") {
    nlohmann::json points = nlohmann::json::array();
    for (const auto &point : shape.points) {
      points.push_back(nlohmann::json::array({point.first, point.second}));
    }
    body["points"] = std::move(points);
  }
  out[shape.kind] = std::move(body);
  return out;
}

}  // namespace

nlohmann::json BlueprintRegistry::WriteBlueprint(const Blueprint &blueprint) {
  nlohmann::json out;
  out["id"] = blueprint.id;
  out["name"] = blueprint.name;
  if (!blueprint.owner.empty()) out["owner"] = blueprint.owner;
  if (!blueprint.purpose.empty()) out["purpose"] = blueprint.purpose;
  out["levels"] = blueprint.levels;

  // Regions are written first and referenced by name, because that is how a
  // design is meant to be read: the footprint and the wall ring are named once.
  nlohmann::json regions = nlohmann::json::object();
  // Sorted so a save is byte-stable, which makes a diff of a revised design
  // readable instead of a reshuffle.
  std::vector<std::string> regionNames;
  regionNames.reserve(blueprint.regions.size());
  for (const auto &entry : blueprint.regions) {
    regionNames.push_back(entry.first);
  }
  std::sort(regionNames.begin(), regionNames.end());
  for (const std::string &name : regionNames) {
    regions[name] = WriteShape(blueprint.regions.at(name));
  }
  if (!regionNames.empty()) {
    out["regions"] = std::move(regions);
  }

  nlohmann::json parts = nlohmann::json::array();
  for (const BlueprintPart &part : blueprint.parts) {
    nlohmann::json entry;
    entry["id"] = part.id;

    // A region reference is emitted by name; anything else is flattened inline.
    // Flattening is what keeps the output round-trippable: the parser stores a
    // region reference as a flag plus a name, so re-emitting it verbatim would be
    // correct, but inlining means a design that used a region inline the parser
    // could not have recovered still writes out valid, complete JSON.
    if (part.shape.isRegionRef) {
      entry["region"] = part.shape.regionName;
    } else {
      entry["shape"] = WriteShape(part.shape.shape);
    }

    nlohmann::json op;
    if (part.op.kind == "paint") {
      op["paint"] = part.op.material;
    } else if (part.op.kind == "place") {
      op["place"] = part.op.material;
    } else if (part.op.kind == "carve") {
      op["carve"] = true;
    } else if (part.op.kind == "fill") {
      op["fill"] = part.op.fillKind == "place"
                       ? nlohmann::json{{"place", part.op.fillMaterial}}
                       : nlohmann::json{{"paint", part.op.fillMaterial}};
    } else {
      op[part.op.kind] = true;
    }
    entry["op"] = std::move(op);

    if (!part.role.empty()) entry["role"] = part.role;
    if (part.level != 0 || part.levelTo != 0) {
      if (part.level == part.levelTo) {
        entry["level"] = part.level;
      } else {
        entry["level"] = nlohmann::json::array({part.level, part.levelTo});
      }
    }
    parts.push_back(std::move(entry));
  }
  out["parts"] = std::move(parts);
  return out;
}

bool BlueprintRegistry::ParseBlueprint(const nlohmann::json& json,
                                       const std::string& id,
                                       const std::string& sourcePath,
                                       Blueprint& out, std::string& error,
                                       std::vector<std::string>& warnings) {
  if (!json.is_object()) {
    error = "blueprint must be a JSON object";
    return false;
  }

  out = Blueprint{};
  out.id = id;
  out.name = json.value("name", id);
  out.owner = json.value("owner", std::string());
  out.purpose = json.value("purpose", std::string());
  out.levels = json.value("levels", 1);
  out.sourcePath = sourcePath;

  if (json.contains("regions")) {
    if (!json["regions"].is_object()) {
      error = "'regions' must be an object of named shapes";
      return false;
    }
    for (auto it = json["regions"].begin(); it != json["regions"].end(); ++it) {
      BlueprintShape shape;
      std::string shapeError;
      if (!ParseShape(it.value(), shape, shapeError)) {
        error = "region '" + it.key() + "': " + shapeError;
        return false;
      }
      out.regions[it.key()] = std::move(shape);
    }
  }

  if (!json.contains("parts")) {
    error = "blueprint has no 'parts' array";
    return false;
  }
  if (!json["parts"].is_array() || json["parts"].empty()) {
    error = "'parts' must be a non-empty array";
    return false;
  }

  std::set<std::string> seenIds;
  for (const auto& partJson : json["parts"]) {
    BlueprintPart part;
    std::string partError;
    if (!ParsePart(partJson, part, partError)) {
      error = partError;
      return false;
    }
    if (!seenIds.insert(part.id).second) {
      error = "duplicate part id '" + part.id + "'";
      return false;
    }
    out.parts.push_back(std::move(part));
  }

  BlueprintExpansion::ComputeBounds(out, out.minX, out.minY, out.maxX, out.maxY);

  return true;
}

namespace {

// Vocabulary check. A material that does not exist is a warning rather than a
// hard failure: it makes the design place fewer cells, and refusing the whole
// blueprint would leave the designer agent with no way to see what it got wrong.
// The warning names the part, which is what points at the fix.
//
// A free function taking the two registries rather than a member, because
// ParseBlueprint is static -- it has to be reachable from the designer agent's
// reply path later, where no registry instance is in scope.
void CheckVocabulary(const Blueprint &blueprint, const TileRegistry *tiles,
                     const ObjectFactory *objects,
                     std::vector<std::string> &warnings) {
  // Grouped by the material that is missing, with the parts that name it listed
  // afterwards. Reporting per part is what made this spam: one wrong material
  // used by six parts produced six near-identical lines. One line per distinct
  // problem, naming who is affected, is both shorter and more useful -- it says
  // the material once, which is the thing that needs fixing.
  //
  // Keyed by "kind|material" so a tile and an object template of the same name
  // stay separate problems.
  struct Missing {
    std::string material;
    std::string kind;
    std::vector<std::string> partIds;
  };
  std::map<std::string, Missing> missing;

  const auto check = [&](const std::string &partId, const std::string &kind,
                         const std::string &material) {
    if (material.empty()) return;
    if (kind == "paint") {
      if (tiles != nullptr && tiles->Get(material) == nullptr) {
        Missing &entry = missing[kind + "|" + material];
        entry.material = material;
        entry.kind = kind;
        entry.partIds.push_back(partId);
      }
    } else if (kind == "place") {
      if (objects != nullptr && objects->GetTemplates().count(material) == 0) {
        Missing &entry = missing[kind + "|" + material];
        entry.material = material;
        entry.kind = kind;
        entry.partIds.push_back(partId);
      }
    }
  };

  for (const BlueprintPart &part : blueprint.parts) {
    if (part.op.kind == "fill") {
      check(part.id, part.op.fillKind, part.op.fillMaterial);
    } else {
      check(part.id, part.op.kind, part.op.material);
    }
  }

  for (const auto &entry : missing) {
    const Missing &problem = entry.second;
    std::string parts;
    for (size_t i = 0; i < problem.partIds.size(); ++i) {
      if (i > 0) parts += (i + 1 == problem.partIds.size()) ? " and " : ", ";
      parts += problem.partIds[i];
    }
    if (problem.kind == "place") {
      warnings.push_back("places object '" + problem.material +
                         "', which is not an object template (part" +
                         (problem.partIds.size() > 1 ? "s " : " ") + parts + ")");
    } else {
      warnings.push_back("paints tile '" + problem.material +
                         "', which is not in the tile catalog (part" +
                         (problem.partIds.size() > 1 ? "s " : " ") + parts + ")");
    }
  }
}

}  // namespace

void BlueprintRegistry::Clear() { blueprints.clear(); }

void BlueprintRegistry::Reload() {
  if (directory.empty()) return;
  LoadBlueprints(directory);
}

BlueprintRegistry::SaveResult BlueprintRegistry::SaveBlueprint(
    const Blueprint &blueprint) {
  SaveResult result;
  if (blueprint.id.empty()) {
    result.error = "the design has no 'id', which is how it is stored and referenced";
    return result;
  }
  if (directory.empty()) {
    result.error =
        "no blueprint directory is loaded, so there is nowhere to save to";
    return result;
  }

  // Copied for the same reason as DeleteBlueprint's id: Reload() at the end
  // clears the map, so a caller passing a reference into this registry (a design
  // it just fetched, say, to re-save under a new name) would otherwise have the
  // object it passed freed mid-call. Serialised *before* the reload so the copy
  // is not merely a lifetime fix but the actual content written.
  const Blueprint design = blueprint;

  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    result.error = "could not create " + directory + ": " + ec.message();
    return result;
  }

  const std::filesystem::path path =
      std::filesystem::path(directory) / (design.id + ".json");
  result.overwrote = std::filesystem::exists(path);
  result.path = path.string();

  std::ofstream file(path);
  if (!file.is_open()) {
    result.error = "could not open " + path.string() + " for writing";
    return result;
  }
  file << WriteBlueprint(design).dump(2) << std::endl;
  file.close();
  if (!file) {
    result.error = "failed while writing " + path.string();
    return result;
  }

  // Reloaded so the new design is registered, savable and placeable straight
  // away. The Blueprints window rebuilds its list from the registry every frame,
  // so this is all that makes a generated design appear there.
  Reload();
  if (Get(design.id) == nullptr) {
    // The file was written but did not come back: a schema round-trip bug, or the
    // design was rejected on reload. Reported rather than left as a silent
    // success, because the window would otherwise show a design that cannot be
    // used.
    result.error = "the design was written to " + path.string() +
                   " but did not load back from it";
    return result;
  }

  result.ok = true;
  return result;
}

BlueprintRegistry::DeleteResult BlueprintRegistry::DeleteBlueprint(
    const std::string& id) {
  DeleteResult result;
  if (id.empty()) {
    result.error = "no design was named";
    return result;
  }

  // Copied immediately, and this is not a style choice. Callers naturally pass a
  // reference that lives inside this registry -- BlueprintWindow passes
  // `blueprint->id`, a key in the map, and DesignerChatWindow passes
  // `blueprint.id` from an entry it just looked up. Reload() at the end clears
  // that map, which frees the very string the parameter refers to, so every use
  // of `id` after it would read freed memory. That was a real heap-use-after-free
  // (ASan: DeleteBlueprint -> LoadBlueprints::clear -> Get(id)).
  //
  // Taking a copy at the boundary makes the function safe to call with a
  // reference into its own storage, which is the only way callers can reasonably
  // use it.
  const std::string designId = id;

  // Refuse an id that is not loaded rather than removing a file by name. The path
  // is built from the id, so this also stops anything that is not a known design
  // from being turned into a filesystem path.
  if (Get(designId) == nullptr) {
    result.error = "no blueprint named '" + designId + "' is loaded";
    return result;
  }
  if (directory.empty()) {
    result.error = "no blueprint directory is loaded, so there is nothing to delete";
    return result;
  }

  const std::filesystem::path path =
      std::filesystem::path(directory) / (designId + ".json");
  result.path = path.string();

  std::error_code ec;
  if (!std::filesystem::remove(path, ec)) {
    result.error = ec ? "could not delete " + path.string() + ": " + ec.message()
                      : "there is no file at " + path.string() +
                            " to delete (the design may be runtime-only)";
    return result;
  }

  // Reloaded rather than erased from the map, so what the registry holds always
  // matches what is on disk. If the reload still sees the design, something else
  // recreated it and the caller is told rather than shown a phantom entry.
  Reload();
  if (Get(designId) != nullptr) {
    result.error = "deleted " + path.string() + " but it is still loaded";
    return result;
  }

  result.ok = true;
  return result;
}

void BlueprintRegistry::LoadBlueprints(const std::string& directoryPath) {
  directory = directoryPath;
  // A reload replaces what was loaded rather than accumulating onto it. Without
  // this, every reload would double the warning history and leave designs behind
  // that were deleted from disk.
  blueprints.clear();

  if (!std::filesystem::exists(directoryPath)) {
    const std::string warnStr =
        "Warning: Blueprint directory not found: " + directoryPath +
        " (no buildings will be available)\n";
    if (debugLog) debugLog->LogWarning(warnStr);
    else std::cerr << warnStr;
    return;
  }

  // One warning per distinct message per load. The same content problem is
  // reported once per part and per design that hits it -- a missing tile is
  // named by every part that paints it -- so a single mistake could otherwise
  // bury the rest of the log under identical lines. The suffix records how many
  // were suppressed, so the dedupe cannot hide that it happened more than once.
  //
  // Scoped to this load, not to the process: a reload should report again, since
  // that is the point of reloading after a fix.
  std::map<std::string, int> warningCounts;
  auto warn = [this, &warningCounts](const std::string& message) {
    const int seen = ++warningCounts[message];
    if (seen > 1) {
      return;
    }
    if (debugLog) debugLog->LogWarning(message);
    else std::cerr << message << std::endl;
  };
  // Called once the load is done, so a suppressed duplicate can still be
  // accounted for without reprinting the message.
  auto warnRepeats = [this, &warningCounts]() {
    for (const auto& entry : warningCounts) {
      if (entry.second <= 1) continue;
      const std::string message =
          "Warning: the previous warning was reported " +
          std::to_string(entry.second - 1) + " more time(s): " + entry.first;
      if (debugLog) debugLog->LogWarning(message);
      else std::cerr << message << std::endl;
    }
  };

  for (const auto& entry : std::filesystem::directory_iterator(directoryPath)) {
    if (entry.path().extension() != ".json") continue;

    std::ifstream file(entry.path());
    if (!file.is_open()) continue;

    // Declared outside the try so the catch can name the design it failed on:
    // "which file?" is the first question a schema error raises.
    const std::string id = entry.path().stem().string();

    try {
      nlohmann::json j;
      file >> j;

      Blueprint blueprint;
      std::string error;
      std::vector<std::string> warnings;

      if (!ParseBlueprint(j, id, entry.path().string(), blueprint, error, warnings)) {
        // An ERROR, not a warning: a rejected design is absent from the
        // Blueprints window, which looks identical to never having authored it.
        const std::string message = "Error: blueprint '" + id + "' (" +
                                    entry.path().string() + ") rejected: " + error;
        if (debugLog) debugLog->LogError(message);
        else std::cerr << message << std::endl;
        continue;
      }

      CheckVocabulary(blueprint, tileRegistry, objectFactory, warnings);

      for (const std::string& message : warnings) {
        warn("Warning: blueprint '" + id + "': " + message);
      }

      // A design that expands to no cells at all is the one content problem that
      // has to be loud: it looks identical to a working design in the list and
      // would place an empty building.
      const BlueprintExpansion::Expansion expansion = BlueprintExpansion::Expand(blueprint);
      for (const std::string& message : expansion.errors) {
        warn("Warning: blueprint '" + id + "': " + message);
      }
      if (expansion.cells.empty()) {
        warn("Warning: blueprint '" + id +
             "' expands to no cells; skipping it so it cannot be placed as an "
             "empty building");
        continue;
      }

      blueprints[id] = std::move(blueprint);
    } catch (const std::exception& e) {
      // An ERROR, not a warning, and this matters. A design that throws here
      // simply never appears in the Blueprints window, which looks exactly like
      // "I only authored one blueprint" -- so a schema mistake was silent until
      // someone happened to have the Debug Log window open. Anything that
      // removes a blueprint from the list has to be loud enough to find.
      //
      // An nlohmann type_error carries only the type it wanted, never which key
      // it was reading, which is useless for fixing a file. So the message says
      // what it usually means.
      const std::string detail = e.what();
      std::string hint;
      if (detail.find("type must be a string") != std::string::npos) {
        hint =
            " -- a field that must be a string holds an object or array. The "
            "usual cause is \"region\": { \"ref\": \"x\" }, which is wrong: the "
            "region is named directly, as \"region\": \"x\".";
      }
      const std::string message = "Error: blueprint '" + id + "' (" +
                                  entry.path().string() +
                                  ") could not be parsed and is unavailable: " +
                                  detail + hint;
      if (debugLog) debugLog->LogError(message);
      else std::cerr << message << std::endl;
    }
  }

  warnRepeats();
}

const Blueprint* BlueprintRegistry::Get(const std::string& id) const {
  const auto it = blueprints.find(id);
  return it == blueprints.end() ? nullptr : &it->second;
}

std::vector<std::string> BlueprintRegistry::GetIds() const {
  std::vector<std::string> ids;
  ids.reserve(blueprints.size());
  for (const auto& pair : blueprints) {
    ids.push_back(pair.first);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<const Blueprint*> BlueprintRegistry::GetAll() const {
  std::vector<const Blueprint*> all;
  all.reserve(blueprints.size());
  for (const auto& id : GetIds()) {
    all.push_back(&blueprints.at(id));
  }
  return all;
}
