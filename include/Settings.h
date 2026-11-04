#pragma once

// One JSON file, read once at startup, holding every setting the game takes from
// outside its own code: which map to start on, which model to talk to, how the
// window and camera behave, where the player starts, and where the content
// registries live.
//
// The member defaults below are exactly the values the compiled-in constants in
// Defaults.h use, so a missing key, a missing file, or a corrupt file reproduces
// today's behaviour rather than a different one. See SETTINGS.md for the field
// reference and settings.json for a populated example.
//
// Secrets do not belong in settings.json. If a settings.local.json sits beside
// it, that file is merged on top (see LoadGameSettings), which is where an API
// key goes.

#include "Defaults.h"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct GameSettings {
  struct MapSection {
    // The map the run starts on. A path on the command line still overrides it.
    std::string file = std::string(MAPS_DIRECTORY) + "/Map_Farm.json";
    // Whether the map's authored NPCs are spawned. Off gives an empty world that
    // is useful for map editing.
    bool spawnNPCs = true;
    // Directory the Map Reload window lists its reloadable maps from.
    std::string mapsDirectory = MAPS_DIRECTORY;
  } map;

  struct ModelSection {
    // The name of an entry in `ai.profiles`, or empty for an inline block. The
    // profile seeds every field below at load; an explicitly written field
    // overrides it. Resolution happens in the loader, so every consumer still
    // reads a plain ModelSection and no backend code knows profiles exist.
    std::string profile;
    // "ollama", "gemini" or "openrouter". Anything else is treated as ollama.
    std::string type = "ollama";
    std::string model = "llama3";
    // May stay empty: a hosted block with no key of its own uses the top-level
    // `model.apiKey`, which is where settings.local.json and the legacy
    // model.json put it. A named profile's key, when it has one, arrives by
    // seeding the block at load (see LoadGameSettings' credential inheritance).
    std::string apiKey;
    // Only read for the ollama backend.
    std::string endpoint = "http://localhost:11434/api/generate";
    // The has* flags exist so "absent" stays distinct from "set to the default":
    // absent means the provider's own default is used, exactly as before.
    bool hasTemperature = false;
    double temperature = 0.0;
    bool hasMaxTokens = false;
    int maxTokens = 1500;
    bool hasProvider = false;
    // Passed through to OpenRouter verbatim as its "provider" routing object.
    nlohmann::json provider;
    // How long Ollama keeps the model resident after a request. Empty sends
    // nothing, leaving Ollama's own default (5 minutes, or OLLAMA_KEEP_ALIVE) in
    // charge. Only read for the ollama backend.
    std::string keepAlive;
  } model;

  // Settings for the second, independent backend behind the Vision Chat window.
  //
  // It is deliberately separate from `model` rather than a flag on it: the
  // vision model is a local Ollama model (moondream) while the text backend is
  // whatever the NPCs talk to, and OllamaAI keys its conversation context by
  // contextId alone, so serving both from one instance would replay a vision
  // model's context into a text model's request.
  struct VisionSection {
    // The name of an `ai.profiles` entry of kind vision, or empty for an inline
    // block. A vision profile maps model/endpoint/keepAlive/temperature into this
    // section; maxTokens and provider have no meaning here and are reported.
    std::string profile;
    // Off leaves the window usable but reporting that no backend is configured,
    // which is friendlier than making the feature look broken when Ollama is not
    // running.
    bool enabled = true;
    // The Ollama model tag. "moondream" resolves to the default tag; write e.g.
    // "moondream:latest" or "moondream:v2" when the installed tag differs.
    std::string model = "moondream";
    // Only the ollama backend is supported for vision today; the field exists so a
    // different host/port works without a rebuild. Point this at /api/chat instead
    // and it will not work -- the payload built here is the /api/generate shape.
    std::string endpoint = "http://localhost:11434/api/generate";
    bool hasTemperature = false;
    double temperature = 0.0;
    // How long Ollama keeps the vision model resident, e.g. "30m". Warm by
    // default: a vision model is several GB, so at Ollama's 5-minute default the
    // first question after any pause pays the full load -- tens of seconds that
    // look like slow inference but are disk I/O. Empty sends nothing and defers to
    // Ollama. Costs VRAM/RAM while it sits there.
    std::string keepAlive = "30m";
  } vision;

  // Settings for the backend behind the Designer Chat window.
  //
  // Its own section rather than reusing `model`, for the same reason Vision has
  // one -- OllamaAI keys conversation context by contextId alone, so one instance
  // cannot serve two different models -- but the reason it is *separately
  // configurable* is different: producing a valid blueprint is a different skill
  // from roleplay, so pointing the designer at a stronger (or cheaper) model
  // without retuning the NPCs is a real thing to want.
  //
  // `useMainModel` defaults on, so an unconfigured run behaves as though the
  // designer shared the NPC backend, which is the least surprising default.
  struct DesignerSection {
    bool useMainModel = true;
    // Read only when useMainModel is false. Same shape and meaning as `model`.
    ModelSection model;
    // The designer's instructions: the grammar, the reply contract and the
    // worked examples. Empty means the compiled-in default (see
    // Game::BuildDesignerSystemPrompt), so a missing file is not a broken run.
    std::string promptFile = "data/blueprints/DESIGNER.md";
    // How many times the engine re-prompts after an invalid design before giving
    // up. Every attempt is shown, so this bounds cost rather than hiding failure.
    int maxDesignAttempts = 3;

    // Ask the classifier which domain a request is for, and send when it answers,
    // instead of waiting for the "Route from text" button. With this on the domain
    // dropdown is hidden, because the route decides the domain per request.
    //
    // Off by default: it costs one decision call per request, and a person who
    // wants a specific domain should not have a classifier overrule them.
    bool autoRoute = false;

    // How much completion budget the designer gets, overriding the `maxTokens` of
    // whichever backend it ends up on. 0 means "inherit", which is what this
    // section did before the field existed -- and that inheritance was the bug it
    // fixes: with `useMainModel` on, the designer shares the *agent* backend, so it
    // shared the agents' output budget too, as though the two jobs wanted the same
    // number.
    //
    // They do not. An agent turn emits one `[COMMAND ...]` line, and 1500 tokens is
    // a ceiling it never approaches. A designer emits a whole set of definitions as
    // JSON: eight objects with art blocks and notes is 1300-1500 tokens before
    // reasoning spends any of it. Measured in logs/tile-designer, three consecutive
    // attempts at a set of eight were cut off at the shared 1500, the last losing a
    // single object out of the eight it had almost finished.
    //
    // Only the OpenRouter backend honours it, matching `model.maxTokens` -- that is
    // the only backend BuildAIBackend sets an output limit on at all. See
    // AI::setMaxTokens.
    int maxTokens = 0;

    // Reading the designer's reply aloud. Off by default: it is a development
    // window, and a feature that starts making noise on first launch is a
    // feature people turn off before judging.
    //
    // Every field here can be absent, and the whole section can be absent, in
    // which case the game runs exactly as it did before this existed.
    struct TtsSection {
      bool enabled = false;
      // The piper CLI. A bare name is resolved on PATH, which is what the
      // nix-shell environment provides; an absolute path also works.
      std::string binary = "piper";
      // The voice model. Piper resolves "<voice>.onnx.json" beside it for the
      // sample rate, so both files must be present.
      std::string voice = "models/tts/en_GB-alba-medium.onnx";
      // Playback gain, 0..1.
      float volume = 1.0f;
      // What gets read out, because the reply is a bad thing to read literally:
      // most of its bytes are the blueprint JSON, and its prose restates the
      // notes. "notes" speaks the verdict and the model's own critique;
      // "prose_and_notes" adds the prose the notes summarise; "everything"
      // exists only for debugging the pipeline and will read the JSON aloud.
      std::string content = "notes";
    } tts;

    // The model behind the Improve button.
    //
    // Its own section because rewriting is a different job from designing and the
    // right model for it is usually a different one: a short text edit wants a fast,
    // cheap, non-reasoning model, and pointing it at the strong reasoning model the
    // designer uses makes the button slow and can produce *no answer at all*.
    // Measured on this repo's default backend: deepseek-v4-flash spends its whole
    // completion budget on reasoning, so a rewrite request came back with content
    // `None` and 5 billed completion tokens. `enabled` defaults off, which means
    // "share the designer's backend but with reasoning turned off" -- correct for a
    // one-paragraph rewrite, and still one decision rather than two.
    struct RewriterSection {
      // False shares the designer's backend. True reads `model` below instead.
      bool enabled = false;
      // Only read when enabled. Same shape and meaning as `model`.
      ModelSection model;
      // Turn reasoning off on the rewriter's backend when it shares the designer's.
      // Worth a setting only because a model that needs reasoning to edit a sentence
      // exists; the default is right for this task.
      bool disableReasoning = true;
    } rewriter;
  } designer;

  // The System 1 backend: the typed-classifier layer described in
  // docs/agent-attention-design.md, whose calls the System 1 Overview window
  // displays.
  //
  // Its own section for the same reason the designer has one. System 1 is a
  // *different model class* from the System 2 chat model -- state and questions
  // in, calibrated answers out -- and the models good at it are not the models
  // good at roleplay. It is also the layer most likely to be swapped or dropped
  // wholesale, which is what the overview window exists to decide, so pointing it
  // somewhere else must not mean touching the NPC backend.
  //
  // Every field here can be absent, and the whole section can be absent, in which
  // case the game runs exactly as it did before this existed: no System 1 backend
  // is constructed and `SystemOneClassifier::Available()` has nothing to report
  // (see SystemOneRecorder.h).
  // One selectable System 1 backend.
  //
  // A profile is *how to reach a model*, plus the two knobs that are only
  // meaningful per model. Everything else about how System 1 is used -- whether the
  // sprite designer routes, how many records the window keeps -- is a property of
  // the game, not of the endpoint, so it stays on the section below.
  //
  // `minConfidence` is per model because it is calibration, and calibration is not
  // portable: measured on this machine, Kev reported 0.69/0.66/0.50 where hosted Jev
  // reported a different spread for the same questions. A threshold tuned for one
  // silently means something else on the other.
  struct SystemOneProfileSection {
    // The identifier used to select this profile: `system1.activeProfile`, the
    // `--system1-profile` flag and every status line. Required in the file.
    std::string id;
    // The `ai.profiles` entry to take the model settings from. The entry's preset
    // and fields are applied before this profile's own `model` block, so a
    // per-profile timeout or calibration can sit on top of a shared backend.
    std::string profile;
    // Display name. Falls back to `id`, so a small config does not have to say the
    // same thing twice.
    std::string label;
    // "hosted_jev" or "local_kev": seeds endpoint/model/key/timeout from the preset
    // before the explicit fields below are applied. Empty means "no seed", which is
    // how a profile points at a server this codebase has no preset for (Lev, Laya,
    // OpenJev, a remote llama-server) using only `model`.
    std::string preset;
    ModelSection model;
    // 0 means "not stated", which follows the preset's budget and otherwise the
    // endpoint rule (120s for a local server, 30s hosted). Unlike the section-level
    // field this needs no `has` flag: 0 is not a usable budget, so it is free to
    // mean "absent".
    long timeoutSeconds = 0;
    // Below this, a verdict is reported as unsure rather than acted on. 0 disables
    // the check, which is why it needs no `has` flag -- unlike a budget, 0 is a
    // legitimate value.
    float minConfidence = 0.0f;
  };

  // How the navigation sub-agent spends System 1 calls and time
  // (docs/navigation-subagent-design.md §12). These are properties of the game's
  // use of the model, not of the endpoint, so they live here rather than on a
  // profile: swapping Jev for Kev must not silently change what a navigation is
  // allowed to do.
  struct SystemOneNavigationSection {
    // The only switch. [NAVIGATE] reports that it is off rather than silently
    // falling back to something else.
    bool enabled = true;
    // One line per action (local compass move or distant place); see
    // DEFAULT_NAVIGATION_MAX_CANDIDATES.
    int maxCandidates = DEFAULT_NAVIGATION_MAX_CANDIDATES;
    // Local compass moves before the distant place list; see
    // DEFAULT_NAVIGATION_MAX_DIRECTIONS.
    int maxDirections = DEFAULT_NAVIGATION_MAX_DIRECTIONS;
    // How far a sector looks for the first place; see
    // DEFAULT_NAVIGATION_COMPASS_RADIUS.
    int compassRadius = DEFAULT_NAVIGATION_COMPASS_RADIUS;
    int maxSurveys = DEFAULT_NAVIGATION_MAX_SURVEYS;
    float maxSeconds = DEFAULT_NAVIGATION_MAX_SECONDS;
    float legTimeoutSeconds = DEFAULT_NAVIGATION_LEG_TIMEOUT_SECONDS;
    // Calibration for *this* task, not a copy of the profile's confidence knob.
    float minProbability = DEFAULT_NAVIGATION_MIN_PROBABILITY;
    float minMargin = DEFAULT_NAVIGATION_MIN_MARGIN;
    // S1 spatial input experiment:
    //   "off"     -- current sector prose (default; unchanged)
    //   "append"  -- sector prose + ASCII map + target list
    //   "replace" -- ASCII map + compact target list instead of the sector/
    //                frontier prose (the map-only baseline)
    std::string mapMode = "off";
    int mapRadius = 6;
    // Examine affordance list (shared by both navigation modes). The radius
    // defaults to the standard visual range; the single navigator lists every
    // qualifying object inside it. The two-context S1 is capped by maxExamine,
    // reserved inside maxCandidates.
    int examineRadius = static_cast<int>(DEFAULT_VISION_RANGE);
    int maxExamine = 2;
  };

  // The strategist's observation is the local grid + the minimal observed graph
  // (docs/llm-s1-navigation-design.md section 8).
  struct NavigationObservationSection {
    int localGridRadius = DEFAULT_NAVIGATION_OBSERVATION_GRID_RADIUS;
    int maxGraphNodes = DEFAULT_NAVIGATION_MAX_GRAPH_NODES;
    int maxGraphEdges = DEFAULT_NAVIGATION_MAX_GRAPH_EDGES;
    int maxTokens = DEFAULT_NAVIGATION_OBSERVATION_MAX_TOKENS;
  };

  // The standard-LLM strategist sits above the System 1 pilot. It is a separate
  // call/context on the main AI backend; the S1 remains the local executor.
  struct NavigationStrategistSection {
    bool enabled = DEFAULT_NAVIGATION_STRATEGIST_ENABLED;
    bool fallbackToS1 = DEFAULT_NAVIGATION_STRATEGIST_FALLBACK;
    int maxStrategies = DEFAULT_NAVIGATION_MAX_STRATEGIES;
    // Attempts per strategist request: the first call plus retries after a
    // transport failure or an invalid reply.
    int maxAttempts = DEFAULT_NAVIGATION_MAX_ATTEMPTS;
    int maxMonitors = DEFAULT_NAVIGATION_MAX_MONITORS;
    float monitorMinConfidence = DEFAULT_NAVIGATION_MONITOR_MIN_CONFIDENCE;
    float timeoutSeconds = DEFAULT_NAVIGATION_STRATEGIST_TIMEOUT_SECONDS;
    NavigationObservationSection observation;
  };

  // The single-context navigator: one main-model context that returns both the
  // plan and the next move each decision. `two` restores the strategist + Jev
  // pilot from before this experiment.
  struct NavigationSingleSection {
    int maxTokens = 500;
    // Context window requested from Ollama (`options.num_ctx`). 8192 allocates a
    // 1 GiB KV cache on phi4-mini and, with the model plus compute buffers,
    // overflows a 6 GB RTX 3050. 4096 fits the reduced prompt and a 2-turn history.
    int contextTokens = 4096;
    float timeoutSeconds = 60.0f;
    bool fallbackToTwo = true;
    int mapRadius = 6;
    // Which model settings the dedicated navigator instance uses. True keeps the
    // NPC model (still a separate instance, so reasoning/token options do not
    // leak to NPC requests). False reads `model` below; that is how a local
    // Ollama navigator or a different hosted model is configured.
    bool useMainModel = true;
    // Only read when useMainModel is false. Same shape and meaning as the
    // top-level `model` block, including `type`, `model`, `endpoint`, `keepAlive`,
    // `temperature` and `provider`. The default type is ollama, so a local model
    // needs only its tag.
    ModelSection model;
    // Build a dedicated AI instance for the single navigator instead of sharing
    // the NPC backend. The separate instance is what makes `disableReasoning`
    // safe: toggling it on the shared backend would mutate options for NPC
    // requests running at the same time.
    bool dedicatedBackend = true;
    // Turn the dedicated backend's chain-of-thought off. OpenRouter enables
    // reasoning by default on the models used here, and reasoning can consume
    // the entire completion budget before any JSON is emitted. Ignored when
    // `dedicatedBackend` is false (the shared backend is left untouched).
    bool disableReasoning = true;
    // Send compact JSON in the single prompt. The pretty-printed form is useful
    // in the debug window, but every space and newline is a prompt token. Off
    // reproduces the previous prompt for A/B comparison.
    bool compactJson = true;
    // Previous decision turns kept in the single navigator's context. The state
    // already carries the route and last result, so long histories mostly repeat
    // observations the model has already acted on. 0 keeps the whole episode.
    int historyTurns = 3;
    // Ask for a one-sentence strategy and a 1-2 sentence think block instead of
    // the previous "a few sentences" / "2-6 sentences" wording. Shorter output is
    // the cheapest latency win after reasoning is off.
    bool concise = true;
    // Stage 2: group objects by type/profile and use the target grammar
    // (objects:all, inspect:<group>, approach:<alias>, <verb>:<alias>) instead
    // of listing every examine candidate. False restores the old per-instance
    // examine list for A/B.
    bool objectSummary = true;
    // Include the observation graph in the single prompt. Off for the small
    // local model: the place graph duplicates what STATE/ACTIONS already say.
    bool includeGraph = true;
    // Include the ASCII map and its target list in the single prompt. Off for
    // the small local model; the candidate actions remain in ACTIONS.
    bool includeMap = true;
    // Groups shown in the default STATE block. The rest stay available through
    // objects:all and inspect.
    int maxObjectGroups = 6;
    // Instances shown by inspect:<group>. inspect:<group>:all ignores this.
    int maxGroupDetails = 8;
    // All eight compass sectors are offered to the main LLM, and there is room
    // for four distant places after them. The two-context S1 keeps its smaller
    // system1.navigation caps.
    int maxDirections = 8;
    int maxCandidates = 12;
    // Consecutive unusable replies allowed for one decision before the
    // fallback. Any model-output error counts: invalid JSON or field types,
    // an invalid plan, a missing move, or a move that is not an offered
    // candidate. An accepted reply resets the counter.
    int maxAttempts = 3;
  };

  // Continuous navigation: the main agent's own context carries the spatial
  // state and [GO] takes one step. There is no [NAVIGATE] episode to enter and
  // none to end, so the prompt shapes it needs are the same facts the single
  // navigator's STATE/ACTIONS block uses (candidate caps, compass radius,
  // examine radius), built fresh at every wake.
  //
  // Named `inlineNav` because `inline` is a C++ keyword; the settings key is
  // `navigation.inline`. The caps are duplicated from `single` on purpose:
  // inline is the mode that is supposed to survive, so its budget should not
  // move when the single navigator's block is eventually retired.
  struct NavigationInlineSection {
    // Compass moves offered in the spatial block, and total steps (compass plus
    // distant places). Same unit and meaning as the single navigator's.
    int maxDirections = 8;
    int maxCandidates = 12;
    // Include the [GO ...] step list. Off leaves the orientation line and the
    // watch, which is the "does standing orientation alone help?" arm.
    bool includeActions = true;
    // Include the north-up ASCII map in the spatial block. Off by default: the
    // block is meant to be cheap on every turn, and the map is the first thing
    // to turn on when a run shows the model misreading the local layout.
    bool includeMap = false;
    int mapRadius = 6;
    // Previous spatial blocks kept in the main context. 0 keeps only the newest.
    // The blocks are the only messages whose content is fully replaced by the
    // next one, and they arrive on every turn, so this is the first place a
    // long session accumulates context it does not need. Retaining a couple
    // still leaves the commands and their results in the transcript, which is
    // what the agent's memory of the walk is built from.
    int spatialHistory = 2;
  };

  struct NavigationSection {
    // "single" -- one main-model context per navigation episode: [NAVIGATE] opens
    //             an episode that plans, walks and returns a summary
    //             (docs/llm-s1-navigation-design.md).
    // "two"    -- the standard-LLM strategist above the System 1 pilot
    //             (docs/llm-s1-navigation-design.md).
    // "inline" -- no episode and no [NAVIGATE]: the agent's ordinary turns carry
    //             the spatial state and [GO] takes one step. The agent keeps its
    //             own topology; see `inlineNav` below.
    std::string agent = "single";
    NavigationStrategistSection strategist;
    NavigationSingleSection single;
    NavigationInlineSection inlineNav;
  };

  struct SystemOneSection {
    // False shares the NPC backend, which is the least surprising default for an
    // unconfigured run. True reads `model` below instead.
    bool useMainModel = true;
    // Only read when useMainModel is false. Same shape and meaning as `model`,
    // with one deliberate exception: an empty `model` means "inherit the main
    // model's id" rather than "use ModelSection's built-in llama3 default".
    //
    // That exception is load-bearing, not tidiness. ModelSection defaults `model`
    // to "llama3", so a config that sets only an endpoint or a key would look
    // configured and send llama3 to a decision endpoint -- which answers
    // `Model llama3 does not exist`. Empty is the only honest spelling of "not
    // set" once a field has a non-empty default.
    ModelSection model;
    // Ask the shared backend for a single non-reasoning answer rather than a
    // chain of thought. On by default, and the opposite of the designer's choice:
    // System 1's whole premise is a cheap, fast, one-shot classification, and a
    // reasoning model handed a classifier's question spends its budget thinking
    // and returns a worse-calibrated answer -- or none at all.
    bool disableReasoning = true;
    // Bounds the in-memory view the overview window keeps. The log file is
    // unaffected and keeps everything, so a long session stays fully recorded.
    int maxRecords = 500;
    // Whole-request budget for one decision. A decision model answers in roughly
    // 100ms, so a request still running after this is not going to produce a useful
    // verdict -- and a designer waiting on a router must not block on a hung socket.
    long timeoutSeconds = 30;
    // Whether that value came from a file.
    //
    // This exists because "absent" and "set to 30" want opposite behaviour: absent
    // should follow the endpoint -- a local llama-server needs ~120s, a hosted call
    // 30s -- while an explicit 30 means the user really does want 30. Without the
    // flag the field's non-zero default makes the two indistinguishable, so an
    // endpoint-derived default can never apply. Same reason `ModelSection` carries
    // `hasTemperature` and `hasMaxTokens`.
    bool hasTimeoutSeconds = false;
    // Whether the sprite designer asks a classifier what kind of sprite a request is
    // for, instead of starting in whichever mode the window is already showing.
    //
    // On by default, and the one place in this file where that is the right call for a
    // feature that costs money: a classification is a *decision* call -- a fraction of a
    // cent, with no image -- and without it the user has to pick the mode themselves
    // before typing, which is the thing the router exists to remove. The image request
    // that follows is separately gated by `art.generation.enabled`.
    bool routeRequests = true;
    // What to do with the route verdict. Below this, the window keeps its current mode
    // and says the classifier was unsure rather than silently acting on a coin flip.
    //
    // Advisory in practice: `choice` answers carry no confidence field at all on hosted
    // Jev (see SystemOneRecorder.h), so this is 0 for most real verdicts and a threshold
    // above 0 would send nearly everything to "unsure". Kept as a knob because a backend
    // that does report confidence -- Laya's decision head -- makes it meaningful.
    float minConfidence = 0.0f;

    // The selectable backends.
    //
    // The flat fields above are always available as one more profile, whose id is
    // "default". That is why nothing has to be migrated: an existing config keeps
    // working untouched, and `profiles` adds choices beside it. A profile may reuse
    // the id "default" to override it.
    std::vector<SystemOneProfileSection> profiles;
    // Navigation knobs; independent of which profile is active.
    SystemOneNavigationSection navigation;
    // Which profile starts active, unless the persisted choice from a previous run
    // or `--system1-profile` says otherwise. Empty means "default".
    std::string activeProfile;
    // `--system1-profile`, applied by main() after the file is loaded. Never read from
    // the file itself, and it outranks everything else -- an explicit flag is a
    // request about *this* run, so a remembered selection must not win over it.
    std::string cliProfile;
  } system1;

  // LLM strategist + System 1 pilot (docs/llm-s1-navigation-design.md).
  NavigationSection navigation;

  // One named AI backend: everything needed to reach a model, declared once and
  // referenced by name from any model block.
  //
  // The same nine fields a model block already has, plus the three that make a
  // profile selectable: the id (its key in `ai.profiles`), a display label, and
  // the kind of model it is. Kind is what keeps a vision model out of the agent
  // backend's picker -- it is advisory, not isolation: the separate backend
  // instances remain separate because that is about conversation context, not
  // configuration.
  struct AIProfileSection {
    std::string label;
    // "s1" (a decisions model), "s2" (a chat/reasoning model) or "vision".
    // Validated at load; an unknown value is reported and treated as "s2".
    std::string kind = "s2";
    // Only meaningful for kind "s1": hosted_jev or local_kev seeds endpoint,
    // model and key before the fields below, exactly as a System 1 profile's
    // preset does.
    std::string preset;
    ModelSection model;
  };

  struct AISection {
    // Path to a text file holding the NPC system prompt template. Empty means the
    // compiled-in DEFAULT_NPC_PROMPT is used. The template keeps the same
    // %COMMANDS_LIST%, %BACKGROUND% and %CHARACTERS% placeholders.
    std::string npcPromptFile;
    // The template text itself, filled by LoadGameSettings. Empty means "use the
    // compiled-in prompt"; Game::createNPC owns that fallback so this struct
    // does not need to know about AgentBrain.
    std::string npcPromptTemplate;

    // --- Stage 5: model consolidation (docs/spatial-memory-design.md section 6.2)
    //
    // Off by default, and the whole feature is a no-op when the backend is
    // unavailable: the engine already wrote everything the game needs, so a
    // missing model changes nothing a player can observe.
    bool memoryDigest = false;
    // How stale a digest may be before a wake asks for a new one. Also the rate
    // limit: one request in flight per character regardless.
    int memoryDigestMinAgeSeconds = 120;
    // How many candidate items the model is shown. A prompt that lists every run
    // is the same problem the digest exists to solve.
    int memoryDigestCandidates = 24;
    // How long after a world loads before the first digest may be asked for. A
    // digest taken at spawn summarises a handful of cells and says nothing useful;
    // the grace is wall-clock seconds, not frames, so it means the same at any
    // frame rate.
    int memoryDigestStartupGraceSeconds = 60;
    // How much ground must be remembered before a digest is worth asking for. One
    // vision sample sees most of a cone, so this is the difference between "the
    // spawn view" and "somewhere I have actually been".
    int memoryDigestMinCells = 400;
    // Output budget for the digest's own backend. The answer is a short array;
    // the budget exists so a model that starts explaining itself is cut off
    // cheaply rather than after a page of prose.
    int memoryDigestMaxTokens = 1200;
    // The System 1 salience probe at wake. Logged only; nothing acts on it yet.
    bool memorySalience = false;

    // The named backends, keyed by id. A map rather than an array because the
    // override layer merges objects key by key: a profile edited in the settings
    // window lands in settings.local.json without replacing the whole registry,
    // which is what makes an apiKey-override for one profile possible at all.
    std::map<std::string, AIProfileSection> profiles;
  } ai;

  struct WindowSection {
    std::string title = "AIRogue";
    // Initial window size. On a fullscreen run these also drive the camera's
    // fit-to-map zoom reference, so changing them changes how much world is on
    // screen.
    int width = 1920;
    int height = 1080;
    // false keeps the windowed size above instead of going fullscreen.
    bool fullscreen = true;
    bool vsync = true;
    int targetFPS = 60;
    // -1 picks the monitor the way the game always has: monitor 1 when more than
    // one display is connected, otherwise monitor 0. 0 or more forces that index.
    int monitor = -1;
  } window;

  struct RenderSection {
    std::string fontPath = DEFAULT_FONT_PATH;
    int fontSize = static_cast<int>(DEFAULT_FONTSIZE);
    // "follow" keeps the camera on the player; "free" leaves it to the arrow
    // keys. Anything else falls back to follow.
    std::string cameraMode = "follow";
    // 0 fits the map to the window, which is the original behaviour. Any other
    // value is used as the starting zoom directly.
    float cameraZoom = 0.0f;
  } render;

  // Acquisition field for every character's Vision. Named `perception` rather
  // than `vision` because `vision` is already the independent Vision Chat AI
  // backend. The default is all-round (360 degrees) so navigation can be
  // evaluated without the forward cone as a second, independent variable. Set
  // angleDegrees to 90 for the forward-cone variant; radarRadius is the
  // all-round near ring in either mode.
  struct PerceptionSection {
    float range = DEFAULT_VISION_RANGE;
    float angleDegrees = DEFAULT_VISION_ANGLE_DEGREES;
    int radarRadius = DEFAULT_VISION_RADAR_RADIUS;
  } perception;

  // Observed base regions (docs/region-place-design.md, Stage 2). Separate from
  // the semantic place derivation and from the model-facing place/candidate caps.
  struct RegionsSection {
    bool enabled = true;
    int nearSize = DEFAULT_REGION_NEAR_SIZE;
    int nearRadius = DEFAULT_REGION_NEAR_RADIUS;
    int midSize = DEFAULT_REGION_MID_SIZE;
    int midRadius = DEFAULT_REGION_MID_RADIUS;
    int farSize = DEFAULT_REGION_FAR_SIZE;
    int farRadius = DEFAULT_REGION_FAR_RADIUS;
    int maxRegions = DEFAULT_MEMORY_REGIONS_MAX;
    int modelNearCount = DEFAULT_REGION_MODEL_NEAR_COUNT;
  } regions;

  // Fixed world chunk grid and the agent-facing area view
  // (docs/chunk-design.md). The size is read at map load; changing it mid-run
  // remaps every handle, so a change wants a reload.
  struct ChunksSection {
    bool enabled = true;
    int size = DEFAULT_CHUNK_SIZE;
    int maxTraversedAreas = DEFAULT_CHUNK_MAX_TRAVERSED_AREAS;
    int maxJourneySegments = DEFAULT_CHUNK_MAX_JOURNEY_SEGMENTS;
    int maxNewAreas = DEFAULT_CHUNK_MAX_NEW_AREAS;
    int maxFeaturesPerArea = DEFAULT_CHUNK_MAX_FEATURES_PER_AREA;
    int cardAspects = DEFAULT_CHUNK_CARD_ASPECTS;
    int cardObjectGroups = DEFAULT_CHUNK_CARD_OBJECT_GROUPS;
    int cardPlaces = DEFAULT_CHUNK_CARD_PLACES;
    bool includeCharacters = DEFAULT_CHUNK_INCLUDE_CHARACTERS;
  } chunks;

  struct PlayerSection {
    std::string name = DEFAULT_PLAYER_ENTITY_NAME;
    // Starting tile. Out-of-bounds or wall positions are relocated to the nearest
    // walkable tile by the same pass LoadMap already runs for every entity.
    int startX = 48;
    int startY = 20;
    // Race the player's body slots come from. Empty means the human default.
    std::string race;
    // PortraitRegistry id for the player's HUD portrait. Empty means the race's
    // default, and then the unknown sprite.
    std::string portrait;
    // ActorSpriteRegistry id the player is drawn with, or empty for the race's
    // own look (and then the ASCII art). An id rather than a path so the picker
    // offers the same catalogue the Content Editor edits; the art is
    // data/sprites/<id>.png, and an id with no art leaves the race's look in
    // place and is reported.
    std::string actorSprite;
    // Top movement speed in pixels per second. Holding a direction adds velocity
    // each frame and the movement system clamps it to this, so this is how fast
    // the player settles at walking, not how quickly they get there. 0 is a
    // deliberate "cannot move"; negative is treated as a mistake.
    float maxSpeed = DEFAULT_MAXSPEED;
    // Total inventory cell capacity. The window shows 30 cells per page; values
    // above 30 create more pages, values below 30 cross out the remainder.
    int inventorySlots = 30;
  } player;

  struct CombatSection {
    // False: pressing an aimed skill shows the indicator and waits for a click.
    // True: the same press fires immediately at the cursor, with no preview.
    // Read on every input, so toggling it in Settings takes effect next press.
    bool instantSkillshots = false;
    // Draw the cast-range ring and (for movement-enabled skills) the positions
    // from which the locked target stays in range. Read at draw time.
    bool showSkillRanges = true;
    // IconRegistry id used as the corpse visual. Read every frame by the corpse
    // icon system, so existing corpses change with the setting.
    std::string deathIcon = "death";
  } combat;

  // Player portrait layout. Resources are referenced by id, so a race that later
  // grants Mana or a shield can be added without schema churn.
  struct HudSection {
    struct PortraitSection {
      bool showIcons = true;
      bool showValues = true;
      bool showNames = true;
      bool compact = false;
      float barHeight = 12.0f;
      // Array of {"id","visible","showIcon"} in display order. Empty means
      // "every current resource, in registry order".
      nlohmann::json resources = nlohmann::json::array();
    } portrait;
  } hud;

  struct WorldSection {
    // 0 seeds from the clock, so every run differs. Any other value makes the run
    // reproducible, which is what the Debug Log's printed seed is for.
    uint64_t seed = 0;
  } world;

  struct ContentSection {
    // Where each data registry loads its definitions from.
    std::string objects = "data/objects";
    std::string recipes = "data/recipes";
    std::string stats = "data/stats";
    std::string skills = "data/skills";
    std::string items = "data/items";
    std::string races = "data/races";
    // NPC definition templates: a named character's starting stats, skills, gear,
    // race, body slots, look and brain. A map names one with "template" and may
    // override any field; nothing here is enforced at runtime.
    std::string npcs = "data/npcs";
    // What drives an NPC: background, life goals and the model profile to talk
    // to. Separate from the body so one brain can drive many templates and a
    // template can be spawned with no brain at all.
    std::string brains = "data/brains";
    std::string portraits = "data/portraits";
    std::string icons = "data/icons";
    // Prose style profiles for generated art. Families are selected by the
    // sprite kind or by an asset's own optional style id.
    std::string artStyles = "data/art_styles";
    // Authored body paper-doll silhouettes: icon + normalised slot anchors.
    std::string bodySilhouettes = "data/body_silhouettes";
    std::string projectiles = "data/projectiles";
    std::string skillshots = "data/skillshots";
    std::string combatStrategies = "data/combat_strategies";
    std::string surfaceSprites = "data/surface_sprites";
    std::string surfaces = "data/surfaces";
    std::string statusEffects = "data/status_effects";
    std::string effects = "data/effects";
    std::string resources = "data/resources";
    // The place taxonomy (docs/place-derivation-design.md section 4): which
    // categories form places, their seed thresholds, link radii and split rule.
    // A file rather than a directory, because it is one vocabulary, not a catalog.
    std::string places = "data/places.json";
    // The world's tile vocabulary. A map's own "tileInfo" is a set of overrides
    // on top of these; with no catalog, maps must declare every tile inline,
    // which is how they all worked before the catalog existed.
    //
    // This is the HAND-AUTHORED catalog and nothing else. Agent-created tiles go
    // to `generatedTiles` below -- see docs/asset-generation-design.md §4.3 --
    // because the vocabulary a designer designs against has to stay exactly as
    // it was written.
    std::string tiles = "data/tiles";
    // The generated overlay for agent-created tiles: loaded additively on top of
    // the catalog at startup, and where the tile designer writes what it makes.
    //
    // Deliberately a sibling of data/tiles rather than a subdirectory of it.
    // TileRegistry::LoadTiles reads every .json in one directory and is not
    // recursive, so a nested overlay would either be picked up as catalog (which
    // would defeat the split) or never be read at all. A separate path also makes
    // the two layers separately deletable: removing this directory reverts the
    // world to hand-authored vocabulary exactly.
    std::string generatedTiles = "data/generated/tiles";
    // The actor-sprite catalogue: one image per being (data/actor_sprites). Not a
    // definition of a creature -- `Actor` is the tag that says something is alive. This
    // is the simple kind, and it stays the right answer for a wolf or a slime.
    std::string actorSprites = "data/actor_sprites";
    // The actor-rig catalogue (data/rigs): layered humanoid rigs made of parts, which is
    // what lets equipment be drawn over a body and hide one part without redrawing
    // another. A rig and an actor sprite are alternatives, not variants of one thing.
    std::string rigs = "data/rigs";
    // Rig definitions the Designer produced, loaded after the catalog so a generated rig
    // can be deleted back to the hand-authored one without touching it.
    std::string generatedRigs = "data/generated/rigs";
    // The rig designer's instructions: the humanoid frame and the per-part reply format.
    std::string rigPromptFile = "data/rigs/RIGDESIGNER.md";
    // Shared movement templates (data/movements): what "walking", "running" or "a kick"
    // is, in words and timing. A rig's clips point at a movement and supply the frames;
    // the same movement is therefore drawn differently on every body.
    std::string movements = "data/movements";
    // Movement definitions the Content Editor or a designer produced, loaded after the
    // catalog so deleting one reverts to the hand-authored entry.
    std::string generatedMovements = "data/generated/movements";
    // Item definitions the Designer produced, loaded after data/items.
    std::string generatedItems = "data/generated/items";
    // The equipment designer's instructions: the worn-item reply format and slot rules.
    std::string equipmentPromptFile = "data/items/EQUIPMENTDESIGNER.md";
    // Where the Content Editor writes its overrides, one directory per kind beneath
    // it (tiles/, objects/). Loaded last, so a human edit wins over both the catalog
    // and the generated overlay -- and the hand-authored files stay untouched, which
    // matters because they are git-tracked content.
    //
    // A sibling of the generated layer rather than a subdirectory of it: the two are
    // different authors with different precedence, and putting a human override inside
    // an agent's overlay would make "clear the generated layer" silently clear the
    // user's edits too.
    std::string editor = "data/editor";
    // The tile designer's instructions: the field grammar, the reply contract and
    // the glyph table. Empty falls back to the generated vocabulary alone, the
    // same way designer.promptFile does.
    std::string tilePromptFile = "data/tiles/TILEDESIGNER.md";
    // The object designer's instructions: the capability vocabulary, the art block
    // grammar and what an object may not do. The reply format is not repeated here --
    // it is substituted from `spriteContractFile` below, so the two kinds cannot
    // drift about how a reply is shaped.
    std::string objectPromptFile = "data/objects/OBJECTDESIGNER.md";
    // The map designer's instructions: the compact legend-and-rows reply format and
    // the rules MapDesigner validates. Empty stops the domain from being useful, which
    // its pane says rather than sending an empty prompt.
    std::string mapPromptFile = "maps/MAPDESIGNER.md";
    // The generated overlay for agent-created objects, the object counterpart of
    // `generatedTiles` above and a sibling of data/objects for the same reason:
    // ObjectFactory::LoadTemplates reads one directory and is not recursive. Empty
    // means an accepted object lives for this run only, which the window says out
    // loud rather than failing silently at the next launch.
    std::string generatedObjects = "data/generated/objects";
    // The reply contract every kind's prompt is built from -- the format, the refusal
    // protocol and the prohibitions. One file rather than a copy per kind, because
    // this is the part that must not drift: a tile and an object are refused the same
    // way or the model has learned two contracts.
    std::string spriteContractFile = "data/prompts/SPRITE_CONTRACT.md";

    // The system prompt for the turn that writes a *pixel document* (the sprite
    // DSL) for a definition the designer already accepted. Separate from the two
    // definition prompts because it answers a different question -- how the sprite
    // is drawn, not what it is -- and because its `%SPRITE_DSL%` and `%RAMPS%`
    // substitutions come from the compiler itself.
    std::string spriteDSLPromptFile = "data/style/SPRITEDESIGNER.md";
    // The prompt-improver's instructions, shared by every window that offers the
    // Improve button -- the rules for rewriting a user's request are about rewriting,
    // not about tiles or buildings, so there is one file rather than one per window.
    // A window supplies its own domain brief into `%BRIEF%`.
    std::string rewritePromptFile = "data/prompts/REWRITE.md";
    // Building designs. Optional: with none, the placement window lists nothing.
    std::string blueprints = "data/blueprints";
  } content;

  // Where the art comes from. Two roots, tried in this order for a convention
  // sprite, so a generated image can never shadow hand-authored art:
  //
  //   1. <pack>/sprites/<id>.png   the art pack: authoritative, wins
  //   2. <generated>/<id>.png      the generated layer: additive, fills only gaps
  //
  // An empty `pack` keeps the pre-split arrangement exactly as it was: every
  // sprite resolves from data/sprites/ and the generated layer is not consulted,
  // because with no pack there is no id to key it by.
  //
  // See docs/asset-generation-design.md for what an art pack is and why the
  // generated layer lives outside it rather than inside the pack.
  struct ArtSection {
    // Art pack directory, e.g. packs/art/pixel_crawler. Empty means "no art
    // pack", which is the current arrangement and needs nothing else changed.
    std::string pack;
    // The directory this pack's generated sprites are in *directly* -- not its
    // parent. Empty derives <pack's grandparent>/generated/<pack dir name>/sprites,
    // which is what the documented layout gives for no configuration at all.
    std::string generated;

    // Live generation of tile art, for tiles an agent invents while the game runs.
    // See docs/tile-art-wiring-design.md sections 4 and 10.2 for why it is a queue
    // and not a call in the frame loop.
    //
    // **This is the one part of the art feature that spends money unattended**, so
    // it is off unless turned on deliberately, and capped.
    struct GenerationSection {
      bool enabled = false;
      // The command that turns a worklist JSON into sprites. `{worklist}` is
      // replaced with the path the game writes; the command must write
      // `<worklist>.result.json` (see tools/gen_tile_art.py --from-worklist).
      //
      // A command rather than a linked-in backend on purpose: image tooling here is
      // Python plus ImageMagick, neither of which is on the default PATH, and the
      // local-LoRA path (tools/gen_tile.py) has to stay swappable behind the same
      // seam. Empty uses `builtinDefaultCommand()` in Settings.cpp, which runs
      // tools/gen_tile_art.py under nix-shell.
      std::string command;
      // How long to collect requests before spending one request on them. One
      // request costs the same whether it carries one tile or nineteen, so this
      // window is what makes a burst of creations cost one request.
      float batchDelaySeconds = 2.0f;
      // Hard ceiling on requests for the session. A request is ~$0.04 (measured),
      // so this default caps a session at about 32 cents.
      int maxRequestsPerSession = 8;
      // How many sprites may share one image request. A set larger than this is split
      // across several requests rather than asked for as one illegible sheet, which costs
      // whole requests -- so this trades money for legibility and is meant to be raised
      // once a real sheet has been looked at. See TileArtQueue::Config for why it is four.
      int maxCellsPerRequest = 4;
      // Where the worklist and the raw sheets go. Empty uses the default,
      // `data/generated/work`, which is where tools/gen_tile_art.py already keeps
      // them and which .gitignore excludes -- a raw sheet is ~1-3MB and is the one
      // thing in this feature that costs money to reproduce, so it is kept on disk
      // but never committed.
      std::string workDirectory;

      // Which producer makes a sprite: "image" (the sheet API behind
      // art.generation.command) or "dsl" (src/PixelDSL.cpp compiling the
      // <conventionId>.sprite document next to it). Defaults to "image" so
      // upgrading does not change what any existing checkout does.
      //
      // A source, not a mode: both write the same PNG through the same placement
      // and the same hot-swap, so everything downstream is shared.
      std::string source = "image";
      // How many compile/describe attempts a designer agent gets when the DSL is
      // the source. Compiles are free and instant, so this is much higher than the
      // image path's maxDesignAttempts -- the cost of a retry is a text request,
      // not $0.04.
      int dslAttempts = 5;
    } generation;
  } art;

  struct DebugSection {
    // Visibility state of the debug windows, written on shutdown and read on
    // start.
    std::string windowStateFile = "./debug_windows_state.json";
    // Starts flecs' unauthenticated REST server, which serves the live ECS world
    // over HTTP. Fine locally, best turned off on a shared machine.
    bool restServer = true;
    int logMaxEntries = 1000;
  } debug;
};

