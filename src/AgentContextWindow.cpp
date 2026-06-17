#include "AgentContextWindow.h"
#include "imgui.h"

NPCContextWindow::NPCContextWindow(std::string context)
    : contextText(context) {}

void NPCContextWindow::Draw() {
  char window_name[64];
  sprintf(window_name, "NPC Context###%p", (void*)this);
  ImGui::Begin(window_name, nullptr, ImGuiWindowFlags_None); 

  ImGui::Checkbox("Auto-scroll", &autoScroll);
  ImGui::Separator();

  if (ImGui::BeginChild("ContextScroll", ImVec2(500, 300),
                        ImGuiChildFlags_Borders)) {
    std::lock_guard<std::mutex> lock(textMutex);
    ImGui::TextWrapped("%s", contextText.c_str());

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
      ImGui::SetScrollHereY(1.0f);
    }
  }
  ImGui::EndChild();

  ImGui::End();
}

AI::StreamCallback NPCContextWindow::getStreamCallback() {
  return [this](const std::string &token) {
    std::lock_guard<std::mutex> lock(textMutex);
    contextText += token;
  };
}
