#include "AgentContextWindow.h"

#include "Components.h"
#include "AgentBrain.h"
#include "imgui.h"
#include <sstream>

NPCContextWindow::NPCContextWindow(flecs::entity entity)
    : entity(entity) {
  if (entity.is_alive() && entity.has<AgentBrainWrapper>()) {
    auto wrapper = entity.get_ref<AgentBrainWrapper>();
    fallbackContext = wrapper->agBrain->getContext();
    name = entity.name();
  } else {
    fallbackContext = "";
    name = "Unknown";
  }
}

void NPCContextWindow::Draw() {
  char window_name[128];
  sprintf(window_name, "NPC Context: %s###%llu", name.c_str(), (unsigned long long)entity.id());
  ImGui::Begin(window_name, nullptr, ImGuiWindowFlags_None); 

  ImGui::Checkbox("Auto-scroll", &autoScroll);
  ImGui::SameLine();
  if (entity.is_alive() && entity.has<AgentBrainWrapper>()) {
    auto wrapper = entity.get_mut<AgentBrainWrapper>();
    if (wrapper->agBrain) {
      ImGui::Checkbox("Stop Brain", &wrapper->agBrain->isStopped);
    }
  }
  ImGui::Separator();

  std::string currentContext = fallbackContext;
  if (entity.is_alive() && entity.has<AgentBrainWrapper>()) {
    auto wrapper = entity.get_ref<AgentBrainWrapper>();
    if (wrapper->agBrain) {
      fallbackContext = wrapper->agBrain->getContext();
      currentContext = fallbackContext;
    }
  }

  if (ImGui::BeginChild("ContextScroll", ImVec2(500, 300),
                        ImGuiChildFlags_Borders)) {
    
    std::istringstream stream(currentContext);
    std::string line;
    bool inThought = false;
    bool inSystem = true;
    bool inYou = false;

    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.empty() || line.find_first_not_of(" \t") == std::string::npos) {
        continue;
      }

      if (line.find("System:") == 0) {
        inSystem = true;
        inYou = false;
      } else if (line.find("You:") == 0) {
        inYou = true;
        inSystem = false;
      }

      if (line.find("<think>") != std::string::npos) {
        inThought = true;
      }

      bool colorPushed = false;
      if (inThought) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        colorPushed = true;
      } else if (inSystem) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.6f, 1.0f, 1.0f)); // Light Blue
        colorPushed = true;
      } else if (inYou) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f)); // Light Green
        colorPushed = true;
      }

      ImGui::TextWrapped("%s", line.c_str());

      if (colorPushed) {
        ImGui::PopStyleColor();
      }

      if (line.find("</think>") != std::string::npos) {
        inThought = false;
      }
    }

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
      ImGui::SetScrollHereY(1.0f);
    }
  }
  ImGui::EndChild();

  ImGui::End();
}
