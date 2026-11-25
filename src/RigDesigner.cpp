#include "RigDesigner.hpp"

#include "DesignerSession.h"

#include <nlohmann/json.hpp>

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

}  // namespace

namespace RigDesigner {

std::string SlugFor(const std::string &name) { return Slugify(name); }

std::string FrameContract() { return FrameContract(RigGeometry{}); }

std::string FrameContract(const RigGeometry &geometry) {
  std::ostringstream out;
  out << "This is one layer of a " << geometry.width << "x" << geometry.height
      << " humanoid rig drawn from the front, in a " << geometry.width << "x"
      << geometry.height << " transparent frame. Draw ONLY the layer being asked for "
      << "and leave the rest of the frame empty; the engine draws the parts on top of "
      << "each other. The frame the other layers are drawn against:\n";
  const auto line = [&out](const char *name, const RigRect &rect, const char *what) {
    out << "  " << name;
    const std::string label(name);
    for (size_t i = label.size(); i < 10; ++i) out << ' ';
    out << "(" << rect.x << "," << rect.y << ")-(" << rect.x + rect.w << ","
        << rect.y + rect.h << ")   " << what << "\n";
  };
  line("head", geometry.head, "face");
  line("hair", geometry.hair, "over the head, hidden by a helmet");
  line("body", geometry.body, "tunic, belt");
  line("arm_left", geometry.armLeft, "sleeve and hand");
  line("arm_right", geometry.armRight, "sleeve and hand; this hand holds weapons");
  line("legs", geometry.legs, "trousers and boots");
  out << "The right hand holds weapons at (" << geometry.handX << ","
      << geometry.handY << "). The soles of the feet sit at (" << geometry.footX
      << "," << geometry.footY << "). Keep the silhouette inside those rectangles so "
      << "equipment drawn over the frame lines up.\n";
  return out.str();
}

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
    return fail("The reply was empty. Send the rig as one ```json block, as "
                "data/rigs/RIGDESIGNER.md requires.");
  }

  nlohmann::json reply;
  try {
    reply = nlohmann::json::parse(block);
  } catch (const std::exception &parseError) {
    if (fromBlock && cleaned.find("```", cleaned.find("```json") + 7) ==
                         std::string::npos) {
      return fail("The reply looks truncated -- the JSON block was never closed. Send "
                  "the six briefs more briefly.");
    }
    return fail(std::string("The rig is not valid JSON: ") + parseError.what());
  }
  if (!reply.is_object()) {
    return fail("The json block must be a JSON object describing one rig.");
  }

  Draft draft;
  draft.name = reply.value("name", std::string());
  draft.slug = Slugify(draft.name);
  if (draft.slug.empty()) {
    return fail("The reply has no \"name\"; a rig needs a name to be saved by and to "
                "name its part sprites after.");
  }
  draft.description = reply.value("description", std::string());
  draft.designerNotes = reply.value("designerNotes", std::string());
  result.notes = draft.designerNotes;
  if (draft.designerNotes.empty()) {
    return fail("The reply has no \"designerNotes\"; say what the body looks like and "
                "why.");
  }
  draft.width = reply.value("width", kReferenceSide);
  draft.height = reply.value("height", kReferenceSide);
  if (draft.width != kReferenceSide || draft.height != kReferenceSide) {
    return fail("A rig is " + std::to_string(kReferenceSide) + "x" +
                std::to_string(kReferenceSide) + "; the worn equipment art is drawn "
                "against that frame, so another size cannot fit it.");
  }

  if (!reply.contains("parts") || !reply["parts"].is_object()) {
    return fail("The reply needs a \"parts\" object: one short brief per canonical "
                "part.");
  }

  // Every canonical part is required. The pane and the worn items address parts by id,
  // so a missing hair or arm is not a style choice -- it is a rig that equipment cannot
  // attach to and a body with a hole in it.
  for (const CanonicalPart &canonical : kCanonicalParts) {
    const std::string id = canonical.id;
    if (!reply["parts"].contains(id)) {
      return fail("The parts object has no \"" + id + "\" brief. The canonical parts "
                  "are legs, body, arm_left, arm_right, head and hair.");
    }
    if (!reply["parts"][id].is_string()) {
      return fail("The \"" + id + "\" brief is not a string.");
    }
    Part part;
    part.id = id;
    part.brief = reply["parts"][id].get<std::string>();
    part.z = canonical.z;
    part.sprite = "rig_" + draft.slug + "_" + id;
    if (part.brief.empty()) {
      return fail("The \"" + id + "\" brief is empty; say what that layer looks like.");
    }
    draft.parts.push_back(std::move(part));
  }

  for (auto it = reply["parts"].begin(); it != reply["parts"].end(); ++it) {
    const bool known = std::any_of(
        std::begin(kCanonicalParts), std::end(kCanonicalParts),
        [&](const CanonicalPart &part) { return it.key() == part.id; });
    if (!known) {
      draft.warnings.push_back("part '" + it.key() +
                               "' is not in the humanoid frame; ignored");
    }
  }

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
    return "ERROR: the rig designer's instruction file '" + promptFile +
           "' could not be read.";
  }
  return prompt;
}

