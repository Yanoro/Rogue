#include "SpriteReply.hpp"

#include <algorithm>

namespace SpriteReply {

namespace {

// A hard ceiling on a set, deliberately far above the prompt's "four to eight".
//
// The prompt asks for a small set; a model that sends forty is not producing something a
// person would place, and every entry costs an overlay file, a catalog entry and a
// generation cell. Refusing loudly at a generous bound is better than accepting it and
// finding out through the spend.
constexpr size_t kMaxSpritesPerReply = 24;

}  // namespace

Parsed Split(const nlohmann::json &json) {
  Parsed result;

  if (!json.is_object()) {
    result.error =
        "the json block must be a JSON object: a definition, or a set with a \"sprites\" "
        "array";
    return result;
  }

  const auto sprites = json.find("sprites");
  if (sprites == json.end()) {
    // The single case, and the older spelling. No envelope key at all.
    result.ok = true;
    result.isSet = false;
    result.definitions.push_back(json);
    return result;
  }

  result.isSet = true;
  result.setName = json.value("set", std::string());

  if (!sprites->is_array()) {
    result.error =
        "\"sprites\" must be an array of definitions. Send one definition per sprite, in "
        "the order you want them drawn";
    return result;
  }
  if (sprites->empty()) {
    result.error = "\"sprites\" is empty, so the reply defines nothing. A set needs at "
                   "least one definition, and the contract asks for four to eight";
    return result;
  }
  if (sprites->size() > kMaxSpritesPerReply) {
    result.error = "\"sprites\" has " + std::to_string(sprites->size()) +
                   " entries, which is more than this engine will accept in one reply "
                   "(the limit is " +
                   std::to_string(kMaxSpritesPerReply) +
                   "). Send a smaller set -- four to eight is the useful range";
    return result;
  }

  for (size_t index = 0; index < sprites->size(); ++index) {
    const nlohmann::json &entry = (*sprites)[index];
    if (!entry.is_object()) {
      // Naming the index matters: the retry is "resend with entry N fixed", and a message
      // that only says "one of these is wrong" makes the model guess.
      result.error = "entry " + std::to_string(index) +
                     " of \"sprites\" is not a JSON object. Every entry must be a complete "
                     "definition";
      return result;
    }
    result.definitions.push_back(entry);
  }

  result.ok = true;
  return result;
}

Parsed SplitText(const std::string &jsonText) {
  Parsed result;
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(jsonText);
  } catch (const std::exception &e) {
    result.error = std::string("the reply is not valid JSON: ") + e.what();
    return result;
  }
  return Split(parsed);
}

}  // namespace SpriteReply
