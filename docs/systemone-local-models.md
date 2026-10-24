# Running a local System 1 model (Kev-4B)

**Status:** working on this machine, CPU and CUDA. CPU verified end to end; the
CUDA build was added 2026-10-18.
**Date:** 2026-10-16 (CPU); 2026-10-18 (CUDA).
**Scope:** how to serve a local decision model and point AIRogue's System 1 client
at it instead of hosted Jev. Companion to `docs/agent-attention-design.md` (what
System 1 is for) and `SETTINGS.md` §`system1` (the config).

---

## 1. Why bother

Hosted Jev costs $0.042/1M input tokens and about 0.5–0.8 s per request, and every
state you send leaves the machine. A local decision model is free and private.
The measured difference on this hardware is not small, though — see §6.

The model in use is **Kev-4B** (`ggml-org/Kev-4B-GGUF`, Apache 2.0, 3.03 GB at
Q4_K_M): a 4B decision model that answers the same `choice`/`noul`/`score`
protocol as Jev. It was chosen over the alternatives for two reasons:
**Laya** (0.4B, 449 MB) fits even more easily but has a much shorter input budget,
and the site-preference state is a list of candidate descriptions; **OpenJev**
(27B) is the closest to Jev in quality but is 19 GB and CC BY-NC, so it is neither
practical here nor usable commercially.

---

## 2. The two traps, both hit while setting this up

**Trap 1: the nixpkgs `llama-cpp` cannot do this.** The package in this machine's
nixpkgs (`25.11pre-git`, llama.cpp build 6981) predates llama.cpp's
decision-model merge of 2026-10-02. Its binary contains `/v1/chat/completions`,
`/v1/embeddings`, `/v1/rerank` — and **no `/v1/systemone`**, so it cannot load a
decision model in decision mode at all. This was checked by grepping the
unstripped binary, not assumed. The server has to be built from upstream master;
`scripts/build_llama_server.sh` does that once.

**Trap 2: the default context will OOM the machine.** llama-server's `-c 0` means
"take the context size from the model metadata", and Kev's is large. On a 15 GiB
machine that produced:

- an **8 GB RSS**, RAM full, the box in heavy swap,
- `available` down to 1.8 GiB,
- a server stuck at "Loading model" for over three minutes.

With `-c 2048` the same server is **3.3 GB RSS** and loads in ~70–115 s. A
decision request is a short state plus a few questions: the three-question probe
cost Kev **143 input tokens**, and even a site-selection state with ten candidate
descriptions is a few hundred. 2048 is generous; do not raise it without a reason.

If you want a hard guarantee, run the server under a scope:

```bash
systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=1G \
  --unit=kev-s1-demo scripts/run_decision_server.sh
```

---

## 3. One-time setup

```bash
# 1. the model (3.03 GB)
mkdir -p models
curl -L -C - -o models/Kev-4B-Q4_K_M.gguf \
  https://huggingface.co/ggml-org/Kev-4B-GGUF/resolve/main/Kev-4B-Q4_K_M.gguf

# 2. the server (one-off build, ~2 min; CPU + OpenBLAS)
scripts/build_llama_server.sh
```

Both artifacts are gitignored: `models/` and `.deps/`.

---

## 4. Running it

Terminal 1 — the server. It prints its URL and stays in the foreground:

```bash
scripts/run_decision_server.sh                     # 127.0.0.1:9931, ctx 2048
PORT=8081 CTX=4096 scripts/run_decision_server.sh # overrides
NGLAYERS=0 scripts/run_decision_server.sh          # explicitly CPU
```

It listens on `127.0.0.1` only, so nothing else on the network can reach it and
**no API key is needed** — which is why the local config uses a placeholder key.

Terminal 2 — verify, cheapest check first:

```bash
scripts/test_decision_server.sh                    # raw curl, no engine involved
./build/AIRogue --settings /tmp/kev-settings.json --systemone-test
```

The first proves the server speaks the protocol. The second proves AIRogue's own
client (`SystemOne::JevClassifier`) parses it, which is the path the call sites
will actually use. Keeping them separate is deliberate: if curl works and the
engine does not, the fault is in the provider.

---

## 5. Pointing AIRogue at it

`settings.local.json` is the natural place, because it is gitignored and merged
over `settings.json`:

```json
{
  "system1": {
    "useMainModel": false,
    "model": {
      "model": "local",
      "endpoint": "http://127.0.0.1:9931/v1/systemone",
      "apiKey": "local-no-key-needed"
    }
  }
}
```

