#pragma once

#include <string>
#include <flecs.h>

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
};
