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

#include <thread>
#include <algorithm>
#include <atomic>
#include <sstream>
#include "MultiTrackVideoReader.h"
#include "VideoBlender.h"
#include "FFUtils.h"
#include "SysUtils.h"

using namespace std;
using namespace Logger;

namespace MediaCore
{
class MultiTrackVideoReader_Impl : public MultiTrackVideoReader
{
public:
    static ALogger* s_logger;

    MultiTrackVideoReader_Impl()
    {
        m_logger = MultiTrackVideoReader::GetLogger();
    }

    MultiTrackVideoReader_Impl(const MultiTrackVideoReader_Impl&) = delete;
    MultiTrackVideoReader_Impl(MultiTrackVideoReader_Impl&&) = delete;
    MultiTrackVideoReader_Impl& operator=(const MultiTrackVideoReader_Impl&) = delete;

    bool Configure(uint32_t outWidth, uint32_t outHeight, const Ratio& frameRate) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is already started!";
            return false;
        }

        Close();

        m_outWidth = outWidth;
        m_outHeight = outHeight;
        m_frameRate = frameRate;
        m_readFrameIdx = 0;
        m_frameInterval = (double)m_frameRate.den/m_frameRate.num;

        m_hMixBlender = VideoBlender::CreateInstance();
        if (!m_hMixBlender)
        {
            m_errMsg = "CANNOT create new 'VideoBlender' instance for mixing!";
            return false;
        }
        if (!m_hMixBlender->Init("rgba", outWidth, outHeight, outWidth, outHeight, 0, 0))
        {
            ostringstream oss;
            oss << "Mixer blender initialization FAILED! Error message: '" << m_hMixBlender->GetError() << "'.";
            m_errMsg = oss.str();
            return false;
        }
        m_hSubBlender = VideoBlender::CreateInstance();
        if (!m_hSubBlender)
        {
            m_errMsg = "CANNOT create new 'VideoBlender' instance for subtitle!";
            return false;
        }
        if (!m_hSubBlender->Init())
        {
            ostringstream oss;
            oss << "Subtitle blender initialization FAILED! Error message: '" << m_hSubBlender->GetError() << "'.";
            m_errMsg = oss.str();
            return false;
        }

