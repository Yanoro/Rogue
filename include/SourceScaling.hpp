#pragma once

// The parts of a source's definition that turn a magnitude into a contribution.
//
// Both StatDef and SkillDef provide one, which is what lets a single contribution
// builder serve both: to the effect system they are the same thing -- an id with
// a neutral point, a scale, and some declared effects. Nothing downstream needs
// to know which registry a source came from.
struct SourceScaling {
  // The value that produces zero effect. 10 for an average-human stat, 0 for an
  // untrained skill.
  float baseline = 0.0f;
  // Effective units per point, so one coefficient stays readable across sources
  // with very different ranges (section 3.2).
  float scale = 1.0f;
  // Only used by BaselineMode::Min.
  float min = 0.0f;
  float max = 0.0f;
};
