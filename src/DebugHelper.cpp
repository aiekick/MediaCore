#include <list>
#include <unordered_map>
#include <iomanip>
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

ostream& operator<<(ostream& os, const TimeSpan& ts)
{
    auto t1 = SysClock::to_time_t(ts.first);
    int32_t ms1 = chrono::duration_cast<chrono::milliseconds>(ts.first.time_since_epoch()).count()%1000;
    auto t2 = SysClock::to_time_t(ts.second);
    int32_t ms2 = chrono::duration_cast<chrono::milliseconds>(ts.second.time_since_epoch()).count()%1000;
    os << put_time(localtime(&t1), "%H:%M:%S") << "." << setfill('0') << setw(3) << ms1 << "~" << put_time(localtime(&t2), "%H:%M:%S") << "." << ms2;
    return os;
}

class PerformanceAnalyzer_Impl : public PerformanceAnalyzer
{
public:
    PerformanceAnalyzer_Impl(const string& name) : m_name(name)
    {}

    void SetLogInterval(uint32_t millisec) override
    {
        m_logInterval = millisec;
    }

    void Start() override
    {
        if (m_currTimeSpan.second.first.time_since_epoch().count() == 0)
            m_currTimeSpan.second.first = SysClock::now();
    }

    void End() override
    {
        EnrollCurrentTimeSpan();
    }

    void SectionStart(const std::string& name) override
    {
        EnrollCurrentTimeSpan(name);
    }

    void SectionEnd() override
    {
        EnrollCurrentTimeSpan();
    }

    void EnterSleep() override
    {
        if (!m_isInSleep)
        {
            EnrollCurrentTimeSpan(m_currTimeSpan.first);
            m_isInSleep = true;
        }
    }

    void QuitSleep() override
    {
        if (m_isInSleep)
        {
            EnrollCurrentTimeSpan(m_currTimeSpan.first);
        }
    }

    TimeSpan LogOnInterval(Level l, ALogger* logger) override
    {
        auto nowTp = SysClock::now();
        auto beginTp = nowTp-chrono::duration_cast<SysClock::duration>(chrono::milliseconds(m_logInterval));
        if (beginTp < m_prevLogTp)
            return TimeSpan();

        unordered_map<string, SysClock::duration> timeCostTable;
        for (auto& elem : m_timeSpanTable)
        {
            SysClock::duration t(0);
            auto& spanList = elem.second;
            auto spanIt = spanList.begin();
            while (spanIt != spanList.end())
            {
                if (spanIt->second <= beginTp)
                {
                    spanIt = spanList.erase(spanIt);
                }
                else
                {
                    if (spanIt->first < beginTp)
                        t += spanIt->second-beginTp;
                    else
                        t += spanIt->second-spanIt->first;
                    spanIt++;
                }
            }
            auto iter = timeCostTable.find(elem.first);
            if (iter != timeCostTable.end())
                iter->second += t;
            else
                timeCostTable[elem.first] = t;
        }
        if (!m_sleepTimeSpans.empty())
        {
            SysClock::duration t(0);
            auto spanIt = m_sleepTimeSpans.begin();
            while (spanIt != m_sleepTimeSpans.end())
            {
                if (spanIt->second <= beginTp)
                {
                    spanIt = m_sleepTimeSpans.erase(spanIt);
                }
                else
                {
                    if (spanIt->first < beginTp)
                        t += spanIt->second-beginTp;
                    else
                        t += spanIt->second-spanIt->first;
                    spanIt++;
                }
            }
            timeCostTable[SLEEP_TIMESPAN_TAG] = t;
        }

        if (!logger)
            logger = GetDefaultLogger();

        TimeSpan logTs = {beginTp, nowTp};
        logger->Log(l) << "PerformanceAnalyzer['" << m_name << "' " << logTs << "] : ";
        if (timeCostTable.empty())
            logger->Log(l) << "<EMPTY>";
        else
        {
            auto iter = timeCostTable.begin();
            auto otherIter = timeCostTable.end();
            auto sleepIter = timeCostTable.end();
            int i = 0;
            while (iter != timeCostTable.end())
            {
                if (iter->first.empty())
                {
                    otherIter = iter++;
                    continue;
                }
                else if (iter->first == SLEEP_TIMESPAN_TAG)
                {
                    sleepIter = iter++;
                    continue;
                }

                if (i++ > 0)
                    logger->Log(l) << ", ";
                logger->Log(l) << "'" << iter->first << "'=>" << chrono::duration_cast<chrono::duration<double>>(iter->second).count();
                iter++;
            }
            if (otherIter != timeCostTable.end())
            {
                if (i > 0)
                    logger->Log(l) << ", ";
                logger->Log(l) << "'" << OTHER_TIMESPAN_TAG << "'=>" << chrono::duration_cast<chrono::duration<double>>(otherIter->second).count();
            }
            if (sleepIter != timeCostTable.end())
            {
                if (i > 0)
                    logger->Log(l) << ", ";
                logger->Log(l) << "'" << SLEEP_TIMESPAN_TAG << "'=>" << chrono::duration_cast<chrono::duration<double>>(sleepIter->second).count();
            }
        }
        logger->Log(l) << endl;

        m_prevLogTp = nowTp;
        return logTs;
    }

private:
    void EnrollCurrentTimeSpan(const string& name = "")
    {
        m_currTimeSpan.second.second = SysClock::now();
        if (m_isInSleep)
        {
            m_sleepTimeSpans.push_back(m_currTimeSpan.second);
            m_isInSleep = false;
        }
        else
        {
            auto iter = m_timeSpanTable.find(m_currTimeSpan.first);
            if (iter != m_timeSpanTable.end())
                iter->second.push_back(m_currTimeSpan.second);
            else
                m_timeSpanTable.insert({m_currTimeSpan.first, {m_currTimeSpan.second}});
        }
        m_currTimeSpan.first = name;
        m_currTimeSpan.second.first = m_currTimeSpan.second.second;
    }

    static const string OTHER_TIMESPAN_TAG;
    static const string SLEEP_TIMESPAN_TAG;

private:
    string m_name;
    unordered_map<string, list<TimeSpan>> m_timeSpanTable;
    list<TimeSpan> m_sleepTimeSpans;
    bool m_isInSleep{false};
    pair<string, TimeSpan> m_currTimeSpan;
    uint32_t m_logInterval{1000};
    TimePoint m_prevLogTp;
};

const string PerformanceAnalyzer_Impl::OTHER_TIMESPAN_TAG = "<other>";
const string PerformanceAnalyzer_Impl::SLEEP_TIMESPAN_TAG = "<sleep>";

PerformanceAnalyzer::Holder PerformanceAnalyzer::CreateInstance(const string& name)
{
    return PerformanceAnalyzer::Holder(new PerformanceAnalyzer_Impl(name), [] (PerformanceAnalyzer* p) {
        PerformanceAnalyzer_Impl* ptr = dynamic_cast<PerformanceAnalyzer_Impl*>(p);
        delete ptr;
    });
}
}