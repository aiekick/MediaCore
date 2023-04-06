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

#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <list>
#include <algorithm>
#include "AudioTrack.h"
#include "MultiTrackAudioReader.h"
#include "FFUtils.h"
#include "SysUtils.h"
extern "C"
{
    #include "libavutil/avutil.h"
    #include "libavutil/avstring.h"
    #include "libavutil/pixdesc.h"
    #include "libavformat/avformat.h"
    #include "libavcodec/avcodec.h"
    #include "libavdevice/avdevice.h"
    #include "libavfilter/avfilter.h"
    #include "libavfilter/buffersrc.h"
    #include "libavfilter/buffersink.h"
    #include "libswscale/swscale.h"
    #include "libswresample/swresample.h"
}

using namespace std;
using namespace Logger;

namespace MediaCore
{
class MultiTrackAudioReader_Impl : public MultiTrackAudioReader
{
public:
    MultiTrackAudioReader_Impl()
    {
        m_logger = MultiTrackAudioReader::GetLogger();
    }

    MultiTrackAudioReader_Impl(const MultiTrackAudioReader_Impl&) = delete;
    MultiTrackAudioReader_Impl(MultiTrackAudioReader_Impl&&) = delete;
    MultiTrackAudioReader_Impl& operator=(const MultiTrackAudioReader_Impl&) = delete;

    bool Configure(uint32_t outChannels, uint32_t outSampleRate, uint32_t outSamplesPerFrame) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (m_started)
        {
            m_errMsg = "This MultiTrackAudioReader instance is already started!";
            return false;
        }

        Close();

        m_outSampleRate = outSampleRate;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        m_outChannels = outChannels;
        m_outChannelLayout = av_get_default_channel_layout(outChannels);
#else
        av_channel_layout_default(&m_outChlyt, outChannels);
#endif
        m_outSamplesPerFrame = outSamplesPerFrame;
        m_samplePos = 0;
        m_readPos = 0;
        m_frameSize = outChannels*4;  // for now, output sample format only supports float32 data type, thus 4 bytes per sample.
        m_isTrackOutputPlanar = av_sample_fmt_is_planar(m_trackOutSmpfmt);
        m_matAvfrmCvter = new AudioImMatAVFrameConverter();
        m_mixOutDataType = GetDataTypeFromSampleFormat(m_mixOutSmpfmt);
        m_outMtsPerFrame = av_rescale_q(m_outSamplesPerFrame, {1, (int)m_outSampleRate}, MILLISEC_TIMEBASE);

        m_aeFilter = AudioEffectFilter::CreateInstance("AEFilter#mix");
        if (!m_aeFilter->Init(
            AudioEffectFilter::VOLUME|AudioEffectFilter::COMPRESSOR|AudioEffectFilter::GATE|AudioEffectFilter::EQUALIZER|AudioEffectFilter::LIMITER|AudioEffectFilter::PAN,
            av_get_sample_fmt_name(m_mixOutSmpfmt), outChannels, outSampleRate))
        {
            m_errMsg = "FAILED to initialize AudioEffectFilter!";
            return false;
        }

        m_configured = true;
        return true;
    }

    Holder CloneAndConfigure(uint32_t outChannels, uint32_t outSampleRate, uint32_t outSamplesPerFrame) override;

    bool Start() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (m_started)
        {
            return true;
        }
        if (!m_configured)
        {
            m_errMsg = "This MultiTrackAudioReader instance is NOT configured yet!";
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

        ReleaseMixer();
        m_tracks.clear();
        m_outputMats.clear();
        m_configured = false;
        m_started = false;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        m_outChannels = 0;
#else
        m_outChlyt = {AV_CHANNEL_ORDER_UNSPEC, 0};
#endif
        m_outSampleRate = 0;
        m_outSamplesPerFrame = 1024;
        if (m_matAvfrmCvter)
        {
            delete m_matAvfrmCvter;
            m_matAvfrmCvter = nullptr;
        }
    }

    AudioTrack::Holder AddTrack(int64_t trackId) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackAudioReader instance is NOT started yet!";
            return nullptr;
        }

        TerminateMixingThread();

        uint32_t outChannels;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        outChannels = m_outChannels;
#else
        outChannels = m_outChlyt.nb_channels;
