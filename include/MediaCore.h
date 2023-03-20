#pragma once
#ifdef _WIN32
#if MEDIACORE_SHARED
#define MEDIACORE_API __declspec( dllexport )
#else
#define MEDIACORE_API
#endif
#else
#define MEDIACORE_API
#endif

#include <cstdint>
#include "immat.h"

namespace MediaCore
{
    struct CorrelativeFrame
    {
        enum Phase
        {
            PHASE_SOURCE_FRAME = 0,
            PHASE_AFTER_FILTER,
            PHASE_AFTER_TRANSFORM,
            PHASE_AFTER_AUDIOEFFECT,
            PHASE_AFTER_TRANSITION,
            PHASE_AFTER_MIXING,
        } phase;
        int64_t clipId;
        int64_t trackId;
        ImGui::ImMat frame;
    };
    MEDIACORE_API void GetVersion(int& major, int& minor, int& patch, int& build);
}
