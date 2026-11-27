#include "EquipmentDesigner.hpp"

#include "DesignerSession.h"
#include "RigDesigner.hpp"
#include "TileDesigner.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>

namespace {

std::string Slugify(const std::string &text) {
  std::string out;
  bool lastUnderscore = false;
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0) {
      out.push_back(static_cast<char>(std::tolower(u)));
      lastUnderscore = false;
    } else if (!out.empty() && !lastUnderscore) {
      out.push_back('_');
      lastUnderscore = true;
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

bool IsSlot(const std::string &slot) {
  return std::any_of(std::begin(EquipmentDesigner::kSlots),
                     std::end(EquipmentDesigner::kSlots),
                     [&slot](const char *candidate) { return slot == candidate; });
}

bool IsRigPart(const std::string &id) {
  return std::any_of(
      std::begin(RigDesigner::kCanonicalParts),
      std::end(RigDesigner::kCanonicalParts),
      [&id](const RigDesigner::CanonicalPart &part) { return id == part.id; });
}

// Where a worn layer sits among the rig's worn layers. A deterministic default per slot
// rather than a number the model has to invent: the layering of a helmet over a cuirass is
// not a creative decision, and a model-chosen z would let a helmet draw behind the hair.
int DefaultWornZ(const std::string &slot) {
  if (slot == "feet") return 45;
  if (slot == "legs") return 50;
  if (slot == "torso") return 60;
  if (slot == "head") return 100;
  if (slot == "hand") return 110;
  return 80;
}

}  // namespace

namespace EquipmentDesigner {

std::string SlugFor(const std::string &name) { return Slugify(name); }

namespace {

// The hard ceiling on one reply. The prompt asks for two to five -- an outfit, not a
// catalogue -- and every accepted item costs a definition file and a worn-art generation.
// A generous bound refused loudly beats accepting forty and finding out through the spend.
constexpr size_t kMaxItemsPerReply = 12;

// Builds one draft from one item document. Shared by both reply shapes, because a set
// differs in how many definitions the reply carries and not in what an item is.
ApplyResult DraftFromJson(const nlohmann::json &reply) {
  ApplyResult result;
  auto fail = [&result](const std::string &reason) {
    result.ok = false;
    result.error = reason;
    return result;
  };

  Draft draft;
  draft.name = reply.value("name", std::string());
  draft.slug = Slugify(draft.name);
  if (draft.slug.empty()) {
    return fail("The reply has no \"name\"; an item needs a name to be saved by and to "
                "name its worn art after.");
  }
  draft.description = reply.value("description", std::string());
  draft.designerNotes = reply.value("designerNotes", std::string());
  result.notes = draft.designerNotes;
  if (draft.designerNotes.empty()) {
    return fail("The reply has no \"designerNotes\"; say what the item is and why it "
                "looks that way.");
  }

  draft.slot = reply.value("slot", std::string());
  if (!IsSlot(draft.slot)) {
    return fail("Slot '" + draft.slot +
                "' is not one a rig can wear. Use one of: head, torso, legs, feet, "
                "hand.");
  }

  draft.tier = reply.value("tier", 1.0f);
  if (draft.tier < 1.0f || draft.tier > 5.0f) {
    return fail("tier must be between 1 and 5.");
  }
  draft.armor = reply.value("armor", 0.0f);
  if (draft.armor < 0.0f) {
    return fail("armor cannot be negative.");
  }
  if (reply.contains("tags")) {
    if (!reply["tags"].is_array()) {
      return fail("\"tags\" must be an array of strings.");
    }
    for (const auto &tag : reply["tags"]) {
      if (!tag.is_string()) {
        return fail("A \"tags\" entry is not a string.");
      }
      draft.tags.push_back(tag.get<std::string>());
    }
  }

  draft.wornSprite = "worn_" + draft.slug;
  draft.wornZ = DefaultWornZ(draft.slot);
  if (!reply.contains("worn") || !reply["worn"].is_object()) {
    return fail("The reply needs a \"worn\" object: how the item looks on the body.");
  }
  const nlohmann::json &worn = reply["worn"];
  draft.wornBrief = worn.value("brief", std::string());
  if (draft.wornBrief.empty()) {
    return fail("The worn object has no \"brief\"; describe the item as it sits on the "
                "body.");
  }
  if (worn.contains("z")) {
    draft.wornZ = worn.value("z", draft.wornZ);
  }
  if (worn.contains("hides")) {
    if (!worn["hides"].is_array()) {
      return fail("\"worn.hides\" must be an array of rig part ids.");
    }
    for (const auto &hide : worn["hides"]) {
      if (!hide.is_string() || !IsRigPart(hide.get<std::string>())) {
        return fail("worn.hides names '" +
                    (hide.is_string() ? hide.get<std::string>() : std::string("?")) +
                    "', which is not a rig part. The parts are legs, body, arm_left, "
                    "arm_right, head, hair.");
      }
      draft.hides.push_back(hide.get<std::string>());
    }
  }

  // A hand item is placed on the hand by its grip, so the reply has to name the point.
  // Without it the grip path would invent a hold, which is exactly the drift the editor's
  // grip sliders exist to correct.
  if (draft.slot == "hand") {
    if (!worn.contains("grip") || !worn["grip"].is_object()) {
      return fail("A hand item needs \"worn.grip\": { \"x\": 24, \"y\": 22, "
                  "\"angle\": 0 } -- the pixel in the 32x32 canvas where the hand holds "
                  "it, and the item's rest angle in degrees.");
    }
    const nlohmann::json &grip = worn["grip"];
    draft.gripX = grip.value("x", -1);
    draft.gripY = grip.value("y", -1);
    draft.gripAngle = grip.value("angle", 0.0f);
    if (draft.gripX < 0 || draft.gripX > 31 || draft.gripY < 0 ||
        draft.gripY > 31) {
      return fail("worn.grip x and y must be inside the 32x32 frame (0..31).");
    }
    if (draft.gripAngle < -180.0f || draft.gripAngle > 180.0f) {
      return fail("worn.grip.angle must be between -180 and 180 degrees.");
    }
  }

  if (draft.slot == "hand") {
    draft.damage = reply.value("damage", 0.0f);
    draft.swingTime = reply.value("swingTime", 1.8f);
    draft.durability = reply.value("durability", 0);
    if (draft.damage < 0.0f) {
      return fail("damage cannot be negative.");
    }
    if (draft.swingTime <= 0.0f) {
      return fail("swingTime must be greater than 0.");
    }
    if (draft.durability < 0) {
      return fail("durability cannot be negative (0 means it never wears out).");
    }
    if (draft.damage <= 0.0f) {
      draft.warnings.push_back(
          "a hand item with damage 0 is not a weapon; it can still be a tool");
    }
  }

  nlohmann::json document = nlohmann::json::object();
  document["name"] = draft.name;
  document["slot"] = draft.slot;
  document["occupies"] = 1;
  document["tier"] = draft.tier;
  if (!draft.tags.empty()) document["tags"] = draft.tags;
  if (draft.armor > 0.0f) document["armor"] = draft.armor;
  if (draft.slot == "hand" && draft.damage > 0.0f) {
    document["damage"] = draft.damage;
    document["swingTime"] = draft.swingTime;
    if (draft.durability > 0) document["durability"] = draft.durability;
  }
  nlohmann::json wornDocument = {{"sprite", draft.wornSprite},
                                  {"brief", draft.wornBrief},
                                  {"z", draft.wornZ}};
  if (!draft.hides.empty()) wornDocument["hides"] = draft.hides;
  if (draft.slot == "hand") {
    wornDocument["grip"] = {{"x", draft.gripX},
                            {"y", draft.gripY},
                            {"angle", draft.gripAngle}};
  }
  document["worn"] = std::move(wornDocument);
  draft.document = std::move(document);

  result.ok = true;
  result.draft = std::move(draft);
  return result;
}

}  // namespace

SetApplyResult ApplySetReply(const std::string &text) {
  SetApplyResult result;

  const std::string cleaned = DesignerReply::StripReasoning(text);
  std::string block = DesignerReply::ExtractJsonBlock(cleaned);
  const bool fromBlock = !block.empty();
  if (!fromBlock) block = cleaned;  // a bare object is accepted, as for tiles
  if (block.find_first_not_of(" \t\r\n") == std::string::npos) {
    result.error = "The reply was empty. Send the item, or the set, as one ```json "
                   "block, as data/items/EQUIPMENTDESIGNER.md requires.";
    return result;
  }

  nlohmann::json reply;
  try {
    reply = nlohmann::json::parse(block);
  } catch (const std::exception &parseError) {
    if (fromBlock && cleaned.find("```", cleaned.find("```json") + 7) ==
                         std::string::npos) {
      result.error = "The reply looks truncated -- the JSON block was never closed. Send "
                     "the brief more briefly.";
      return result;
    }
    result.error = std::string("The reply is not valid JSON: ") + parseError.what();
    return result;
  }
  if (!reply.is_object()) {
    result.error = "The json block must be a JSON object: one item, or a set with an "
                   "\"items\" array.";
    return result;
  }

  const auto items = reply.find("items");
  result.isSet = items != reply.end();
  if (!result.isSet) {
    // The single shape, and the older spelling: no envelope key at all. Kept as a
    // one-entry result so the window has one path for both shapes.
    result.ok = true;
    result.items.push_back(DraftFromJson(reply));
  } else {
    // The name is cosmetic, and a non-string is a malformed envelope rather than a reason to
    // refuse every item: the set still parses and the pane shows the items.
    if (reply.contains("set") && reply["set"].is_string()) {
      result.setName = reply["set"].get<std::string>();
    }
    if (!items->is_array()) {
      result.error = "\"items\" must be an array of item definitions. Send one complete "
                     "item per entry, in the order the set should read.";
      return result;
    }
    if (items->empty()) {
      result.error = "\"items\" is empty, so the reply defines nothing. A set needs at "
                     "least one item, and the contract asks for two to five.";
      return result;
    }
    if (items->size() > kMaxItemsPerReply) {
      result.error = "\"items\" has " + std::to_string(items->size()) +
                     " entries, which is more than this engine will accept in one reply "
                     "(the limit is " +
                     std::to_string(kMaxItemsPerReply) +
                     "). Send a smaller set -- two to five is the useful range.";
      return result;
    }
    for (size_t index = 0; index < items->size(); ++index) {
      if (!(*items)[index].is_object()) {
        result.error = "entry " + std::to_string(index) +
                       " of \"items\" is not a JSON object. Every entry must be a "
                       "complete item definition.";
        return result;
      }
    }
    result.ok = true;
    for (const nlohmann::json &entry : *items) {
      result.items.push_back(DraftFromJson(entry));
    }
  }

  // The set's name becomes a tag on every item: that is what makes the pieces findable as
  // a set later, and it saves the model from repeating the name in each item's tags. A
  // single reply has no set name to add.
  const std::string setTag = Slugify(result.setName);

  std::map<std::string, size_t> slugOwner;
  for (size_t index = 0; index < result.items.size(); ++index) {
    ApplyResult &entry = result.items[index];
    if (!entry.ok) {
      // The index is the whole point of per-entry refusal: the retry can say "entry 3",
      // and a sentence that only says "one of these is wrong" makes the model guess.
      if (result.isSet) {
        entry.error = "entry " + std::to_string(index) + ": " + entry.error;
      }
      ++result.refused;
      continue;
    }
    if (!setTag.empty() &&
        std::find(entry.draft.tags.begin(), entry.draft.tags.end(), setTag) ==
            entry.draft.tags.end()) {
      entry.draft.tags.push_back(setTag);
      entry.draft.document["tags"] = entry.draft.tags;
    }
    // Two entries with the same name save to the same file, so the second would replace
    // the first. Reported on the entry now rather than at save time, when the pane has
    // already moved on.
    const auto [owner, inserted] = slugOwner.emplace(entry.draft.slug, index);
    (void)owner;
    if (!inserted) {
      entry.draft.warnings.push_back(
          "another item in this set is also named '" + entry.draft.name +
          "'; rename one before saving or it will overwrite the other");
    }
    ++result.accepted;
  }
  return result;
}

std::string BuildSystemPrompt(const std::string &promptFile, bool set) {
  std::string prompt;
  if (!promptFile.empty()) {
    std::ifstream file(promptFile);
    if (file.is_open()) {
      prompt.assign(std::istreambuf_iterator<char>(file),
                    std::istreambuf_iterator<char>());
    }
  }
  if (prompt.empty()) {
    return "ERROR: the equipment designer's instruction file '" + promptFile +
           "' could not be read.";
  }
  // The shape-fence convention is shared with the sprite documents -- the same
  // `<!--%%SINGLE%%-->` / `<!--%%SET%%-->` comment pairs -- even though the stripper lives
  // in TileDesigner. A second copy here would be a second thing that can disagree about
  // what a section is. An unfenced document passes through unchanged, so an older prompt
  // file still works and simply has no set instructions.
  return TileDesigner::SelectShape(
      std::move(prompt),
      set ? TileDesigner::PromptShape::Set : TileDesigner::PromptShape::One);
}

std::string WornArtRequirement(const Draft &draft) {
  std::string hides;
  for (const std::string &part : draft.hides) {
    if (!hides.empty()) hides += ", ";
    hides += part;
  }
  std::string text = RigDesigner::FrameContract();
  text += "\n\nThis layer is worn equipment: " + draft.name + " (" + draft.slot +
          "). " + draft.wornBrief + "\n";
  if (!hides.empty()) {
    text += "It hides the rig part(s): " + hides +
            ". Draw only the worn item; the parts it hides are drawn by the engine, "
            "not by this layer.\n";
  } else {
    text += "It hides no rig part; it is drawn in front of the body. Draw only the "
            "worn item, not the body underneath.\n";
  }
  if (draft.slot == "hand") {
    text += "The hand holds it at (" + std::to_string(draft.gripX) + "," +
            std::to_string(draft.gripY) + ") in the 32x32 frame; draw the weapon's grip " +
            "(the handle, where a hand would close) at that point, not lying flat. Its " +
            "rest angle is " + std::to_string(draft.gripAngle) + " degrees.\n";
  }
  return text;
}

bool Save(const Draft &draft, const std::string &directory, std::string *savedPath,
          std::string *error) {
  if (savedPath != nullptr) savedPath->clear();
  if (draft.slug.empty()) {
    if (error != nullptr) *error = "The item has no name to save under.";
    return false;
  }
  if (directory.empty()) {
    if (error != nullptr) *error = "No generated items directory is configured.";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    if (error != nullptr) *error = "Could not create " + directory + ": " + ec.message();
    return false;
  }
  const std::filesystem::path path =
      std::filesystem::path(directory) / (draft.slug + ".json");
  std::ofstream file(path);
  if (!file.is_open()) {
    if (error != nullptr) *error = "Could not write " + path.string() + ".";
    return false;
  }
  file << draft.document.dump(2) << "\n";
  if (!file.good()) {
    if (error != nullptr) *error = "Failed while writing " + path.string() + ".";
    return false;
  }
  if (savedPath != nullptr) *savedPath = path.string();
  return true;
}

}  // namespace EquipmentDesigner