        m_configured = true;
        return true;
    }

    Holder CloneAndConfigure(uint32_t outWidth, uint32_t outHeight, const Ratio& frameRate) override;

    bool Start() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (m_started)
        {
            return true;
        }
        if (!m_configured)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT configured yet!";
            return false;
        }

        StartMixingThread();

        m_started = true;
        return true;
    }

    void Close() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        TerminateMixingThread();

        m_tracks.clear();
        m_outputCache.clear();
        m_configured = false;
        m_started = false;
        m_outWidth = 0;
        m_outHeight = 0;
        m_frameRate = { 0, 0 };
        m_frameInterval = 0;
    }

    VideoTrack::Holder AddTrack(int64_t trackId, int64_t insertAfterId) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return nullptr;
        }

        TerminateMixingThread();

        VideoTrack::Holder hNewTrack = VideoTrack::CreateInstance(trackId, m_outWidth, m_outHeight, m_frameRate);
        hNewTrack->SetDirection(m_readForward);
        {
            lock_guard<recursive_mutex> lk2(m_trackLock);
            if (insertAfterId == -1)
            {
                m_tracks.push_back(hNewTrack);
            }
            else
            {
                auto insertBeforeIter = m_tracks.begin();
                if (insertAfterId != -2)
                {
                    insertBeforeIter = find_if(m_tracks.begin(), m_tracks.end(), [insertAfterId] (auto trk) {
                        return trk->Id() == insertAfterId;
                    });
                    if (insertBeforeIter == m_tracks.end())
                    {
                        ostringstream oss;
                        oss << "CANNOT find the video track specified by argument 'insertAfterId' " << insertAfterId << "!";
                        m_errMsg = oss.str();
                        return nullptr;
                    }
                    insertBeforeIter++;
                }
                m_tracks.insert(insertBeforeIter, hNewTrack);
            }
            UpdateDuration();
            for (auto track : m_tracks)
                track->SeekTo(ReadPos());
            m_outputCache.clear();
        }

        StartMixingThread();
        return hNewTrack;
    }

    VideoTrack::Holder RemoveTrackByIndex(uint32_t index) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return nullptr;
        }
        if (index >= m_tracks.size())
        {
            m_errMsg = "Invalid value for argument 'index'!";
            return nullptr;
        }

        TerminateMixingThread();

        VideoTrack::Holder delTrack;
        {
            lock_guard<recursive_mutex> lk2(m_trackLock);
            auto iter = m_tracks.begin();
            while (index > 0 && iter != m_tracks.end())
            {
                iter++;
                index--;
            }
            if (iter != m_tracks.end())
            {
                delTrack = *iter;
                m_tracks.erase(iter);
                UpdateDuration();
                for (auto track : m_tracks)
                    track->SeekTo(ReadPos());
                m_outputCache.clear();
            }
        }

        StartMixingThread();
        return delTrack;
    }

    VideoTrack::Holder RemoveTrackById(int64_t trackId) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return nullptr;
        }

        TerminateMixingThread();

        VideoTrack::Holder delTrack;
        {
            lock_guard<recursive_mutex> lk2(m_trackLock);
            auto iter = find_if(m_tracks.begin(), m_tracks.end(), [trackId] (const VideoTrack::Holder& track) {
                return track->Id() == trackId;
            });
            if (iter != m_tracks.end())
            {
                delTrack = *iter;
                m_tracks.erase(iter);
                UpdateDuration();
                for (auto track : m_tracks)
                    track->SeekTo(ReadPos());
                m_outputCache.clear();
            }
        }

        StartMixingThread();
        return delTrack;
    }

    bool ChangeTrackViewOrder(int64_t targetId, int64_t insertAfterId) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (targetId == insertAfterId)
        {
            m_errMsg = "INVALID arguments! 'targetId' must NOT be the SAME as 'insertAfterId'!";
            return false;
        }

        lock_guard<recursive_mutex> lk2(m_trackLock);
        auto targetTrackIter = find_if(m_tracks.begin(), m_tracks.end(), [targetId] (auto trk) {
            return trk->Id() == targetId;
        });
        if (targetTrackIter == m_tracks.end())
        {
            ostringstream oss;
            oss << "CANNOT find the video track specified by argument 'targetId' " << targetId << "!";
            m_errMsg = oss.str();
            return false;
        }
        if (insertAfterId == -1)
        {
            auto moveTrack = *targetTrackIter;
            m_tracks.erase(targetTrackIter);
            m_tracks.push_back(moveTrack);
        }
        else
        {
            auto insertBeforeIter = m_tracks.begin();
            if (insertAfterId != -2)
            {
                insertBeforeIter = find_if(m_tracks.begin(), m_tracks.end(), [insertAfterId] (auto trk) {
                    return trk->Id() == insertAfterId;
                });
                if (insertBeforeIter == m_tracks.end())
                {
                    ostringstream oss;
                    oss << "CANNOT find the video track specified by argument 'insertAfterId' " << insertAfterId << "!";
                    m_errMsg = oss.str();
                    return false;
                }
                insertBeforeIter++;
            }
            auto moveTrack = *targetTrackIter;
            m_tracks.erase(targetTrackIter);
            m_tracks.insert(insertBeforeIter, moveTrack);
        }
        return true;
    }

    bool SetDirection(bool forward) override
    {
        if (m_readForward == forward)
            return true;

        TerminateMixingThread();

        m_readForward = forward;
        for (auto& track : m_tracks)
            track->SetDirection(forward);

        for (auto track : m_tracks)
            track->SeekTo(ReadPos());
        m_outputCache.clear();

        StartMixingThread();
        return true;
    }

    bool SeekTo(int64_t pos, bool async) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return false;
        }

        m_nextReadPos = INT64_MIN;
        m_seekPos = pos;
        m_inSeekingState = true;
        m_seeking = true;
        m_logger->Log(DEBUG) << "------> SeekTo seekPos=" << pos << endl;

        if (!async)
        {
            while (m_inSeekingState && !m_quit)
                this_thread::sleep_for(chrono::milliseconds(5));
            if (m_quit)
                return false;
        }
        else
        {
            lock_guard<mutex> lk(m_outputCacheLock);
            m_outputCache.clear();
        }
        return true;
    }

    bool SetTrackVisible(int64_t id, bool visible) override
    {
        auto track = GetTrackById(id, false);
        if (track)
        {
            track->SetVisible(visible);
            return true;
        }
        ostringstream oss;
        oss << "Track with id=" << id << " does NOT EXIST!";
        m_errMsg = oss.str();
        return false;
    }

    bool IsTrackVisible(int64_t id) override
    {
        auto track = GetTrackById(id, false);
        if (track)
            return track->IsVisible();
        return false;
    }

    bool ReadVideoFrameEx(int64_t pos, std::vector<CorrelativeFrame>& frames, bool nonblocking, bool precise) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return false;
        }
        if (pos < 0)
        {
            m_errMsg = "Invalid argument value for 'pos'! Can NOT be NEGATIVE.";
            return false;
        }

        int64_t targetFrmidx = (int64_t)(floor((double)pos*m_frameRate.num/(m_frameRate.den*1000)));
        if (nonblocking)
        {
            if (!m_inSeekingState && pos != m_prevReadPos)
            {
                m_logger->Log(DEBUG) << ">> Read video frame at pos=" << pos << ", targetFrmidx=" << targetFrmidx
                        << ", m_readFrameIdx=" << m_readFrameIdx << endl;
                m_prevReadPos = pos;
                m_nextReadPos = pos+33;
            }

            {
                lock_guard<mutex> lk2(m_outputCacheLock);
                if (((m_readForward && targetFrmidx > m_readFrameIdx) || (!m_readForward && m_readFrameIdx > targetFrmidx))
                    && !m_inSeekingState)
                {
                    auto popCnt = m_readForward ? targetFrmidx-m_readFrameIdx : m_readFrameIdx-targetFrmidx;
                    if (popCnt >= m_outputCache.size())
                    {
                        popCnt = m_outputCache.size();
                        m_outputCache.clear();
                    }
                    else
                    {
                        int i = popCnt;
                        while (i-- > 0)
                            m_outputCache.pop_front();
                    }
                    if (m_readForward)
                        m_readFrameIdx += popCnt;
                    else
                        m_readFrameIdx -= popCnt;
                }
                if (precise)
                {
                    if (targetFrmidx != m_readFrameIdx || m_outputCache.empty())
                    {
                        m_logger->Log(DEBUG) << "---> NO AVAILABLE frame" << endl;
                        return false;
                    }
                    frames = m_outputCache.front();
                }
                else if (!m_seekingFlash.empty())
                {
                    m_logger->Log(DEBUG) << "---> USE m_seekingFlash." << endl;
                    frames = m_seekingFlash;
                }
                else if (!m_outputCache.empty())
                {
                    m_logger->Log(DEBUG) << "---> USE m_outputCache.front()" << endl;
                    frames = m_outputCache.front();
                }
                else
                {
                    m_logger->Log(WARN) << "No AVAILABLE frame to read!" << endl;
                    return false;
                }
            }

            auto& vmat = frames[0].frame;
            const double timestamp = (double)pos/1000;
            // m_logger->Log(DEBUG) << "--> ReadVideoFrame lagging is " << timestamp-vmat.time_stamp << " second(s)." << endl;

            if (!m_subtrks.empty() && !frames.empty())
                frames[0].frame = BlendSubtitle(frames[0].frame);
        }
        else
        {
            while (!m_quit && m_inSeekingState)
                this_thread::sleep_for(chrono::milliseconds(5));

            if ((m_readForward && (targetFrmidx < m_readFrameIdx || targetFrmidx-m_readFrameIdx >= m_outputCacheSize)) ||
                (!m_readForward && (targetFrmidx > m_readFrameIdx || m_readFrameIdx-targetFrmidx >= m_outputCacheSize)))
            {
                if (!SeekTo(pos, false))
                    return false;
            }

            // the frame queue may not be filled with the target frame, wait for the mixing thread to fill it
            bool lockAquaired = false;
            while (!m_quit)
            {
                m_outputCacheLock.lock();
                lockAquaired = true;
                if ((m_readForward && targetFrmidx < m_outputCache.size()+m_readFrameIdx) ||
                    (!m_readForward && m_readFrameIdx < m_outputCache.size()+targetFrmidx))
                    break;
                m_outputCacheLock.unlock();
                lockAquaired = false;
                this_thread::sleep_for(chrono::milliseconds(5));
            }
            if (m_quit)
            {
                if (lockAquaired) m_outputCacheLock.unlock();
                m_errMsg = "This 'MultiTrackVideoReader' instance is quit.";
                return false;
            }

            lock_guard<mutex> lk2(m_outputCacheLock, adopt_lock);
            if ((m_readForward && targetFrmidx > m_readFrameIdx) || (!m_readForward && m_readFrameIdx > targetFrmidx))
            {
                auto popCnt = m_readForward ? targetFrmidx-m_readFrameIdx : m_readFrameIdx-targetFrmidx;
                while (popCnt-- > 0)
                {
                    m_outputCache.pop_front();
                    if (m_readForward)
                        m_readFrameIdx++;
                    else
                        m_readFrameIdx--;
                }
            }
            if (m_outputCache.empty())
            {
                m_logger->Log(Error) << "No AVAILABLE frame to read!" << endl;
                return false;
            }
            frames = m_outputCache.front();
            auto& vmat = frames[0].frame;
            const double timestamp = (double)pos/1000;
            if (vmat.time_stamp > timestamp+m_frameInterval || vmat.time_stamp < timestamp-m_frameInterval)
                m_logger->Log(Error) << "WRONG image time stamp!! Required 'pos' is " << timestamp
                    << ", output vmat time stamp is " << vmat.time_stamp << "." << endl;

            if (!m_subtrks.empty())
                vmat = BlendSubtitle(vmat);
        }
        return true;
    }

    bool ReadVideoFrame(int64_t pos, ImGui::ImMat& vmat, bool nonblocking) override
    {
        vector<CorrelativeFrame> frames;
        bool success = ReadVideoFrameEx(pos, frames, nonblocking, true);
        if (!success)
            return false;
        vmat = frames[0].frame;
        return true;
    }

    bool ReadNextVideoFrameEx(vector<CorrelativeFrame>& frames) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return false;
        }

        bool lockAquaired = false;
        while (!m_quit)
        {
            m_outputCacheLock.lock();
            lockAquaired = true;
            if (m_outputCache.size() > 1)
                break;
            m_outputCacheLock.unlock();
            lockAquaired = false;
            this_thread::sleep_for(chrono::milliseconds(5));
        }
        if (m_quit)
        {
            if (lockAquaired) m_outputCacheLock.unlock();
            m_errMsg = "This 'MultiTrackVideoReader' instance is quit.";
            return false;
        }

        lock_guard<mutex> lk2(m_outputCacheLock, adopt_lock);
        if (m_readForward)
        {
            m_outputCache.pop_front();
            m_readFrameIdx++;
        }
        else if (m_readFrameIdx > 0)
        {
            m_outputCache.pop_front();
            m_readFrameIdx--;
        }
        frames = m_outputCache.front();
        if (!m_subtrks.empty())
            frames[0].frame = BlendSubtitle(frames[0].frame);
        return true;
    }

    bool ReadNextVideoFrame(ImGui::ImMat& vmat) override
    {
        vector<CorrelativeFrame> frames;
        bool success = ReadNextVideoFrameEx(frames);
        if (!success)
            return false;
        vmat = frames[0].frame;
        return true;
    }

    void UpdateDuration() override
    {
        lock_guard<recursive_mutex> lk(m_trackLock);
        int64_t dur = 0;
        for (auto& track : m_tracks)
        {
            const int64_t trackDur = track->Duration();
            if (trackDur > dur)
                dur = trackDur;
        }
        m_duration = dur;
    }

    bool Refresh(bool async) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return false;
        }

        UpdateDuration();

        int64_t currPos = m_inSeekingState ? m_seekPos : ReadPos();
        SeekTo(currPos, async);
        return true;
    }

    uint32_t TrackCount() const override
    {
        return m_tracks.size();
    }

    list<VideoTrack::Holder>::iterator TrackListBegin() override
    {
        return m_tracks.begin();
    }

    list<VideoTrack::Holder>::iterator TrackListEnd() override
    {
        return m_tracks.end();
    }

    VideoTrack::Holder GetTrackByIndex(uint32_t idx) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (idx >= m_tracks.size())
            return nullptr;
        lock_guard<recursive_mutex> lk2(m_trackLock);
        auto iter = m_tracks.begin();
        while (idx-- > 0 && iter != m_tracks.end())
            iter++;
        return iter != m_tracks.end() ? *iter : nullptr;
    }

    VideoTrack::Holder GetTrackById(int64_t id, bool createIfNotExists) override
    {
        lock(m_apiLock, m_trackLock);
        lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
        lock_guard<recursive_mutex> lk2(m_trackLock, adopt_lock);
        auto iter = find_if(m_tracks.begin(), m_tracks.end(), [id] (const VideoTrack::Holder& track) {
            return track->Id() == id;
        });
        if (iter != m_tracks.end())
            return *iter;
        if (createIfNotExists)
            return AddTrack(id, -1);
        else
            return nullptr;
    }

    VideoClip::Holder GetClipById(int64_t clipId) override
    {
        lock(m_apiLock, m_trackLock);
        lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
        lock_guard<recursive_mutex> lk2(m_trackLock, adopt_lock);
        VideoClip::Holder clip;
        for (auto& track : m_tracks)
        {
            clip = track->GetClipById(clipId);
            if (clip)
                break;
        }
        if (!clip)
        {
            ostringstream oss;
            oss << "CANNOT find clip with id " << clipId << "!";
            m_errMsg = oss.str();
        }
        return clip;
    }

    VideoOverlap::Holder GetOverlapById(int64_t ovlpId) override
    {
        lock(m_apiLock, m_trackLock);
        lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
        lock_guard<recursive_mutex> lk2(m_trackLock, adopt_lock);
        VideoOverlap::Holder ovlp;
        for (auto& track : m_tracks)
        {
            ovlp = track->GetOverlapById(ovlpId);
            if (ovlp)
                break;
        }
        return ovlp;
    }

    int64_t Duration() const override
    {
        return m_duration;
    }

    int64_t ReadPos() const override
    {
        return m_readFrameIdx*1000*m_frameRate.den/m_frameRate.num;
    }

    SubtitleTrackHolder BuildSubtitleTrackFromFile(int64_t id, const string& url, int64_t insertAfterId) override
    {
        SubtitleTrackHolder newSubTrack = SubtitleTrack::BuildFromFile(id, url);
        newSubTrack->SetFrameSize(m_outWidth, m_outHeight);
        newSubTrack->SetAlignment(5);
        newSubTrack->SetOffsetCompensationV((int32_t)((double)m_outHeight*0.43));
        newSubTrack->SetOffsetCompensationV(0.43f);
        newSubTrack->EnableFullSizeOutput(false);
        lock_guard<mutex> lk(m_subtrkLock);
        if (insertAfterId == -1)
        {
            m_subtrks.push_back(newSubTrack);
        }
        else
        {
            auto insertBeforeIter = m_subtrks.begin();
            if (insertAfterId != -2)
            {
                insertBeforeIter = find_if(m_subtrks.begin(), m_subtrks.end(), [insertAfterId] (auto trk) {
                    return trk->Id() == insertAfterId;
                });
                if (insertBeforeIter == m_subtrks.end())
                {
                    ostringstream oss;
                    oss << "CANNOT find the subtitle track specified by argument 'insertAfterId' " << insertAfterId << "!";
                    m_errMsg = oss.str();
                    return nullptr;
                }
                insertBeforeIter++;
            }
            m_subtrks.insert(insertBeforeIter, newSubTrack);
        }
        return newSubTrack;
    }

    SubtitleTrackHolder NewEmptySubtitleTrack(int64_t id, int64_t insertAfterId) override
    {
        SubtitleTrackHolder newSubTrack = SubtitleTrack::NewEmptyTrack(id);
        newSubTrack->SetFrameSize(m_outWidth, m_outHeight);
        newSubTrack->SetAlignment(5);
        newSubTrack->SetOffsetCompensationV((int32_t)((double)m_outHeight*0.43));
        newSubTrack->SetOffsetCompensationV(0.43f);
        newSubTrack->EnableFullSizeOutput(false);
        lock_guard<mutex> lk(m_subtrkLock);
        if (insertAfterId == -1)
        {
            m_subtrks.push_back(newSubTrack);
        }
        else
        {
            auto insertBeforeIter = m_subtrks.begin();
            if (insertAfterId != -2)
            {
                insertBeforeIter = find_if(m_subtrks.begin(), m_subtrks.end(), [insertAfterId] (auto trk) {
                    return trk->Id() == insertAfterId;
                });
                if (insertBeforeIter == m_subtrks.end())
                {
                    ostringstream oss;
                    oss << "CANNOT find the subtitle track specified by argument 'insertAfterId' " << insertAfterId << "!";
                    m_errMsg = oss.str();
                    return nullptr;
                }
                insertBeforeIter++;
            }
            m_subtrks.insert(insertBeforeIter, newSubTrack);
        }
        return newSubTrack;
    }

    SubtitleTrackHolder GetSubtitleTrackById(int64_t trackId) override
    {
        lock_guard<mutex> lk(m_subtrkLock);
        auto iter = find_if(m_subtrks.begin(), m_subtrks.end(), [trackId] (SubtitleTrackHolder& hTrk) {
            return hTrk->Id() == trackId;
        });
        if (iter == m_subtrks.end())
            return nullptr;
        return *iter;
    }

    SubtitleTrackHolder RemoveSubtitleTrackById(int64_t trackId) override
    {
        lock_guard<mutex> lk(m_subtrkLock);
        auto iter = find_if(m_subtrks.begin(), m_subtrks.end(), [trackId] (SubtitleTrackHolder& hTrk) {
            return hTrk->Id() == trackId;
        });
        if (iter == m_subtrks.end())
            return nullptr;
        SubtitleTrackHolder hTrk = *iter;
        m_subtrks.erase(iter);
        return hTrk;
    }

    bool ChangeSubtitleTrackViewOrder(int64_t targetId, int64_t insertAfterId) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (targetId == insertAfterId)
        {
            m_errMsg = "INVALID arguments! 'targetId' must NOT be the SAME as 'insertAfterId'!";
            return false;
        }

        lock_guard<mutex> lk2(m_subtrkLock);
        auto targetTrackIter = find_if(m_subtrks.begin(), m_subtrks.end(), [targetId] (auto trk) {
            return trk->Id() == targetId;
        });
        if (targetTrackIter == m_subtrks.end())
        {
            ostringstream oss;
            oss << "CANNOT find the video track specified by argument 'targetId' " << targetId << "!";
            m_errMsg = oss.str();
            return false;
        }
        if (insertAfterId == -1)
        {
            auto moveTrack = *targetTrackIter;
            m_subtrks.erase(targetTrackIter);
            m_subtrks.push_back(moveTrack);
        }
        else
        {
            auto insertBeforeIter = m_subtrks.begin();
            if (insertAfterId != -2)
            {
                insertBeforeIter = find_if(m_subtrks.begin(), m_subtrks.end(), [insertAfterId] (auto trk) {
                    return trk->Id() == insertAfterId;
                });
                if (insertBeforeIter == m_subtrks.end())
                {
                    ostringstream oss;
                    oss << "CANNOT find the video track specified by argument 'insertAfterId' " << insertAfterId << "!";
                    m_errMsg = oss.str();
                    return false;
                }
                insertBeforeIter++;
            }
            auto moveTrack = *targetTrackIter;
            m_subtrks.erase(targetTrackIter);
            m_subtrks.insert(insertBeforeIter, moveTrack);
        }
        return true;
    }

    string GetError() const override
    {
        return m_errMsg;
    }

