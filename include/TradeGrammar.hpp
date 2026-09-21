#pragma once

#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "ItemStack.hpp"
#include "StringUtils.hpp"

// Pure parsing for the trade command grammar. Deliberately has no ECS or raylib
// dependency: the grammar is the highest-risk part of the trade feature (a
// mis-parse means offers are rejected forever), so it stays testable on its own.
//
//   [OFFER <give> FOR <receive>]
//   [OFFER 3 Iron Ingot FOR 8 Flour, 5 Bronze Coins]
//
// Always from the speaker's perspective: the left side is what the speaker
// hands over. Counts are optional and default to 1, written "3" or "3x". A
// comma separates stacks and FOR is the only separator between the two sides.
namespace TradeGrammar {

enum class ParseResult {
  Ok,
  Empty,
  MissingSeparator,
  EmptySide,
  EmptyStack,
  TooManyStacks,
};

struct ParsedOffer {
  std::vector<ItemStack> give;
  std::vector<ItemStack> receive;
};

inline constexpr size_t kMaxStacksPerSide = 8;

inline std::string StripTrailingS(const std::string &text) {
  if (text.size() > 1 && (text.back() == 's' || text.back() == 'S')) {
    return text.substr(0, text.size() - 1);
  }
  return text;
}

// Case-insensitive, and tolerant of one trailing plural 's' on either side, so
// a model that writes "Iron Ingots" still matches the real "Iron Ingot".
inline bool ItemNameMatches(const std::string &a, const std::string &b) {
  if (StringUtils::EqualsIgnoreCase(a, b)) {
    return true;
  }
  return StringUtils::EqualsIgnoreCase(StripTrailingS(a), StripTrailingS(b));
}

// "3 Iron Ingot" -> {Iron Ingot, 3}; "3x Iron Ingot" -> the same;
// "Iron Ingot" -> {Iron Ingot, 1}.
inline bool ParseStack(const std::string &text, ItemStack &out) {
  std::string trimmed = StringUtils::Trim(text);
  if (trimmed.empty()) {
    return false;
  }

  // A bare count with no item name ("3", "3x") is meaningless, not an item
  // called "3".
  static const std::regex countOnly(R"(^\d+\s*[xX]?$)");
  if (std::regex_match(trimmed, countOnly)) {
    return false;
  }

  static const std::regex withCount(R"(^(\d+)\s*[xX]?\s+(.+)$)");
  std::smatch match;
  if (std::regex_match(trimmed, match, withCount)) {
    int count = 0;
    try {
      count = std::stoi(match[1].str());
    } catch (const std::exception &) {
      return false;
    }
    std::string name = StringUtils::Trim(match[2].str());
    if (count <= 0 || name.empty()) {
      return false;
    }
    out.item = name;
    out.count = count;
    return true;
  }

  // "Iron Ingot x2": the same thing with the count trailing, which models also
  // write. Checked after the prefix form so "3x Iron Ingot" still parses there.
  static const std::regex withSuffixCount(R"(^(.+?)\s*[xX]\s*(\d+)$)");
  if (std::regex_match(trimmed, match, withSuffixCount)) {
    int count = 0;
    try {
      count = std::stoi(match[2].str());
    } catch (const std::exception &) {
      return false;
    }
    std::string name = StringUtils::Trim(match[1].str());
    if (count <= 0 || name.empty()) {
      return false;
    }
    out.item = name;
    out.count = count;
    return true;
  }

  out.item = trimmed;
  out.count = 1;
  return true;
}

inline ParseResult ParseSide(const std::string &side,
                             std::vector<ItemStack> &into) {
  // Models join stacks with commas, but "+", "and", "&" and ";" all turned up in
  // practice. A rejected separator is indistinguishable from not owning the item,
  // which sent a model off debugging its inventory instead of its syntax, so
  // treat them all as separators. No item name contains any of them.
  static const std::regex wordAnd(R"(\band\b)", std::regex_constants::icase);
  std::string normalised = std::regex_replace(side, wordAnd, ",");

  std::vector<std::string> parts;
  std::string current;
  for (char c : normalised) {
    if (c == ',' || c == '+' || c == ';' || c == '&') {
      parts.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  parts.push_back(current);

  if (parts.size() > kMaxStacksPerSide) {
    return ParseResult::TooManyStacks;
  }

  for (const std::string &part : parts) {
    ItemStack stack;
    if (!ParseStack(part, stack)) {
      return ParseResult::EmptyStack;
    }
    // Merge repeated entries so "2 Flour, 3 Flour" is one requirement of five.
    bool merged = false;
    for (ItemStack &existing : into) {
      if (ItemNameMatches(existing.item, stack.item)) {
        existing.count += stack.count;
        merged = true;
        break;
      }
    }
    if (!merged) {
      into.push_back(stack);
    }
  }
  return ParseResult::Ok;
}

inline ParseResult Parse(const std::string &text, ParsedOffer &out) {
  out = ParsedOffer{};

  std::string trimmed = StringUtils::Trim(text);
  if (trimmed.empty()) {
    return ParseResult::Empty;
  }

  // Split on the first standalone FOR. No item name contains the word.
  static const std::regex separator(R"(\bFOR\b)", std::regex_constants::icase);
  std::smatch match;
  if (!std::regex_search(trimmed, match, separator)) {
    return ParseResult::MissingSeparator;
  }

  std::string left = StringUtils::Trim(trimmed.substr(0, match.position()));
  std::string right =
      StringUtils::Trim(trimmed.substr(match.position() + match.length()));
  if (left.empty() || right.empty()) {
    return ParseResult::EmptySide;
  }

  ParseResult result = ParseSide(left, out.give);
  if (result != ParseResult::Ok) {
    return result;
  }
  result = ParseSide(right, out.receive);
  if (result != ParseResult::Ok) {
    return result;
  }

  // A one-sided offer is a gift, which is a separate verb that does not exist
  // yet, so OFFER stays strictly bilateral.
  if (out.give.empty() || out.receive.empty()) {
    return ParseResult::EmptySide;
  }
  return ParseResult::Ok;
}

// "8 Flour, 5 Bronze Coins"
inline std::string Format(const std::vector<ItemStack> &stacks) {
  std::string text;
  for (size_t i = 0; i < stacks.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    text += std::to_string(stacks[i].count) + " " + stacks[i].item;
  }
  return text;
}

// Human-readable reason, appended to a system warning.
inline std::string Explain(ParseResult result) {
  switch (result) {
  case ParseResult::Ok:
    return "";
  case ParseResult::Empty:
    return "the offer was empty - write it as [OFFER <what you give> FOR "
           "<what you want>]";
  case ParseResult::MissingSeparator:
    return "an offer must separate the two sides with FOR, for example "
           "[OFFER 3 Iron Ingot FOR 8 Flour]";
  case ParseResult::EmptySide:
    return "an offer must exchange something for something: put at least one "
           "item on each side of FOR";
  case ParseResult::EmptyStack:
    return "each entry must be an item name with an optional count in front, "
           "for example 3 Iron Ingot";
  case ParseResult::TooManyStacks:
    return "an offer can list at most 8 different item stacks on each side";
  }
  return "the offer could not be read";
}

} // namespace TradeGrammar
