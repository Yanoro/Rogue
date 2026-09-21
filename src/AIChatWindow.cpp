#include "AIChatWindow.h"
#include "Components.h"
#include <cmath>
#include <thread>
#include <chrono>
#include <iostream>
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "rlImGui.h"
#include "raylib-cpp.hpp"

const size_t DEFAULT_DELAY_AI_TEXT_MS = 90;

AIChatWindow::AIChatWindow(AI* aiInstance, const std::string& startPrompt, flecs::entity owner) 
    : ai(aiInstance), owner(owner), contextId(std::to_string(reinterpret_cast<uintptr_t>(this))), context(startPrompt) {
    generateResponse(context);
}

AIChatWindow::AIChatWindow(AI *aiInstance, flecs::entity owner) : ai(aiInstance), owner(owner), contextId(std::to_string(reinterpret_cast<uintptr_t>(this))) {}

void AIChatWindow::Draw() {
    bool isOpen = true;
    // Resizable with a sane first-use size. The close button is enabled by
    // passing &isOpen instead of nullptr; closing removes the ActiveWindow.
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin("AI Chat", &isOpen, ImGuiWindowFlags_None);

    if (!isOpen) {
        if (owner.is_alive()) {
            owner.remove<ActiveWindow>();
        }
        ImGui::End();
        return;
    }

    // Reserve room at the bottom for the status line and the input row so the
    // log fills the window and grows with it. Mirrors NPCContextWindow.
    const float footerHeightToReserve =
        ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing() * 2.0f;

    if (ImGui::BeginChild("LogScroll", ImVec2(0, -footerHeightToReserve), ImGuiChildFlags_Borders)) {
        if (not couldConnect) {
          ImGui::TextWrapped("%s", "Could not connect to AI Backend!");
        }
        else {
          std::lock_guard<std::mutex> lock(promptMutex);
          ImGui::TextWrapped("%s", context.c_str());
        }
    }
    ImGui::EndChild();

    
    if (ai->isBusy(contextId) and couldConnect) {
        ImGui::Text("AI is thinking...");
        ImGui::SameLine();
        DrawSpinner();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Answer:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    
    if (ImGui::InputText("##Input", &inputBuffer, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (!ai->isBusy(contextId)) {
            std::string currentInput = inputBuffer;
            {
                std::lock_guard<std::mutex> lock(promptMutex);
                context += "\nUser: " + inputBuffer + "\nYou: ";
            }
            
            generateResponse(currentInput);
            
            inputBuffer.clear();
            ImGui::SetKeyboardFocusHere(-1);
        }
    }

    ImGui::End();  
}

void AIChatWindow::DrawSpinner() {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float radius = 8.0f;
    pos.x += radius;
    pos.y += radius;
    float time = (float)raylib::Window::GetTime();
    for (int i = 0; i < 8; i++) {
        float angle = time * 4.0f + i * (M_PI / 4.0f);
        float alpha = (float)i / 8.0f;
        drawList->AddCircleFilled(
            ImVec2(pos.x + cosf(angle) * radius, pos.y + sinf(angle) * radius),
            2.0f, ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)));
    }
    ImGui::Dummy(ImVec2(radius * 2 + 5, radius * 2));
}

void AIChatWindow::generateResponse(std::string currentPrompt) {
    std::thread([this, currentPrompt]() {
        std::vector<ChatMessage> history = { {"user", currentPrompt} };
        bool res = ai->generateStream(contextId, history, [this](const std::string &token) {
            for (const char c : token) {
                {
                    std::lock_guard<std::mutex> lock(promptMutex);
                    context += c;  
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(DEFAULT_DELAY_AI_TEXT_MS));
            }
        }); 
        if (not res) {
          couldConnect = false;
        }
        else {
          couldConnect = true;
        }
    }).detach();
}
