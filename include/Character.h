#pragma once 
#include <string>
#include "AI.h"

class Character {
private:
  std::string name;
  std::string currPrompt;
  AI *AIBackend;

public:
  Character(const std::string charName, std::string initPrompt, AI *AI);

  void addToPrompt(std::string prompt);
  
  // Send prompt to AI and consume it in the process, context gets saved in AI Class
  std::string sendPrompt();
};
