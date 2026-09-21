#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <flecs.h>
#include "AgentBrain.h"

struct ImGuiInputTextCallbackData;

#include "Window.h"


class NPCContextWindow : public Window {
public:
    NPCContextWindow(flecs::entity entity);
    void Draw() override;

private:
    flecs::entity entity;
    std::string name;
    std::string fallbackContext;
    bool autoScroll = true;
    char inputBuf[256] = "";

    std::vector<std::string> history;
    int historyPos = -1;

    static int TextEditCallbackStub(ImGuiInputTextCallbackData* data);
    int TextEditCallback(ImGuiInputTextCallbackData* data);
};
