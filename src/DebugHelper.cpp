#include <list>
#include "DebugHelper.h"

using namespace std;
using namespace Logger;

namespace MediaCore
{
static const TimePoint _FIRST_TP = SysClock::now();

int64_t GetMillisecFromTimePoint(const TimePoint& tp)
{
    return chrono::duration_cast<chrono::milliseconds>(tp-_FIRST_TP).count();
}

struct _CheckPoint
{
    string name;
    TimePoint tp;
};

static list<_CheckPoint> _DEFAULT_CHECK_POINT_LIST;

void AddCheckPoint(const string& name)
{
    _DEFAULT_CHECK_POINT_LIST.push_back({name, SysClock::now()});
}

void LogCheckPointsTimeInfo(ALogger* logger, Logger::Level loglvl)
{
    if (!logger)
        logger = GetDefaultLogger();
    logger->Log(loglvl) << "Check points: ";
    if (!_DEFAULT_CHECK_POINT_LIST.empty())
    {
        list<_CheckPoint>::iterator it1, it2;
        it1 = _DEFAULT_CHECK_POINT_LIST.end();
        it2 = _DEFAULT_CHECK_POINT_LIST.begin();
        do {
            if (it1 != _DEFAULT_CHECK_POINT_LIST.end())
                logger->Log(loglvl) << " -> " << it2->name << "(" << GetMillisecFromTimePoint(it2->tp)
                    << "), d=" << CountElapsedMillisec(it1->tp, it2->tp);
            else
                logger->Log(loglvl) << it2->name << "(" << GetMillisecFromTimePoint(it2->tp) << ")";
            it1 = it2++;
        } while (it2 != _DEFAULT_CHECK_POINT_LIST.end());
        _DEFAULT_CHECK_POINT_LIST.clear();
    }
    else
    {
        logger->Log(loglvl) << "(EMPTY)";
    }
    logger->Log(loglvl) << endl;
}
}