private:
    void StartMixingThread()
    {
        m_quit = false;
        m_mixingThread = thread(&MultiTrackVideoReader_Impl::MixingThreadProc, this);
        SysUtils::SetThreadName(m_mixingThread, "MtvMixing");
    }

    void TerminateMixingThread()
    {
        if (m_mixingThread.joinable())
        {
            m_quit = true;
            m_mixingThread.join();
        }
    }

    void MixingThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter MixingThreadProc(VIDEO)..." << endl;

        bool afterSeek = false;
        while (!m_quit)
        {
            bool idleLoop = true;

            if (m_seeking.exchange(false))
            {
                int64_t seekPos = m_seekPos;
                m_readFrameIdx = (int64_t)(floor((double)seekPos*m_frameRate.num/(m_frameRate.den*1000)));
                seekPos = m_readFrameIdx*m_frameRate.den*1000/m_frameRate.num;  // seek pos aligned to frame positon
                m_logger->Log(DEBUG) << "\t\t ===== Seeking to pos=" << seekPos << endl;
                for (auto track : m_tracks)
                    track->SeekTo(seekPos);
                {
                    lock_guard<mutex> lk(m_outputCacheLock);
                    m_outputCache.clear();
                }
                afterSeek = true;
                m_inSeekingState = false;
            }

            if (m_nextReadPos != INT64_MIN)
            {
                int64_t nextReadFrameIdx = (int64_t)(floor((double)m_nextReadPos*m_frameRate.num/(m_frameRate.den*1000)))-1;
                if (nextReadFrameIdx > m_readFrameIdx)
                {
                    lock_guard<mutex> lk(m_outputCacheLock);
                    auto popCnt = m_readForward ? nextReadFrameIdx-m_readFrameIdx : m_readFrameIdx-nextReadFrameIdx;
                    while (popCnt-- > 0 && !m_outputCache.empty())
                    {
                        m_outputCache.pop_front();
                        if (m_readForward)
                            m_readFrameIdx++;
                        else
                            m_readFrameIdx--;
                    }
                    if (m_readFrameIdx != nextReadFrameIdx)
                    {
                        lock_guard<recursive_mutex> trackLk(m_trackLock);   
                        for (auto pTrack : m_tracks)
                        {
                            pTrack->SetReadFrameIndex(nextReadFrameIdx);
                        }
                        m_readFrameIdx = nextReadFrameIdx;
                        m_logger->Log(DEBUG) << "\t-----=====-----> SetReadFrameIndex(" << nextReadFrameIdx << ") <-----=====-----" << endl;
                    }
                }
                m_nextReadPos = INT64_MIN;
            }

            if (m_outputCache.size() < m_outputCacheSize)
            {
                ImGui::ImMat mixedFrame;
                list<VideoTrack::Holder> tracks;
                {
                    lock_guard<recursive_mutex> trackLk(m_trackLock);
                    tracks = m_tracks;
                }
                vector<CorrelativeFrame> frames;
                frames.reserve(tracks.size()*7);
                frames.push_back({CorrelativeFrame::PHASE_AFTER_MIXING, 0, 0, mixedFrame});
                double timestamp = (double)m_readFrameIdx*m_frameRate.den/m_frameRate.num;
                auto trackIter = tracks.begin();
                bool isFirstTrack = true;
                while (trackIter != tracks.end())
                {
                    auto hTrack = *trackIter++;
                    if (!hTrack->IsVisible())
                    {
                        hTrack->SkipOneFrame();
                        continue;
                    }

                    ImGui::ImMat vmat;
                    hTrack->ReadVideoFrame(frames, vmat);
                    if (!vmat.empty())
                    {
                        if (mixedFrame.empty())
                            mixedFrame = vmat;
                        else
                            mixedFrame = m_hMixBlender->Blend(vmat, mixedFrame);
                    }
                    if (isFirstTrack)
                        timestamp = vmat.time_stamp;
                    else if (timestamp != vmat.time_stamp)
                        m_logger->Log(WARN) << "'vmat' got from non-1st track has DIFFERENT TIMESTAMP against the 1st track! "
                            << timestamp << " != " << vmat.time_stamp << "." << endl;
                }

                if (mixedFrame.empty())
                {
                    mixedFrame.create_type(m_outWidth, m_outHeight, 4, IM_DT_INT8);
                    memset(mixedFrame.data, 0, mixedFrame.total()*mixedFrame.elemsize);
                    mixedFrame.time_stamp = timestamp;
                }
                frames[0].frame = mixedFrame;
                m_logger->Log(DEBUG) << "---------> Got mixed frame at pos=" << (int64_t)(timestamp*1000) << endl;

                if (afterSeek)
                {
                    int64_t frameIdx = (int64_t)(round(timestamp*m_frameRate.num/m_frameRate.den));
                    if ((m_readForward && frameIdx >= m_readFrameIdx) || (!m_readForward && frameIdx <= m_readFrameIdx))
                    {
                        m_readFrameIdx = frameIdx;
                        afterSeek = false;
                    }
                    lock_guard<mutex> lk(m_outputCacheLock);
                    m_seekingFlash = frames;
                }

                if (!afterSeek)
                {
                    lock_guard<mutex> lk(m_outputCacheLock);
                    if (!m_inSeekingState)
                        m_outputCache.push_back(frames);
                    m_seekingFlash = frames;
                    idleLoop = false;
                }
                else
                {
                    m_logger->Log(WARN) << "!!! Mixed frame discarded !!!" << endl;
                }
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }

        m_logger->Log(DEBUG) << "Leave MixingThreadProc(VIDEO)." << endl;
    }

    ImGui::ImMat BlendSubtitle(ImGui::ImMat& vmat)
    {
        if (m_subtrks.empty())
            return vmat;

        ImGui::ImMat res = vmat;
        bool cloned = false;
        int64_t pos = (int64_t)(vmat.time_stamp*1000);
        lock_guard<mutex> lk(m_subtrkLock);
        for (auto& hSubTrack : m_subtrks)
        {
            if (!hSubTrack->IsVisible())
                continue;

            auto hSubClip = hSubTrack->GetClipByTime(pos);
            if (hSubClip)
            {
                auto subImg = hSubClip->Image(pos-hSubClip->StartTime());
                if (subImg.Valid())
                {
                    // blend subtitle-image
                    SubtitleImage::Rect dispRect = subImg.Area();
                    ImGui::ImMat submat = subImg.Vmat();
                    res = m_hSubBlender->Blend(res, submat, dispRect.x, dispRect.y);
                    if (res.empty())
                    {
                        m_logger->Log(Error) << "FAILED to blend subtitle on the output image! Error message is '" << m_hSubBlender->GetError() << "'." << endl;
                    }
                }
                else
                {
                    m_logger->Log(Error) << "Invalid 'SubtitleImage' at " << MillisecToString(pos) << "." << endl;
                }
            }
        }
        return res;
    }

