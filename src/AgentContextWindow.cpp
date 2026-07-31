#include "AgentContextWindow.h"

#include "Components.h"
#include "imgui.h"

NPCContextWindow::NPCContextWindow(flecs::entity entity)
    : entity(entity) {
  if (entity.is_alive() && entity.has<NPCContext>()) {
    auto ctx = entity.get_ref<NPCContext>();
    fallbackContext = ctx->context;
    name = entity.name();
  } else {
    fallbackContext = "";
    name = "Unknown";
  }
}

void NPCContextWindow::Draw() {
  char window_name[64];
  sprintf(window_name, "NPC Context: %s###%p", name.c_str(), (void*)this);
  ImGui::Begin(window_name, nullptr, ImGuiWindowFlags_None); 

  ImGui::Checkbox("Auto-scroll", &autoScroll);
  ImGui::Separator();

  std::string currentContext = fallbackContext;
  if (entity.is_alive() && entity.has<NPCContext>()) {
    auto ctx = entity.get_ref<NPCContext>();
    fallbackContext = ctx->context; 
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
