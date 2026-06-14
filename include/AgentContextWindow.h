#pragma once

#include <string>
#include <mutex>

#include "Window.h"
#include "AI.h"

class NPCContextWindow : public Window {
public:
    NPCContextWindow(std::string context);
    void Draw() override;
    
    // Returns a callback that can be passed to AI::generateStream
    AI::StreamCallback getStreamCallback();

private:
    std::string contextText;
    std::mutex textMutex;
    bool autoScroll = true;
};