#endif
        AudioTrack::Holder hTrack = AudioTrack::CreateInstance(trackId, outChannels, m_outSampleRate, av_get_sample_fmt_name(m_trackOutSmpfmt));
        hTrack->SetDirection(m_readForward);
        {
            lock_guard<recursive_mutex> lk2(m_trackLock);
            m_tracks.push_back(hTrack);
            UpdateDuration();
            int64_t pos = m_samplePos*1000/m_outSampleRate;
            for (auto track : m_tracks)
                track->SeekTo(pos);
            m_outputMats.clear();
        }

        ReleaseMixer();
        if (!CreateMixer())
            return nullptr;

        StartMixingThread();
        return hTrack;
    }

    AudioTrack::Holder RemoveTrackByIndex(uint32_t index) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackAudioReader instance is NOT started yet!";
            return nullptr;
        }
        if (index >= m_tracks.size())
        {
            m_errMsg = "Invalid value for argument 'index'!";
            return nullptr;
        }

        TerminateMixingThread();

        AudioTrack::Holder delTrack;
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
                m_outputMats.clear();

                ReleaseMixer();
                if (!m_tracks.empty())
                {
                    if (!CreateMixer())
                        return nullptr;
                }
            }
        }

        StartMixingThread();
        return delTrack;
    }

    AudioTrack::Holder RemoveTrackById(int64_t trackId) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return nullptr;
        }

        TerminateMixingThread();

        AudioTrack::Holder delTrack;
        {
            lock_guard<recursive_mutex> lk2(m_trackLock);
            auto iter = find_if(m_tracks.begin(), m_tracks.end(), [trackId] (const AudioTrack::Holder& track) {
                return track->Id() == trackId;
            });
            if (iter != m_tracks.end())
            {
                delTrack = *iter;
                m_tracks.erase(iter);
                UpdateDuration();
                for (auto track : m_tracks)
                    track->SeekTo(ReadPos());
                m_outputMats.clear();

                ReleaseMixer();
                if (!m_tracks.empty())
                {
                    if (!CreateMixer())
                        return nullptr;
                }
            }
        }

        StartMixingThread();
        return delTrack;
    }

    bool SetDirection(bool forward) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (m_readForward == forward)
            return true;

        TerminateMixingThread();

        m_readForward = forward;
        for (auto& track : m_tracks)
            track->SetDirection(forward);

        int64_t readPos = ReadPos();
        for (auto track : m_tracks)
            track->SeekTo(readPos);
        m_samplePos = readPos*m_outSampleRate/1000;

        m_outputMats.clear();
        ReleaseMixer();
        if (!m_tracks.empty())
        {
            if (!CreateMixer())
                return false;
        }

        StartMixingThread();
        return true;
    }

    bool SeekTo(int64_t pos, bool probeMode = false) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackAudioReader instance is NOT started yet!";
            return false;
        }
        if (pos < 0)
        {
            m_errMsg = "INVALID argument! 'pos' must in the range of [0, Duration()].";
            return false;
        }

        m_logger->Log(DEBUG) << "------> SeekTo(pos=" << pos << "), probeMode=" << probeMode << endl;
        if (probeMode)
        {
            if (fabs(m_prevSeekPos-pos) <= m_probeDuration)
            {
                m_logger->Log(DEBUG) << "---->>> Too small seek gap, skip this seek operation" << endl;
            }
            else
            {
                m_prevSeekPos = m_seekPos = pos;
                m_seeking = true;
                m_probeMode = probeMode;
            }
        }
        else
        {
            TerminateMixingThread();

            // disable probe mode effect if there is any
            m_probeMode = probeMode;
            m_seeking = false;
            m_prevSeekPos = m_seekPos = INT64_MIN;
            m_aeFilter->SetMuted(false);

            m_outputMats.clear();
            m_samplePos = pos*m_outSampleRate/1000;
            m_readPos = pos;
            for (auto track : m_tracks)
                track->SeekTo(pos);
            StartMixingThread();
        }
        return true;
    }

    bool SetTrackMuted(int64_t id, bool muted) override
    {
        auto track = GetTrackById(id, false);
        if (track)
        {
            track->SetMuted(muted);
            return true;
        }
        return false;
    }

    bool IsTrackMuted(int64_t id) override
    {
        auto track = GetTrackById(id, false);
        if (track)
            return track->IsMuted();
        return false;
    }


    bool ReadAudioSamplesEx(vector<CorrelativeFrame>& amats, bool& eof) override
    {
        amats.clear();
        eof = false;
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackAudioReader instance is NOT started yet!";
            return false;
        }

        m_outputMatsLock.lock();
        if (m_probeMode && m_outputMats.empty())
        {
            m_logger->Log(DEBUG) << "In probe-mode, NO more pcm samples." << endl;
            m_outputMatsLock.unlock();
            return false;
        }

        while (m_outputMats.empty() && !m_quit)
        {
            m_outputMatsLock.unlock();
            this_thread::sleep_for(chrono::milliseconds(5));
            m_outputMatsLock.lock();
        }
        lock_guard<mutex> lk2(m_outputMatsLock, adopt_lock);
        if (m_quit)
        {
            m_errMsg = "This 'MultiTrackAudioReader' instance is quit.";
            return false;
        }

        amats = m_outputMats.front();
        m_outputMats.pop_front();
        m_readPos += (int64_t)amats[0].frame.w*1000/m_outSampleRate;
        eof = m_eof;
        return true;
    }

    bool ReadAudioSamples(ImGui::ImMat& amat, bool& eof) override
    {
        vector<CorrelativeFrame> amats;
        bool res = ReadAudioSamplesEx(amats, eof);
        amat = amats[0].frame;
        return res;
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

    bool Refresh() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This MultiTrackVideoReader instance is NOT started yet!";
            return false;
        }

        UpdateDuration();

        SeekTo(ReadPos());
        return true;
    }

    int64_t SizeToDuration(uint32_t sizeInByte) override
    {
        if (!m_configured)
            return -1;
        uint32_t sampleCnt = sizeInByte/m_frameSize;
        return av_rescale_q(sampleCnt, {1, (int)m_outSampleRate}, MILLISEC_TIMEBASE);
    }

    uint32_t TrackCount() const override
    {
        return m_tracks.size();
    }

    list<AudioTrack::Holder>::iterator TrackListBegin() override
    {
        return m_tracks.begin();
    }

    list<AudioTrack::Holder>::iterator TrackListEnd() override
    {
        return m_tracks.end();
    }

    AudioTrack::Holder GetTrackByIndex(uint32_t idx) override
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

    AudioTrack::Holder GetTrackById(int64_t id, bool createIfNotExists) override
    {
        lock(m_apiLock, m_trackLock);
        lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
        lock_guard<recursive_mutex> lk2(m_trackLock, adopt_lock);
        auto iter = find_if(m_tracks.begin(), m_tracks.end(), [id] (const AudioTrack::Holder& track) {
            return track->Id() == id;
        });
        if (iter != m_tracks.end())
            return *iter;
        if (createIfNotExists)
            return AddTrack(id);
        else
            return nullptr;
    }

    AudioClip::Holder GetClipById(int64_t clipId) override
    {
        lock(m_apiLock, m_trackLock);
        lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
        lock_guard<recursive_mutex> lk2(m_trackLock, adopt_lock);
        AudioClip::Holder clip;
        for (auto& track : m_tracks)
        {
            clip = track->GetClipById(clipId);
            if (clip)
                break;
        }
        return clip;
    }

    AudioOverlap::Holder GetOverlapById(int64_t ovlpId) override
    {
        lock(m_apiLock, m_trackLock);
        lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
        lock_guard<recursive_mutex> lk2(m_trackLock, adopt_lock);
        AudioOverlap::Holder ovlp;
        for (auto& track : m_tracks)
        {
            ovlp = track->GetOverlapById(ovlpId);
            if (ovlp)
                break;
        }
        return ovlp;
    }

    AudioEffectFilter::Holder GetAudioEffectFilter() override
    {
        return m_aeFilter;
    }

    int64_t Duration() const override
    {
        return m_duration;
    }

    int64_t ReadPos() const override
    {
        return m_readPos;
    }

    string GetError() const override
    {
        return m_errMsg;
    }

