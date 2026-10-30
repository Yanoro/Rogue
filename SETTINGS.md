# AIRogue settings reference

The game reads one JSON file at startup: `settings.json` in the working
directory. It is loaded by `LoadGameSettings()` in `src/Settings.cpp`, before
`Game::Init()`, and every subsystem reads the resulting object through
`Settings()` (`include/Settings.h`).

Nothing here changes the compiled-in defaults in `include/Defaults.h`: a missing
key, a missing file, or a malformed file leaves the default in place. A file that
fails to parse prints a reason on stderr and the game starts with defaults.

## Load order

1. `settings.json` (tracked).
2. `settings.local.json` beside it, if present. Merged **on top** with an
   RFC 7396 merge patch, so it only needs the keys it overrides. This file is
   gitignored and is where `model.apiKey` belongs.
3. `model.json`, read **only** for `model.apiKey` when the merged result still
   has no key. This is a migration fallback for checkouts that predate
   `settings.json`; it can be deleted once the key lives in
   `settings.local.json`.

Command line:

| invocation | effect |
|---|---|
| `./AIRogue` | `settings.json`, and the map it names |
| `./AIRogue maps/Map_Village.json` | `settings.json`, map overridden |
| `./AIRogue --settings other.json` | a different settings file |
| `./AIRogue --settings other.json maps/Map_Village.json` | both |

An unknown top-level key (such as `_readme`) is ignored.

---

## `map`

| field | type | default | meaning |
|---|---|---|---|
| `file` | string | `./maps/Map_Farm.json` | Map loaded at startup. A command-line map path overrides it. |
| `spawnNPCs` | bool | `true` | Spawn the map's authored NPCs. `false` gives an empty world for map editing. |
| `mapsDirectory` | string | `./maps` | Directory the **Map Reload** debug window lists maps from. |

`map.file` is also what the **Map Reload** window's *Make current map default*
button writes: it edits this one value in place, so the map on screen becomes the
map the next launch starts on. The rest of the file is left exactly as written.
If `settings.local.json` sets `map.file`, that file still wins at startup, and
the window says so after the write.

## `model`

Read by `Game::ECSInit` when it builds the `AI` backend.

| field | type | default | meaning |
|---|---|---|---|
| `type` | string | `"ollama"` | Backend: `ollama`, `gemini` or `openrouter`. Anything else is treated as ollama. |
| `model` | string | `"llama3"` | Model id passed to the backend. |
| `apiKey` | string | `""` | Key for `gemini` / `openrouter`. **Leave empty here**; put it in `settings.local.json`. |
| `endpoint` | string | `http://localhost:11434/api/generate` | Ollama endpoint. Ignored by the other backends. |
| `temperature` | number | *absent* | Sampling temperature. Absent means the provider's own default is used. |
| `maxTokens` | int | *absent* | Output token cap. `max_tokens` is also accepted, for `model.json` compatibility. |
| `provider` | object | *absent* | Passed through verbatim to OpenRouter as its `provider` routing object. |
| `keepAlive` | string | `""` | Ollama only: how long the model stays resident after a request, e.g. `"30m"`. Empty sends nothing and defers to Ollama's own default (5 minutes, or `OLLAMA_KEEP_ALIVE`). |

A hosted `type` with an empty `apiKey` logs a warning at startup instead of
failing silently on the first NPC request.

## `vision`

The second, independent backend behind the **Vision Chat** window (debug menu →
`Vision Chat`): it sends a screenshot of the game plus a question to a local
vision model. Read by `Game::ECSInit`, which builds it through the same helper as
`model`.

It is a separate backend rather than a flag on `model` because `OllamaAI` keys its
server-side conversation context by `contextId` alone: pointing a text model and a
vision model at one instance would replay one model's context into the other's
request. The NPC text backend is completely unaffected by this section.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `true` | Build the vision backend. `false` leaves the window usable but reporting that no backend is configured. |
| `model` | string | `"moondream"` | Ollama model tag. Write `moondream:latest` (or whatever `ollama list` shows) when the tag differs. |
| `endpoint` | string | `http://localhost:11434/api/generate` | Ollama endpoint. Only the `/api/generate` payload shape is built, so pointing this at `/api/chat` will not work. |
| `temperature` | number | *absent* | Sampling temperature. Absent means the model's own default. |
| `keepAlive` | string | `"30m"` | How long Ollama keeps the vision model resident. See below. |

`keepAlive` defaults to warm because the alternative is misleading measurements: a
vision model is several GB, and Ollama's own default unloads it after 5 idle
minutes, so the first question after a pause pays the whole load — tens of seconds
that look like slow inference but are disk I/O. Set it to `""` to send nothing and
let Ollama (or `OLLAMA_KEEP_ALIVE`) decide. The cost is VRAM/RAM held while idle.

Every answer in the window is followed by a timing breakdown taken from Ollama's
final streaming chunk, e.g.:

```
timings: total 37.0s | load 0.0s | read 1280 tok in 18.2s (70.3 tok/s) | wrote 45 tok in 6.1s (7.4 tok/s)  -> mostly reading the image
```

The trailing "mostly …" names the dominant phase, because each cause needs a
different fix: `load` means a cold model (raise `keepAlive`), `reading the image`
means the screenshot is expensive (send a smaller one), and `writing the answer`
means the model is being verbose (cap `num_predict`). Only Ollama reports this; on
another backend no line is shown rather than an invented one.

Only `ollama` is supported for vision today; the other providers ignore the images
rather than mistranslating them into the wrong wire format (see `include/AI.h`).

Captures are written to `logs/<run-start>/vision_capture_<timestamp>_<n>.png` unless the
window's "Save capture" box is unchecked, so what the model was shown can be
checked independently of its answer.

## `ai`

| field | type | default | meaning |
|---|---|---|---|
| `npcPromptFile` | string | `""` | Path to a text file holding the NPC system prompt template. Empty uses the compiled-in `DEFAULT_NPC_PROMPT` (`include/AgentBrain.h`). |

The template keeps the same placeholders `Game::createNPC` substitutes today:
`%COMMANDS_LIST%`, `%LOCATIONS%`, `%BACKGROUND%`, `%CHARACTERS%`. Substitution
runs identically for an authored template, so a custom prompt cannot silently
lose the live command list.

## `designer`

The Designer Chat window's backend and its speech. The whole section may be
absent: `useMainModel` then keeps the designer on the NPC backend, which is the
least surprising default for an unconfigured run.

