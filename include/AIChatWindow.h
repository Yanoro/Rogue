#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <memory>

#include <flecs.h>

#include "Window.h"
#include "AI.h"

class AIChatWindow : public Window {
public:
    // `owner` is the entity carrying the ActiveWindow component; it is needed so
    // the window's X button can remove itself the way the other windows do.
    AIChatWindow(AI *aiInstance, const std::string& startPrompt,
                 flecs::entity owner = flecs::entity::null());
    AIChatWindow(AI *aiInstance, flecs::entity owner = flecs::entity::null());
    void Draw();

private:
    AI *ai;
    flecs::entity owner;
    std::string contextId;
    bool couldConnect = true;
    std::string context;
    std::string inputBuffer;
    std::mutex promptMutex;

    void DrawSpinner();
    void generateResponse(std::string currentPrompt);
};
