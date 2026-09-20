#include "AgentContextWindow.h"

#include "Components.h"
#include "AgentBrain.h"
#include "Defaults.h"
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
  bool isOpen = true;
  ImGui::Begin(window_name, &isOpen, ImGuiWindowFlags_None); 
  
  if (!isOpen) {
    if (entity.is_alive()) {
      entity.remove<ActiveWindow>();
    }
    ImGui::End();
    return;
  } 

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
    
    enum Speaker { SYSTEM, YOU, OTHER };
    Speaker currentSpeaker = SYSTEM;

    float window_visible_x2 = ImGui::GetWindowContentRegionMax().x;

    auto render_word = [&](const std::string& w, const ImVec4* overrideColor = nullptr) {
        if (w.empty()) return;
        if (overrideColor) ImGui::PushStyleColor(ImGuiCol_Text, *overrideColor);
        ImVec2 text_size = ImGui::CalcTextSize(w.c_str());
        if (ImGui::GetCursorPosX() + text_size.x >= window_visible_x2 - 5.0f) {
            ImGui::NewLine();
        }
        ImGui::TextUnformatted(w.c_str());
        ImGui::SameLine(0, 0);
        if (overrideColor) ImGui::PopStyleColor();
    };

    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.empty() || line.find_first_not_of(" \t") == std::string::npos) {
        continue;
      }

      if (line.find("<think>") != std::string::npos) {
        inThought = true;
      }

      size_t start_idx = 0;
      ImVec4 prefixColor;
      bool hasPrefix = false;
      std::string prefixStr;

      if (line.find("System: ") == 0) {
          currentSpeaker = SYSTEM;
          prefixStr = "System: ";
          prefixColor = DEFAULT_COLOR_SYSTEM_PREFIX;
          hasPrefix = true;
          start_idx = 8;
      } else if (line.find("System:") == 0) {
          currentSpeaker = SYSTEM;
          prefixStr = "System:";
          prefixColor = DEFAULT_COLOR_SYSTEM_PREFIX;
          hasPrefix = true;
          start_idx = 7;
      } else if (line.find("You: ") == 0) {
          currentSpeaker = YOU;
          prefixStr = "You: ";
          prefixColor = DEFAULT_COLOR_YOU_PREFIX;
          hasPrefix = true;
          start_idx = 5;
      } else if (line.find("You:") == 0) {
          currentSpeaker = YOU;
          prefixStr = "You:";
          prefixColor = DEFAULT_COLOR_YOU_PREFIX;
          hasPrefix = true;
          start_idx = 4;
      } else {
          size_t says_pos = line.find(" says: ");
          if (says_pos != std::string::npos && says_pos < 30) {
              currentSpeaker = OTHER;
              prefixStr = line.substr(0, says_pos + 7);
              prefixColor = DEFAULT_COLOR_OTHER_PREFIX;
              hasPrefix = true;
              start_idx = says_pos + 7;
          }
      }

      ImVec4 baseColor;
      if (inThought) {
          baseColor = DEFAULT_COLOR_THOUGHT;
      } else if (currentSpeaker == SYSTEM) {
          baseColor = DEFAULT_COLOR_SYSTEM_TEXT;
      } else if (currentSpeaker == YOU) {
          baseColor = DEFAULT_COLOR_YOU_TEXT;
      } else {
          baseColor = DEFAULT_COLOR_OTHER_TEXT;
      }

      ImGui::PushStyleColor(ImGuiCol_Text, baseColor);

      if (hasPrefix) {
          render_word(prefixStr, &prefixColor);
      }

      std::string current_word;
      bool in_cmd = false;
      ImVec4 cmdColor = DEFAULT_COLOR_COMMAND;

      for (size_t i = start_idx; i <= line.length(); ++i) {
          char c = (i < line.length()) ? line[i] : '\0';
          if (c == '\0') {
              render_word(current_word, in_cmd ? &cmdColor : nullptr);
              break;
          }
          
          if (c == '[') {
              render_word(current_word, nullptr);
              current_word.clear();
              in_cmd = true;
              current_word += c;
          } else if (c == ']' && in_cmd) {
              current_word += c;
              render_word(current_word, &cmdColor);
              current_word.clear();
              in_cmd = false;
          } else if ((c == ' ' || c == '\t') && !in_cmd) {
              current_word += c;
              render_word(current_word, nullptr);
              current_word.clear();
          } else {
              current_word += c;
          }
      }
      ImGui::NewLine();

      ImGui::PopStyleColor(); // Pop baseColor

      if (line.find("</think>") != std::string::npos) {
        inThought = false;
      }
    }

    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
      ImGui::SetScrollHereY(1.0f);
    }
  }
  ImGui::EndChild();

  if (entity.is_alive() && entity.has<AgentBrainWrapper>()) {
    auto wrapper = entity.get_mut<AgentBrainWrapper>();
    if (wrapper->agBrain) {
      if (wrapper->agBrain->isStopped) {
        ImGui::Separator();
        ImGui::PushItemWidth(-100);
        bool enterPressed = ImGui::InputText("##AI_Input", inputBuf, sizeof(inputBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        bool sendPressed = ImGui::Button("Send as AI");
        if (enterPressed || sendPressed) {
          std::string inputStr(inputBuf);
          if (!inputStr.empty()) {
            wrapper->agBrain->appendContext("assistant", "You: " + inputStr + "\n");
            auto msgCmd = wrapper->agBrain->ParseMessageCommand(inputStr);
            wrapper->agBrain->addCmdToQueue(msgCmd);
            memset(inputBuf, 0, sizeof(inputBuf));
          }
        }
      } else {
        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::PushItemWidth(-100);
        ImGui::InputText("##AI_Input", inputBuf, sizeof(inputBuf));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Button("Send as AI");
        ImGui::EndDisabled();
      }
    }
  }

  ImGui::End();
}