private:
    ALogger* m_logger;
    string m_errMsg;
    recursive_mutex m_apiLock;

    thread m_mixingThread;
    list<VideoTrack::Holder> m_tracks;
    recursive_mutex m_trackLock;
    VideoBlender::Holder m_hMixBlender;

    list<vector<CorrelativeFrame>> m_outputCache;
    mutex m_outputCacheLock;
    uint32_t m_outputCacheSize{4};

    uint32_t m_outWidth{0};
    uint32_t m_outHeight{0};
    Ratio m_frameRate;
    double m_frameInterval{0};
    int64_t m_duration{0};
    int64_t m_readFrameIdx{0};
    bool m_readForward{true};
    int64_t m_prevReadPos{INT64_MIN};
    int64_t m_nextReadPos{INT64_MIN};

    int64_t m_seekPos{0};
    atomic_bool m_seeking{false};
    bool m_inSeekingState{false};
    vector<CorrelativeFrame> m_seekingFlash;

    list<SubtitleTrackHolder> m_subtrks;
    mutex m_subtrkLock;
    VideoBlender::Holder m_hSubBlender;

    bool m_configured{false};
    bool m_started{false};
    bool m_quit{false};
};

static const auto MULTI_TRACK_VIDEO_READER_DELETER = [] (MultiTrackVideoReader* p) {
    MultiTrackVideoReader_Impl* ptr = dynamic_cast<MultiTrackVideoReader_Impl*>(p);
    ptr->Close();
    delete ptr;
};

