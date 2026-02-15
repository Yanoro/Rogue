#include "AIChatWindow.h"
#include <cmath>
#include <thread>
#include <chrono>
#include <iostream>
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "rlImGui.h"
#include "raylib-cpp.hpp"

const size_t DEFAULT_DELAY_AI_TEXT_MS = 90;

AIChatWindow::AIChatWindow(AI* aiInstance, const std::string& startPrompt) 
    : ai(aiInstance), prompt(startPrompt) {
    generateResponse(prompt);
}

void AIChatWindow::Draw() {
    ImGui::Begin("AI Chat", nullptr, ImGuiWindowFlags_AlwaysAutoResize); 

    if (ImGui::BeginChild("LogScroll", ImVec2(500, 300), ImGuiChildFlags_Borders)) {
        if (not couldConnect) {
          ImGui::TextWrapped("%s", "Could not connect to AI Backend!");
        }
        else {
          std::lock_guard<std::mutex> lock(promptMutex);
          ImGui::TextWrapped("%s", prompt.c_str());
        }
    }
    ImGui::EndChild();

    
    if (isGenerating and couldConnect) {
        ImGui::Text("AI is thinking...");
        ImGui::SameLine();
        DrawSpinner();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Answer:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    
    if (ImGui::InputText("##Input", &inputBuffer, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (!isGenerating) {
            {
                std::lock_guard<std::mutex> lock(promptMutex);
                prompt += "\nUser: " + inputBuffer + "\nYou: ";
            }
            
            generateResponse(prompt);
            
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
    isGenerating = true;
    std::thread([this, currentPrompt]() {
        bool res = ai->generateStream(currentPrompt, [this](const std::string &token) {
            for (const char c : token) {
                {
                    std::lock_guard<std::mutex> lock(promptMutex);
                    prompt += c;  
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(DEFAULT_DELAY_AI_TEXT_MS));
            }
        }); 
        if (not res) {
          couldConnect = false;
        }
        else {
          isGenerating = false;
          couldConnect = true;
        }
    }).detach();
}
