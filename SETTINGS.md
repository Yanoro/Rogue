# AIRogue settings reference

The game reads one JSON file at startup: `settings.json` in the working
directory. It is loaded by `LoadGameSettings()` in `src/Settings.cpp`, before
`Game::Init()`, and every subsystem reads the resulting object through
`Settings()` (`include/Settings.h`).

Nothing here changes the compiled-in defaults in `include/Defaults.h`: a
missing key, a missing file, or a malformed file leaves the default in place.
A file that fails to parse prints a reason on stderr and the game starts with
defaults.

*This file is generated from the table in `include/SettingsSchema.h`. Do not
edit it by hand; the settings window's **Write reference** button regenerates
it, and a row that is wrong there is wrong in the loader too.*

The **Settings** window (opened from the Debug Console) lists every field
below and edits it in place. Its writes go to `settings.local.json`, and a
row tagged *restart to apply* is one whose consumer only reads at startup.
A row tagged *cli* was set by a command-line flag for that run -- a map
argument, `--art-pack` or `--system1-profile`. No file contains it, and
editing it from the window writes `settings.local.json` for runs that omit
the flag.

## Load order

1. `settings.json` (tracked).
2. `settings.local.json` beside it, if present. Merged **on top** with an
   RFC 7396 merge patch, so it only needs the keys it overrides. This file
   is gitignored and is where `model.apiKey` belongs. The settings window
   writes every edit here.
3. `model.json`, read **only** for `model.apiKey` when the merged result
   still has no key. A migration fallback for checkouts that predate
   `settings.json`.

Command line:

| invocation | effect |
|---|---|
| `./AIRogue` | `settings.json`, and the map it names |
| `./AIRogue maps/Map_Village.json` | `settings.json`, map overridden |
| `./AIRogue --settings other.json` | a different settings file |
| `./AIRogue --settings other.json maps/Map_Village.json` | both |

An unknown top-level key (such as `_readme`) is ignored. The override file
is always the sibling of whichever settings file was named.

---

## `map`

Which map the run starts on, and how its authored NPCs are spawned.

| field | type | default | meaning |
|---|---|---|---|
| `file` | string | `./maps/Map_Farm.json` | Map loaded at startup. A map path on the command line overrides it. *Restart to apply.* |
| `spawnNPCs` | bool | `true` | Whether the map's authored NPCs are spawned. Off gives an empty world for map editing. *Restart to apply.* |
| `mapsDirectory` | string | `./maps` | Directory the Map Reload window lists reloadable maps from. *Restart to apply.* |

---

## `model`

The backend the agents talk to. Read when the ECS world builds its AI instance.

| field | type | default | meaning |
|---|---|---|---|
| `profile` | string | `""` | Name of an ai.profiles entry to take this block's settings from; any inline field overrides it. Empty uses the inline fields alone. *Restart to apply.* |
| `type` | string | `ollama` | Backend to use. Anything other than the three known names is treated as ollama. One of `ollama`, `gemini` or `openrouter`. Other values are passed through unchanged. *Restart to apply.* |
| `model` | string | `llama3` | Model id passed to the backend. *Restart to apply.* |
| `apiKey` | string | `""` | Key for the gemini and openrouter backends. Leave empty here; put it in settings.local.json. *Restart to apply.* |
| `endpoint` | string | `http://localhost:11434/api/generate` | Ollama endpoint. Only read by the ollama backend. *Restart to apply.* |
| `temperature` | number | *absent* | Sampling temperature. Absent leaves the provider's own default in charge. *Restart to apply.* |
| `maxTokens` | int | *absent* | Completion budget. Absent leaves the provider's own default in charge. *Restart to apply.* |
| `provider` | object | *absent* | OpenRouter provider routing object, passed through verbatim. *Restart to apply.* |
| `keepAlive` | string | `""` | How long Ollama keeps the model resident, e.g. 30m. Empty sends nothing and leaves Ollama's default in charge. *Restart to apply.* |

---

## `vision`

A second, independent Ollama backend behind the Vision Chat window. Separate from `model` because one Ollama instance keys conversation context by id alone, so it cannot serve two models.

| field | type | default | meaning |
|---|---|---|---|
| `profile` | string | `""` | Name of an ai.profiles entry of kind vision. model, endpoint, keepAlive and temperature come from it; maxTokens and provider have no meaning for vision and are reported. *Restart to apply.* |
| `enabled` | bool | `true` | Off leaves the Vision Chat window usable but reporting that no backend is configured. *Restart to apply.* |
| `model` | string | `moondream` | Ollama model tag for vision. *Restart to apply.* |
| `endpoint` | string | `http://localhost:11434/api/generate` | Ollama endpoint. The payload is the /api/generate shape. *Restart to apply.* |
| `temperature` | number | *absent* | Sampling temperature. Absent leaves the provider's own default in charge. *Restart to apply.* |
| `keepAlive` | string | `30m` | How long Ollama keeps the vision model resident. A vision model is several GB, so the default is warm. *Restart to apply.* |

---

## `ai`

The NPC prompt source and the spatial-memory digest.

