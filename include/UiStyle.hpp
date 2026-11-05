#pragma once

#include "Settings.h"

#include "imgui.h"

#include <string>

// The ImGui window colours are settings (ui.windowColor / ui.popupColor). They are RGBA
// hex strings so settings.json stays hand-editable and the Settings window can put a
// colour picker over the same value. Apply() reads the running settings and writes the
// two style colours; the caller must have an ImGui context (after rlImGuiSetup).
namespace UiStyle {

ImVec4 ParseColor(const std::string &text, const ImVec4 &fallback);
std::string FormatColor(const ImVec4 &color);
void Apply(const GameSettings &settings);

}  // namespace UiStyle