private:
    double ConvertSampleCountToTs(int64_t sampleCount)
    {
        return (double)sampleCount/m_outSampleRate;
    }

    double ConvertPtsToTs(int64_t pts)
    {
        return (double)pts/m_outSampleRate;
    }

    void StartMixingThread()
    {
        m_quit = false;
        m_mixingThread = thread(&MultiTrackAudioReader_Impl::MixingThreadProc, this);
        SysUtils::SetThreadName(m_mixingThread, "MtaMixing");
    }

    void TerminateMixingThread()
    {
        if (m_mixingThread.joinable())
        {
            m_quit = true;
            m_mixingThread.join();
        }
    }

    bool CreateMixer()
    {
        const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
        const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

        m_filterGraph = avfilter_graph_alloc();
        if (!m_filterGraph)
        {
            m_errMsg = "FAILED to allocate new 'AVFilterGraph'!";
            return false;
        }

        ostringstream oss;
        oss << "time_base=1/" << m_outSampleRate << ":sample_rate=" << m_outSampleRate
            << ":sample_fmt=" << av_get_sample_fmt_name(m_trackOutSmpfmt);
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        oss << ":channel_layout=" << av_get_default_channel_layout(m_outChannels);
#else
        char chlytDescBuff[256] = {0};
        av_channel_layout_describe(&m_outChlyt, chlytDescBuff, sizeof(chlytDescBuff));
        oss << ":channel_layout=" << chlytDescBuff;
#endif
        string bufsrcArgs = oss.str(); oss.str("");
        int fferr;

        AVFilterInOut* prevFiltInOutPtr = nullptr;
        for (uint32_t i = 0; i < m_tracks.size(); i++)
        {
            oss << "in_" << i;
            string filtName = oss.str(); oss.str("");
            m_logger->Log(DEBUG) << "buffersrc name '" << filtName << "'." << endl;

            AVFilterContext* bufSrcCtx = nullptr;
            fferr = avfilter_graph_create_filter(&bufSrcCtx, abuffersrc, filtName.c_str(), bufsrcArgs.c_str(), nullptr, m_filterGraph);
            if (fferr < 0)
            {
                oss << "FAILED when invoking 'avfilter_graph_create_filter' for INPUTs! fferr=" << fferr << ".";
                m_errMsg = oss.str();
                return false;
            }

            AVFilterInOut* filtInOutPtr = avfilter_inout_alloc();
            if (!filtInOutPtr)
            {
                m_errMsg = "FAILED to allocate 'AVFilterInOut' instance!";
                return false;
            }
            filtInOutPtr->name       = av_strdup(filtName.c_str());
            filtInOutPtr->filter_ctx = bufSrcCtx;
            filtInOutPtr->pad_idx    = 0;
            filtInOutPtr->next       = nullptr;
            if (prevFiltInOutPtr)
                prevFiltInOutPtr->next = filtInOutPtr;
            else
                m_filterOutputs = filtInOutPtr;
            prevFiltInOutPtr = filtInOutPtr;

            m_bufSrcCtxs.push_back(bufSrcCtx);
        }

        {
            string filtName = "out";

            AVFilterContext* bufSinkCtx = nullptr;
            fferr = avfilter_graph_create_filter(&bufSinkCtx, abuffersink, filtName.c_str(), nullptr, nullptr, m_filterGraph);
            if (fferr < 0)
            {
                oss << "FAILED when invoking 'avfilter_graph_create_filter' for OUTPUTS! fferr=" << fferr << ".";
                m_errMsg = oss.str();
                return false;
            }

            const AVSampleFormat out_sample_fmts[] = { m_mixOutSmpfmt, (AVSampleFormat)-1 };
            fferr = av_opt_set_int_list(bufSinkCtx, "sample_fmts", out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN);
            if (fferr < 0)
            {
                oss << "FAILED when invoking 'av_opt_set_int_list' for OUTPUTS! fferr=" << fferr << ".";
                m_errMsg = oss.str();
                return false;
            }

            AVFilterInOut* filtInOutPtr = avfilter_inout_alloc();
            if (!filtInOutPtr)
            {
                m_errMsg = "FAILED to allocate 'AVFilterInOut' instance!";
                return false;
            }
            filtInOutPtr->name        = av_strdup(filtName.c_str());
            filtInOutPtr->filter_ctx  = bufSinkCtx;
            filtInOutPtr->pad_idx     = 0;
            filtInOutPtr->next        = nullptr;
            m_filterInputs = filtInOutPtr;

            m_bufSinkCtxs.push_back(bufSinkCtx);
        }

        for (uint32_t i = 0; i < m_tracks.size(); i++)
            oss << "[in_" << i << "]";
        oss << "amix=inputs=" << m_tracks.size();
        oss << ":normalize=0";
        string filtArgs = oss.str(); oss.str("");
        m_logger->Log(DEBUG) << "'MultiTrackAudioReader' mixer filter args: '" << filtArgs << "'." << endl;
        fferr = avfilter_graph_parse_ptr(m_filterGraph, filtArgs.c_str(), &m_filterInputs, &m_filterOutputs, nullptr);
        if (fferr < 0)
        {
            oss << "FAILED to invoke 'avfilter_graph_parse_ptr'! fferr=" << fferr << ".";
            m_errMsg = oss.str();
            return false;
        }

        fferr = avfilter_graph_config(m_filterGraph, nullptr);
        if (fferr < 0)
        {
            oss << "FAILED to invoke 'avfilter_graph_config'! fferr=" << fferr << ".";
            m_errMsg = oss.str();
            return false;
        }

        FreeAVFilterInOutPtrs();
        return true;
    }

    void ReleaseMixer()
    {
        if (m_filterGraph)
        {
            avfilter_graph_free(&m_filterGraph);
            m_filterGraph = nullptr;
        }
        m_bufSrcCtxs.clear();
        m_bufSinkCtxs.clear();

        FreeAVFilterInOutPtrs();
    }

    void FreeAVFilterInOutPtrs()
    {
        if (m_filterOutputs)
            avfilter_inout_free(&m_filterOutputs);
        if (m_filterInputs)
            avfilter_inout_free(&m_filterInputs);
    }

    void MixingThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter MixingThreadProc(AUDIO)..." << endl;

        SelfFreeAVFramePtr outfrm = AllocSelfFreeAVFramePtr();
        while (!m_quit)
        {
            bool idleLoop = true;
            int fferr;

            int64_t mixingPos = m_samplePos*1000/m_outSampleRate;
            m_eof = m_readForward ? mixingPos >= Duration() : mixingPos <= 0;
            if (m_outputMats.size() < m_outputMatsMaxCount)
            {
                // handle probe mode transition
                if (m_probeMode)
                {
                    if (m_seeking.exchange(false))
                    {
                        m_probeStage = -1;
                        m_aeFilter->SetMuted(true);
                        m_logger->Log(DEBUG) << "ProbeMode: stage=-1" << endl;
                    }
                    else if (m_seekPos != INT64_MIN)
                    {
                        {
                            lock_guard<recursive_mutex> lk(m_trackLock);
                            for (auto track : m_tracks)
                                track->SeekTo(m_seekPos);
                        }
                        m_seekPos = INT64_MIN;
                        m_probeStage = 1;
                        m_aeFilter->SetMuted(false);
                        m_probeSampleDur = 0;
                        m_logger->Log(DEBUG) << "ProbeMode: stage=+1" << endl;
                    }
                    else if (m_probeStage == 1)
                    {
                        m_probeStage = 0;
                        m_logger->Log(DEBUG) << "ProbeMode: stage= 0" << endl;
                    }
                    else if (m_probeStage == -1)
                    {
                        // stop reading more samples
                        this_thread::sleep_for(chrono::milliseconds(5));
                        continue;
                    }
                    else if (m_probeSampleDur >= m_probeDuration)
                    {
                        m_probeStage = -1;
                        m_aeFilter->SetMuted(true);
                        m_logger->Log(DEBUG) << "ProbeMode: m_probeSampleDur=" << m_probeSampleDur << endl;
                    }
                }

                vector<CorrelativeFrame> corFrames;
                corFrames.push_back({CorrelativeFrame::PHASE_AFTER_MIXING, 0, 0, ImGui::ImMat()});
                if (!m_tracks.empty())
                {
                    {
                        lock_guard<recursive_mutex> lk(m_trackLock);
                        uint32_t i = 0;
                        for (auto iter = m_tracks.begin(); iter != m_tracks.end(); iter++, i++)
                        {
                            auto& track = *iter;
                            ImGui::ImMat amat = track->ReadAudioSamples(m_outSamplesPerFrame);
                            corFrames.push_back({CorrelativeFrame::PHASE_AFTER_TRANSITION, 0, track->Id(), amat});
                            SelfFreeAVFramePtr audfrm = AllocSelfFreeAVFramePtr();
                            m_matAvfrmCvter->ConvertImMatToAVFrame(amat, audfrm.get(), m_samplePos);
                            fferr = av_buffersrc_add_frame(m_bufSrcCtxs[i], audfrm.get());
                            if (fferr < 0)
                            {
                                m_logger->Log(Error) << "FAILED to invoke 'av_buffersrc_add_frame'(In MixingThreadProc)! fferr=" << fferr << "." << endl;
                                break;
                            }
                        }
                        if (m_readForward)
                            m_samplePos += m_outSamplesPerFrame;
                        else
                            m_samplePos -= m_outSamplesPerFrame;
                    }

                    fferr = av_buffersink_get_frame(m_bufSinkCtxs[0], outfrm.get());
                    if (fferr >= 0)
                    {
                        ImGui::ImMat amat;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
                        int outChannels = m_outChannels;
#else
                        int outChannels = m_outChlyt.nb_channels;
#endif
                        amat.create((int)m_outSamplesPerFrame, 1, outChannels, (size_t)4);
                        if (amat.total()*4 == outfrm->linesize[0])
                        {
                            memcpy(amat.data, outfrm->data[0], outfrm->linesize[0]);
                            amat.time_stamp = ConvertPtsToTs(outfrm->pts);
                            amat.type = m_mixOutDataType;
                            amat.flags = IM_MAT_FLAGS_AUDIO_FRAME;
                            amat.rate = { (int)m_outSampleRate, 1 };
                            amat.elempack = outChannels;
                            av_frame_unref(outfrm.get());
                            list<ImGui::ImMat> aeOutMats;
                            if (!m_aeFilter->ProcessData(amat, aeOutMats))
                            {
                                m_logger->Log(Error) << "FAILED to apply AudioEffectFilter after mixing! Error is '" << m_aeFilter->GetError() << "'." << endl;
                            }
                            else if (aeOutMats.size() != 1)
                                m_logger->Log(Error) << "After mixing AudioEffectFilter returns " << aeOutMats.size() << " mats!" << endl;
                            else
                            {
                                auto& frontMat = aeOutMats.front();
                                if (frontMat.total() != amat.total())
                                    m_logger->Log(Error) << "After mixing AudioEffectFilter, front mat has different size (" << (frontMat.total()*4)
                                        << ") against input mat (" << (amat.total()*4) << ")!" << endl;
                                else
                                    amat = frontMat;
                            }
                            corFrames[0].frame = amat;
                            lock_guard<mutex> lk(m_outputMatsLock);
                            m_outputMats.push_back(corFrames);
                            idleLoop = false;
                        }
                        else
                            m_logger->Log(Error) << "Audio frame linesize(" << outfrm->linesize[0] << ") is ABNORMAL!" << endl;
                    }
                    else if (fferr != AVERROR(EAGAIN))
                    {
                        m_logger->Log(Error) << "FAILED to invoke 'av_buffersink_get_frame'(In MixingThreadProc)! fferr=" << fferr << "." << endl;
                    }
                }
                else
                {
                    ImGui::ImMat amat;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
                    int outChannels = m_outChannels;
#else
                    int outChannels = m_outChlyt.nb_channels;
#endif
                    amat.create((int)m_outSamplesPerFrame, 1, outChannels, (size_t)4);
                    memset(amat.data, 0, amat.total()*amat.elemsize);
                    amat.time_stamp = ConvertPtsToTs(m_samplePos);
                    amat.flags = IM_MAT_FLAGS_AUDIO_FRAME;
                    amat.rate = { (int)m_outSampleRate, 1 };
                    amat.elempack = outChannels;
                    if (m_readForward)
                        m_samplePos += m_outSamplesPerFrame;
                    else
                        m_samplePos -= m_outSamplesPerFrame;
                    corFrames[0].frame = amat;
                    lock_guard<mutex> lk(m_outputMatsLock);
                    m_outputMats.push_back(corFrames);
                    idleLoop = false;
                }

                if (m_probeMode)
                {
                    m_probeSampleDur += m_outMtsPerFrame;
                }
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }

        m_logger->Log(DEBUG) << "Leave MixingThreadProc(AUDIO)." << endl;
    }