| field | type | default | meaning |
|---|---|---|---|
| `npcPromptFile` | string | `""` | Text file holding the NPC system prompt template. Empty uses the compiled-in prompt. *Restart to apply.* |
| `memoryDigest` | bool | `false` | Whether a wake asks the model to summarise what the character remembers. |
| `memoryDigestMinAgeSeconds` | int | `120` | How stale a digest may be before a wake asks for a new one. |
| `memoryDigestCandidates` | int | `24` | How many candidate items the model is shown. |
| `memoryDigestStartupGraceSeconds` | int | `60` | Wall-clock seconds after a world loads before the first digest may be asked for. |
| `memoryDigestMinCells` | int | `400` | How much ground must be remembered before a digest is worth asking for. |
| `memoryDigestMaxTokens` | int | `1200` | Output budget for the digest's own request. |
| `memorySalience` | bool | `false` | The System 1 salience probe at wake. Logged only; nothing acts on it yet. |

---

## `ai.profiles`

Named AI backends: the endpoint, model, key and options for one model, declared once and referenced by id from any model block. A block's inline fields override the profile; a profile of kind s1, s2 or vision is offered only in the pickers it belongs to.

| field | type | default | meaning |
|---|---|---|---|
| `label` | string | `""` | Display name in the pickers. |
| `kind` | string | `s2` | What the model is for. The picker for a block only offers matching kinds; a hand-written mismatch is reported and still used. One of `s1`, `s2` or `vision`. |
| `preset` | string | `""` | S1 only: hosted_jev or local_kev seeds endpoint, model and key before the fields below. |
| `model.type` | string | `ollama` | Backend: ollama, gemini or openrouter. Anything else is treated as ollama. One of `ollama`, `gemini` or `openrouter`. |
| `model.model` | string | `llama3` | Model id passed to the backend. |
| `model.apiKey` | string | `""` | Key for gemini and openrouter. Empty falls back to the block's own key and then the main key. |
| `model.endpoint` | string | `http://localhost:11434/api/generate` | Ollama endpoint. Ignored by the other backends. |
| `model.keepAlive` | string | `""` | How long Ollama keeps the model resident, e.g. 30m. |
| `model.temperature` | number | *absent* | Sampling temperature. Absent leaves the provider's own default in charge. |
| `model.maxTokens` | int | *absent* | Completion budget. Absent leaves the provider's own default in charge. |
| `model.provider` | object | *absent* | OpenRouter provider routing object, passed through verbatim. |

A profile is resolved **into** the block that names it, not at runtime: the
loader copies the profile's model fields into the block, then reads the
block's own keys over them. So a block can share a backend and still change
one thing -- a cheaper model id, a smaller budget -- and every consumer keeps
reading a plain model block.

```json
{
  "ai": {
    "profiles": {
      "deepseek": {
        "label": "DeepSeek V4 Flash",
        "kind": "s2",
        "model": { "type": "openrouter",
                   "model": "deepseek/deepseek-v4-flash",
                   "maxTokens": 1500 }
      }
    }
  },
  "model": { "profile": "deepseek" },
  "designer": { "model": { "profile": "deepseek", "maxTokens": 3000 } }
}
```

The registry is an **object keyed by id**, not an array, because the override
layer (`settings.local.json`) merges objects key by key: a profile saved from
the settings window lands beside the others instead of replacing the whole
registry, which is what lets one profile's `apiKey` live in the untracked file.
Removing a profile is written as a JSON `null`, which RFC 7396 reads as a
deletion.

`kind` decides which pickers offer the profile: `s2` for the agent, designer,
rewriter and single navigator blocks; `s1` for System 1; `vision` for the Vision
Chat backend. A hand-written mismatch is reported and still used, never refused,
so an experiment is not blocked by a taxonomy.

A System 1 entry may name a profile instead of carrying a model block, in which
case the profile's preset and fields are applied first and the entry's own
`model` block and calibration sit on top -- the reason `system1.profiles` still
exists at all.

In the settings window every block that takes a model starts with its profile
picker. Choosing **(no profile)** reveals the block's own fields, and forks the
profile it was using into them first, so switching to a manual configuration
keeps what already worked. With a profile selected the supplied fields are
hidden; **override fields** reveals them, and the picker row reports how many
explicit fields the files set, so a hand-written override is never invisible.
A System 1 entry follows the same rule for its model block, while its
calibration and timeout stay visible because they are per-entry. The last
choice in a picker creates a new profile and jumps to this editor.

---

## `designer`

The blueprint designer's backend, prompt and output budget.

| field | type | default | meaning |
|---|---|---|---|
| `useMainModel` | bool | `true` | Share the NPC backend instead of using the model block below. *Restart to apply.* |
| `promptFile` | string | `data/blueprints/DESIGNER.md` | The designer's instructions: the grammar, the reply contract and the worked examples. Empty uses the compiled-in default. *Restart to apply.* |
| `maxDesignAttempts` | int | `3` | How many times the engine re-prompts after an invalid design before giving up. Must be at least 1. A value outside the range is reported and replaced. *Restart to apply.* |
| `autoRoute` | bool | `false` | Ask the classifier which domain a request is for, and send once it answers, instead of waiting for the Route button. Hides the domain dropdown. Costs one decision call per request. |
| `maxTokens` | int | `0` | The designer's own completion budget, overriding the backend's. 0 inherits it. Must be at least 0. A value outside the range is reported and replaced. *Restart to apply.* |
| `model.profile` | string | `""` | Name of an ai.profiles entry for the designer's own model block; inline fields override it. *Restart to apply.* |
| `model.type` | string | `ollama` | Backend for the designer's own model block. One of `ollama`, `gemini` or `openrouter`. Other values are passed through unchanged. *Restart to apply.* |
| `model.model` | string | `llama3` | Model id for the designer's own model block. *Restart to apply.* |
| `model.apiKey` | string | `""` | Key for the designer's own model block. *Restart to apply.* |
| `model.endpoint` | string | `http://localhost:11434/api/generate` | Ollama endpoint for the designer's own model block. *Restart to apply.* |
| `model.temperature` | number | *absent* | Sampling temperature for the designer's own model block. *Restart to apply.* |
| `model.maxTokens` | int | *absent* | Completion budget for the designer's own model block. *Restart to apply.* |
| `model.provider` | object | *absent* | OpenRouter provider routing for the designer's own model block. *Restart to apply.* |
| `model.keepAlive` | string | `""` | Ollama keep-alive for the designer's own model block. *Restart to apply.* |