| field | type | default | meaning |
|---|---|---|---|
| `useMainModel` | bool | `true` | Share the `model` backend. `false` uses the `model` sub-object below instead. |
| `model` | object | — | Same shape as `model`. Read only when `useMainModel` is `false`. Producing valid blueprint JSON is a different skill from roleplay, so it is worth pointing at a different model without retuning the NPCs. |
| `promptFile` | string | `"data/blueprints/DESIGNER.md"` | The designer's instructions: grammar, reply contract, worked examples. Empty uses the compiled-in prompt. |
| `maxDesignAttempts` | int | `3` | How many times the engine re-prompts after an invalid design. Values below 1 are clamped to 1. |
| `maxTokens` | int | `0` | The designer's own output budget, overriding the `maxTokens` of whichever backend it ends up on. `0` inherits, which is the old behaviour. Needed because the designer and the agents share a backend by default but not a shape of answer: an agent turn emits one command line, while a set of eight object definitions with art and notes is 1300–1500 tokens before reasoning spends any. Honoured by the OpenRouter backend, the only one `model.maxTokens` applies to. Negative values are read as `0`. |

### `designer.tts`

Reading the designer's reply aloud. Off in the compiled-in defaults, so a config
with no `designer.tts` block stays silent; `settings.json` turns it on.
| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `false` | Build the speech backend at startup. `false` leaves the window's Mute button disabled and never shells out. |
| `binary` | string | `"piper"` | The piper CLI. A bare name is resolved on `PATH`, which `nix-shell` provides; an absolute path also works. |
| `voice` | string | `"models/tts/en_GB-alba-medium.onnx"` | The voice model. Piper resolves `<voice>.onnx.json` beside it for the sample rate, so both files must be present. |
| `volume` | float | `1.0` | Playback gain. Clamped to `0..1`: above 1.0 raylib clips, which sounds like a broken voice rather than a loud one. |
| `content` | string | `"notes"` | What gets read out. See below. |

`content` exists because a designer reply is a bad thing to read literally: most
of its bytes are the blueprint JSON, and its prose restates its notes.

| value | spoken |
|---|---|
| `notes` | The verdict (`Saved as <id>.`) plus the model's own `designerNotes` — its critique, which is the reason the window exists. |
| `prose_and_notes` | The above plus the prose that precedes the JSON block. Roughly doubles the length for little new information, since the notes summarise the prose. |
| `everything` | The verdict plus the whole reply, JSON included. For debugging the speech pipeline only: the JSON is read as "id two bedroom house, name two bedroom house, rect_fill". |

Anything else warns and falls back to `notes` — never to `everything`, which
would read the JSON aloud.

Only the *final* attempt is spoken: a saved design, a refusal that exhausted
`maxDesignAttempts`, or a transport failure. Intermediate refusals that are about
to be retried stay silent, so a retry does not read three blueprints and two
refusals interleaved.

### The playback bar

Every reply that was spoken carries its own bar under it in the transcript:
play/pause, a draggable position, `elapsed / total` seconds, and stop. It is
independent per reply — starting one stops whatever else was playing.

- **`preparing...`** replaces the time while the engine is still synthesizing
  (~1 s warm, but several seconds on the first reply of a session, when the voice
  model is being read from disk for the first time).
- **Dragging the position** on a reply that has never played loads it, paused, at
  that point. You do not have to press play first to find a spot.
- **Stop** rewinds and keeps the audio; it does not discard the reading.
- **A `Speak` button** appears instead of the bar when there is no audio for a
  reply — it was never spoken (speech was off or muted when it arrived), the
  engine produced nothing, or the audio was dropped by the retention cap. The
  tooltip says which.

