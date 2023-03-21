#include "DebugHelper.h"

using namespace std;

namespace MediaCore
{
static const TimePoint _FIRST_TP = SysClock::now();

int64_t GetMillisecFromTimePoint(const TimePoint& tp)
{
    return chrono::duration_cast<chrono::milliseconds>(tp-_FIRST_TP).count();
}
}