Three details that are easy to get wrong:

- **`endpoint` must be the decisions URL**, not `/v1/chat/completions`. A chat
  endpoint has no `questions`/`answers` shape.
- **`apiKey` must be non-empty** even though the local server ignores it:
  `JevClassifier::Available()` returns false on an empty key, so the client refuses
  before sending anything. It is a placeholder, not a credential.
- **`model` is ignored by llama-server** (it reports the path it loaded), so any
  non-empty string works. It is still recorded in the log so a later reader can
  tell local from hosted.

---

## 6. What it actually costs, measured here

| | hosted Jev | Kev-4B, CPU-only |
|---|---|---|
| per decision (3 questions) | 0.47–1.08 s | **10.5–12.8 s** |
| input tokens | 413 | 143 |
| cost | $0.0000173 | 0 |
| load / startup | none | 70–115 s |
| RAM | none | 3.3 GB |

The 143 vs 413 gap is protocol, not model: the hosted endpoint also charges for
its own scaffolding.

**10+ seconds is fine for a comparison run and too slow for a game loop.** The
build above is CPU + OpenBLAS because a CUDA configure failed on toolkit discovery
(`ggml/src/ggml-cuda/CMakeLists.txt`: "CUDA Toolkit not found" — the bare
`cudaPackages.cuda_nvcc` shell does not place it where CMake looks). The RTX 3050
in this machine would plausibly bring it to well under a second, and the
reconfigure is documented at the end of `scripts/build_llama_server.sh`. It was
not forced through here because the CPU build answers the correctness question,
and building CUDA toolchains on a 15 GiB box while another build is running is how
you OOM the machine.

### CUDA build (2026-10-18)

A CUDA tree now exists at `.deps/llama.cpp/build-cuda` (RTX 3050, `sm_86`), and
`scripts/run_decision_server.sh` prefers it over `build/`. `LLAMA_BUILD=build`
forces the CPU tree.

- **FlashAttention is off** (`-DGGML_CUDA_FA=OFF`): CUDA 12.8.93's `cicc`
  segfaults on the FA MMA template instances with this host compiler. Decision
  requests are short, so the fallback attention kernels are fine.
- **Why the toolkit is merged:** nix's `cuda_nvcc` output has `nvcc` but not
  `cuda_runtime.h` or cuBLAS, while CMake's `FindCUDAToolkit` expects one root.
  Merge `cuda_nvcc`, `cuda_cudart`, `cuda_cccl` and `libcublas` into a temporary
  tree and pass `-DCUDAToolkit_ROOT=<tree>`, `CUDACXX=<nvcc>` and
  `-DCMAKE_CUDA_ARCHITECTURES=86-real`.
- **Measured, warm:** 143-token probe **~0.25 s**; 663-token capability-list
  route **~0.37–0.50 s**. The first request after a load takes **~4 s** while CUDA
  warms up, and the model loads in ~15 s versus 70–115 s on CPU.
- **Replayed the game's own calls:** the three CPU calls that took 40.9–41.8 s in
  `logs/<run-start>/system1/` answer in **~0.37 s** each on the GPU.

The RPATH of `libggml-cuda.so` points at the real store paths for
`libcudart`/`libcublas` rather than the temporary merged tree, so it survives a
reboot.

---

## 7. What this does not prove

Only the three fixed probe questions crossed the wire. Nothing has compared Kev's
**site-preference weight vectors** against Jev's, which is the question that
decides whether it can replace Jev for `[SITE ... WITH PREFERENCE]`. That needs the
site-selection call site to exist first.

The suspected limitations, to test rather than assume:

- **Input budget.** A decision state listing many candidate sites with descriptions
  may exceed what a 4B decision model handles well, even with `-c 2048` available.
  Kev's own limit is its trained input length, not the server's flag.
- **Subtlety.** "A quiet place without much foot traffic" is the kind of judgement
  where a 4B model is most likely to be worse than Jev. Clear-cut questions are
  where these models shine; the probe here was deliberately trivial.
- **Calibration.** A probability is only usable if 0.9 means "right nine times in
  ten". Check that against real outputs before thresholding on it.

The comparison that answers all three is the same prompt sent to both backends,
side by side. That is the next piece of work, not this document.

---

## 8. References

