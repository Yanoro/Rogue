# Designer speech: reading the designer's reply aloud

**Status: implemented.** Everything below this line was written before any code existed; this
paragraph is what actually shipped, and the sections after it are left as the reasoning that produced
it, with the places the implementation diverged marked inline.

| built | where |
|---|---|
| engine abstraction | `include/TextToSpeech.h` |
| piper as a subprocess | `include/PiperSpeech.h`, `src/PiperSpeech.cpp` |
| worker thread, per-reply audio, transport | `include/SpeechQueue.h`, `src/SpeechQueue.cpp` |
| audio device, per-frame pump | `Game::Init` / `Game::Shutdown` / `Game::Update` (`src/Game.cpp`) |
| backend construction | `Game::ECSInit` (`src/GameECS.cpp`) |
| speech on a finished reply, bars, mute | `DesignerChatWindow::SpeakFor` / `DrawSpeechBar` / `Draw` |
| settings | `designer.tts` (`include/Settings.h`, `src/Settings.cpp`, `SETTINGS.md`) |
| voice model | `models/tts/en_GB-alba-medium.onnx` + `.onnx.json`, gitignored, fetched per SETTINGS.md |

Measured on the author's machine (Ryzen 5 5500, 6c/12t, no GPU), with the real 71-word notes from
`logs/designer/2026-10-15_14-31-17-designer.txt`:

| quantity | measured |
|---|---|
| synthesis time for 71 words | **1.69–1.78 s** |
| audio produced | 21.7–21.8 s |
| throughput | **12.3–12.8× realtime** |
| one 15-word sentence | 194,048 bytes = 4.40 s of audio |

That is roughly eight times faster than the two-core ARM figures in §4.1, and it is what makes the
non-streaming design in §6 correct rather than a compromise: a 1.7-second wait before the first word
is below the threshold where anyone would notice.

**Runtime behaviour is unverified.** The build is clean under AddressSanitizer, piper's exact
invocation was smoke-tested by hand, and the listening test — does it come out of the speakers, is
the accent bearable, does Mute actually silence it — has not been run. That is the repo owner's step.

Postscript: the first attempt *did* come back with "installed it and there is no sound". The cause was
not piper, not the settings and not the speech code — raylib was falling back to miniaudio's null
backend and reporting success. See §6.3; it is the most useful section in this document.

~~Nothing in the tree plays or synthesises audio today — there is no `InitAudioDevice` call anywhere
in `src/`, and the only matches for "speak" are prose in the NPC prompt and `TalkAction`.~~
(True when this was written; no longer.)

Line numbers were verified against the tree on 2026-10-15. `DesignerChatWindow` is under active
development and gained 11 lines while this document was being written, so prefer the symbol names in
each reference — they are the stable identifier, the numbers are not.

The goal: when the designer answers in the Designer Chat window, you hear its critique instead of
reading it — so you can look at the blueprint preview pane (or at the world) while it talks.

## 1. The idea

The designer window already draws the thing it made and the model's own opinion of it side by side
(`DesignerChatWindow::DrawPreviewPane`, `src/DesignerChatWindow.cpp:362`). Speech is the natural
next step for exactly the same reason the preview pane exists: `designerNotes` is *critique*, and
critique is better heard while looking at the object than read after it.

It is also the cheapest possible place to build a text-to-speech path. The designer is one agent,
one conversation, one reply at a time, and it is explicitly a development harness
(`include/DesignerChatWindow.h:17-24`) — so a broken or ugly voice costs nothing but a wasted
afternoon, while the pipeline it proves is the one NPC dialogue will need later.

### 1.1 Scope

In scope:

- Speak the designer's reply, in the Designer Chat window, when a stream finishes.
- A stop control. Non-negotiable, see §4.
- A backend abstraction, so the engine is a configuration choice rather than a rewrite.
- Settings and documentation for both.

Out of scope for the first version:

- Speaking during generation (§4.4 discusses the streaming variant).
- NPC dialogue, multiple simultaneous speakers, positional audio.
- Per-NPC voice assignment. When it happens, the voice belongs in the same data-driven place
  `RaceDef`/`Character` and the object templates already live, not in code.

## 2. What already exists

Everything needed to *trigger* speech on the main thread already exists and was written for
validation, not for audio. This is the reason the feature is cheap.