The last 24 spoken replies keep their audio (~1–2 MB each, in memory only —
nothing is written to disk, so bars do not survive a restart and a previous
session's transcript cannot be replayed). Older ones keep their bar but revert to
the `Speak` button.

**Mute** in the window header is global and stops everything immediately. It is
the only way to say "do not speak again this session"; per-reply stopping is the
bar's job.

### Downloading a voice

Voice models are not part of the `piper-tts` package and are not in the repo
(`models/` is gitignored — a medium voice is ~63 MB). One fetch, once:

```sh
python3 -m piper.download_voices en_GB-alba-medium --data-dir models/tts
```

Anything in the [piper voices catalogue](https://huggingface.co/rhasspy/piper-voices)
works; point `voice` at the `.onnx`. The two files must keep their names, because
piper finds the config by appending `.json`.

If the voice or the binary is missing, the game logs a warning and runs
silently — a missing voice is an unfinished download, not a broken install. The
Designer window shows `speech: off` in that case.

### If there is no sound

Check the Designer window's status line before changing anything else:

| what it says | meaning |
|---|---|
| `speech: off` | No engine. `designer.tts.enabled` is false, or the voice model or the `piper` binary was not found — hover the orange `(?)` for the exact reason. |
| `speech: <engine>` + orange **`SILENT`** | The engine works and raylib has **no real audio backend**: it fell back to miniaudio's null device, which reports success and discards every sample. Hover the marker for the details. Run the game from `nix-shell` (`./compile_and_run.sh`) or rebuild, since `CMakeLists.txt` links the audio libraries into the binary for exactly this reason. |

A `SPOKEN: …` line in `logs/<run-start>/designer/<timestamp>-designer.txt` means the text was
handed to the engine. It does **not** mean anything was audible — that line is
written before playback, and it was written for hours of debugging during which
raylib was silently discarding the audio.

## `system1`

The System 1 backend: the typed-classifier layer described in
`docs/agent-attention-design.md`, whose calls the **System 1 Overview** window
displays. The whole section may be absent, and every field in it may be absent.

System 1 is a *different model class* from the System 2 chat model — state and
questions in, calibrated answers out — and the models good at it are not the
models good at roleplay. It is also the layer most likely to be swapped or
dropped outright, which is what the overview window exists to decide, so pointing
it elsewhere must not mean retuning the NPCs.

> **Partially consumed.** `maxRecords` sizes the recorder and `model.model` /
> `model.apiKey` configure the one System 1 provider that exists
> (`SystemOne::JevClassifier`, a decisions-API client). A real call site now exists:
> the sprite designer's **router** (`SpriteRouter`) asks which kind, shape and size a
> request is, gated by `routeRequests` below. `useMainModel` and `disableReasoning` are
> still parsed and documented but not read — the provider's `Ask` does not consult them.
> The overview window is legitimately empty until some call site records, and
> `--systemone-test` / `--sprite-route` are how the provider itself is verified.

| field | type | default | meaning |
|---|---|---|---|
| `useMainModel` | bool | `true` | Share the `model` backend. `false` uses the `model` sub-object below instead. |
| `model` | object | — | Same shape as `model`. Read only when `useMainModel` is `false`. **An empty `model` inherits the main model's id**, unlike every other `ModelSection`, because `ModelSection` defaults the field to `llama3` — which a decisions endpoint rejects with `Model llama3 does not exist`. |
| `disableReasoning` | bool | `true` | Ask the shared backend for a single non-reasoning answer. On by default, and the opposite of the designer's rewriter: System 1's premise is a cheap one-shot classification, and a reasoning model handed a classifier's question spends its budget thinking and returns a worse-calibrated answer — or none. |
| `maxRecords` | int | `500` | How many records the overview window keeps in memory. Values below 1 are clamped to 1. The **log file is unaffected** and always keeps everything, so a long session stays fully recorded. |
| `timeoutSeconds` | int | `30` | Whole-request budget for one decision. A decision model answers in ~100ms, so a request still running after this is not going to produce a useful verdict — and a designer waiting on a route must not block on a hung socket. Values below 1 are clamped to 30. **Absent is not the same as 30:** when the key is missing and the endpoint is a local server (`127.0.0.1`, `localhost`, `[::1]`), the budget becomes **120s**, because a 4B model on a CPU takes seconds to tens of seconds (measured: ~10s for three short questions, ~42s once the state carries an object-capability list). Setting the key explicitly always wins. This needs `hasTimeoutSeconds`, tracked alongside the value, since a non-zero default otherwise makes "absent" and "set to 30" indistinguishable. |
| `routeRequests` | bool | `true` | Whether the sprite designer asks a classifier what kind of sprite a request is for, instead of starting in whichever mode the window already shows. On by default: a classification is a *decision* call costing a fraction of a cent, and without it the user has to pick the mode before typing, which is the thing the router removes. The image request that follows is separately gated by `art.generation.enabled`. |
| `minConfidence` | float | `0.0` | Below this, the window keeps its current mode and reports the classifier as unsure rather than acting on it. Mostly advisory: measured against hosted Jev, `Choice` answers *did* report confidence (kind ~0.74–0.98, shape ~0.54, size ~0.20), so a threshold above 0 now has something to work with — but it is not a calibrated probability and a high value will send most verdicts to "unsure". |

`system1.enabled` does not exist: whether System 1 is *on* is a property of the
call site, not of the backend.

The router's three questions are chosen so each is answerable from the request alone:
`kind` is a `Choice` of `tile`/`object`, `shape` a `Choice` of `single`/`set`, and `size` a
`Choice` of `small`/`medium`/`large` (≈ 4/8/12 sprites) — read only for a set. `size` began
as a numeric `Value` question and was replaced after it answered wrongly on four of five
real requests; see `docs/sprite-designer-design.md` §17. `--sprite-route "<text>"` runs one
classification and prints the verdict **and the raw answers**, which is how that was found.

### Profiles: switching the System 1 model at runtime

`system1.profiles` lists the backends the game can switch between while it is
running. The flat fields above are **always available as a profile called
`default`**, which is why this is additive: a config with no `profiles` array
behaves exactly as it did before profiles existed.

```json
"system1": {
  "maxRecords": 500,
  "activeProfile": "kev-local",
  "profiles": [
    { "id": "kev-local",  "label": "Kev-4B (this machine)", "preset": "local_kev",
      "minConfidence": 0.45 },
    { "id": "kev-strict", "label": "Kev, strict threshold",  "preset": "local_kev",
      "timeoutSeconds": 90, "minConfidence": 0.95 },
    { "id": "hosted",     "label": "Hosted Jev",             "preset": "hosted_jev" }
  ]
}
```

| profile field | type | default | meaning |
|---|---|---|---|
| `id` | string | — | **Required.** The name used by `activeProfile`, `--system1-profile` and every status line. An entry without one is skipped with a warning, because it could never be selected. |
| `label` | string | the `id` | Display name for the picker. |
| `preset` | string | `""` | `"local_kev"` or `"hosted_jev"`: seeds endpoint, model, key and timeout before the fields below are applied. Empty means explicit fields only, which is how a profile points at a Lev/Laya/OpenJev/remote `llama-server`. An unknown preset is reported and ignored rather than silently treated as hosted. |
| `model` | object | — | Same shape as the top-level `model`. **Absent fields keep the preset's value**, unlike every other `ModelSection`: the section's defaults are Ollama's chat endpoint and model `llama3`, so leaving them in place would override a preset with a chat backend and fail as "model does not exist". |
| `timeoutSeconds` | int | `0` = follow preset / endpoint | `0` means "not stated": the preset's budget, otherwise 120 s for a local endpoint and 30 s hosted. |
| `minConfidence` | float | `0.0` | Below this, a route verdict is reported as unsure and the window keeps its mode. Per profile because it is calibration, which does not transfer between models — measured here, Kev reported ~0.68 where hosted Jev reported a different spread for the same questions. |

Global, not per profile: `maxRecords`, `routeRequests`, the log path and the
overview window. Those describe the game, not the endpoint.

**Which profile is active.** Highest precedence first:

1. `--system1-profile <id>` — a request about *this* run.
2. The selection persisted in `debug_windows_state.json` (`systemOneProfile`), so a
   restart resumes on the model you were using.
3. `system1.activeProfile` in this file.
4. `default`.

Every step is skipped when it does not name an existing profile, so deleting a
profile — or a typo — falls through instead of leaving the game with no backend.

**Switching** happens in the System 1 Overview window (the backend picker at the
top). A switch is immediate and **cancels any request already in flight**, which
appears as an error on that one record rather than as a silent loss. Before
committing it probes the target: a profile with no key, or an endpoint that
refuses the connection, is refused and the current backend stays. A local
`llama-server` that is still loading its model accepts the connection and
therefore stays selectable — the first answer may simply take a while.

Switches are recorded as `EVENT` lines in the System 1 log, so a run whose backend
changed mid-way is explained rather than looking like inconsistent model labels.

### The endpoint is a decisions endpoint, not a chat endpoint

`model.endpoint` defaults to `http://localhost:11434/api/generate`, which is
**Ollama's chat** endpoint. A decisions API is a different API on a different
host, so the provider ignores that value and uses the decisions default unless a
system1 endpoint was explicitly configured. Setting `system1.model.endpoint` to
an `/api/generate` URL is treated as "not configured" rather than honoured.

The default is OpenRouter's `https://openrouter.ai/api/alpha/decisions`, which
reaches every decision model with an ordinary OpenRouter key and no separate
TypeSafe account. `model.model` defaults to nothing that works on its own, so
`settings.json` names `~typesafe/jev-latest`; point it at any of the other
decision models on the same endpoint to swap backends without touching code.

### `system1.navigation`

Resource and prompt bounds for `[NAVIGATE]`, the verb that lets the main model delegate
"which move next" to System 1 (`docs/navigation-subagent-design.md`). A move is a local
compass direction (`here:east`) or a remembered place (`p12`). There is deliberately no
per-episode decision cap and no back-and-forth stop: the strategist/pilot decide when to
stop, and a repeated target is a soft `REPETITION` signal rather than a `Finish`.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `true` | When false, `[NAVIGATE]` reports that it is off and does nothing. |
| `maxCandidates` | int | `8` | Action lines shown per ask: local compass moves plus distant places. More than this turns the choice into reading comprehension for a small model. |
| `maxDirections` | int | `4` | Local compass moves before the distant list. One slot is reserved for the nearest adjacent place, so a wide current place cannot crowd out the way out. |
| `compassRadius` | int | `6` | How far a sector looks for the first place. A place beyond this belongs in the distant list with its own distance. |
| `examineRadius` | int | `22` | Radius of the examine affordance list. Defaults to the standard visual range. The single navigator lists every observed examinable object inside it; the two-context S1 caps the list with `maxExamine`. |
| `maxExamine` | int | `2` | Two-context only: maximum examine candidates, reserved inside `maxCandidates`. The single navigator uses grouped object actions when `navigation.single.objectSummary` is true; `objectSummary=false` restores its old per-instance list. |
| `maxSurveys` | int | `4` | Full-circle recovery budget per episode. One per unsure decision is enforced separately; a failed survey still hands to the strategist, so this is a cheap recovery bound, not a navigation stop. |
| `maxSeconds` | float | `600` | Wall-clock resource guard for the whole episode, including model latency and walking. It is not a navigation decision; the strategist/pilot stop normally on arrival, none, or give-up. |
| `legTimeoutSeconds` | float | `60` | One leg that cannot finish in this long is abandoned and that target cell is excluded for the episode. A hang guard, kept even though the decision caps are gone. |
| `minProbability` | float | `0.3` | Below this, the chosen option is treated as "not sure" rather than acted on. A choice over eight or ten actions rarely puts half its mass on the top one; the margin is the sharper test. |
| `minMargin` | float | `0.15` | For distant-place choices, the winner must beat the runner-up by this much; a 0.51/0.49 split is a coin flip. Local compass moves are reversible and use only `minProbability`, so a torn east/west does not freeze the character. |
| `mapMode` | string | `"off"` | S1 spatial-input experiment. `off` = current sector prose (unchanged default); `append` = prose + ASCII map + map target list; `replace` = ASCII map + compact target list instead of the `SURROUNDINGS`/`FRONTIER` prose (the map-only baseline). |
| `mapRadius` | int | `6` | Radius of the S1 ASCII map: 13×13 at 6, 17×17 at 8, up to 25×25 at 12. Only used when `mapMode` is not `off`. |

The map is agent-centred and north-up. `@` is the character; `.` open, `#` blocked, `~`
water, `T` tree, `B` building, `S` sign, `C` character, `?` unknown. Semantic places
fill open ground with their initial (`V` Village, `R` road, `O` orchard, `P` other).
Candidate targets are digit-marked and repeated in `MAP TARGETS`; the map is context,
while the target list/`ACTIONS` remain the choice vocabulary. A/B `off` against
`replace` first; use `append` if the prose still helps alongside the map.

### `navigation`

| field | type | default | meaning |
|---|---|---|---|
| `agent` | string | `"single"` | Navigation architecture. `single` = one main-model context that returns the plan and the next move in one decision call. `two` = the previous strategist + Jev pilot (the control/rollback). |

### `navigation.single`

The single-context navigator (`docs/llm-s1-navigation-design.md`). It uses the **main AI
backend** with its own `navsingle-<entity>` context and a full episode history: the map,
graph, plan, route and previous results are all in the same conversation. Each decision
returns an updated plan and the next move.

| field | type | default | meaning |
|---|---|---|---|
| `maxTokens` | int | `500` | Output budget for one decision (think + plan + move). The previous 350 was hit mid-JSON by phi-4 mini on a long think; 500 leaves room for the strict two-sentence think plus the object. Raise it for more planning at the cost of latency. Sent as `num_predict` on Ollama. |
| `useMainModel` | bool | `true` | When `dedicatedBackend` is `true`, use the top-level `model` settings for navigation. `false` reads `navigation.single.model` instead, which is how a local Ollama navigator or a different hosted model is selected. |
| `model` | object | `{}` | The navigator's own model block when `useMainModel` is `false`. Same shape as the top-level `model` block (`type`, `model`, `endpoint`, `apiKey`, `keepAlive`, `temperature`, `provider`). `type` defaults to `ollama`. |
| `dedicatedBackend` | bool | `true` | Build a separate AI instance for the single navigator instead of sharing the NPC backend. The separate instance is what makes `disableReasoning` and the navigator's token budget safe. `false` reproduces the previous shared-backend behaviour. |
| `disableReasoning` | bool | `true` | Turn the dedicated navigation backend's chain-of-thought off. OpenRouter enables reasoning by default on the models used here, and reasoning can consume the whole completion budget before any JSON is emitted. Ignored when `dedicatedBackend` is `false`. |
| `compactJson` | bool | `true` | Send compact JSON for the observation and plan in the single prompt. The debug window and transcripts keep the pretty form. `false` reproduces the previous pretty-printed prompt for A/B comparison. |
| `historyTurns` | int | `3` | Previous decision turns kept in the single navigator's context. The current state already carries the route and last result. `0` keeps the whole episode (the previous behaviour); useful for measuring the prompt-growth cost. |
| `concise` | bool | `true` | Ask for a 1–2 sentence think block, a one-sentence strategy and a short subtask instead of the previous “2–6 sentences” and “a few sentences”. `false` reproduces the older prompt. |
| `maxAttempts` | int | `3` | Consecutive unusable replies allowed for one decision. Any model-output error counts: invalid JSON or field types, an invalid plan, a missing `move`, or a move that is not an offered candidate. An accepted reply (a move, `look_around`, or `arrived`) resets the counter. On the cap, `fallbackToTwo` decides whether the episode switches to the two-context navigator or returns to the main LLM. |
| `maxDirections` | int | `8` | Local compass moves offered to the main LLM. All eight sectors are shown when they have a move; the two-context S1 keeps `system1.navigation.maxDirections` (4). |
| `objectSummary` | bool | `true` | Use grouped object profiles and the `objects:all` / `inspect:<group>` / `approach:<alias>` / `<verb>:<alias>` grammar. `false` restores the old per-instance `examine:<entityId>` list for A/B comparison. |
| `maxObjectGroups` | int | `6` | Object groups shown in the default STATE block. The rest remain available through `objects:all`; `inspect` still works on any group. |
| `maxGroupDetails` | int | `8` | Instances listed by `inspect:<group>`. When the cap is reached the result tells the model to use `inspect:<group>:all` for the rest. There is no separate detail-request cap. |
| `maxCandidates` | int | `12` | Total single-navigator movement options: up to eight local moves plus four distant places. Object actions use the separate target grammar, not this list. |
| `timeoutSeconds` | float | `60` | One decision's ceiling. On expiry the episode switches to the two-context navigator when `fallbackToTwo` is on. |
| `fallbackToTwo` | bool | `true` | On timeout, transport failure, a failed leg, or exhausting the `maxAttempts` reply budget, switch to the strategist + Jev navigator for the rest of the episode. |
| `mapRadius` | int | `6` | Radius of the ASCII map in the single prompt: 13×13 at 6, up to 25×25 at 12. |

The single navigator uses its own AI instance by default. `useMainModel` chooses whether that
instance reads the top-level `model` block or `navigation.single.model`; the shipped
`settings.json` points it at a local Ollama `phi4-mini:latest`. `dedicatedBackend: true` keeps
that instance separate from the NPC backend, so `maxTokens` (sent as `num_predict` on Ollama)
and reasoning options do not affect NPC requests. Set `dedicatedBackend: false` to share the
NPC backend, or toggle `compactJson` / `historyTurns` / `concise` for A/B tests.

Example local navigator:

```json
"single": {
  "useMainModel": false,
  "model": {
    "type": "ollama",
    "model": "phi4-mini:latest",
    "endpoint": "http://localhost:11434/api/generate",
    "keepAlive": "30m"
  }
}
```

Changing `model.model` is enough to test a different Ollama tag. `keepAlive` keeps the model
resident between decisions; loading a multi-GB model on the first request otherwise dwarfs
inference time.

The single navigator receives the JSON grid + graph, the ASCII map with target digits and
the action/state list. It chooses one action key; the engine resolves the target cell,
walks the leg and validates arrival. `navigation.agent: "two"` switches back to the
previous version without rebuilding.

### `navigation.strategist`

The standard-LLM strategist above the System 1 pilot (`docs/llm-s1-navigation-design.md`).
It uses the **main AI backend** with its own navigation context id; there is no separate
provider/model block in Stage 1. `[NAVIGATE]` still uses `system1.navigation` for the pilot's
budgets; these fields bound the outer layer. The strategist's time is part of the episode's
`system1.navigation.maxSeconds`.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `true` | When false, `[NAVIGATE]` runs the S1-only navigation. |
| `fallbackToS1` | bool | `true` | If the strategist is unavailable or invalid, keep the last accepted strategy (monitors dropped) and continue with the S1; the strategist is asked again on the pilot's next uncertainty/no-move. When the strategist call budget is exhausted, the episode ends with a context summary that keeps the last strategy and route. |
| `maxStrategies` | int | `12` | Strategist calls per episode: the initial plan, re-plans, and retries. A cost guard, not a navigation-decision cap; the strategist's own give-up is the normal stop. |
| `maxAttempts` | int | `3` | Attempts per strategist request: the first call plus retries after a transport failure or an invalid reply. At 3, a failed request is retried twice before control returns to the main model. |
| `maxMonitors` | int | `3` | Monitor questions per plan. Extra monitors in a reply are truncated; the S1 is a small model and a long list degrades its local choice. |
| `monitorMinConfidence` | float | `0.6` | The minimum confidence for a monitor answer to **trigger** a handoff. A Score answer's confidence is `2*|p-0.5|`: 0.5 is unknown, 0 or 1 is certain. An unknown/low answer is recorded in the debug view but does not hand back and does not block the pilot's move; a confident answer matching `handoff_when` does. The pilot's own uncertainty/no-move still asks the strategist. A monitor's own `min_confidence` overrides it. |
| `timeoutSeconds` | float | `30` | One strategist call's ceiling. On expiry the call is abandoned and the S1 fallback runs. |
| `observation.localGridRadius` | int | `8` | Radius of the observed JSON grid around the character. |
| `observation.maxGraphNodes` | int | `24` | Cap on the minimal observed graph's nodes (places, objects). |
| `observation.maxGraphEdges` | int | `48` | Cap on the graph's edges. |
| `observation.maxTokens` | int | `2000` | Reserved for Stage 2's envelope; Stage 1 enforces the grid/graph caps instead. |

The observation passed to the strategist is observed-only: the `SpatialMemory` cells, places and
objects plus the episode's route. It never contains the authored map, place seeds, or an unobserved
place. The monitor questions and the S1's state are described in
`docs/llm-s1-navigation-design.md` sections 7–10.

### Records on disk

Every recorded call is also appended to
`logs/<run-start>/system1/<run-start>-system1.txt`. The file is created on the
first record, so a run that never calls System 1 leaves nothing behind. A failure
to create it is reported through the game's debug log and is not fatal — the
session still runs, it is just not recorded.

### Per-session log layout

Every game run gets one session directory named with its start timestamp:

```
logs/<run-start>/
  npcs/<Agent>/<run-start>-<Agent>.txt
  npcs/<Agent>/<run-start>-<Agent>-memory.json
  designer/<time>-designer.txt
  system1/<run-start>-system1.txt
  tile-designer/<time>-tile-designer.txt
  navigator/<Agent>.txt
```

NPC folders are unique per agent; duplicate display names get the entity id
suffix. The navigator file is per agent and records the strategist prompt/response
(including partial/cut-off streams), parse errors and retries, the parsed plan,
handoffs, the S1 state/answer, legs and the final summary. This is the log to read
when a strategist reply is invalid or truncated. Vision captures and other
non-navigator artifacts stay at the session root. Old flat `logs/*.txt`,
`logs/system1/` and `logs/tile-designer/` runs remain as historical records; new
runs use the session directory.

## `perception`

The acquisition field used by every character's `Vision` (`include/Components.h`,
`include/VisionQuery.hpp`). Named `perception` because `vision` above is the independent Vision Chat
AI backend. Values are applied when `Game::AttachPerception` runs at spawn; the player's Vision Debug
sliders can tune the live player independently, and the same component is attached to NPCs.
Acquisition is always line-of-sight and observed-only: this changes what enters `SpatialMemory`, not
what the engine reveals.

| field | type | default | meaning |
|---|---|---|---|
| `range` | float | `22` | Reach in tiles. With `angleDegrees: 360` this is an all-round radius; with a cone it is the forward reach. |
| `angleDegrees` | float | `360` | Full field width. `360` = all-round (the default, to isolate navigation from perception); `90` = the forward-cone variant. |
| `radarRadius` | int | `2` | All-round near ring in tiles. At `angleDegrees: 360` it is redundant; it keeps the one-to-three-tile range reliable in cone mode. |

The circular default at range 22 is about 1,500 cells per sweep; the 90-degree cone plus the
2-tile radar is about 390. A smaller circular baseline can be set with, for example,
`"range": 8, "angleDegrees": 360`, which matches the navigator's 8-tile local grid. For the original
forward-cone variant use:

```json
"perception": { "range": 22, "angleDegrees": 90, "radarRadius": 2 }
```

## `regions`

Observed base regions (`docs/region-place-design.md`, Stages 2–4). A region is a `KnownPlace` with
`PlaceOrigin::Region`; it lives in `SpatialMemory::regions`, separate from the semantic `places`
that `DerivePlaces` rebuilds.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `true` | Create/update base regions from observed cells. |
| `nearSize` | int | `8` | Near region grid cell size (8×8), within `nearRadius`. |
| `nearRadius` | int | `12` | Chebyshev radius of the near level; nearer observed ground is represented at 8×8. |
| `midSize` | int | `16` | Mid region grid cell size (16×16), between `nearRadius` and `midRadius`. |
| `midRadius` | int | `24` | Chebyshev radius of the mid level. |
| `farSize` | int | `32` | Far region grid cell size (32×32). |
| `farRadius` | int | `48` | Chebyshev radius of the far level's rebuild band. Far regions outside it stay in memory untouched instead of being rebuilt from the whole store every sample. Must be at least `midRadius`. |
| `maxRegions` | int | `64` | Storage cap, independent of `MAX_PLACES` and the model-facing caps. |
| `modelNearCount` | int | `6` | Regions shown by the model-facing view (semantic places are always included; the current region is always included). |

The three levels are parent/child views of the same ground: a near cell appears as 8×8, 16×16 and
32×32; a mid cell as 16×16 and 32×32; a far cell as 32×32. `PlaceQuery::At` returns the finest
region underfoot, and `PlaceQuery::ModelPlaces` hides a coarser parent while a finer selected child
is contained in it. Eviction keeps the current region first, then named regions, then regions on
semantic ground, then near→far and distance. The model-facing caps remain 5 nearby places, 8
candidates and 24 graph nodes; `modelNearCount` bounds only the nearby regions.

Regions claim every observed cell in their grid cell, including walls and water. Their display comes
from the dominant surface through the `regionLabel` field in `data/places.json` (`grass field`,
`sand field`, `marsh`, `rocky ground`, `water`, ...), falling back to “open ground”. The terrain
categories (`grass`, `sand`, `marsh`, `rock`, `path`, `tilled soil`, `packed earth`) also provide the
surface aspects. Regions are nameable with `[NAME_PLACE]` and usable as `[MOVE_TO]` targets once
named. They are observed-only: no authored map cell contributes.

## `window`

| field | type | default | meaning |
|---|---|---|---|
| `title` | string | `"AIRogue"` | Window title. |
| `width` | int | `1920` | Initial window width, and the camera's fit-to-map reference width. |
| `height` | int | `1080` | Initial window height, and the camera's fit-to-map reference height. |
| `fullscreen` | bool | `true` | `false` keeps the windowed size above. |
| `vsync` | bool | `true` | Sets `FLAG_VSYNC_HINT` before the window is created. |
| `targetFPS` | int | `60` | Frame cap. |
| `monitor` | int | `-1` | `-1` = the original rule (monitor 1 when several are connected, else 0). `0` or more forces that display; a nonexistent index warns and falls back to 0. |

## `render`

| field | type | default | meaning |
|---|---|---|---|
| `fontPath` | string | the CGA bitmap font in `./fonts/oldschool/...` | Font loaded for the game. The **Font Selection** window can still override it for the session; its saved choice wins if the file exists. |
| `fontSize` | int | `32` | Font size passed to `LoadFontEx`. |
| `cameraMode` | string | `"follow"` | `"follow"` keeps the camera on the player; `"free"` leaves it to the arrow keys. Anything else warns and uses `"follow"`. |
| `cameraZoom` | number | `0.0` | `0` fits the map to the window (original behaviour). Any positive value is used as the starting zoom directly. |

## `player`

| field | type | default | meaning |
|---|---|---|---|
| `name` | string | `"playerCharacter"` | Entity name of the player. |
| `startX` | int | `48` | Starting tile X. A blocked or out-of-bounds spawn is relocated to the nearest walkable tile by the pass `LoadMap` already runs. |
| `startY` | int | `20` | Starting tile Y. |
| `race` | string | `""` | Race the player's body slots come from. Empty means the human default. |

## `world`

| field | type | default | meaning |
|---|---|---|---|
| `seed` | int (unsigned) | `0` | `0` seeds the RNG from the clock, so runs differ. Any other value makes the run reproducible; the value used is printed to the Debug Log. |

## `content`

Registry roots loaded in `Game::ECSInit`. Point these at a different directory to
run a different content pack.

| field | type | default |
|---|---|---|
| `objects` | string | `data/objects` |
| `recipes` | string | `data/recipes` |
| `stats` | string | `data/stats` |
| `skills` | string | `data/skills` |
| `items` | string | `data/items` |
| `races` | string | `data/races` |
| `tiles` | string | `data/tiles` |
| `generatedTiles` | string | `data/generated/tiles` |
| `tileDesignerPromptFile` | string | `data/tiles/TILEDESIGNER.md` |
| `objectDesignerPromptFile` | string | `data/objects/OBJECTDESIGNER.md` |
| `generatedObjects` | string | `data/generated/objects` |
| `spriteContractFile` | string | `data/prompts/SPRITE_CONTRACT.md` |
| `spriteDSLPromptFile` | string | `data/style/SPRITEDESIGNER.md` | The system prompt for the document turn — the second turn that writes a sprite's pixel document in the sprite DSL (`docs/pixel-dsl.md`). Its `%SPRITE_DSL%` and `%RAMPS%` substitutions come from the compiler itself, so the grammar a model is told and the grammar the compiler accepts cannot drift. A missing file degrades to a built-in prompt. |
| `blueprints` | string | `data/blueprints` |

`tiles` is the **hand-authored** catalog and nothing else. `generatedTiles` is the
generated overlay: tile definitions an agent invented, loaded additively on top of
the catalog at startup and written to by the Tile Designer window. A name in the
overlay cannot shadow a catalog tile — the overlay may only replace its own — and
deleting the directory reverts the world to hand-authored vocabulary exactly.

The two are separate directories rather than nested because `TileRegistry::LoadTiles`
reads every `.json` in one directory and is not recursive: an overlay inside
`data/tiles` would either be loaded as catalog (defeating the split) or never read.
`generatedObjects` is the same arrangement for objects, and for the same reason
(`ObjectFactory::LoadTemplates` is also non-recursive). It is written to by the object
designer and loaded at startup after `objects`, so a created object survives a restart.

`tileDesignerPromptFile` and `objectDesignerPromptFile` are each kind's instructions: the
field or capability grammar, and the vocabulary placeholders that are replaced with the
live world. A missing file is not fatal — the prompt falls back to a document carrying the
shared contract and the vocabulary, so the model is under-informed rather than
unconstrained.

The sprite designer also has a **per-kind rewrite brief** for the Improve button
(`RewriteBriefs::Tile()` / `RewriteBriefs::Object()`), chosen from the window's current
kind. It is not a setting: a brief is code because it describes what the engine can express,
and the two vocabularies do not overlap — an object request "improved" with ground-surface
words would ask for something the object designer cannot build. `AIRogue --rewrite "<text>"
--rewrite-brief tile|object|building` prints the assembled request for each, which is how the
brief text is read without a window.

`spriteContractFile` is the **shared** part of every kind's instructions — the reply
format, the refusal protocol and the prohibitions. It is one file rather than a copy per
kind because this is the part that must not drift: a tile and an object are refused the
same way, or the model has learned two contracts. Each kind document pulls it in where it
writes `%CONTRACT%`.

Placeholders, all replaced **only when present** (nothing is ever appended, so a document
may list a vocabulary by hand and be left alone):

| token | replaced with |
|---|---|
| `%CONTRACT%` | the shared contract document |
| `%VOCAB%` | the live vocabulary: tiles for the tile kind, components for the object kind |
| `%GLYPHS%` | the glyph legend with collisions named (tile kind only) |
| `%TILES%` | the legacy spelling of `%VOCAB%`, still honoured |

Do not name these tokens in a document's prose: a bare mention is indistinguishable from
a slot that still needs filling. Describe what happens instead. `AIRogue --sprite-prompt
<kind>` prints the assembled prompt and names any token that survived, which is how that
rule is enforced rather than hoped for.

## `art`

Where sprites come from. Read by `SpritePaths.cpp`, which resolves a convention id
(`tile_grass`, `altar`, a race id) against these roots **in order**, first hit
wins:

1. `<pack>/sprites/<id>.png` — the art pack, authoritative
2. `<generated>/<id>.png` — the generated layer, strictly additive
3. `data/sprites/<id>.png` — only when `pack` is empty (the pre-split behaviour)

The pack always beats the generated layer, so a generated image can never shadow
hand-authored art, and a pack that later ships the real sprite takes over
silently. An empty `pack` leaves everything exactly as it was before art packs
existed, which is the current default.

| field | type | default | meaning |
|---|---|---|---|
| `pack` | string | `""` | Art pack directory, e.g. `packs/art/pixel_crawler`. Empty means "no art pack": sprites resolve from `data/sprites/` and the generated layer is not consulted, because there is no pack id to key it by. |
| `generated` | string | `""` | Directory this pack's generated sprites are in **directly** (not its parent). Empty derives `<pack's grandparent>/generated/<pack dir name>/sprites`, which is what the documented layout produces with no configuration. |

Also settable per run with `AIRogue --art-pack <dir>`, which overrides `art.pack`.
It must be set before the first sprite resolves — the roots are read once and
cached, since the editor's template palette asks for a candidate list every frame.

See `docs/asset-generation-design.md` for what an art pack is, why the generated
layer lives outside it rather than inside it, and why nothing that models a style
may ever read the generated layer.

### `art.generation`

Live generation of art for tiles an agent invents while the game runs. See
`docs/tile-art-wiring-design.md` for the design.

**This is the one part of the art feature that spends money by itself, so it is off
unless switched on, and capped.** One image request costs roughly **$0.04**, and the
price is **per request, not per tile** — so requests are batched into one, and
`batchDelaySeconds` is the knob that decides how many tiles share a request.

| field | type | default | meaning |
|---|---|---|---|
| `enabled` | bool | `false` | Turns the Tile Designer's "Generate art" on. Off leaves the button disabled, saying why. |
| `command` | string | `""` | What turns a worklist into sprites. `{worklist}` is replaced with the path the game writes; the command must write `<worklist>.result.json`. Empty means the built-in default, which runs `tools/gen_tile_art.py` under `nix-shell` (ImageMagick and Pillow are not on the default PATH here). |
| `batchDelaySeconds` | float | `2.0` | How long requests are collected before one request is spent on them. Larger is cheaper and slower. |
| `maxCellsPerRequest` | int | `4` | How many sprites may share **one** image request. A set larger than this is split across several requests rather than asked for as one illegible sheet, so this trades money for legibility. Values below 1 are clamped to 1. Four is the largest sheet this project has measured as usable; raise it once you have looked at a real set. |
| `maxRequestsPerSession` | int | `8` | Hard ceiling on requests for the session. At the measured ~$0.04 this caps a session at about 32 cents. |
| `workDirectory` | string | `data/generated/work` | Where the worklist, the raw sheets and the log go. Raw sheets are ~1–3MB and are the only thing here that costs money to reproduce, so they are kept on disk but gitignored. |
| `source` | string | `"image"` | Which producer makes a sprite. `"image"` runs `command`; `"dsl"` compiles the `<conventionId>.sprite` document found beside the sprite with the in-process compiler (`docs/pixel-dsl.md`). A source, not a mode: both end at the same placement and hot-swap, and the pack still wins. `"dsl"` ignores `command`, `batchDelaySeconds`, `maxCellsPerRequest` and `maxRequestsPerSession`, because a compile is local, free and instantaneous. Anything other than `"image"` or `"dsl"` is refused with a message and treated as `"image"`. |
| `dslAttempts` | int | `5` | How many compile/describe attempts a designer agent gets when `source` is `"dsl"`. Compiles are free, so this is much higher than the image path's attempt budget: the cost of a retry is a text request, not $0.04. Values below 1 are clamped to 1. |

Batching is not an optimisation here, it is the point. Measured: **1,290 image tokens
per request regardless of how many cells it fills**, so one request for four tiles
costs what one request for one tile costs — while one request *per tile* is roughly
19× the cost per asset. `batchDelaySeconds` is how long the game waits to collect
creations before spending; two seconds turns "the agent just made three tiles" into
one request instead of three.

The queue never blocks a frame: a request runs on a worker thread and the sprite is
swapped in from a main-thread drain when it lands. A tile therefore draws as its
ASCII glyph until the art arrives — that glyph is a legitimate rendering, not a
placeholder, and the feature works with no art at all.

Example, using a stub that draws flat colour instead of calling any API. This is how
the live path is verified without spending anything:

```json
{
  "art": {
    "pack": "packs/art/village",
    "generation": {
      "enabled": true,
      "command": "python3 tests/stub_tile_art.py {worklist}",
      "batchDelaySeconds": 0.05,
      "maxRequestsPerSession": 3
    }
  }
}
```

**Point the stub at a throwaway `art.pack`.** The generator (and therefore the stub)
writes to whatever `spritesDirectory` the worklist names, which is derived from
`art.pack` — so a test run with the real pack overwrites the real generated sprites.
The settings file used for the harnesses looks like this, and the matching pack is a
copy (`cp -r packs/art/village/sprites /tmp/tiledesign/pack/sprites`):

```json
{
  "map": {
    "file": "/tmp/tiledesign/maps/MemoMap.json",
    "mapsDirectory": "/tmp/tiledesign/maps",
    "spawnNPCs": false
  },
  "art": {
    "pack": "/tmp/tiledesign/pack",
    "generated": "/tmp/tiledesign/pack/generated",
    "generation": {
      "enabled": true,
      "command": "python3 tests/stub_tile_art.py {worklist}",
      "batchDelaySeconds": 0.5,
      "workDirectory": "/tmp/tiledesign/work"
    }
  }
}
```

Map paths in a settings file are used **verbatim**, so an absolute path is the reliable
spelling for a map outside the repo.

The modes that prove the live path, all free (no API call) with the stub command:

| mode | what it proves |
|---|---|
| `--tile-art <name> --tile-art-queue` | request → batch window → worker → main-thread drain → the cells gain a `Sprite`. Now asserts the *render entities* carry art, not just that the swap function was called. Add `--tile-art-hold <n>` to draw the pipeline panels for `n` frames against the settled job. |
| `--art-cache-test` | a stage file rewritten in place reloads instead of being served from the panel cache. This is the "the pipeline stages only show the old tile" regression. |
| `--paint-memo-test <tile>` | art that appears *after* the cell was painted is noticed. The tile must be on the loaded map and must have a generated sprite and no pack sprite, which is what makes its absence observable. |

## `debug`

| field | type | default | meaning |
|---|---|---|---|
| `windowStateFile` | string | `./debug_windows_state.json` | Where debug-window visibility is saved on shutdown and read on start. |
| `restServer` | bool | `true` | Starts flecs' REST server, which serves the live ECS world over **unauthenticated** HTTP. Turn off on a shared machine. |
| `logMaxEntries` | int | `1000` | Entries the Debug Log keeps before dropping the oldest. |

---

## Wired vs. not wired

Everything in the tables above is read **and used**. These are the knobs that
still live only in `include/Defaults.h` and are *not* in `settings.json`. They are
listed so the boundary is explicit; adding one means adding a field to
`GameSettings` and replacing its constant reference.

| area | constants still hardcoded |
|---|---|
| physics / movement | `DEFAULT_MAXSPEED`, `DEFAULT_WAYPOINT_ACCEL`, `DEFAULT_PLAYER_FRICTION`, `DEFAULT_ENTITY_FRICTION`, `DEFAULT_VELOCITY`, `DEFAULT_MINIMUM_VEL_FOR_WALL_REPEL`, `DEFAULT_WALL_REPEL_FORCE`, `DEFAULT_MINIMUM_WAYPOINT_SPEED`, `DEFAULT_MINIMUM_WAYPOINT_ACCEL`, `DEFAULT_MINIMUM_WAYPOINT_DISTANCE`, `DEFAULT_MINIMUM_SPEED_FOR_SLOWING_RADIUS` |
| perception / interaction | `DEFAULT_OBJECT_SEARCH_RADIUS`, `DEFAULT_PLACEMENT_MAX_DISTANCE`, `DEFAULT_HARVEST_DURABILITY_COST` |
| input | `DEFAULT_INPUT_MOVEMENT`, `DEFAULT_INPUT_ZOOM`, `DEFAULT_MINIMUM_INPUT_ZOOM`, `DEFAULT_MAXIMUM_INPUT_ZOOM`, `DEFAULT_DOUBLE_CLICK_TIME` |
| hitboxes / visuals | `DEFAULT_PLAYER_HITBOX_*`, `DEFAULT_PLAYER_VISUAL_*`, `DEFAULT_ENTITY_HITBOX_*`, `DEFAULT_ENTITY_VISUAL_*` |
| HUD / chat colours | `DEFAULT_ACTION_BAR_*`, `DEFAULT_FLOATING_TEXT_*`, `DEFAULT_COLOR_*`, `DEFAULT_SEE_INVENTORY_OPTION`, `DEFAULT_SHOW_STATUS_OPTION` |
| AI timing | `DEFAULT_EMPTY_RESPONSE_RETRY_MS`, `DEFAULT_DO_NOTHING_COMMAND_SLEEP_TIME_SECONDS` |
| render target | `gameTexture` is still created at `1920x1080` and `Draw()` still blits it to a `1920x1080` destination; `virtualWidth`/`virtualHeight` (`640x512`) are still fixed in `Game::Init`. |

Two related files are *not* settings:

- `debug_windows_state.json` is runtime state (which windows are open), written by
  the game. Only its **path** is configurable.
- `openrouter_debug.log` is AI debug output, not configuration.
