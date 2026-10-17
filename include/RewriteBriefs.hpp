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

// For the object designer. What an *object* can express, which is a different question
// from what a tile can: an object is a thing with capabilities -- usable, carried,
// holding, harvested, crafted at, grown -- drawn as ASCII art in a palette, and the thing
// it is not is ground.
//
// A separate brief rather than a paragraph added to Tile's, because the Improve button is
// supposed to keep a request inside the engine's vocabulary and the two vocabularies do
// not overlap: an "improved" tile request enriched with the word "stored" would push the
// designer to invent a tile that cannot store anything, and an improved object request
// that talked about surfaces would lose the capability that makes it an object at all.
PromptRewriter::Brief Object();

// For the blueprint designer. What a building design can express.
PromptRewriter::Brief Blueprint();

}  // namespace RewriteBriefs