private:
    ALogger* m_logger;
    string m_errMsg;
    recursive_mutex m_apiLock;
    thread m_mixingThread;
    AVSampleFormat m_mixOutSmpfmt{AV_SAMPLE_FMT_FLT};
    ImDataType m_mixOutDataType;

    list<AudioTrack::Holder> m_tracks;
    recursive_mutex m_trackLock;
    int64_t m_duration{0};
    int64_t m_samplePos{0};
    uint32_t m_outSampleRate{0};
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
    uint32_t m_outChannels{0};
    int64_t m_outChannelLayout{0};
#else
    AVChannelLayout m_outChlyt{AV_CHANNEL_ORDER_UNSPEC, 0};
#endif
    AVSampleFormat m_trackOutSmpfmt{AV_SAMPLE_FMT_FLTP};
    bool m_isTrackOutputPlanar{false};
    uint32_t m_frameSize{0};
    uint32_t m_outSamplesPerFrame{1024};
    int64_t m_outMtsPerFrame{0};
    int64_t m_readPos{0};
    bool m_readForward{true};
    bool m_eof{false};
    bool m_probeMode{false};
    int32_t m_probeStage;  // -1 : fade out; 1 : fade in; 0 : do nothing
    int64_t m_probeDuration{1000};  // 2000 milliseconds
    int64_t m_probeSampleDur{0};
    atomic_bool m_seeking{false};
    int64_t m_seekPos{INT64_MIN};
    int64_t m_prevSeekPos{INT64_MIN};

    AudioImMatAVFrameConverter* m_matAvfrmCvter{nullptr};
    list<vector<CorrelativeFrame>> m_outputMats;
    mutex m_outputMatsLock;
    uint32_t m_outputMatsMaxCount{4};

    bool m_configured{false};
    bool m_started{false};
    bool m_quit{false};

    AVFilterGraph* m_filterGraph{nullptr};
    AVFilterInOut* m_filterOutputs{nullptr};
    AVFilterInOut* m_filterInputs{nullptr};
    vector<AVFilterContext*> m_bufSrcCtxs;
    vector<AVFilterContext*> m_bufSinkCtxs;

    AudioEffectFilter::Holder m_aeFilter;
};