// The one settings object for the process. LoadGameSettings fills it before any
// subsystem reads it; Settings() is how the rest of the code reaches it.
GameSettings &MutableSettings();
const GameSettings &Settings();

// Reads `path` into the settings object. When a file named settings.local.json
// sits beside it, that file is merged on top with an RFC 7396 merge patch, so it
// only needs the keys it overrides -- which is where an API key belongs, since
// settings.json is tracked and settings.local.json is not.
//
// Never throws. A missing or malformed file leaves every default in place and
// returns false after reporting on stderr, so a bad settings file is a visible
// "started with defaults" rather than a crash.
bool LoadGameSettings(const std::string &path);

// The file LoadGameSettings was given, remembered so a window can write a setting
// back to the file this run actually read (`--settings other.json` included).
// "settings.json" until the first load.
const std::string &SettingsFilePath();

// Writes `mapPath` as `map.file` in the settings file at `path`, which is what
// makes the map on screen the map the next run starts on.
//
// The write changes that one value and nothing else. settings.json is
// hand-authored and hand-grouped (see SETTINGS.md), so the key is edited in place
// rather than parsed and dumped -- a dump would reflow the whole file and lose
// the blank lines and comments-by-arrangement that make it readable. Only a file
// that does not yet contain a `map.file` string is rewritten through nlohmann,
// because adding a key to arbitrary layout is not something a value substitution
// can do; that path is reported in `message`.
//
// Never throws. Returns true when the file now names `mapPath`; `message` always
// describes the outcome, and on success warns when settings.local.json sets
// `map.file` itself, since that layer is merged on top and would win at the next
// startup.
bool SaveStartupMapToSettingsFile(const std::string &path,
                                  const std::string &mapPath,
                                  std::string &message);

