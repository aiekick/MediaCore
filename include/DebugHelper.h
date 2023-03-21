#pragma
#include <chrono>

namespace MediaCore
{
    using SysClock = std::chrono::system_clock;
    using TimePoint = std::chrono::time_point<SysClock>;

    inline TimePoint GetTimePoint()
    {
        return SysClock::now();
    }

    constexpr int64_t CountElapsedMillisec(const TimePoint& t0, const TimePoint& t1)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count();
    }

    int64_t GetMillisecFromTimePoint(const TimePoint& tp);
}