- `scripts/build_llama_server.sh` — clone + build, with the endpoint check
- `scripts/run_decision_server.sh` — the server, with the context-size lesson
- `scripts/test_decision_server.sh` — raw protocol check, no engine
- `src/main.cpp` — `RunSystemOneTest`, the `--systemone-test` mode
- `include/JevClassifier.h`, `src/JevClassifier.cpp` — the provider
- `include/SystemOneRecorder.h` — the interface and the record
- `SETTINGS.md` §`system1` — the config surface

---

## 9. The Kev instance in code, and swapping at runtime

`SystemOne::JevClassifier::Preset` is where the two backends this engine has been
run against are named, so "use Kev" is a preset rather than three magic strings in
a settings file:

```cpp
SystemOne::JevClassifier::Config config =
    SystemOne::JevClassifier::Preset::LocalKev();   // endpoint, model, placeholder key, 120s
// or Preset::HostedJev() for the default
SystemOne::JevClassifier classifier(config);
```

There is deliberately **one** provider class. Kev speaks the same `/v1/systemone`
protocol as Jev, so a `KevClassifier` subclass would differ only in two default
strings — duplicate code whose only visible effect would be that a parser fix has to
be made twice. What differs between backends is configuration, so configuration is
what is named. The other open decision models (Lev, Laya, Julia-1, OpenJev) take the
identical request shape and need no preset at all: point `endpoint` and `model` at a
`llama-server` running them.

**Local endpoints get a longer timeout automatically.** `system1.timeoutSeconds`
defaults to 30s for a hosted call, but a local endpoint that has *not* stated a
budget gets 120s (`Config::IsLocalEndpoint()`). This is not a nicety: the first run
of the sprite router against Kev failed with `transport error: Timeout was reached`
at exactly 30s, on a request that finished in 42s. An explicit
`system1.timeoutSeconds` always wins.

**Runtime swapping is supported, by design.** `Game::systemOneClassifier` is a
`shared_ptr` that callers re-read at every use rather than caching, so a
model-switch window can replace it with one assignment, and the recorder is owned by
`Game` rather than the provider so the overview window keeps its history across the
swap and each record still names the backend that answered.

The one thing that had to change for that was lifetime: `Ask` runs its request on a
**detached** worker that captures `this`, which was only sound while a provider lived
for the whole run. A switch window breaks that. `JevClassifier` now counts in-flight
requests, raises the count *before* the thread exists, decrements it inside the
worker on every exit path, and has the destructor ask workers to stop and wait for
them (bounded at 5s so a wedged socket cannot hang shutdown). Verified under
AddressSanitizer by destroying a provider mid-request: one callback delivered,
destruction returned in **6 ms**, no use-after-free.

---

## 10. Switching between backends without restarting

`system1.profiles` (see `SETTINGS.md`) lists the backends the game can switch
between. The flat `system1` fields are always available as a profile named
`default`, so nothing needs migrating.

```json
"system1": {
  "activeProfile": "kev-local",
  "profiles": [
    { "id": "kev-local", "preset": "local_kev", "minConfidence": 0.45 },
    { "id": "hosted",    "preset": "hosted_jev" }
  ]
}
```

Switching is the **backend picker** at the top of the System 1 Overview window:

1. it constructs the target and refuses if it has no key,
2. probes the endpoint at transport level (`CURLOPT_CONNECT_ONLY`, ~2s), so a server
   that is not running is refused while a `llama-server` still loading its model is
   accepted — it answers the connect and only then returns 503,
3. attaches the recorder and swaps `Game::systemOneClassifier`,
4. notes the switch in the System 1 log as an `EVENT` line.

A failed switch leaves the current backend untouched, so the game never ends up with
no classifier.

**In-flight requests are cancelled by the switch.** That is deliberate: waiting for
a Kev answer can mean ~40s, and an instant deterministic switch is worth one
cancelled request — which is visible as an error on that record, not silent. The
mechanism is the provider's in-flight guard (§9); swapping the `shared_ptr` drops the
last reference and the destructor cancels and waits.

Headless equivalent, for harnesses:

```bash
./build/AIRogue --settings /tmp/profiles.json --system1-profile kev-strict \
    --sprite-route "a prison's furniture and fittings"
```

That prints the profile and its threshold, and reports `RESULT: unsure (...)` when the
verdict falls below it — the same decision the window makes, verifiable without a
display.

### What is not verified

The picker itself is a GUI surface and has not been exercised: this machine has no
display. The parts it calls — profile resolution, selection precedence, the probe,
the swap, the threshold — are verified headlessly and by the harnesses above.
