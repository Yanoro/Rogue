#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace StringUtils {

// Compares two strings for equality, ignoring case
inline bool EqualsIgnoreCase(const std::string &a, const std::string &b) {
  if (a.length() != b.length()) {
    return false;
  }

  return std::equal(a.begin(), a.end(), b.begin(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

// Checks if a string contains a substring, ignoring case
inline bool ContainsIgnoreCase(const std::string &str,
                               const std::string &substr) {
  auto it =
      std::search(str.begin(), str.end(), substr.begin(), substr.end(),
                  [](char ch1, char ch2) {
                    return std::tolower(static_cast<unsigned char>(ch1)) ==
                           std::tolower(static_cast<unsigned char>(ch2));
                  });
  return it != str.end();
}

inline std::string substringAfterLast(const std::string &text,
                                      const std::string &delimiter) {
  size_t pos = text.rfind(delimiter);

  if (pos == std::string::npos) {
    return "";
  }

  return text.substr(pos + delimiter.length());
}

inline std::string ToUpper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return text;
}

// Removes leading and trailing whitespace (spaces, tabs, newlines).
inline std::string Trim(const std::string &text) {
  size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// Splits a trailing item count off a command argument string, so item names that
// contain spaces still parse: "Wheat Seeds 3" -> {"Wheat Seeds", 3}. A missing or
// non-positive count means 1, so "Wheat" -> {"Wheat", 1}.
inline std::pair<std::string, int> SplitTrailingCount(const std::string &args) {
  size_t begin = args.find_first_not_of(" \t");
  if (begin == std::string::npos) {
    return {"", 1};
  }
  size_t end = args.find_last_not_of(" \t");
  std::string trimmed = args.substr(begin, end - begin + 1);

  size_t lastSpace = trimmed.find_last_of(" \t");
  if (lastSpace != std::string::npos) {
    std::string suffix = trimmed.substr(lastSpace + 1);
    bool isNumber =
        !suffix.empty() && suffix.size() <= 9 &&
        std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        });

    if (isNumber) {
      int count = std::stoi(suffix);
      std::string name = trimmed.substr(0, lastSpace);
      size_t nameEnd = name.find_last_not_of(" \t");
      name = nameEnd == std::string::npos ? "" : name.substr(0, nameEnd + 1);
      return {name, count > 0 ? count : 1};
    }
  }

  return {trimmed, 1};
}

// Collapses identical names into a single counted entry, preserving the order in
// which each name was first seen. Comparison ignores case, so "Wheat" and "wheat"
// stack together and the first-seen spelling is the one reported.
inline std::vector<std::pair<std::string, int>>
StackNames(const std::vector<std::string> &names) {
  std::vector<std::pair<std::string, int>> stacked;
  for (const std::string &name : names) {
    auto it = std::find_if(stacked.begin(), stacked.end(),
                           [&name](const std::pair<std::string, int> &entry) {
                             return EqualsIgnoreCase(entry.first, name);
                           });
    if (it != stacked.end()) {
      it->second++;
    } else {
      stacked.emplace_back(name, 1);
    }
  }
  return stacked;
}

// Renders stacked names as one comma-separated list: "Wheat (4), Apple".
// Single items carry no count suffix.
inline std::string
FormatStackedNames(const std::vector<std::pair<std::string, int>> &stacked) {
  std::string out;
  for (const auto &entry : stacked) {
    if (!out.empty()) {
      out += ", ";
    }
    out += entry.first;
    if (entry.second > 1) {
      out += " (" + std::to_string(entry.second) + ")";
    }
  }
  return out;
}

} // namespace StringUtils