| what | where | why it matters |
|---|---|---|
| `InspectFinishedExchange(size_t)` | `src/DesignerChatWindow.cpp:426` | Runs **on the main thread, exactly once per finished stream**. This is the hook. |
| `Exchange::designerNotes` | `include/DesignerChatWindow.h:72` | The parsed critique, already extracted at `src/DesignerChatWindow.cpp:481-490`. |
| `Exchange::designOk` / `designId` / `savedPath` | `include/DesignerChatWindow.h:68-71` | Gives the one-line verdict prefix. |
| `Exchange::failureReason` / `design.refusal` | `include/DesignerChatWindow.h:73`, `:100` | The short, high-value refusals worth speaking. |
| `Exchange::fromUser`, `attempts`, `pendingRetryFor` | `include/DesignerChatWindow.h:63`, `:142-145` | Lets us speak only the final attempt. |
| `ExtractJsonBlock` | `src/DesignerChatWindow.cpp:216` | Its complement is the prose; the block itself must never be spoken. |
| `ThinkTagFilter` | `src/DesignerChatWindow.cpp:48` | Reasoning blocks are already dropped before anything sees them. No work needed. |
| raylib `AudioStream`, `UpdateAudioStream` | `build/_deps/raylib-src/src/config.h:39` | `SUPPORT_MODULE_RAUDIO` is **1**, verified in the actual build tree. |

Two absences to fix:

1. **`InitAudioDevice()` is never called.** raylib compiling its audio module in is not enough — the
   device must be opened in `Game::Init` and closed in `Game::Shutdown`. Guard with
   `IsAudioDeviceReady()`: a missing or busy device must not take the game down, in the same spirit
   as the null-designer-backend path documented at `include/DesignerChatWindow.h:45-47`.
2. **Nothing pumps the stream.** See §7.3 for why this cannot live in `Draw()`.

## 3. What to speak

This section matters more than the choice of engine. A correct engine reading the wrong text
produces a worse feature than a bad engine reading the right text.

### 3.1 Never speak the JSON

In `logs/designer/2026-10-15_14-31-17-designer.txt` the reply is 6499 bytes, of which the fenced
`json` block is the overwhelming majority. Spoken verbatim it becomes *"id two bedroom house name
two bedroom house purpose dwelling designer notes the layout gives…"* followed by tile names like
`rect_fill`, `rect_outline` and `carve`. It is unlistenable and it is 90% of the bytes.

The rule: strip the fenced block, speak what surrounds it. `ExtractJsonBlock` already isolates the
block; the prose is the rest.

### 3.2 Prose and `designerNotes` are largely redundant

Counted from that same transcript:

| part | words | chars |
|---|---|---|
| prose before the fence | 70 | 394 |
| `designerNotes` | 71 | 400 |
| **total if both are read** | **141** | **794** |