static const auto MULTI_TRACK_AUDIO_READER_DELETER = [] (MultiTrackAudioReader* p) {
    MultiTrackAudioReader_Impl* ptr = dynamic_cast<MultiTrackAudioReader_Impl*>(p);
    ptr->Close();
    delete ptr;
};

MultiTrackAudioReader::Holder MultiTrackAudioReader::CreateInstance()
{
    return MultiTrackAudioReader::Holder(new MultiTrackAudioReader_Impl(), MULTI_TRACK_AUDIO_READER_DELETER);
}

MultiTrackAudioReader::Holder MultiTrackAudioReader_Impl::CloneAndConfigure(uint32_t outChannels, uint32_t outSampleRate, uint32_t outSamplesPerFrame)
{
    lock_guard<recursive_mutex> lk(m_apiLock);
    MultiTrackAudioReader_Impl* newInstance = new MultiTrackAudioReader_Impl();
    if (!newInstance->Configure(outChannels, outSampleRate, outSamplesPerFrame))
    {
        m_errMsg = newInstance->GetError();
        newInstance->Close(); delete newInstance;
        return nullptr;
    }

    lock_guard<recursive_mutex> lk2(m_trackLock);
    // clone all the tracks
    for (auto track : m_tracks)
    {
        newInstance->m_tracks.push_back(track->Clone(outChannels, outSampleRate, av_get_sample_fmt_name(m_trackOutSmpfmt)));
    }
    newInstance->UpdateDuration();
    // create mixer in the new instance
    if (!newInstance->CreateMixer())
    {
        m_errMsg = newInstance->GetError();
        newInstance->Close(); delete newInstance;
        return nullptr;
    }

    // seek to 0
    newInstance->m_outputMats.clear();
    newInstance->m_samplePos = 0;
    newInstance->m_readPos = 0;
    for (auto track : newInstance->m_tracks)
        track->SeekTo(0);

    // start new instance
    if (!newInstance->Start())
    {
        m_errMsg = newInstance->GetError();
        newInstance->Close(); delete newInstance;
        return nullptr;
    }
    return MultiTrackAudioReader::Holder(newInstance, MULTI_TRACK_AUDIO_READER_DELETER);
}

ALogger* MultiTrackAudioReader::GetLogger()
{
    return Logger::GetLogger("MTAReader");
}

ostream& operator<<(ostream& os, MultiTrackAudioReader::Holder hMtaReader)
{
    os << ">>> MultiTrackAudioReader :" << endl;
    auto trackIter = hMtaReader->TrackListBegin();
    while (trackIter != hMtaReader->TrackListEnd())
    {
        auto& track = *trackIter;
        os << "\t Track#" << track->Id() << " : " << track << endl;
        trackIter++;
    }
    os << "<<< [END]MultiTrackAudioReader";
    return os;
}
}