// --- the settings window's write path -------------------------------------
//
// Everything below writes `settings.local.json` beside the loaded settings
// file, never the tracked one. That layer is merge-patched on top at load, so an
// edit is durable without reflowing the hand-authored file, and it is where a
// value this machine disagrees about belongs.
//
// The running object is the same one the window displays, so an edit applies
// immediately. Whether the subsystem that consumed the old value notices is a
// different question: see SettingsSchema::Apply in SettingsSchema.h.

// The override file this run writes: `settings.local.json` in the settings
// file's directory.
std::string SettingsOverrideFilePath(const std::string &settingsPath);

// Whether the override file currently carries this field.
bool HasSettingOverride(const std::string &settingsPath,
                        const std::string &fieldPath);

// Applies `value` to the running settings, then records it in the override file.
// A JSON null on a field with a presence flag means "unset": the flag is
// cleared and the key is removed, which is how `temperature` goes back to
// "absent". `message` always describes what happened, including any correction
// the schema applied (a clamped or rejected value is written as corrected).
bool ApplySettingValue(const std::string &settingsPath,
                       const std::string &fieldPath,
                       const nlohmann::json &value, std::string &message);

// Removes a field from the override file and reloads, so the value comes back
// from settings.json or the compiled default -- exactly what the next launch
// would see. Reloading is why this is not just an erase.
bool ResetSettingValue(const std::string &settingsPath,
                       const std::string &fieldPath, std::string &message);

