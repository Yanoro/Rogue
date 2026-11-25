#include "UiStyle.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace UiStyle {

ImVec4 ParseColor(const std::string &text, const ImVec4 &fallback) {
  std::string hex = text;
  if (!hex.empty() && hex.front() == '#') hex.erase(hex.begin());
  if (hex.size() != 6 && hex.size() != 8) return fallback;
  for (const char c : hex) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return fallback;
  }
  const auto channel = [&hex](size_t i) {
    return static_cast<float>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)) /
           255.0f;
  };
  return ImVec4(channel(0), channel(2), channel(4),
                hex.size() == 8 ? channel(6) : 1.0f);
}

std::string FormatColor(const ImVec4 &color) {
  const auto byte = [](float value) {
    return static_cast<int>(
        std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
  };
  char buffer[10];
  std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", byte(color.x),
                byte(color.y), byte(color.z), byte(color.w));
  return buffer;
}

void Apply(const GameSettings &settings) {
  ImGuiStyle &style = ImGui::GetStyle();
  style.Colors[ImGuiCol_WindowBg] =
      ParseColor(settings.ui.windowColor, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  style.Colors[ImGuiCol_PopupBg] =
      ParseColor(settings.ui.popupColor, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
}

}  // namespace UiStyle
