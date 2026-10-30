#include "SettingsSchema.h"

#include <sstream>

namespace SettingsSchema {
namespace {

std::string TypeText(const FieldSpec &field) {
  switch (field.type) {
  case ValueType::Bool:
    return "bool";
  case ValueType::Int:
  case ValueType::Int64:
  case ValueType::UInt64:
    return "int";
  case ValueType::Float:
  case ValueType::Double:
    return "number";
  case ValueType::String:
    return "string";
  case ValueType::EnumString:
    return "string";
  case ValueType::Json:
    return "object";
  }
  return "string";
}

std::string DefaultText(const FieldSpec &field) {
  const nlohmann::json value = DefaultJson(field);
  if (value.is_null()) {
    return "*absent*";
  }
  if (value.is_string()) {
    const std::string text = value.get<std::string>();
    return "`" + (text.empty() ? std::string("\"\"") : text) + "`";
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "`true`" : "`false`";
  }
  return "`" + value.dump() + "`";
}

std::string AllowedText(const FieldSpec &field) {
  if (field.type != ValueType::EnumString || field.enumValues.empty()) {
    return "";
  }
  std::string text = " One of ";
  for (std::size_t i = 0; i < field.enumValues.size(); ++i) {
    if (i > 0) {
      text += (i + 1 == field.enumValues.size()) ? " or " : ", ";
    }
    text += "`" + field.enumValues[i] + "`";
  }
  text += ".";
  if (!field.strictEnum) {
    text += " Other values are passed through unchanged.";
  }
  return text;
}

std::string BoundsText(const RangeRule &rule) {
  std::ostringstream out;
  if (rule.min && rule.max) {
    out << " Must be " << *rule.min << ".." << *rule.max << ".";
  } else if (rule.min) {
    out << " Must be " << (rule.minInclusive ? "at least " : "greater than ")
        << *rule.min << ".";
  } else if (rule.max) {
    out << " Must be " << (rule.maxInclusive ? "at most " : "less than ")
        << *rule.max << ".";
  }
  if (rule.clamp) {
    out << " A value outside the range is clamped.";
  } else if (rule.min || rule.max) {
    out << " A value outside the range is reported and replaced.";
  }
  return out.str();
}

// The section prefix a field's row label is relative to.
std::string RowLabel(const FieldSpec &field) {
  const std::string prefix = field.section + ".";
  if (field.path.rfind(prefix, 0) == 0) {
    return field.path.substr(prefix.size());
  }
  return field.path;
}

void WriteFieldTable(std::ostringstream &out, const std::string &sectionKey) {
  out << "| field | type | default | meaning |\n";
  out << "|---|---|---|---|\n";
  for (const FieldSpec &field : Fields()) {
    if (field.section != sectionKey || !field.persisted) {
      continue;
    }
    out << "| `" << RowLabel(field) << "` | " << TypeText(field) << " | "
        << DefaultText(field) << " | " << field.doc << AllowedText(field)
        << BoundsText(field.range.value_or(RangeRule{}));
    if (field.apply == Apply::Restart) {
      out << " *Restart to apply.*";
    }
    out << " |\n";
  }
}

void WriteProfileTable(std::ostringstream &out) {
  const CollectionSpec &profiles = Profiles();
  out << "| field | type | default | meaning |\n";
  out << "|---|---|---|---|\n";
  for (const CollectionFieldSpec &field : profiles.fields) {
    std::string type;
    switch (field.type) {
    case ValueType::Bool:
      type = "bool";
      break;
    case ValueType::Int:
    case ValueType::Int64:
    case ValueType::UInt64:
      type = "int";
      break;
    case ValueType::Float:
    case ValueType::Double:
      type = "number";
      break;
    case ValueType::Json:
      type = "object";
      break;
    default:
      type = "string";
      break;
    }
    const nlohmann::json fallback = ProfileDefaultJson(field);
    std::string defaultValue;
    if (fallback.is_null()) {
      defaultValue = "*absent*";
    } else if (fallback.is_string()) {
      defaultValue = "`\"" + fallback.get<std::string>() + "\"`";
    } else if (fallback.is_boolean()) {
      defaultValue = fallback.get<bool>() ? "`true`" : "`false`";
    } else {
      defaultValue = "`" + fallback.dump() + "`";
    }
    out << "| `" << field.path << "` | " << type << " | " << defaultValue
        << " | " << field.doc << BoundsText(field.range.value_or(RangeRule{}))
        << " |\n";
  }
}

const char *LoadOrder() {
  return
      "1. `settings.json` (tracked).\n"
      "2. `settings.local.json` beside it, if present. Merged **on top** with an\n"
      "   RFC 7396 merge patch, so it only needs the keys it overrides. This file\n"
      "   is gitignored and is where `model.apiKey` belongs. The settings window\n"
      "   writes every edit here.\n"
      "3. `model.json`, read **only** for `model.apiKey` when the merged result\n"
      "   still has no key. A migration fallback for checkouts that predate\n"
      "   `settings.json`.\n"
      "\n"
      "Command line:\n"
      "\n"
      "| invocation | effect |\n"
      "|---|---|\n"
      "| `./AIRogue` | `settings.json`, and the map it names |\n"
      "| `./AIRogue maps/Map_Village.json` | `settings.json`, map overridden |\n"
      "| `./AIRogue --settings other.json` | a different settings file |\n"
      "| `./AIRogue --settings other.json maps/Map_Village.json` | both |\n"
      "\n"
      "An unknown top-level key (such as `_readme`) is ignored. The override file\n"
      "is always the sibling of whichever settings file was named.\n";
}

const char *TtsProse() {
  return
      "The reply is a bad thing to read out literally: most of its bytes are the\n"
      "blueprint JSON, and its prose restates the notes. `content` chooses:\n"
      "\n"
      "- `notes` speaks the verdict and the model's own critique;\n"
      "- `prose_and_notes` adds the prose the notes summarise;\n"
      "- `everything` exists only for debugging the pipeline and will read the\n"
      "  JSON aloud.\n"
      "\n"
      "Piper is invoked as `<binary> --model <voice> --output-raw`, so `voice`\n"
      "must have `<voice>.onnx.json` beside it for the sample rate. A bare\n"
      "`binary` is resolved on `PATH`, which the nix-shell provides; an absolute\n"
      "path also works. The designer window has a playback bar with pause, stop\n"
      "and a per-sentence skip, because a long reply is worth interrupting\n"
      "without losing the text.\n"
      "\n"
      "If there is no sound, check in this order: `enabled`; that `piper` is on\n"
      "`PATH` (or `binary` is absolute); that both voice files exist; the volume\n"
      "knob, which is applied at playback and silently clamps. A missing engine\n"
      "is reported in the window rather than failing the design.\n"
      "\n"
      "A voice is two files: `en_GB-alba-medium.onnx` and the matching\n"
      "`.onnx.json` that carries the sample rate. Piper ships them together; a\n"
      "voice copied without the json plays at the wrong rate (too fast or too\n"
      "slow) rather than failing, which is why the pair is worth checking first.\n";
}

const char *SystemOneProse() {
  return
      "The flat fields above are always available as one more profile, whose id\n"
      "is `default`. That is why nothing had to be migrated: an existing config\n"
      "keeps working untouched, and `profiles` adds choices beside it. A profile\n"
      "may reuse the id `default` to override it.\n"
      "\n"
      "`preset` seeds endpoint, model, key and timeout, and the explicit fields\n"
      "are applied on top. Empty means no seed, which is how a profile points at\n"
      "a server this codebase has no preset for (a remote llama-server) using\n"
      "only `model`.\n"
      "\n"
      "**The endpoint is a decisions endpoint, not a chat endpoint.** A correct\n"
      "profile answers a state-and-question probe with calibrated fields; pointing\n"
      "one at a chat model produces prose and no verdict. The timeout rule follows\n"
      "the endpoint because the two cases are genuinely different: 120s for a\n"
      "local server, 30s hosted. An explicit `timeoutSeconds` (or a profile's own)\n"
      "overrides it.\n"
      "\n"
      "`system1.activeProfile` is only the *starting* choice. The picker in the\n"
      "System 1 Overview window switches at runtime, and the selection is stored\n"
      "in `debug_windows_state.json` rather than here, because it is session\n"
      "state. `--system1-profile` outranks both: an explicit flag is a request\n"
      "about *this* run.\n"
      "\n"
      "`maxRecords` bounds what the overview window keeps in memory; the log file\n"
      "keeps everything, so a long session stays fully recorded on disk.\n";
}

const char *ArtGenerationProse() {
  return
      "**This is the one part of the art feature that spends money unattended.**\n"
      "It is off unless turned on deliberately, and capped twice:\n"
      "`maxRequestsPerSession` bounds the session, `batchDelaySeconds` decides how\n"
      "many creations share one request. One request costs the same whether it\n"
      "carries one tile or nineteen, so a short delay is what makes a burst cost\n"
      "one request.\n"
      "\n"
      "`command` is a command rather than a linked-in backend on purpose: image\n"
      "tooling here is Python plus ImageMagick, and the local-LoRA path has to\n"
      "stay swappable behind the same seam. `{worklist}` is replaced with the path\n"
      "the game writes, and the command must write `<worklist>.result.json`.\n"
      "Empty uses the built-in command, which runs `tools/gen_tile_art.py` under\n"
      "`nix-shell`.\n"
      "\n"
      "`source` is a *source*, not a mode: `image` and `dsl` both write the same\n"
      "PNG through the same placement and hot-swap, so everything downstream is\n"
      "shared. `dslAttempts` is much higher than the image path's\n"
      "`designer.maxDesignAttempts` because a DSL compile is free and instant,\n"
      "and `dsl` ignores `command`, `batchDelaySeconds`, `maxCellsPerRequest` and\n"
      "`maxRequestsPerSession` entirely.\n"
      "\n"
      "Batching is not an optimisation here, it is the point. Measured: **1,290\n"
      "image tokens per request regardless of how many cells it fills**, so one\n"
      "request for four tiles costs what one request for one tile costs, while one\n"
      "request *per tile* is roughly 19x the cost per asset. The queue never blocks\n"
      "a frame: a request runs on a worker thread and the sprite is swapped in from\n"
      "a main-thread drain when it lands. A tile therefore draws as its ASCII glyph\n"
      "until the art arrives; that glyph is a legitimate rendering, not a\n"
      "placeholder.\n"
      "\n"
      "**Point a test run at a throwaway `art.pack`.** The generator writes to\n"
      "whatever `spritesDirectory` the worklist names, which is derived from\n"
      "`art.pack`, so a test with the real pack overwrites the real generated\n"
      "sprites. `map.file` is used verbatim, so an absolute path is the reliable\n"
      "spelling for a map outside the repo.\n"
      "\n"
      "The CLI modes that prove the live path, all free with a stub command:\n"
      "\n"
      "| mode | what it proves |\n"
      "|---|---|\n"
      "| `--tile-art <name> --tile-art-queue` | request, batch window, worker,\n"
      "main-thread drain and the render entities carrying a `Sprite`. `--tile-art-hold\n"
      "<n>` draws the pipeline panels for n frames against the settled job. |\n"
      "| `--art-cache-test` | a stage file rewritten in place reloads instead of being\n"
      "served from the panel cache. |\n"
      "| `--paint-memo-test <tile>` | art that appears *after* the cell was painted is\n"
      "noticed. The tile must be on the map with a generated sprite and no pack\n"
      "sprite, which is what makes its absence observable. |\n";
}

const char *WiredAppendix() {
  return
      "Everything in the tables above is read **and used**. These are the knobs\n"
      "that still live only in `include/Defaults.h` and are *not* in\n"
      "`settings.json`. They are listed so the boundary is explicit; adding one\n"
      "means adding a field to `GameSettings` and a row to `SettingsSchema.h`.\n"
      "\n"
      "| area | constants still hardcoded |\n"
      "|---|---|\n"
      "| physics / movement | `DEFAULT_MAXSPEED`, `DEFAULT_WAYPOINT_ACCEL`, `DEFAULT_PLAYER_FRICTION`, `DEFAULT_ENTITY_FRICTION`, `DEFAULT_VELOCITY`, `DEFAULT_MINIMUM_VEL_FOR_WALL_REPEL`, `DEFAULT_WALL_REPEL_FORCE`, `DEFAULT_MINIMUM_WAYPOINT_SPEED`, `DEFAULT_MINIMUM_WAYPOINT_ACCEL`, `DEFAULT_MINIMUM_WAYPOINT_DISTANCE`, `DEFAULT_MINIMUM_SPEED_FOR_SLOWING_RADIUS` |\n"
      "| perception / interaction | `DEFAULT_OBJECT_SEARCH_RADIUS`, `DEFAULT_PLACEMENT_MAX_DISTANCE`, `DEFAULT_HARVEST_DURABILITY_COST` |\n"
      "| input | `DEFAULT_INPUT_MOVEMENT`, `DEFAULT_INPUT_ZOOM`, `DEFAULT_MINIMUM_INPUT_ZOOM`, `DEFAULT_MAXIMUM_INPUT_ZOOM`, `DEFAULT_DOUBLE_CLICK_TIME` |\n"
      "| hitboxes / visuals | `DEFAULT_PLAYER_HITBOX_*`, `DEFAULT_PLAYER_VISUAL_*`, `DEFAULT_ENTITY_HITBOX_*`, `DEFAULT_ENTITY_VISUAL_*` |\n"
      "| HUD / chat colours | `DEFAULT_ACTION_BAR_*`, `DEFAULT_FLOATING_TEXT_*`, `DEFAULT_COLOR_*`, `DEFAULT_SEE_INVENTORY_OPTION`, `DEFAULT_SHOW_STATUS_OPTION` |\n"
      "| AI timing | `DEFAULT_EMPTY_RESPONSE_RETRY_MS`, `DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS` |\n"
      "| render target | `gameTexture` is still created at `1920x1080` and `Draw()` still blits it to a `1920x1080` destination; `virtualWidth`/`virtualHeight` (`640x512`) are still fixed in `Game::Init`. |\n"
      "\n"
      "Two related files are *not* settings:\n"
      "\n"
      "- `debug_windows_state.json` is runtime state (which windows are open),\n"
      "  written by the game. Only its **path** is configurable.\n"
      "- `openrouter_debug.log` is AI debug output, not configuration.\n";
}

const char *SectionProse(const std::string &key) {
  if (key == "designer.tts") {
    return TtsProse();
  }
  if (key == "system1") {
    return SystemOneProse();
  }
  if (key == "art.generation") {
    return ArtGenerationProse();
  }
  return "";
}

}  // namespace

std::string MarkdownReference() {
  std::ostringstream out;
  out << "# AIRogue settings reference\n\n";
  out << "The game reads one JSON file at startup: `settings.json` in the working\n";
  out << "directory. It is loaded by `LoadGameSettings()` in `src/Settings.cpp`, before\n";
  out << "`Game::Init()`, and every subsystem reads the resulting object through\n";
  out << "`Settings()` (`include/Settings.h`).\n\n";
  out << "Nothing here changes the compiled-in defaults in `include/Defaults.h`: a\n";
  out << "missing key, a missing file, or a malformed file leaves the default in place.\n";
  out << "A file that fails to parse prints a reason on stderr and the game starts with\n";
  out << "defaults.\n\n";
  out << "*This file is generated from the table in `include/SettingsSchema.h`. Do not\n";
  out << "edit it by hand; the settings window's **Write reference** button regenerates\n";
  out << "it, and a row that is wrong there is wrong in the loader too.*\n\n";
  out << "The **Settings** window (opened from the Debug Console) lists every field\n";
  out << "below and edits it in place. Its writes go to `settings.local.json`, and a\n";
  out << "row tagged *restart to apply* is one whose consumer only reads at startup.\n";
  out << "A row tagged *cli* was set by a command-line flag for that run -- a map\n";
  out << "argument, `--art-pack` or `--system1-profile`. No file contains it, and\n";
  out << "editing it from the window writes `settings.local.json` for runs that omit\n";
  out << "the flag.\n\n";
  out << "## Load order\n\n" << LoadOrder();
  out << "\n---\n\n";

  for (const SectionSpec &section : Sections()) {
    out << "## `" << section.title << "`\n\n";
    if (!section.doc.empty()) {
      out << section.doc << "\n\n";
    }
    if (section.key == Profiles().section) {
      WriteProfileTable(out);
    } else {
      WriteFieldTable(out, section.key);
    }
    const char *prose = SectionProse(section.key);
    if (prose[0] != '\0') {
      out << "\n" << prose;
    }
    out << "\n---\n\n";
  }

  out << "## Wired vs. not wired\n\n" << WiredAppendix();
  return out.str();
}

}  // namespace SettingsSchema
