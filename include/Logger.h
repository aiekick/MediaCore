#pragma once
#include <string>
#include <ostream>
#include "MediaCore.h"

namespace Logger
{
    enum Level
    {
        VERBOSE = 0,
        DEBUG,
        INFO,
        WARN,
        Error,
    };

    struct ALogger
    {
        virtual void Log(Level l, const std::string fmt, ...) = 0;
        virtual std::ostream& Log(Level l) = 0;
        virtual ALogger* SetShowLoggerName(bool show) = 0;
        virtual ALogger* SetShowLevels(Level l , int n = 1) = 0;
        virtual ALogger* SetShowLevelName(bool show) = 0;
        virtual ALogger* SetShowTime(bool show) = 0;
    };

    MEDIACORE_API void SetSingleLogMaxSize(uint32_t size);

    MEDIACORE_API bool SetDefaultLoggerType(const std::string& loggerType);
    MEDIACORE_API ALogger* GetDefaultLogger();
    MEDIACORE_API void Log(Level l, const std::string fmt, ...);
    MEDIACORE_API std::ostream& Log(Level l);

    MEDIACORE_API ALogger* GetLogger(const std::string& name);
}