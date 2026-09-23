#pragma once

#include <cstdint>
#include <random>

// Deterministic pseudo-random source for all gameplay randomness.
//
// Why not rand(): every previous call site used rand() and nothing ever called
// srand(), so the process kept the default seed and produced the *same loot
// sequence on every playthrough*. Streams here are derived from an explicit
// world seed instead, which is logged at startup so a run can be replayed and a
// test can pin it, without touching any global state.
//
// Why not std::uniform_real_distribution: its output is not specified by the
// standard and may differ between standard library implementations, which would
// break cross-platform reproducibility. Unit() builds the float directly from
// the engine's output, and std::mt19937's own output *is* specified, so a
// derived stream is reproducible everywhere.
class Rng {
public:
  Rng() = default;

  explicit Rng(uint64_t seed)
      : engine(static_cast<std::mt19937::result_type>(seed)) {}

  // Uniform in [0, 1). 24 bits is exactly the mantissa width of a float, so the
  // conversion is lossless and the result can never be 1.0f.
  float Unit() {
    return static_cast<float>(engine() >> 8) * (1.0f / 16777216.0f);
  }

  // True with probability `chance`. 1.0f always succeeds and 0.0f never does.
  //
  // Always consumes exactly one value, including for the certain and impossible
  // cases. That is deliberate: it keeps the stream advancing one step per entry
  // regardless of the chances, so an effect that adjusts one entry's chance
  // cannot shift the rolls of the entries after it. It also matches what the
  // previous `rand()` call sites did, which drew unconditionally.
  //
  // `<` rather than `<=`: Unit() already excludes 1.0f, so only this form makes
  // chance 1.0f certain.
  bool Chance(float chance) { return Unit() < chance; }

  // An independent stream derived from a world seed and two identifying values
  // (typically an action or actor id and a source id).
  //
  // Per-source streams are what let two effects roll without their outcomes
  // depending on registration order: with one shared engine, whichever rolled
  // first would shift the other's result. Two streams derived from the same
  // seed but different ids are unrelated.
  static Rng Derived(uint64_t worldSeed, uint64_t a, uint64_t b) {
    return Rng(Mix(worldSeed ^ Mix(a ^ Mix(b))));
  }

private:
  // splitmix64 finalizer. Cheap, and mixes well enough that adjacent ids give
  // unrelated streams -- which matters because entity ids and counters are
  // themselves sequential.
  static uint64_t Mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
  }

  std::mt19937 engine{0};
};
