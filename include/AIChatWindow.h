#pragma once

#include <string>
#include <mutex>
#include <atomic>
#include "AI.h"

//TODO: Make trying to connect to AI Backend if it fails
class AIChatWindow {
public:
    AIChatWindow(AI* aiInstance, const std::string& startPrompt);
    void Draw();

private:
    AI* ai;
    bool couldConnect = true;
    std::string prompt;
    std::string inputBuffer;
    std::mutex promptMutex;
    std::atomic<bool> isGenerating{false};

    void DrawSpinner();
    void generateResponse(std::string currentPrompt);
};
