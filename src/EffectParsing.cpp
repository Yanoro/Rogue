#include "EffectParsing.h"

namespace {

// Unrecognised values are reported rather than silently defaulted: a typo in a
// data file would otherwise mean "the effect just never happens", which is
// exactly the kind of bug that is hard to see.
bool ParseOp(const std::string& text, Op& out) {
  if (text == "pct") {
    out = Op::Pct;
    return true;
  }
  if (text == "flat") {
    out = Op::Flat;
    return true;
  }
  if (text == "override") {
    out = Op::Override;
    return true;
  }
  return false;
}

bool ParseCurve(const std::string& text, Curve& out) {
  if (text == "linear") {
    out = Curve::Linear;
    return true;
  }
  if (text == "diminishing") {
    out = Curve::Diminishing;
    return true;
  }
  return false;
}

bool ParseBaseline(const std::string& text, BaselineMode& out) {
  if (text == "default") {
    out = BaselineMode::RegistryDefault;
    return true;
  }
  if (text == "zero") {
    out = BaselineMode::Zero;
    return true;
  }
  if (text == "min") {
    out = BaselineMode::Min;
    return true;
  }
  return false;
}

} // namespace

void ParseEffectEntries(const nlohmann::json &effects,
                        const std::string &ownerId, DeclaredEffects &out,
                        const std::function<void(const std::string &)> &warn) {
  if (!effects.is_array()) {
    warn("Warning: '" + ownerId +
         "' has a non-array 'effects'; ignoring it.");
    return;
  }

  for (const auto &entryJson : effects) {
    if (entryJson.is_string()) {
      out.hooks[ownerId].push_back(entryJson.get<std::string>());
      continue;
    }
    if (!entryJson.is_object()) {
      warn("Warning: '" + ownerId +
           "' has an effect that is neither a hook id nor an object; "
           "skipping it.");
      continue;
    }

    StatLink link;
    link.stat = ownerId;
    link.point = entryJson.value("point", "");
    link.activity = entryJson.value("activity", "");
    link.coef = entryJson.value("perPoint", 0.0f);
    link.exponent = entryJson.value("exponent", 2.0f);
    link.priority = entryJson.value("priority", 0);
    link.flavor = entryJson.value("flavor", "");

    const std::string opText = entryJson.value("op", "pct");
    const std::string curveText = entryJson.value("curve", "linear");
    const std::string baselineText = entryJson.value("baseline", "default");
    if (!ParseOp(opText, link.op)) {
      warn("Warning: '" + ownerId + "' has unknown op '" + opText +
           "' (expected pct, flat or override); skipping that effect.");
      continue;
    }
    if (!ParseCurve(curveText, link.curve)) {
      warn("Warning: '" + ownerId + "' has unknown curve '" + curveText +
           "' (expected linear or diminishing); skipping that effect.");
      continue;
    }
    if (!ParseBaseline(baselineText, link.baseline)) {
      warn("Warning: '" + ownerId + "' has unknown baseline '" + baselineText +
           "' (expected default, zero or min); skipping that effect.");
      continue;
    }

    // Section 4.1: both are required, and a deliberate global must say so.
    // Defaulting the activity would let a link become global by accident, which
    // is the one thing the explicit "*" exists to stop.
    if (link.point.empty()) {
      warn("Warning: '" + ownerId +
           "' has an effect with no 'point'; skipping it.");
      continue;
    }
    if (link.activity.empty()) {
      warn("Warning: '" + ownerId + "' has a '" + link.point +
           "' effect with no 'activity'. Use \"*\" for an effect that applies "
           "everywhere; skipping it.");
      continue;
    }

    // An override is scaled by the source's deviation, so it resolves to 0 for a
    // neutral character. Almost always a mistake in a declarative link.
    if (link.op == Op::Override) {
      warn("Warning: '" + ownerId + "' declares an 'override' " + link.point +
           "' effect. It is scaled by the source's deviation and so resolves to "
           "0 at the neutral value; a named hook is usually what is wanted "
           "here.");
    }

    out.links[ownerId].push_back(link);
  }
}
