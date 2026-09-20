#pragma once

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

class TimeUtils {
public:
    static std::string GetStartTimeString() {
        static std::string startTimeStr = []() {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf;
#if defined(_WIN32)
            localtime_s(&tm_buf, &t);
#else
            localtime_r(&t, &tm_buf);
#endif
            std::ostringstream oss;
            oss << std::put_time(&tm_buf, "%Y-%m-%d_%H-%M-%S");
            return oss.str();
        }();
        return startTimeStr;
    }
};
