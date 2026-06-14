#include "Character.h"
#include <iostream>

Character::Character(const std::string charName, std::string initPrompt, AI *AI)
: name(std::move(charName)), AIBackend(AI), currPrompt(std::move(initPrompt)) {}

void Character::addToPrompt(std::string prompt) {
  currPrompt += prompt;
}

std::string Character::sendPrompt() {
  std::string res;
  std::string contextId = std::to_string(reinterpret_cast<uintptr_t>(this));
  AIBackend->generateStream(contextId, currPrompt, [&res](const std::string& token) {
    std::cout << token << std::flush;
    res += token;
  });
  currPrompt = "";
  return res;
}
