#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include <memory>

#include "Window.h"
#include "AI.h"

class AIChatWindow : public Window {
public:
    AIChatWindow(std::shared_ptr<AI> aiInstance, const std::string& startPrompt);
    AIChatWindow(std::shared_ptr<AI> aiInstance);
    void Draw();

private:
    std::shared_ptr<AI> ai;
    std::string contextId;
    bool couldConnect = true;
    std::string context;
    std::string inputBuffer;
    std::mutex promptMutex;

    void DrawSpinner();
    void generateResponse(std::string currentPrompt);
};