---

## `designer.tts`

Reading the designer's reply aloud.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `false` | Read the designer's reply aloud. |
| `binary` | string | `piper` | The piper CLI. A bare name is resolved on PATH. |
| `voice` | string | `models/tts/en_GB-alba-medium.onnx` | The voice model. Piper resolves <voice>.onnx.json beside it for the sample rate. |
| `volume` | number | `1.0` | Playback gain, 0..1. Above 1.0 raylib amplifies into clipping; below 0 the phase inverts. Must be 0..1. A value outside the range is clamped. |
| `content` | string | `notes` | What gets read out. Most of a reply's bytes are the blueprint JSON, so the default speaks only the verdict and critique. One of `notes`, `prose_and_notes` or `everything`. |

The reply is a bad thing to read out literally: most of its bytes are the
blueprint JSON, and its prose restates the notes. `content` chooses:

- `notes` speaks the verdict and the model's own critique;
- `prose_and_notes` adds the prose the notes summarise;
- `everything` exists only for debugging the pipeline and will read the
  JSON aloud.

Piper is invoked as `<binary> --model <voice> --output-raw`, so `voice`
must have `<voice>.onnx.json` beside it for the sample rate. A bare
`binary` is resolved on `PATH`, which the nix-shell provides; an absolute
path also works. The designer window has a playback bar with pause, stop
and a per-sentence skip, because a long reply is worth interrupting
without losing the text.

If there is no sound, check in this order: `enabled`; that `piper` is on
`PATH` (or `binary` is absolute); that both voice files exist; the volume
knob, which is applied at playback and silently clamps. A missing engine
is reported in the window rather than failing the design.

A voice is two files: `en_GB-alba-medium.onnx` and the matching
`.onnx.json` that carries the sample rate. Piper ships them together; a
voice copied without the json plays at the wrong rate (too fast or too
slow) rather than failing, which is why the pair is worth checking first.

---

## `designer.rewriter`

The model behind the Improve button. A short text edit wants a fast, cheap, non-reasoning model, which is usually not the designer's.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `false` | False shares the designer's backend with reasoning turned off. True reads the model block below. *Restart to apply.* |
| `disableReasoning` | bool | `true` | Turn reasoning off on the rewriter's backend when it shares the designer's. *Restart to apply.* |
| `model.profile` | string | `""` | Name of an ai.profiles entry for the rewriter's own model block; inline fields override it. *Restart to apply.* |
| `model.type` | string | `ollama` | Backend for the rewriter's own model block. One of `ollama`, `gemini` or `openrouter`. Other values are passed through unchanged. *Restart to apply.* |
| `model.model` | string | `llama3` | Model id for the rewriter's own model block. *Restart to apply.* |
| `model.apiKey` | string | `""` | Key for the rewriter's own model block. *Restart to apply.* |
| `model.endpoint` | string | `http://localhost:11434/api/generate` | Ollama endpoint for the rewriter's own model block. *Restart to apply.* |
| `model.temperature` | number | *absent* | Sampling temperature for the rewriter's own model block. *Restart to apply.* |
| `model.maxTokens` | int | *absent* | Completion budget for the rewriter's own model block. *Restart to apply.* |
| `model.provider` | object | *absent* | OpenRouter provider routing for the rewriter's own model block. *Restart to apply.* |
| `model.keepAlive` | string | `""` | Ollama keep-alive for the rewriter's own model block. *Restart to apply.* |

---

## `system1`

The typed-classifier layer and its selectable backends.

| field | type | default | meaning |
|---|---|---|---|
| `useMainModel` | bool | `true` | Share the NPC backend instead of using the model block below. *Restart to apply.* |
| `model.profile` | string | `""` | Name of an ai.profiles entry of kind s1 for the System 1 block; inline fields override it. *Restart to apply.* |
| `model.model` | string | `""` | Model id. Empty means inherit the main model's id rather than ModelSection's llama3 default. *Restart to apply.* |
| `model.type` | string | `ollama` | Backend for the System 1 model block. One of `ollama`, `gemini` or `openrouter`. Other values are passed through unchanged. *Restart to apply.* |
| `model.apiKey` | string | `""` | Key for the System 1 model block. *Restart to apply.* |
| `model.endpoint` | string | `http://localhost:11434/api/generate` | Decisions endpoint for the System 1 model block. *Restart to apply.* |
| `model.temperature` | number | *absent* | Sampling temperature for the System 1 model block. *Restart to apply.* |
| `model.maxTokens` | int | *absent* | Completion budget for the System 1 model block. *Restart to apply.* |
| `model.provider` | object | *absent* | OpenRouter provider routing for the System 1 model block. *Restart to apply.* |
| `model.keepAlive` | string | `""` | Ollama keep-alive for the System 1 model block. *Restart to apply.* |
| `disableReasoning` | bool | `true` | Ask the backend for a single non-reasoning answer. System 1's premise is a cheap, fast, one-shot classification. *Restart to apply.* |
| `maxRecords` | int | `500` | Bounds the in-memory view the overview window keeps. The log file keeps everything. Must be at least 1. A value outside the range is reported and replaced. *Restart to apply.* |
| `timeoutSeconds` | int | *absent* | Whole-request budget for one decision. Absent follows the endpoint: 120s local, 30s hosted. Must be at least 1. A value outside the range is reported and replaced. *Restart to apply.* |
| `activeProfile` | string | `""` | Which profile starts active, unless the persisted choice or --system1-profile says otherwise. Empty means 'default'. |
| `routeRequests` | bool | `true` | Whether the sprite designer asks a classifier what kind of sprite a request is for. |
| `minConfidence` | number | `0.0` | Below this, the route verdict is reported as unsure rather than acted on. 0 disables the check. |

