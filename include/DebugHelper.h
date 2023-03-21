/*
    Copyright (c) 2023 CodeWin

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once
#include <chrono>
#include <string>
#include "MediaCore.h"
#include "Logger.h"

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

    MEDIACORE_API int64_t GetMillisecFromTimePoint(const TimePoint& tp);

    MEDIACORE_API void AddCheckPoint(const std::string& name);
    MEDIACORE_API void LogCheckPointsTimeInfo(Logger::ALogger* logger = nullptr, Logger::Level loglvl = Logger::DEBUG);
}