#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <memory>

#include "Window.h"
#include "AI.h"

class AIChatWindow : public Window {
public:
    AIChatWindow(AI *aiInstance, const std::string& startPrompt);
    AIChatWindow(AI *aiInstance);
    void Draw();

private:
    AI *ai;
    std::string contextId;
    bool couldConnect = true;
    std::string context;
    std::string inputBuffer;
    std::mutex promptMutex;

    void DrawSpinner();
    void generateResponse(std::string currentPrompt);
};
