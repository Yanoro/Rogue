#include "EquipmentDesigner.hpp"

#include "DesignerSession.h"
#include "RigDesigner.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

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

ApplyResult ApplyReply(const std::string &text) {
  ApplyResult result;
  auto fail = [&result](const std::string &reason) {
    result.ok = false;
    result.error = reason;
    return result;
  };

  const std::string cleaned = DesignerReply::StripReasoning(text);
  std::string block = DesignerReply::ExtractJsonBlock(cleaned);
  const bool fromBlock = !block.empty();
  if (!fromBlock) block = cleaned;  // a bare object is accepted, as for tiles
  if (block.find_first_not_of(" \t\r\n") == std::string::npos) {
    return fail("The reply was empty. Send the item as one ```json block, as "
                "data/items/EQUIPMENTDESIGNER.md requires.");
  }

  nlohmann::json reply;
  try {
    reply = nlohmann::json::parse(block);
  } catch (const std::exception &parseError) {
    if (fromBlock && cleaned.find("```", cleaned.find("```json") + 7) ==
                         std::string::npos) {
      return fail("The reply looks truncated -- the JSON block was never closed. Send "
                  "the brief more briefly.");
    }
    return fail(std::string("The item is not valid JSON: ") + parseError.what());
  }
  if (!reply.is_object()) {
    return fail("The json block must be a JSON object describing one item.");
  }

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
  nlohmann::json wornDocument = {{"sprite", draft.wornSprite}, {"z", draft.wornZ}};
  if (!draft.hides.empty()) wornDocument["hides"] = draft.hides;
  document["worn"] = std::move(wornDocument);
  draft.document = std::move(document);

  result.ok = true;
  result.draft = std::move(draft);
  return result;
}

std::string BuildSystemPrompt(const std::string &promptFile) {
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
  return prompt;
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
    text += "It is held in the right hand at (22,12)-(25,23); draw it gripped there, "
            "not lying flat.\n";
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