// The one collection, system1.profiles. The whole list is written, because a
// merge patch cannot delete an element of an array. `profiles` is the effective
// list (settings.json's entries plus the override's), so what is written is what
// the window shows.
bool ApplyProfilesValue(
    const std::string &settingsPath,
    const std::vector<GameSettings::SystemOneProfileSection> &profiles,
    std::string &message);

// Regenerates the field reference (SETTINGS.md) from the schema table.
bool WriteSettingsReference(const std::string &path, std::string &message);

// --- named AI backends ----------------------------------------------------
//
// `ai.profiles` entries are written one key at a time, so the override file
// merges per profile instead of replacing the registry: that is what makes an
// apiKey override for a single profile possible. Every write reloads, because a
// model block's `profile` reference is resolved during a load.
nlohmann::json AIProfileToJson(const GameSettings::AIProfileSection &profile);
bool ApplyAIProfileValue(const std::string &settingsPath, const std::string &id,
                         const nlohmann::json &entry, std::string &message);
bool RemoveAIProfile(const std::string &settingsPath, const std::string &id,
                     std::string &message);

// How many keys the files (main plus override) set inside `blockPath` other than
// `profile`. Used by the window to say "N explicit fields override this profile"
// so a hand-written override is never hidden behind the picker.
int CountExplicitBlockKeys(const std::string &settingsPath,
                           const std::string &blockPath);

// The command `art.generation.command` falls back to when it is empty but
// generation is enabled: `tools/gen_tile_art.py --from-worklist {worklist}`, run
// under `nix-shell -p python3Packages.pillow imagemagick` because neither tool is
// on the default PATH here. Lives in Settings.cpp so the one place that knows how
// to spell a working command is the one place that resolves it.
std::string BuiltinTileArtCommand();