void CarryAnimations(Draft &to, const Draft &from) {
  to.clips = from.clips;
  for (Part &part : to.parts) {
    const auto found = std::find_if(
        from.parts.begin(), from.parts.end(),
        [&](const Part &candidate) { return candidate.id == part.id; });
    if (found != from.parts.end() && found->sprite == part.sprite) {
      part.clips = found->clips;
    }
  }
}

bool Save(const Draft &draft, const std::string &directory, std::string *savedPath,
          std::string *error) {
  if (savedPath != nullptr) savedPath->clear();
  if (draft.slug.empty()) {
    if (error != nullptr) *error = "The rig has no name to save under.";
    return false;
  }
  if (directory.empty()) {
    if (error != nullptr) *error = "No generated rigs directory is configured.";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    if (error != nullptr) *error = "Could not create " + directory + ": " + ec.message();
    return false;
  }

  nlohmann::json clips = nlohmann::json::array();
  for (const RigClip &clip : draft.clips) {
    nlohmann::json entry = {{"id", clip.id}, {"frames", clip.frames},
                            {"fps", clip.fps}, {"loop", clip.loop}};
    if (!clip.movement.empty()) entry["movement"] = clip.movement;
    if (!clip.brief.empty()) entry["brief"] = clip.brief;
    if (!clip.poses.empty()) entry["poses"] = clip.poses;
    if (!clip.variants.empty()) {
      nlohmann::json variants = nlohmann::json::array();
      for (const RigClipVariant &variant : clip.variants) {
        nlohmann::json variantEntry = {{"id", variant.id},
                                       {"name", variant.name},
                                       {"parts", variant.parts}};
        if (!variant.created.empty()) variantEntry["created"] = variant.created;
        variants.push_back(std::move(variantEntry));
      }
      entry["variants"] = std::move(variants);
      if (!clip.activeVariant.empty()) entry["activeVariant"] = clip.activeVariant;
    }
    clips.push_back(std::move(entry));
  }
  nlohmann::json parts = nlohmann::json::array();
  for (const Part &part : draft.parts) {
    nlohmann::json entry = {{"id", part.id}, {"sprite", part.sprite}, {"z", part.z}};
    if (!part.brief.empty()) entry["brief"] = part.brief;
    if (!part.clips.empty()) {
      nlohmann::json partClips = nlohmann::json::object();
      for (const RigPartClip &partClip : part.clips) {
        partClips[partClip.clip] = partClip.frames;
      }
      entry["clips"] = std::move(partClips);
    }
    parts.push_back(std::move(entry));
  }
  nlohmann::json document = nlohmann::json::object();
  document["name"] = draft.name;
  if (!draft.description.empty()) document["description"] = draft.description;
  document["width"] = draft.width;
  document["height"] = draft.height;
  if (!draft.style.empty()) document["style"] = draft.style;
  if (draft.geometry) document["geometry"] = RigGeometryToJson(*draft.geometry);
  if (!clips.empty()) document["clips"] = std::move(clips);
  document["parts"] = std::move(parts);

  const std::filesystem::path path =
      std::filesystem::path(directory) / (draft.slug + ".json");
  std::ofstream file(path);
  if (!file.is_open()) {
    if (error != nullptr) *error = "Could not write " + path.string() + ".";
    return false;
  }
  file << document.dump(2) << "\n";
  if (!file.good()) {
    if (error != nullptr) *error = "Failed while writing " + path.string() + ".";
    return false;
  }
  if (savedPath != nullptr) *savedPath = path.string();
  return true;
}

}  // namespace RigDesigner