MultiTrackVideoReader::Holder MultiTrackVideoReader::CreateInstance()
{
    return MultiTrackVideoReader::Holder(new MultiTrackVideoReader_Impl(), MULTI_TRACK_VIDEO_READER_DELETER);
}

MultiTrackVideoReader::Holder MultiTrackVideoReader_Impl::CloneAndConfigure(uint32_t outWidth, uint32_t outHeight, const Ratio& frameRate)
{
    lock_guard<recursive_mutex> lk(m_apiLock);
    MultiTrackVideoReader_Impl* newInstance = new MultiTrackVideoReader_Impl();
    if (!newInstance->Configure(outWidth, outHeight, frameRate))
    {
        m_errMsg = newInstance->GetError();
        newInstance->Close(); delete newInstance;
        return nullptr;
    }

    // clone all the video tracks
    {
        lock_guard<recursive_mutex> lk2(m_trackLock);
        for (auto track : m_tracks)
        {
            newInstance->m_tracks.push_back(track->Clone(outWidth, outHeight, frameRate));
        }
    }
    newInstance->UpdateDuration();
    // seek to 0
    newInstance->m_outputCache.clear();
    for (auto track : newInstance->m_tracks)
        track->SeekTo(0);

    // clone all the subtitle tracks
    {
        lock_guard<mutex> lk2(m_subtrkLock);
        for (auto subtrk : m_subtrks)
        {
            if (!subtrk->IsVisible())
                continue;
            newInstance->m_subtrks.push_back(subtrk->Clone(outWidth, outHeight));
        }
    }

    // start new instance
    if (!newInstance->Start())
    {
        m_errMsg = newInstance->GetError();
        newInstance->Close(); delete newInstance;
        return nullptr;
    }
    return MultiTrackVideoReader::Holder(newInstance, MULTI_TRACK_VIDEO_READER_DELETER);
}

ALogger* MultiTrackVideoReader::GetLogger()
{
    return Logger::GetLogger("MTVReader");
}

ostream& operator<<(ostream& os, MultiTrackVideoReader::Holder hMtvReader)
{
    os << ">>> MultiTrackVideoReader :" << std::endl;
    auto trackIter = hMtvReader->TrackListBegin();
    while (trackIter != hMtvReader->TrackListEnd())
    {
        auto& track = *trackIter;
        os << "\t Track#" << track->Id() << " : " << track << std::endl;
        trackIter++;
    }
    os << "<<< [END]MultiTrackVideoReader";
    return os;
}
}