141 words is roughly **56 seconds of speech** at 150 wpm. And the two halves say nearly the same
thing: the prose reports "two bedrooms on the north side, kitchen and bathroom on the south", and
the notes report the same layout plus the actual critique ("I had to use door tiles for interior
doors too because there is no separate interior door material").

**The transcript's "door tiles" is the vocabulary error, not a fact about the world.** A door is an
**object**, never a tile; the phrase should be read as "door objects" (see
[tile-and-object-vocabulary.md](tile-and-object-vocabulary.md)). The log is quoted as written, but
the speech target below uses the corrected word.

**Recommendation:** speak the verdict line plus `designerNotes`, not the prose. The notes are the
reason the window exists (`include/DesignerChatWindow.h:30-32`), they are the part that is *not*
recoverable by looking at the preview, and reading both doubles the length for half the value.

Target utterance:

> *Saved as two bedroom house.* The layout gives both bedrooms a windowless but private north side,
> while the living room, kitchen, and bathroom share the south side. I had to use the same door
> object for interior doors too…

### 3.3 Speak only the final attempt

With `maxDesignAttempts = 3` (`include/Settings.h:107`) a conversation can produce three streams
before it succeeds. Speaking every stream means reading three blueprints and two refusals, each
interleaved with the next. Gate on: speak when there is no pending retry
(`pendingRetryFor == -1`) and the attempt is the last one.

### 3.4 Refusals are the best speech in the feature

`"REFUSED — material 'glass' is not in the catalog"` is one sentence, arrives right when you are
looking at the failed design in the preview pane, and tells you what to ask for next. This is the
case where speech beats the transcript outright: it is short and it is actionable.

Do not pad it into "I could not save your design because…". Read the engine's reason
(`failureReason`) as written.

### 3.5 The stop control is mandatory

A 56-second monologue that cannot be interrupted is worse than no feature. There must be a way to
kill audio from:

- a new user message (`SendUserMessage`, `src/DesignerChatWindow.cpp:240`),
- the start of a retry,
- an explicit Stop button in the window,
- game shutdown.

Any queued audio must be dropped, not merely muted.

### 3.6 Log what was spoken

`LogEvent` already timestamps the interesting moments (send, reply, verdict, retry). Add the spoken
text as an event too: when the audio is wrong, the only way to tell a wrong transcript from a wrong
voice is to see what was handed to the engine.

## 4. The backend: Piper vs Kokoro

Both are local, both are CPU-only here, and both are viable. They are not equivalent, and the
difference is not "quality vs speed" so much as "turnkey vs assembly required".

### 4.1 Piper

Piper is a VITS-based neural TTS with per-voice ONNX models. It ships in the pinned nixpkgs
channel as `piper-tts` 1.3.0 (OHF-Voice/piper1-gpl), which is `mainProgram = "piper"`.

The interface is close to ideal for this project: with no text argument the CLI **reads text line by
line from stdin**, and with `--output-raw` it **writes int16 mono PCM to stdout, flushing after
every chunk**.

| | |
|---|---|
| model size | 63.2 MB per medium voice (`fr_FR-siwis-medium`), 22050 Hz |
| measured throughput | **x8.11–x8.47 realtime**, median x8.32, on 2 ARM cores |
| load time | 1.71–1.96 s (that machine) |
| peak RSS | ~431 MB with two voices resident |
| effect of sentence-splitting | **none** — x8.38–8.59 on 12 chunks vs x8.50–8.66 on 22 |
| licence (engine) | **GPL-3.0** — nixpkgs metadata says `gpl3Only`, the pip package reports GPL-3.0-or-later |
| licence (voices) | varies per voice: `siwis` CC-BY 4.0, `tom` **AGPLv3** |

Two caveats that matter here:

- **It uses both cores.** At ~190% CPU during synthesis, throughput roughly halves if a core is
  taken. In a game, the render loop and the game loop are the competition.
- **The GPL is only a problem if you link it.** As a subprocess it is an independent program and
  AIRogue stays clean. Linking `libpiper` in-process would make the GPL's terms apply to the
  combined work. This is a real architectural argument for the subprocess, independent of ASan.

### 4.2 Kokoro

Kokoro-82M is a StyleTTS2 + ISTFTNet model: Apache-2.0 weights, 82M parameters, 8 languages and 54
voices, 24 kHz output. Its quality is, by community consensus, clearly better than Piper's mid-size
voices — more natural prosody, more expressive.

| | |
|---|---|
| weights | `kokoro-v1.0.onnx` 325,532,387 B + `voices-v1.0.bin` 28,214,398 B = **~354 MB** |
| sample rate | 24000 Hz (Piper medium is 22050 Hz) |
| measured throughput | **x0.91–0.93 realtime** — *slower than it speaks*, same 2-core machine |
| load time | 1.04–1.06 s |
| peak RSS | ~578 MB |
| effect of sentence-splitting | **−8%** (x0.95 → x0.87), ~0.51 s fixed cost per call |
| licence (weights) | Apache-2.0 |
| frontend | `misaki` G2P, which itself needs `espeak-ng` installed |

The benchmarking article is emphatic that it measured speed and licence, not quality, and I have no
listening test either — so treat "Kokoro sounds better" as reported consensus, not a number.

The decisive facts for *this* repo are not in that table:

1. **Kokoro is not in nixpkgs.** `kokoro`, `kokoro-onnx`, `misaki` and `sherpa-onnx` are all absent
   from the pinned channel. (PyTorch *is* packaged, but it is a multi-gigabyte dependency to pull in
   for one voice line.)
2. **The ONNX file is not a drop-in VITS model.** Piper voices are self-contained: model + config,
   and the engine owns the whole frontend. Kokoro needs text → IPA via misaki → a specific phoneme
   vocabulary → token ids, plus a 510-token chunk limit and a per-voice style vector out of
   `voices-v1.0.bin`. That frontend is the work, and it is not small.
3. **The practical route is `sherpa-onnx`**, which does support Kokoro through its C API and ships
   prebuilt shared libraries as release tarballs (e.g. `sherpa-onnx-v1.13.8-linux-x64-shared-cpu-lib`,
   also mirrored on HuggingFace as `csukuangfj/sherpa-onnx-libs`). That is a real option — but it is
   a manually-pinned binary dependency in a project whose dependency discipline is nixpkgs +
   FetchContent, and it must be verified before it is relied on.

### 4.3 The comparison that decides it

| | Piper | Kokoro |
|---|---|---|
| in nixpkgs | **yes** (`piper-tts`) | no |
| integration work | one shell.nix line, then subprocess | write a backend against sherpa-onnx, or build one |
| zero-integration path | **yes** — stdin lines → raw PCM stdout | no |
| model on disk | 63 MB | 354 MB |
| CPU per second of speech (2-core ARM) | ~0.12 s | ~1.08 s |
| sentence-splitting penalty | none | −8% |
| license of weights | per-voice, mixed (one AGPLv3 seen) | Apache-2.0 |
| voice quality | good, TTS-ish | better |
| voice count / variety | large catalog, incl. en_GB | 54 voices, 8 languages |

Neither catalog detail should be assumed: confirm exact voice IDs (both have British English voices,
which suits a medieval village) when picking one.

### 4.4 Recommendation: build the seam, ship Piper, keep Kokoro as a second backend

Define `TextToSpeech` as an interface with `PiperSpeech` as the first implementation, exactly as
`AI` (`include/AI.h:47`) fronts `OllamaAI`/`GeminiAI`/`OpenRouterAI` today. Then:

- **v1 ships Piper**, because it is one line in `shell.nix`, needs no build changes, and its CLI is
  already the interface we want.
- **Kokoro becomes `KokoroSpeech`** behind the same interface whenever the voice quality starts to
  matter — which is when NPC dialogue arrives, not now, because the designer's notes are a
  developer-facing critique.
- The seam costs almost nothing to define now and is expensive to retrofit later, which is the whole
  argument for doing it first.

If the answer turns out to be "Piper's voice is not good enough to listen to", that is a reason to
add the Kokoro backend, not to abandon the feature: the content policy in §3 is what makes the
feature pleasant, and it is backend-independent.

## 5. Process model and framing

The one genuine engineering decision: `--output-raw` has **no end-of-utterance marker**. Feed a
long-lived Piper process two lines and the reader cannot tell where the first utterance's samples
end and the second's begin — it does not know the sample count in advance.

| approach | first-audio latency | framing | notes |
|---|---|---|---|
| **one process per utterance** (`piper --output-raw -- "text"`, read to EOF) | ~1–1.5 s (est.) | **EOF does it for you** | correct by construction; no state between utterances |
| long-lived process, line per utterance | ~0.3 s (est.) | ambiguous — needs a protocol we do not have | the latency win is the only argument for it |
| `piper.http_server` + POST `/synthesize` | ~0.3 s (est.) | WAV header gives framing | a second process, and the `[http]` extra |

**Recommendation: one process per utterance for v1.** The reply itself takes 5–30 s to generate, so
a ~1 s start latency is invisible, and the failure mode it avoids (statement 3's audio bleeding into
statement 4, or a stop that cannot find the boundary) is exactly the kind of bug that is miserable to
debug. Move to the HTTP server later only if the delay is actually perceptible.

Latency estimates are inferences from the load/import times above, not measurements on this machine.

Consequences to handle:

- **Reap the child.** `pclose`/`waitpid`, always, including on stop and on the crash path. A leaked
  synthesis process per reply is a slow leak of a 200 MB process.
- **A missing voice or a missing binary must be a reported state, not a crash** — same discipline as
  the null designer backend.
- **Backpressure is free.** If the ring buffer (§7.2) is full, stop reading the pipe; Piper blocks on
  write and synthesis pauses. Do not buffer the whole utterance: ~56 s of 22050 Hz int16 is ~2.4 MB,
  which is not a problem in itself, but buffering it all defeats the point of streaming.

## 6. The audio path

raylib owns the device. `SUPPORT_MODULE_RAUDIO` is 1 in this build tree, `shell.nix` already has
`alsa-lib`, and this machine runs PipeWire with `pipewire-pulse`, so miniaudio will find a device.

- `InitAudioDevice()` in `Game::Init`, `CloseAudioDevice()` in `Game::Shutdown`, guarded by
  `IsAudioDeviceReady()`.
- **The sample rate comes from the backend, never a constant.** Piper medium is 22050 Hz and Kokoro
  is 24000 Hz; hardcoding either makes the other one sound like a chipmunk or a demon, and the
  symptom looks like a synthesis bug rather than a rate mismatch. This is why the interface has
  `SampleRate()`.

### 6.1 IMPLEMENTED DIFFERENTLY: a whole utterance, not a stream

§6 as first drafted said "one `AudioStream` per speaking consumer" fed chunk by chunk with
`UpdateAudioStream`. **That is not what shipped**, because raylib 5.5 makes it a trap:

> `UpdateAudioStream()` writes into one half of a double buffer and **zero-fills whatever is left
> over of that half** (`raudio.c`, `UpdateAudioStreamInLockedState`).

So any write smaller than the sub-buffer inserts a burst of silence. The sub-buffer is
`sampleRate/30` frames **or the device period if that is larger** — whichever wins, and neither is
reachable through the public API (`SetAudioStreamBufferSizeDefault` exists, but there is no getter,
and the device period can silently override what you set). Streaming chunk-at-a-time therefore
stutters unless the chunk size happens to match a value the game cannot query.

**What shipped instead:** the worker collects the whole utterance, and the main thread plays it as a
single `Sound` built by hand from the samples (`Wave` → `LoadSoundFromWave`, which copies and converts
to the device format). No `AudioStream`, no pump, no sub-buffer alignment, nothing to get wrong.

This is defensible rather than lazy because of the numbers in the status block: the notes synthesize
in 1.7 s and play for 21.7 s. Streaming would save about a second of latency and cost a class of
audible stutter bugs. It is also bounded honestly — an utterance is tens of seconds of 22050 Hz mono,
a couple of megabytes before conversion.

Revisit only for a long utterance (a full blueprint read aloud under `content = everything` would be
minutes, and holding that in memory as converted float32 starts to matter).

### 6.2 Why the "pump" cannot live in `Draw()`

`DrawGameWindows` iterates `ecs.filter<ActiveWindow>()` (`src/GameDraw.cpp:24`), so
`DesignerChatWindow::Draw()` **only runs while the window is visible**. Collecting audio from `Draw()`
means closing the window mid-reading leaves the sound un-fed -- and it would look like the audio
device dying, not like a window toggle.

`SpeechQueue::Update()` is called from `Game::Update` (`src/Game.cpp`), before `ecs.progress()`. With
Music streams the rule is stricter than before, not weaker: raylib requires `UpdateMusicStream()` every
frame or a stream stalls once its initial buffers drain, so this is not merely where new audio is
picked up, it is what keeps playback running.

### 6.4 The per-reply transport bar

Added after the first version shipped, on the request that a reply should be scrubbable and
stoppable where it is. This is the change that forced `Sound` out and `Music` in.

**`raylib::Sound` cannot do this.** It has no seek and no time API at all: `LoadSoundFromWave`,
`PlaySound`, `StopSound`, `PauseSound`, `ResumeSound`, `SetSoundVolume`, `IsSoundPlaying`, and
nothing that says *where* in the sound playback is. A scrub bar is not implementable with it.

**`Music` has the whole transport.** The utterance is wrapped in a 44-byte PCM WAV header in memory
and loaded with `LoadMusicStreamFromMemory(".wav", bytes, size)`, which unlocks
`SeekMusicStream(seconds)`, `GetMusicTimePlayed`, `GetMusicTimeLength`, `PauseMusicStream`,
`ResumeMusicStream`, `StopMusicStream` and `UpdateMusicStream`. It is also *cheaper* than the `Sound`
path: a Music stream decodes on demand, where `LoadSoundFromWave` converted the whole utterance to
float32 at the device rate up front.

Measured on the machine, exercising the real queue under ASan:

| step | result |
|---|---|
| synthesis of a 12-word line | ~1.1 s warm (4.17 s of audio produced) |
| same, cold | > 4 s — which is why the bar shows `preparing...` rather than appearing late |
| seek to 75 % while playing | position 1.22 s → 3.15 s of 4.17 s |
| pause | position holds within 0.15 s over 400 ms |
| stop | back to `Ready`, position 0, plays again from the top |
| scrub a never-played clip | loads it, paused, at that point |
| `Stop()` then scrub | retained audio still usable (stopping ≠ discarding) |

**Retention, and the pool limit that shapes it.** Every spoken reply keeps its audio, so its bar keeps
working for the rest of the session (cap: `kMaxRetainedClips = 24`; past that the oldest non-playing
clip is dropped and its bar offers `Speak` again). Only the WAV *bytes* are retained — 1–2 MB each.
The `Music` stream is created on demand for whichever clip is being played and the previous one is
unloaded, because raylib's audio buffer pool holds **16** entries shared with `Sound` objects:
loading a stream per retained reply would exhaust it at the seventeenth.

That split is also what keeps eviction safe. `dr_wav` keeps a **pointer** into the buffer handed to
`LoadMusicStreamFromMemory` rather than copying it, so freeing a clip's bytes while a stream still
points at them is a use-after-free — which is why eviction skips the clip the live stream was loaded
from, and why the docs comment there says so.

**Which replies get a bar.** Only ones that were actually handed to the engine: saved designs, the
refusal that exhausted `maxDesignAttempts`, and transport failures. An intermediate refusal that is
about to be retried never reached the engine, so a bar under it would be a control for something that
never happened. `Exchange::spokenText` marks a reply as spoken — including the case where it was
handed over and no audio came back, where the bar shows `Speak` instead. The user's own messages get
no bar.

The window keeps no transport state of its own. `Exchange::speechId` is a handle into the queue, and
every button is a direct call on it, so the bar cannot disagree with what is playing. The one write
back into the transcript is when `Speak` creates a fresh clip for an evicted or never-spoken reply.

### 6.3 THE TRAP THAT COST A SESSION: the silent null device

**Symptom.** Everything looks correct. The designer replies, `SPOKEN: …` is written to the session
log, `InitAudioDevice()` logs `AUDIO: Device initialized successfully`,
`IsAudioDeviceReady()` returns **true**, `PlaySound()` runs, `IsSoundPlaying()` goes true and then
false after exactly the right duration. And there is no sound, ever.

**Cause.** raylib's raudio `dlopen()`s its audio backend at runtime — `MA_NO_JACK` is the only
backend it hard-disables, and neither ALSA nor PulseAudio appears in the link line raylib hands out.
When every backend fails to load, miniaudio silently falls back to **`ma_backend_null`**, which
accepts every device request and discards every sample. raylib reports that as a success, because
from its side it *is* one.

Measured here, by running the same binary with and without the ALSA directory on the loader path:

| | ALSA not loadable | ALSA loadable |
|---|---|---|
| `ma_get_backend_name(context.backend)` | `miniaudio \| **Null**` | `miniaudio \| ALSA` |
| `IsAudioDeviceReady()` | **true** | true |
| `libasound` actually loaded | **no** | yes |

So `IsAudioDeviceReady()` cannot be trusted as "you will hear something". It only ever meant "a
device object exists".

**Why it happened here.** This is NixOS and there is no `/etc/ld.so.cache` (verified: `ldconfig -p`
reports 0 entries), so `libasound.so.2` is not on the default search path. The game's RUNPATH did
contain a `nix-shell/lib` directory, but that directory does not contain `libasound.so.2`, and
nothing set `LD_LIBRARY_PATH` to include the `alsa-lib` output. net effect: no backend could be
loaded.

**The fix, in `CMakeLists.txt`.** Link the audio backends explicitly, with `--no-as-needed` so they
are not dropped as unreferenced:

```cmake
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(PULSEAUDIO REQUIRED libpulse)
target_link_options(AIRogue PRIVATE "-Wl,--no-as-needed")
target_link_libraries(AIRogue PRIVATE ${ALSA_LIBRARIES} ${PULSEAUDIO_LIBRARIES})
set_target_properties(AIRogue PROPERTIES
    BUILD_RPATH "${ALSA_LIBRARY_DIRS}:${PULSEAUDIO_LIBRARY_DIRS}")
```

Both halves matter. Making them `DT_NEEDED` means the loader maps them before `main()`, so
miniaudio's later `dlopen("libasound.so.2")` finds an already-loaded object and never searches the
filesystem. The RUNPATH is what makes that startup load succeed in the first place. Verified after
the change: `ldd build/AIRogue` lists `libasound.so.2` and `libpulse.so.0`, and the backend is
`miniaudio | PulseAudio` — via PipeWire's pulse server — with **no** audio path in
`LD_LIBRARY_PATH`.

`SetAudioStreamBufferSizeDefault` is not the issue here and neither is the sample rate; the device
was never silent because of a format problem.

**The guard, in `SpeechQueue::Init()`.** Because raylib will not admit to the null backend, the
queue probes for the backend libraries itself (`dlopen` with `RTLD_NOLOAD`, then a plain `dlopen`),
and when none of them can be loaded it sets `AudioWarning()`. The Designer window renders that as a
`SILENT` marker next to the speech status, with the explanation in its tooltip, and `Game::ECSInit`
logs it. Speech still runs in that state — refusing would replace a clear symptom with a confusing
one — but it can no longer be mistaken for working.

The general lesson, worth keeping: **a silent audio failure in this project is a loader-path
problem until proven otherwise, and `IsAudioDeviceReady()` is not evidence.**
The window's visibility then decides only whether you *see* the transcript, not whether you *hear*
it — which is the desirable behaviour for this feature: closing the pane to watch the world while
the critique plays is a real use.

## 7. Proposed code shape

Deliberately the same shape as the AI providers, so there is one pattern in the codebase for
"backend the game talks to", not two.

```
include/TextToSpeech.h   // abstract backend, mirrors AI.h
include/SpeechQueue.h    // owns backend + AudioStream + ring buffer + worker thread
include/PiperSpeech.h
src/PiperSpeech.cpp      // subprocess: piper --output-raw, stdin lines -> stdout PCM
src/SpeechQueue.cpp
src/TextToSpeech.cpp     // if anything turns out to be shared
```

```cpp
// include/TextToSpeech.h
class TextToSpeech {
public:
  // int16 mono PCM, in chunks, as the engine produces them. Runs on the synthesis
  // thread, never the main thread.
  using AudioCallback = std::function<void(const int16_t *samples, size_t count)>;

  // Blocking for the duration of the utterance. Returns false if the engine could
  // not be started or produced no audio; `errorLogger` carries the reason.
  virtual bool Speak(const std::string &text, AudioCallback onAudio) = 0;
  virtual void Stop() = 0;

  // The device stream must be created at this rate. Piper medium is 22050, Kokoro
  // is 24000; a mismatch is audible and looks like a synthesis fault.
  virtual int SampleRate() const = 0;

  virtual std::string getEngineName() const = 0;
  virtual std::string getVoiceName() const = 0;
  virtual ~TextToSpeech() = default;
};
```

`SpeechQueue` owns one `TextToSpeech*` (borrowed, like `designerAI`), one `AudioStream`, one worker
thread and one small ring buffer with a mutex. `Enqueue(text)` may be called from the main thread at
any time; `Stop()` clears the queue, kills the child and silences the stream. `Game` holds the
`SpeechQueue` and pumps it in `Update`.

Files to add to `SOURCES` in `CMakeLists.txt` — forgetting that is exactly how `src/GameSystems.cpp`
became dead code.

## 8. Settings and data

`designer` is its own section (`include/Settings.h:86-108`) and currently absent from both
`settings.json` and `SETTINGS.md` — the TTS work makes that gap visible, so document the section
while adding to it.

```jsonc
"designer": {
  "speakReplies": false,        // off by default: this is a dev window
  "tts": {
    "backend": "piper",         // "piper" | "kokoro" (latter not implemented in v1)
    "voice": "en_GB-alba-medium",
    "voiceDir": "models/tts",
    "lengthScale": 1.0,         // speaking rate; 1.0 is the voice's default
    "what": "notes"             // "notes" | "prose+notes" | "all"  (see §3.2)
  }
}
```

Parse it in `src/Settings.cpp` beside the existing `designer` block (`src/Settings.cpp:180-213`),
with the same clamping discipline as `maxDesignAttempts`.

**Voices do not go in git.** A Piper voice is 63 MB and Kokoro's package is 354 MB, against a
`.gitignore` that currently covers only secrets (`key`, `router-key`, `model.json`,
`settings.local.json`). Add `models/` to `.gitignore` and document the one-time fetch, the way a
missing `model.json` is already a documented state rather than a broken checkout.

Fetch step (Piper, once):

```sh
python3 -m piper.download_voices en_GB-alba-medium --data-dir models/tts
```

## 9. Build, Nix and ASan

- **`shell.nix`**: `piper-tts` added to `buildInputs`. Nothing else — no CMake dependency, because the
  engine is a subprocess. Voices are a separate download (§8 / SETTINGS.md).
- **Name trap:** `pkgs.piper` is **libratbag's gaming-mouse configuration tool**, not a TTS engine.
  The package is `piper-tts`. Verified in the pinned channel source, not from memory.
- **Closure note:** nixpkgs builds `piper-tts` with `withTrain`/`withHTTP`/`withAlignment` all
  defaulting to true, which drags in PyTorch, pytorch-lightning, librosa and Flask for a CLI that
  only needs to read stdin and write stdout. Overriding them off is one line, but that variant is not
  in the binary cache and builds piper-tts plus its patched espeak-ng from source. The plain package
  is what `shell.nix` uses, with the override recorded in a comment for when closure size matters.
- **ASan is an argument for the subprocess, not against the feature.** Linking `libpiper` in-process
  would put a system-built onnxruntime and espeak-ng inside an `-fsanitize=address` binary: leak
  reports at every exit, interceptor noise, and threading inside ORT that the sanitizer cannot see
  into. Out of process, none of it is AIRogue's address space, and a wedged synthesis is a `kill()`
  rather than a hung game. The GPL argument in §4.1 points the same way.

## 10. Incremental plan

1. ~~**Audio out, no model.**~~ Shipped as part of step 2 once the subprocess path was proven: the
   voice was downloaded and piper's exact invocation smoke-tested (`printf ... | piper -m ... 
   --output-raw`) before any C++ was written, which is the same de-risking with one less throwaway.
2. **`TextToSpeech` + `PiperSpeech` + `SpeechQueue`.** ✅ Subprocess per utterance, whole-utterance
   playback (§6.1), collected on a worker thread, played from `Game::Update`.
3. **Wire into `InspectFinishedExchange`.** ✅ Content policy from §3, mute and stop controls,
   settings, `SPOKEN:` lines in the session log.
4. **Deferred:** sentence-chunked streaming, and the Kokoro backend. Streaming bought ~1 s of latency
   in exchange for the sub-buffer problem in §6.1, so it is not worth doing for its own sake — but a
   `KokoroSpeech` behind `TextToSpeech` is still the right next move *if the voice quality is judged
   insufficient*, which is a listening decision, not an engineering one.

Step 4's streaming variant is worth a note on *how*, because the natural place already exists: the
stream callback (`src/DesignerChatWindow.cpp:308`) carries tokens on a provider thread, and
`ThinkTagFilter` is already a stateful filter sitting in that path. A sentence chunker beside it
would speak the prose as the model types it — but it must be gated on "before the opening fence",
or it will read the JSON aloud, and prose is the half §3.2 recommends dropping anyway. That tension
is why streaming belongs after the content policy is settled, not before.

## 11. Limits and risks

- **Cost per reply is real but small.** ~141 words is ~56 s of audio; at Piper's measured ~0.12 s of
  CPU per second of speech that is ~7 s of CPU per reply, spread across the playback. It steals
  roughly one core while speaking. Acceptable for one agent; needs a real answer before fifty NPCs
  speak at once.
- **Piper's output duration varies run to run** (2.8–3.7% spread measured). Irrelevant for
  playback; relevant if audio is ever synced to animation.
- **Quality is unmeasured.** No blind listening test exists in either direction here, and the
  benchmark author says the same. Expect to judge it by ear in step 2 or 3.
- **`--output-raw` framing** is the one design risk, and §5's per-utterance process removes it.
- **The designer window is a dev harness.** If it is ever deleted, the speech path should survive it:
  another reason `SpeechQueue` hangs off `Game`, not off the window.

## 12. Open questions

1. **Speak the prose at all?** §3.2 recommends notes-only. The counter-argument is that the prose is
   the part that describes the *shape* of the building, and the notes assume you can see it.
2. **Should closing the Designer window stop the speech?** §6.1 leans no — the point is to look at
   the world while it talks — but "the pane is closed so it should be quiet" is defensible.
3. **Is there a per-NPC voice identity worth designing now?** The villagers are the interesting case,
   and voice-per-NPC would need a data field. It is out of scope for v1, but the `TextToSpeech`
   interface should not make it harder.
4. **Does the voice need to be British?** A medieval village with American-accented villagers is a
   tonal decision, not a technical one.
5. **`piper.http_server` instead of per-utterance subprocess**, once the start latency is measured
   rather than estimated.

## 13. References

Verified locally:

- `include/DesignerChatWindow.h`, `src/DesignerChatWindow.cpp` — the hook points in §2.
- `include/Settings.h:86-108`, `src/Settings.cpp:180-213` — the `designer` settings section.
- `build/_deps/raylib-src/src/config.h:39` — `SUPPORT_MODULE_RAUDIO 1`.
- `logs/designer/2026-10-15_14-31-17-designer.txt` — the word counts in §3.2.
- Pinned nixpkgs source: `pkgs/by-name/pi/piper-tts/package.nix` (1.3.0, GPL-3.0-only),
  `pkgs/by-name/pi/piper/package.nix` (the libratbag mouse tool), and the absence of
  `kokoro`/`kokoro-onnx`/`misaki`/`sherpa-onnx`.

External:

- Piper repository and CLI docs — [OHF-Voice/piper1-gpl](https://github.com/OHF-Voice/piper1-gpl),
  [docs/CLI.md](https://github.com/OHF-Voice/piper1-gpl/blob/main/docs/CLI.md),
  [libpiper C/C++ API](https://github.com/OHF-Voice/piper1-gpl/tree/main/libpiper),
  [HTTP API](https://github.com/OHF-Voice/piper1-gpl/blob/main/docs/API_HTTP.md).
- Kokoro-82M model card —
  [hexgrad/Kokoro-82M](https://huggingface.co/hexgrad/Kokoro-82M) (Apache-2.0, 82M params,
  8 languages / 54 voices, 24 kHz, ~354 MB of weights, `misaki` G2P).
- CPU benchmark and licence table behind the speed numbers and the sentence-splitting result —
  ["Kokoro-82M computed slower than it spoke. Piper TTS was 8.7 to 9.3 times faster."](https://dev.to/obole/kokoro-82m-computed-slower-than-it-spoke-piper-tts-was-87-to-93-times-faster-3lgh),
  with its [raw data and scripts](https://github.com/obole-ia/tts-cpu-benchmark). Machine: 2-core
  ARM Neoverse-N1, no GPU — so the *ratios* are indicative, not portable to this Ryzen.
- sherpa-onnx, for the Kokoro route —
  [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (C API examples for Kokoro) and
  prebuilt shared libraries via [csukuangfj/sherpa-onnx-libs](https://huggingface.co/csukuangfj/sherpa-onnx-libs).
