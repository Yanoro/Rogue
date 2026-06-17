#include "AgentContextWindow.h"
#include "NPC.h"
#include "imgui.h"

NPCContextWindow::NPCContextWindow(std::weak_ptr<NPC> npc)
    : npc(npc) {
  if (auto locked = npc.lock()) {
    fallbackContext = locked->getContext();
  }
}

void NPCContextWindow::Draw() {
  char window_name[64];
  sprintf(window_name, "NPC Context###%p", (void*)this);
  ImGui::Begin(window_name, nullptr, ImGuiWindowFlags_None); 

  ImGui::Checkbox("Auto-scroll", &autoScroll);
  ImGui::Separator();

  std::string currentContext = fallbackContext;
  if (auto locked = npc.lock()) {
    currentContext = locked->getContext();
    fallbackContext = currentContext;
  }

  if (ImGui::BeginChild("ContextScroll", ImVec2(500, 300),
                        ImGuiChildFlags_Borders)) {
    ImGui::TextWrapped("%s", currentContext.c_str());

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
      ImGui::SetScrollHereY(1.0f);
    }
  }
  ImGui::EndChild();

  ImGui::End();
}
