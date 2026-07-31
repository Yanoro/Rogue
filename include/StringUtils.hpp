#pragma once

#include <algorithm>
#include <cctype>
#include <string>

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

} // namespace StringUtils
