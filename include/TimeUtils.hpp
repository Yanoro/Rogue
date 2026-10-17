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

    // The time right now, in the same format GetStartTimeString uses.
    //
    // GetStartTimeString is deliberately cached so every NPC transcript of one run
    // shares a timestamp and sorts together. A log that is created mid-session --
    // the designer's, which starts when the first message is sent rather than at
    // launch -- needs the real moment instead, or a conversation held an hour in
    // would be filed under the launch time.
    static std::string GetCurrentTimeString() {
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
    }
    // The time of day, on its own. Used to stamp individual lines of a log that
    // spans a session -- a transcript of task transitions wants to say *when*
    // each change happened, not just which run it belonged to (see
    // docs/task-system-design.md section 11).
    static std::string GetClockTimeString() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#if defined(_WIN32)
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%H:%M:%S");
        return oss.str();
    }
};
