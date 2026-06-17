#pragma once

#include <string>
#include <memory>

#include "Window.h"

class NPC;

class NPCContextWindow : public Window {
public:
    NPCContextWindow(std::weak_ptr<NPC> npc);
    void Draw() override;

private:
    std::weak_ptr<NPC> npc;
    std::string fallbackContext;
    bool autoScroll = true;
};
