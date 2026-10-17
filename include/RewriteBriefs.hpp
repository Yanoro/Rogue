#pragma once

#include "PromptRewriter.hpp"

// The per-domain briefs the Improve button is given.
//
// These live outside the windows that use them for one reason: they are the part of
// the feature that a headless mode has to be able to inspect and print. A brief that
// can only be reached through an ImGui window is a brief nothing can verify, and the
// text of a brief is exactly what determines whether a rewrite stays inside the
// engine's vocabulary -- so it is the part most worth reading.
//
// Each returns the prose that goes into `%BRIEF%`: what the rewriter may and may not
// promise in that domain. The shared rules about *rewriting* are in
// data/prompts/REWRITE.md and are the same for every domain.
namespace RewriteBriefs {

// For the tile designer. The limits of one 32x32 tile sprite, plus the current art
// pack's own description of its style when it has one.
PromptRewriter::Brief Tile();

// For the blueprint designer. What a building design can express.
PromptRewriter::Brief Blueprint();

}  // namespace RewriteBriefs