The flat fields above are always available as one more profile, whose id
is `default`. That is why nothing had to be migrated: an existing config
keeps working untouched, and `profiles` adds choices beside it. A profile
may reuse the id `default` to override it.

`preset` seeds endpoint, model, key and timeout, and the explicit fields
are applied on top. Empty means no seed, which is how a profile points at
a server this codebase has no preset for (a remote llama-server) using
only `model`.

**The endpoint is a decisions endpoint, not a chat endpoint.** A correct
profile answers a state-and-question probe with calibrated fields; pointing
one at a chat model produces prose and no verdict. The timeout rule follows
the endpoint because the two cases are genuinely different: 120s for a
local server, 30s hosted. An explicit `timeoutSeconds` (or a profile's own)
overrides it.

`system1.activeProfile` is only the *starting* choice. The picker in the
System 1 Overview window switches at runtime, and the selection is stored
in `debug_windows_state.json` rather than here, because it is session
state. `--system1-profile` outranks both: an explicit flag is a request
about *this* run.

`maxRecords` bounds what the overview window keeps in memory; the log file
keeps everything, so a long session stays fully recorded on disk.

---

## `system1.profiles`

The selectable System 1 backends. Each entry is how to reach a model plus the two knobs that are meaningful per model.

| field | type | default | meaning |
|---|---|---|---|
| `id` | string | `""` | Identifier used to select this profile: system1.activeProfile, the --system1-profile flag and every status line. Required. |
| `profile` | string | `""` | Name of an ai.profiles entry to take the model settings from. Its preset and fields are applied before this entry's own model block, so calibration and timeout can sit on top of a shared backend. |
| `label` | string | `""` | Display name. Falls back to the id. |
| `preset` | string | `""` | hosted_jev or local_kev: seeds endpoint, model, key and timeout before the explicit fields below are applied. Empty means no seed. |
| `model.type` | string | `"ollama"` | Backend for this profile's model block. |
| `model.model` | string | `""` | Model id. Empty means use the preset's. |
| `model.apiKey` | string | `""` | Key for this profile's model block. Empty means use the preset's or the main key. |
| `model.endpoint` | string | `""` | Endpoint. Empty means use the preset's. |
| `model.keepAlive` | string | `""` | Ollama keep-alive for this profile. |
| `model.temperature` | number | `0.0` | Sampling temperature. Absent leaves the preset's in charge. |
| `model.maxTokens` | int | `1500` | Completion budget. Absent follows the preset. |
| `model.provider` | object | *absent* | OpenRouter provider routing. |
| `timeoutSeconds` | int | `0` | Whole-request budget. 0 follows the preset, then the endpoint rule (120s local, 30s hosted). Must be at least 0. A value outside the range is reported and replaced. |
| `minConfidence` | number | `0.0` | Below this, a verdict is reported as unsure rather than acted on. 0 disables the check. Calibration is per model, which is why it lives here. |

---

## `system1.navigation`

How the System 1 navigator spends calls and time. Properties of the game's use of the model, not of the endpoint, so swapping models does not silently change them.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `true` | The only switch. [NAVIGATE] reports that it is off rather than silently falling back. |
| `maxCandidates` | int | `8` | One line per action: local compass move or distant place. Must be at least 1. A value outside the range is reported and replaced. |
| `maxDirections` | int | `4` | Local compass moves offered before the distant place list. Must be at least 0. A value outside the range is reported and replaced. |
| `compassRadius` | int | `6` | How far a sector looks for the first place. Must be at least 1. A value outside the range is reported and replaced. |
| `maxSurveys` | int | `4` | Surveys allowed for one navigation before it gives up. Must be at least 0. A value outside the range is reported and replaced. |
| `maxSeconds` | number | `600.0` | Wall-clock budget for one navigation. Must be greater than 0. A value outside the range is reported and replaced. |
| `legTimeoutSeconds` | number | `60.0` | Budget for one leg of a navigation. Must be greater than 0. A value outside the range is reported and replaced. |
| `minProbability` | number | `0.30000001192092896` | Below this probability a candidate is not acted on. Must be 0..1. A value outside the range is clamped. |
| `minMargin` | number | `0.15000000596046448` | Required difference between the best and the runner-up candidate. Must be 0..1. A value outside the range is clamped. |
| `mapMode` | string | `off` | What spatial input the sector prompt gets: none, an appended ASCII map, or a map that replaces the prose. One of `off`, `append` or `replace`. |
| `mapRadius` | int | `6` | Radius of the ASCII map around the character. Must be 3..12. A value outside the range is reported and replaced. |
| `examineRadius` | int | `22` | Radius searched for the examine affordance list. Must be 1..64. A value outside the range is reported and replaced. |
| `maxExamine` | int | `2` | Objects from that list offered to the two-context System 1, reserved inside maxCandidates. Must be 0..12. A value outside the range is reported and replaced. |

---

## `navigation`

Which navigator runs.

| field | type | default | meaning |
|---|---|---|---|
| `agent` | string | `single` | Which navigator runs: the single-context plan-and-move navigator, or the strategist plus System 1 pilot. One of `single` or `two`. |

---

## `navigation.single`

One main-model context that returns both the plan and the next move each decision.

| field | type | default | meaning |
|---|---|---|---|
| `maxTokens` | int | `500` | Completion budget for one decision. Must be 64..8192. A value outside the range is reported and replaced. *Restart to apply.* |
| `dedicatedBackend` | bool | `true` | Build a dedicated AI instance for the navigator instead of sharing the NPC backend. *Restart to apply.* |
| `disableReasoning` | bool | `true` | Turn the dedicated backend's chain-of-thought off. Reasoning can spend the whole budget before any JSON is emitted. *Restart to apply.* |
| `compactJson` | bool | `true` | Send compact JSON in the single prompt. Every space and newline is a prompt token. |
| `historyTurns` | int | `3` | Previous decision turns kept in the navigator's context. 0 keeps the whole episode. Must be 0..64. A value outside the range is reported and replaced. |
| `concise` | bool | `true` | Ask for a one-sentence strategy and a short think block. |
| `objectSummary` | bool | `true` | Group objects by type/profile in the STATE block and use the target grammar (objects:all, inspect:<group>, approach:<alias>) instead of listing every examine candidate. |
| `maxObjectGroups` | int | `6` | Object groups shown in the default STATE block. The rest stay available through objects:all and inspect. Must be 1..64. A value outside the range is reported and replaced. |
| `maxGroupDetails` | int | `8` | Instances shown by inspect:<group>. inspect:<group>:all ignores this. Must be 1..128. A value outside the range is reported and replaced. |
| `includeGraph` | bool | `true` | Include the observation graph in the single prompt. Off for a small local model: the place graph duplicates what STATE/ACTIONS already say. |
| `includeMap` | bool | `true` | Include the ASCII map and its target list in the single prompt. Off for a small local model; the candidate actions remain in ACTIONS. |
| `contextTokens` | int | `4096` | Context window requested from Ollama (options.num_ctx). Too small and the model loses the start of the prompt. Must be 2048..32768. A value outside the range is reported and replaced. *Restart to apply.* |
| `timeoutSeconds` | number | `60.0` | Whole-request budget for one decision. Must be at least 1. A value outside the range is reported and replaced. |
| `fallbackToTwo` | bool | `true` | Fall back to the strategist plus pilot when the single navigator's replies are unusable. |
| `mapRadius` | int | `6` | Radius of the ASCII map offered with each decision. Must be 3..12. A value outside the range is reported and replaced. |
| `useMainModel` | bool | `true` | Keep the NPC model settings. False reads the model block below. *Restart to apply.* |
| `model.profile` | string | `""` | Name of an ai.profiles entry for the navigator's own model block; inline fields override it. *Restart to apply.* |
| `model.model` | string | `""` | Model id for the navigator's own model block. Empty is not configured. *Restart to apply.* |
| `model.type` | string | `ollama` | Backend for the navigator's own model block. One of `ollama`, `gemini` or `openrouter`. Other values are passed through unchanged. *Restart to apply.* |
| `model.apiKey` | string | `""` | Key for the navigator's own model block. *Restart to apply.* |
| `model.endpoint` | string | `http://localhost:11434/api/generate` | Ollama endpoint for the navigator's own model block. *Restart to apply.* |
| `model.temperature` | number | *absent* | Sampling temperature for the navigator's own model block. *Restart to apply.* |
| `model.maxTokens` | int | *absent* | Completion budget for the navigator's own model block. *Restart to apply.* |
| `model.provider` | object | *absent* | OpenRouter provider routing for the navigator's own model block. *Restart to apply.* |
| `model.keepAlive` | string | `""` | Ollama keep-alive for the navigator's own model block. *Restart to apply.* |
| `maxAttempts` | int | `3` | Consecutive unusable replies allowed for one decision before the fallback. Must be 1..8. A value outside the range is reported and replaced. |
| `maxDirections` | int | `8` | Compass moves offered to the main model. Must be 1..8. A value outside the range is reported and replaced. |
| `maxCandidates` | int | `12` | Total candidates offered: compass moves plus distant places. Must be 1..32. A value outside the range is reported and replaced. |

---

## `navigation.strategist`

The standard-LLM strategist above the System 1 pilot.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `true` | Whether the strategist runs above the System 1 pilot. |
| `fallbackToS1` | bool | `true` | Fall back to the System 1 pilot alone when the strategist gives nothing usable. |
| `maxStrategies` | int | `12` | Strategies kept per navigation. Must be at least 1. A value outside the range is reported and replaced. |
| `maxAttempts` | int | `3` | Attempts per strategist request: the first call plus retries. Must be at least 1. A value outside the range is reported and replaced. |
| `maxMonitors` | int | `3` | Monitors the strategist may keep running. Must be at least 0. A value outside the range is reported and replaced. |
| `monitorMinConfidence` | number | `0.6000000238418579` | Below this confidence a monitor is not acted on. Must be 0..1. A value outside the range is clamped. |
| `timeoutSeconds` | number | `30.0` | Whole-request budget for one strategist call. Must be greater than 0. A value outside the range is reported and replaced. |
| `observation.localGridRadius` | int | `8` | Radius of the local grid in the strategist's observation. Must be at least 1. A value outside the range is reported and replaced. |
| `observation.maxGraphNodes` | int | `24` | Nodes kept in the minimal observed graph. Must be at least 0. A value outside the range is reported and replaced. |
| `observation.maxGraphEdges` | int | `48` | Edges kept in the minimal observed graph. Must be at least 0. A value outside the range is reported and replaced. |
| `observation.maxTokens` | int | `2000` | Completion budget for the observation text. Must be at least 0. A value outside the range is reported and replaced. |

---

## `perception`

The acquisition field for every character's vision. Named `perception` because `vision` is already the Vision Chat backend.

| field | type | default | meaning |
|---|---|---|---|
| `range` | number | `22.0` | How far a character can see. Must be 1..40. A value outside the range is reported and replaced. |
| `angleDegrees` | number | `360.0` | Field of view. 360 is all-round; 90 is the forward cone. Must be 10..360. A value outside the range is reported and replaced. |
| `radarRadius` | int | `2` | The all-round near ring in either mode. Must be 0..10. A value outside the range is reported and replaced. |

---

## `regions`

Observed base regions: the coarse spatial memory places are derived from.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `true` | Whether observed base regions are formed at all. |
| `nearSize` | int | `8` | Cell size of the near region grid. Must be 1..64. A value outside the range is reported and replaced. |
| `nearRadius` | int | `12` | How far the near grid extends. Must be 0..40. A value outside the range is reported and replaced. |
| `midSize` | int | `16` | Cell size of the mid region grid. Must be 1..128. A value outside the range is reported and replaced. |
| `midRadius` | int | `24` | How far the mid grid extends. Must be 0..80. A value outside the range is reported and replaced. |
| `farSize` | int | `32` | Cell size of the far region grid. Must be 1..256. A value outside the range is reported and replaced. |
| `farRadius` | int | `48` | How far the far grid extends. Must be at least midRadius. Must be 1..512. A value outside the range is reported and replaced. |
| `maxRegions` | int | `64` | Regions remembered per character. Must be 0..512. A value outside the range is reported and replaced. |
| `modelNearCount` | int | `6` | Near regions offered to the model in one prompt. Must be 0..32. A value outside the range is reported and replaced. |

---

## `chunks`

Fixed world chunk grid and the agent-facing area card/route strip.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `true` | Whether the agent-facing chunk area card and route strip are built. |
| `size` | int | `8` | Cells per chunk side. Read at map load; a change remaps handles. Must be 4..64. A value outside the range is reported and replaced. |
| `maxTraversedAreas` | int | `8` | Areas listed in one last-leg strip. Must be 1..32. A value outside the range is reported and replaced. |
| `maxJourneySegments` | int | `12` | Journey stretches kept in the prompt. Must be 1..64. A value outside the range is reported and replaced. |
| `maxNewAreas` | int | `6` | Off-path newly seen areas listed after a leg. Must be 1..32. A value outside the range is reported and replaced. |
| `maxFeaturesPerArea` | int | `3` | Features listed per area in the last-leg strip. Must be 0..16. A value outside the range is reported and replaced. |
| `cardAspects` | int | `3` | Surfaces listed in the area card. Must be 0..16. A value outside the range is reported and replaced. |
| `cardObjectGroups` | int | `6` | Object groups listed in the area card. Must be 0..32. A value outside the range is reported and replaced. |
| `cardPlaces` | int | `4` | Places listed in the area card. Must be 0..16. A value outside the range is reported and replaced. |
| `includeCharacters` | bool | `true` | Whether the area card lists observed characters. |

---

## `window`

Window creation.

| field | type | default | meaning |
|---|---|---|---|
| `title` | string | `AIRogue` | Window title. *Restart to apply.* |
| `width` | int | `1920` | Initial window width. Also drives the camera's fit-to-map zoom reference. *Restart to apply.* |
| `height` | int | `1080` | Initial window height. Also drives the camera's fit-to-map zoom reference. *Restart to apply.* |
| `fullscreen` | bool | `true` | False keeps the windowed size above. *Restart to apply.* |
| `vsync` | bool | `true` | Request vertical sync. *Restart to apply.* |
| `targetFPS` | int | `60` | Frame rate cap. *Restart to apply.* |
| `monitor` | int | `-1` | -1 picks a monitor the way the game always has. 0 or more forces that index. *Restart to apply.* |

---

## `render`

Font and starting camera.

| field | type | default | meaning |
|---|---|---|---|
| `fontPath` | string | `./fonts/oldschool/otb - Bm (linux bitmap)/BmPlus_IBM_CGA.otb` | The bitmap font file. *Restart to apply.* |
| `fontSize` | int | `32` | Font size in pixels. *Restart to apply.* |
| `cameraMode` | string | `follow` | follow keeps the camera on the player; free leaves it to the arrow keys. One of `follow` or `free`. Other values are passed through unchanged. *Restart to apply.* |
| `cameraZoom` | number | `0.0` | 0 fits the map to the window. Any other value is the starting zoom. *Restart to apply.* |

---

## `player`

The player entity and its starting tile.

| field | type | default | meaning |
|---|---|---|---|
| `name` | string | `playerCharacter` | The player entity's name. *Restart to apply.* |
| `startX` | int | `48` | Starting tile. Out-of-bounds or wall positions are relocated to the nearest walkable tile. *Restart to apply.* |
| `startY` | int | `20` | Starting tile. *Restart to apply.* |
| `race` | string | `""` | Race the player's body slots come from. Empty means the human default. *Restart to apply.* |
| `actorSprite` | string | `""` | Actor sprite the player is drawn with. Empty uses the race's own look, then the ASCII art. |
| `maxSpeed` | number | `100.0` | Top movement speed in pixels per second. Holding a direction adds velocity and the movement system clamps it to this. Must be at least 0. A value outside the range is reported and replaced. |
| `portrait` | string | `""` | PortraitRegistry id for the player's HUD portrait. Empty means the race's default, then the unknown sprite. *Restart to apply.* |

---

## `world`

World generation.

| field | type | default | meaning |
|---|---|---|---|
| `seed` | int | `0` | 0 seeds from the clock, so every run differs. Any other value makes the run reproducible. Must be at least 0. A value outside the range is reported and replaced. *Restart to apply.* |

---

## `content`

Where each content registry loads its definitions from.

| field | type | default | meaning |
|---|---|---|---|
| `objects` | string | `data/objects` | Object template directory. *Restart to apply.* |
| `recipes` | string | `data/recipes` | Recipe registry directory. *Restart to apply.* |
| `stats` | string | `data/stats` | Stat registry directory. *Restart to apply.* |
| `skills` | string | `data/skills` | Skill registry directory. *Restart to apply.* |
| `items` | string | `data/items` | Item registry directory. *Restart to apply.* |
| `actorSprites` | string | `data/actor_sprites` | Actor-sprite catalogue: art for beings, keyed by sprite id. Not a definition of a creature. *Restart to apply.* |
| `races` | string | `data/races` | Race registry directory. *Restart to apply.* |
| `npcs` | string | `data/npcs` | NPC template directory: named starting configurations a map spawns by name and overrides. *Restart to apply.* |
| `brains` | string | `data/brains` | Brain template directory: background, goals and model profile for agents. A template with no brain is a body with no agent. *Restart to apply.* |
| `portraits` | string | `data/portraits` | Portrait registry directory: one JSON per portrait, mapping an id to a sprite path or convention id. *Restart to apply.* |
| `projectiles` | string | `data/projectiles` | Projectile registry directory: one JSON per flying object, with its sprite, speed and size. *Restart to apply.* |
| `places` | string | `data/places.json` | The place taxonomy file: which categories form places, their seed thresholds, link radii and split rule. *Restart to apply.* |
| `tiles` | string | `data/tiles` | The hand-authored tile catalog directory. Agent-created tiles go to generatedTiles instead. *Restart to apply.* |
| `generatedTiles` | string | `data/generated/tiles` | Generated overlay for agent-created tiles, loaded additively on top of the catalog. *Restart to apply.* |
| `editor` | string | `data/editor` | Where the Content Editor saves its overrides (tiles/ and objects/ beneath it). Loaded last, so an edit wins over the catalog and the generated overlay; the hand-authored files are never rewritten. *Restart to apply.* |
| `tilePromptFile` | string | `data/tiles/TILEDESIGNER.md` | The tile designer's instructions. Empty falls back to the generated vocabulary alone. *Restart to apply.* |
| `objectPromptFile` | string | `data/objects/OBJECTDESIGNER.md` | The object designer's instructions. *Restart to apply.* |
| `generatedObjects` | string | `data/generated/objects` | Generated overlay for agent-created objects. Empty means an accepted object lives for this run only. *Restart to apply.* |
| `spriteContractFile` | string | `data/prompts/SPRITE_CONTRACT.md` | The reply contract every kind's prompt is built from: format, refusal protocol and prohibitions. *Restart to apply.* |
| `spriteDSLPromptFile` | string | `data/style/SPRITEDESIGNER.md` | The system prompt for the turn that writes a pixel document for an accepted definition. *Restart to apply.* |
| `rewritePromptFile` | string | `data/prompts/REWRITE.md` | The prompt-improver's instructions, shared by every window that offers the Improve button. *Restart to apply.* |
| `blueprints` | string | `data/blueprints` | Building designs. Optional: with none, the placement window lists nothing. *Restart to apply.* |

---

## `art`

The art pack and its generated overlay. Convention sprites resolve from `<pack>/sprites/` first, then the generated layer.

| field | type | default | meaning |
|---|---|---|---|
| `pack` | string | `""` | Art pack directory. Empty means no art pack, and the generated layer is not consulted. *Restart to apply.* |
| `generated` | string | `""` | Where this pack's generated sprites live directly. Empty derives it from the pack's location. *Restart to apply.* |

---

## `art.generation`

Live generation of art for content an agent invents while the game runs. This is the one part of the art feature that spends money unattended.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `false` | Turn live art generation on. Off by default because it spends money unattended. |
| `command` | string | `""` | The command that turns a worklist JSON into sprites. Empty uses the built-in command. *Restart to apply.* |
| `batchDelaySeconds` | number | `2.0` | How long to collect requests before spending one request on them. |
| `maxRequestsPerSession` | int | `8` | Hard ceiling on generation requests for the session. Must be at least 1. A value outside the range is reported and replaced. *Restart to apply.* |
| `maxCellsPerRequest` | int | `4` | How many sprites may share one image request. Must be at least 1. A value outside the range is reported and replaced. |
| `workDirectory` | string | `""` | Where the worklist and raw sheets go. Empty uses data/generated/work. *Restart to apply.* |
| `source` | string | `image` | Which producer makes a sprite: the sheet API behind the command, or the pixel DSL compiler. One of `image` or `dsl`. |
| `dslAttempts` | int | `5` | Compile/describe attempts a designer agent gets when the DSL is the source. Must be at least 1. A value outside the range is reported and replaced. |

**This is the one part of the art feature that spends money unattended.**
It is off unless turned on deliberately, and capped twice:
`maxRequestsPerSession` bounds the session, `batchDelaySeconds` decides how
many creations share one request. One request costs the same whether it
carries one tile or nineteen, so a short delay is what makes a burst cost
one request.

`command` is a command rather than a linked-in backend on purpose: image
tooling here is Python plus ImageMagick, and the local-LoRA path has to
stay swappable behind the same seam. `{worklist}` is replaced with the path
the game writes, and the command must write `<worklist>.result.json`.
Empty uses the built-in command, which runs `tools/gen_tile_art.py` under
`nix-shell`.

`source` is a *source*, not a mode: `image` and `dsl` both write the same
PNG through the same placement and hot-swap, so everything downstream is
shared. `dslAttempts` is much higher than the image path's
`designer.maxDesignAttempts` because a DSL compile is free and instant,
and `dsl` ignores `command`, `batchDelaySeconds`, `maxCellsPerRequest` and
`maxRequestsPerSession` entirely.

Batching is not an optimisation here, it is the point. Measured: **1,290
image tokens per request regardless of how many cells it fills**, so one
request for four tiles costs what one request for one tile costs, while one
request *per tile* is roughly 19x the cost per asset. The queue never blocks
a frame: a request runs on a worker thread and the sprite is swapped in from
a main-thread drain when it lands. A tile therefore draws as its ASCII glyph
until the art arrives; that glyph is a legitimate rendering, not a
placeholder.

**Point a test run at a throwaway `art.pack`.** The generator writes to
whatever `spritesDirectory` the worklist names, which is derived from
`art.pack`, so a test with the real pack overwrites the real generated
sprites. `map.file` is used verbatim, so an absolute path is the reliable
spelling for a map outside the repo.

The CLI modes that prove the live path, all free with a stub command:

| mode | what it proves |
|---|---|
| `--tile-art <name> --tile-art-queue` | request, batch window, worker,
main-thread drain and the render entities carrying a `Sprite`. `--tile-art-hold
<n>` draws the pipeline panels for n frames against the settled job. |
| `--art-cache-test` | a stage file rewritten in place reloads instead of being
served from the panel cache. |
| `--paint-memo-test <tile>` | art that appears *after* the cell was painted is
noticed. The tile must be on the map with a generated sprite and no pack
sprite, which is what makes its absence observable. |

---

## `debug`

Debug-window state, the ECS REST server and log retention.

| field | type | default | meaning |
|---|---|---|---|
| `windowStateFile` | string | `./debug_windows_state.json` | Where debug-window visibility is written on shutdown and read on start. *Restart to apply.* |
| `restServer` | bool | `true` | Starts flecs' unauthenticated ECS REST server. Turn off on a shared machine. *Restart to apply.* |
| `logMaxEntries` | int | `1000` | Entries the Debug Log keeps before dropping the oldest. *Restart to apply.* |

---

## Wired vs. not wired

Everything in the tables above is read **and used**. These are the knobs
that still live only in `include/Defaults.h` and are *not* in
`settings.json`. They are listed so the boundary is explicit; adding one
means adding a field to `GameSettings` and a row to `SettingsSchema.h`.

| area | constants still hardcoded |
|---|---|
| physics / movement | `DEFAULT_WAYPOINT_ACCEL`, `DEFAULT_PLAYER_FRICTION`, `DEFAULT_ENTITY_FRICTION`, `DEFAULT_VELOCITY`, `DEFAULT_MINIMUM_VEL_FOR_WALL_REPEL`, `DEFAULT_WALL_REPEL_FORCE`, `DEFAULT_MINIMUM_WAYPOINT_SPEED`, `DEFAULT_MINIMUM_WAYPOINT_ACCEL`, `DEFAULT_MINIMUM_WAYPOINT_DISTANCE`, `DEFAULT_MINIMUM_SPEED_FOR_SLOWING_RADIUS` |
| perception / interaction | `DEFAULT_OBJECT_SEARCH_RADIUS`, `DEFAULT_PLACEMENT_MAX_DISTANCE`, `DEFAULT_HARVEST_DURABILITY_COST` |
| input | `DEFAULT_INPUT_MOVEMENT`, `DEFAULT_INPUT_ZOOM`, `DEFAULT_MINIMUM_INPUT_ZOOM`, `DEFAULT_MAXIMUM_INPUT_ZOOM`, `DEFAULT_DOUBLE_CLICK_TIME` |
| hitboxes / visuals | `DEFAULT_PLAYER_HITBOX_*`, `DEFAULT_PLAYER_VISUAL_*`, `DEFAULT_ENTITY_HITBOX_*`, `DEFAULT_ENTITY_VISUAL_*` |
| HUD / chat colours | `DEFAULT_ACTION_BAR_*`, `DEFAULT_FLOATING_TEXT_*`, `DEFAULT_COLOR_*`, `DEFAULT_SEE_INVENTORY_OPTION`, `DEFAULT_SHOW_STATUS_OPTION` |
| AI timing | `DEFAULT_EMPTY_RESPONSE_RETRY_MS`, `DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS` |
| render target | `gameTexture` is still created at `1920x1080` and `Draw()` still blits it to a `1920x1080` destination; `virtualWidth`/`virtualHeight` (`640x512`) are still fixed in `Game::Init`. |

Two related files are *not* settings:

- `debug_windows_state.json` is runtime state (which windows are open),
  written by the game. Only its **path** is configurable.
- `openrouter_debug.log` is AI debug output, not configuration.
