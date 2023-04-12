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
#include <mutex>
#include <sstream>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <list>
#include "MediaReader.h"
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
}
#include "DebugHelper.h"

#define VIDEO_DECODE_PERFORMANCE_ANALYSIS 0
#define VIDEO_FRAME_CONVERSION_PERFORMANCE_ANALYSIS 0

using namespace std;
using namespace Logger;

namespace MediaCore
{
class VideoReader_Impl : public MediaReader
{
public:
    VideoReader_Impl(const string& loggerName = "")
    {
        if (loggerName.empty())
            m_logger = Logger::GetLogger("VReader");
        else
            m_logger = Logger::GetLogger(loggerName);
        int n;
        Level l = MediaReader::GetLogger()->GetShowLevels(n);
        m_logger->SetShowLevels(l, n);
    }

    virtual ~VideoReader_Impl() {}

    bool Open(const string& url) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (IsOpened())
            Close();

        MediaParser::Holder hParser = MediaParser::CreateInstance();
        if (!hParser->Open(url))
        {
            m_errMsg = hParser->GetError();
            return false;
        }

        if (!OpenMedia(hParser))
        {
            Close();
            return false;
        }
        m_hParser = hParser;
        m_close = false;
        m_opened = true;
        return true;
    }

    bool Open(MediaParser::Holder hParser) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!hParser || !hParser->IsOpened())
        {
            m_errMsg = "Argument 'hParser' is nullptr or not opened yet!";
            return false;
        }

        if (IsOpened())
            Close();

        if (!OpenMedia(hParser))
        {
            Close();
            return false;
        }
        m_hParser = hParser;
        m_close = false;
        m_opened = true;
        return true;
    }

    MediaParser::Holder GetMediaParser() const override
    {
        return m_hParser;
    }

    bool ConfigVideoReader(
            uint32_t outWidth, uint32_t outHeight,
            ImColorFormat outClrfmt, ImInterpolateMode rszInterp) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_opened)
        {
            m_errMsg = "This 'VideoReader' instance is NOT OPENED yet!";
            return false;
        }
        if (m_started)
        {
            m_errMsg = "This 'VideoReader' instance is ALREADY STARTED!";
            return false;
        }
        if (m_vidStmIdx < 0)
        {
            m_errMsg = "Can NOT configure this 'VideoReader' as video reader since no video stream is found!";
            return false;
        }

        auto vidStream = GetVideoStream();
        m_isImage = vidStream->isImage;

        if (!m_frmCvt.SetOutSize(outWidth, outHeight))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }
        if (!m_frmCvt.SetOutColorFormat(outClrfmt))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }
        if (!m_frmCvt.SetResizeInterpolateMode(rszInterp))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }

        m_vidDurTs = vidStream->duration;
        AVRational timebase = { vidStream->timebase.num, vidStream->timebase.den };
        AVRational frameRate;
        if (Ratio::IsValid(vidStream->avgFrameRate))
            frameRate = { vidStream->avgFrameRate.num, vidStream->avgFrameRate.den };
        else if (Ratio::IsValid(vidStream->realFrameRate))
            frameRate = { vidStream->realFrameRate.num, vidStream->realFrameRate.den };
        else
            frameRate = av_inv_q(timebase);
        m_vidfrmIntvMts = av_q2d(av_inv_q(frameRate))*1000.;

        m_configured = true;
        return true;
    }

    bool ConfigVideoReader(
            float outWidthFactor, float outHeightFactor,
            ImColorFormat outClrfmt, ImInterpolateMode rszInterp) override
    {
        if (!m_opened)
        {
            m_errMsg = "Can NOT configure a 'VideoReader' until it's been configured!";
            return false;
        }
        if (m_started)
        {
            m_errMsg = "Can NOT configure a 'VideoReader' after it's already started!";
            return false;
        }
        if (m_vidStmIdx < 0)
        {
            m_errMsg = "Can NOT configure this 'VideoReader' as video reader since no video stream is found!";
            return false;
        }
        lock_guard<recursive_mutex> lk(m_apiLock);

        auto vidStream = GetVideoStream();
        m_isImage = vidStream->isImage;

        m_ssWFacotr = outWidthFactor;
        m_ssHFacotr = outHeightFactor;
        uint32_t outWidth = (uint32_t)ceil(vidStream->width*outWidthFactor);
        if ((outWidth&0x1) == 1)
            outWidth++;
        uint32_t outHeight = (uint32_t)ceil(vidStream->height*outHeightFactor);
        if ((outHeight&0x1) == 1)
            outHeight++;
        if (!m_frmCvt.SetOutSize(outWidth, outHeight))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }
        if (!m_frmCvt.SetOutColorFormat(outClrfmt))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }
        if (!m_frmCvt.SetResizeInterpolateMode(rszInterp))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }

        m_vidDurTs = vidStream->duration;
        AVRational timebase = { vidStream->timebase.num, vidStream->timebase.den };
        AVRational frameRate;
        if (Ratio::IsValid(vidStream->avgFrameRate))
            frameRate = { vidStream->avgFrameRate.num, vidStream->avgFrameRate.den };
        else if (Ratio::IsValid(vidStream->realFrameRate))
            frameRate = { vidStream->realFrameRate.num, vidStream->realFrameRate.den };
        else
            frameRate = av_inv_q(timebase);
        m_vidfrmIntvMts = av_q2d(av_inv_q(frameRate))*1000.;

        m_configured = true;
        return true;
    }

    bool ConfigAudioReader(uint32_t outChannels, uint32_t outSampleRate, const string& outPcmFormat, uint32_t audioStreamIndex) override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method ConfigAudioReader()!");
    }

    bool Start(bool suspend) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_configured)
        {
            m_errMsg = "This 'VideoReader' instance is NOT CONFIGURED yet!";
            return false;
        }
        if (m_started)
            return true;

        if (!suspend)
            StartAllThreads();
        else
            ReleaseVideoResource();
        m_started = true;
        return true;
    }

    bool Stop() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_configured)
        {
            m_errMsg = "This 'VideoReader' instance is NOT CONFIGURED yet!";
            return false;
        }
        if (!m_started)
            return true;

        WaitAllThreadsQuit();
        FlushAllQueues();

        if (m_viddecCtx)
        {
            avcodec_free_context(&m_viddecCtx);
            m_viddecCtx = nullptr;
        }
        m_vidAvStm = nullptr;
        m_dmxEof = false;
        m_readPos = 0;
        m_prevReadResult.first = 0;
        m_prevReadResult.second.release();
        m_readForward = true;
        m_seekPosUpdated = false;
        m_seekPosTs = 0;
        m_vidfrmIntvMts = 0;
        m_vidDurTs = 0;

        m_prepared = false;
        m_started = false;
        m_configured = false;
        m_errMsg = "";
        return true;
    }

    void Close() override
    {
        m_close = true;
        lock_guard<recursive_mutex> lk(m_apiLock);
        WaitAllThreadsQuit();
        FlushAllQueues();

        if (m_viddecCtx)
        {
            avcodec_free_context(&m_viddecCtx);
            m_viddecCtx = nullptr;
        }
        if (m_avfmtCtx)
        {
            avformat_close_input(&m_avfmtCtx);
            m_avfmtCtx = nullptr;
        }
        m_vidStmIdx = -1;
        m_vidAvStm = nullptr;
        m_hParser = nullptr;
        m_hMediaInfo = nullptr;
        m_dmxEof = false;
        m_readPos = 0;
        m_prevReadResult.first = 0;
        m_prevReadResult.second.release();
        m_readForward = true;
        m_seekPosUpdated = false;
        m_seekPosTs = 0;
        m_vidfrmIntvMts = 0;
        m_vidDurTs = 0;

        m_prepared = false;
        m_started = false;
        m_configured = false;
        m_streamInfoFound = false;
        m_opened = false;
        m_errMsg = "";
    }

    bool SeekTo(double ts) override
    {
        if (!m_configured)
        {
            m_errMsg = "Can NOT use 'SeekTo' until the 'VideoReader' obj is configured!";
            return false;
        }
        if (ts < 0 || ts > m_vidDurTs)
        {
            m_errMsg = "INVALID argument 'ts'! Can NOT be negative or exceed the duration.";
            return false;
        }

        m_logger->Log(DEBUG) << "--> Seek[0]: Set seek pos " << ts << endl;
        lock_guard<mutex> lk(m_seekPosLock);
        m_seekPosTs = ts;
        m_seekPosUpdated = true;
        return true;
    }

    void SetDirection(bool forward) override
    {
        if (m_readForward == forward)
            return;
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_opened)
        {
            m_errMsg = "This 'VideoReader' instance is NOT OPENED yet!";
            return;
        }
        if (m_readForward != forward)
        {
            m_readForward = forward;
        }
    }

    void Suspend() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This 'VideoReader' is NOT started yet!";
            return;
        }
        if (m_quitThread || m_isImage)
            return;

        ReleaseVideoResource();
    }

    void Wakeup() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!m_started)
        {
            m_errMsg = "This 'VideoReader' is NOT started yet!";
            return;
        }
        if (!m_quitThread || m_isImage)
            return;

        double readPos = m_seekPosUpdated ? m_seekPosTs : m_prevReadResult.first;
        if (!OpenMedia(m_hParser))
        {
            m_logger->Log(Error) << "FAILED to re-open media when waking up this MediaReader!" << endl;
            return;
        }
        m_seekPosTs = readPos;
        m_seekPosUpdated = true;

        StartAllThreads();
    }

    bool IsSuspended() const override
    {
        return m_started && m_quitThread;
    }

    bool IsPlanar() const override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method ReadAudioSamples()!");
    }

    bool IsDirectionForward() const override
    {
        return m_readForward;
    }

    bool ReadVideoFrame(double pos, ImGui::ImMat& m, bool& eof, bool wait) override
    {
        m.release();
        if (!m_started)
        {
            m_errMsg = "This 'VideoReader' instance is NOT STARTED yet!";
            return false;
        }
        if (pos < 0 || (!m_isImage && pos >= m_vidDurTs))
        {
            m_errMsg = "Invalid argument! 'pos' can NOT be negative or larger than video's duration.";
            eof = true;
            return false;
        }
        if (!wait && !m_prepared)
        {
            eof = false;
            return true;
        }
        while (!m_quitThread && !m_prepared && wait)
            this_thread::sleep_for(chrono::milliseconds(5));
        if (m_close || !m_prepared)
        {
            m_errMsg = "This 'VideoReader' instance is NOT READY to read!";
            return false;
        }

        lock_guard<recursive_mutex> lk(m_apiLock);
        eof = false;
        if (!m_prevReadResult.second.empty() && pos == m_prevReadResult.first)
        {
            m = m_prevReadResult.second;
            return true;
        }
        if (IsSuspended() && !m_isImage)
        {
            m_errMsg = "This 'VideoReader' instance is SUSPENDED!";
            return false;
        }

        int64_t pts = CvtMtsToPts(pos*1000);
        if (m_readForward && pts > m_readPos || !m_readForward && pts < m_readPos)
            UpdateReadPos(pts);
        m_logger->Log(VERBOSE) << ">> TO READ frame: pts=" << pts << ", ts=" << pos << "." << endl;

        VideoFrame::Holder hVfrm;
        while (!m_quitThread)
        {
            if (pts < m_cacheRange.first || pts > m_cacheRange.second)
                break;
            if (!m_inSeeking)
            {
                lock_guard<mutex> _lk(m_vfrmQLock);
                auto iter = m_vfrmQ.end();
                iter = find_if(m_vfrmQ.begin(), m_vfrmQ.end(), [pts] (auto& vf) {
                    return vf->pts > pts;
                });
                if (iter != m_vfrmQ.end())
                {
                    if (iter != m_vfrmQ.begin())
                        hVfrm = *(--iter);
                    else if (pts >= (*iter)->pts && pts <= (*iter)->pts+(*iter)->dur)
                        hVfrm = *iter;
                    break;
                }
                else if (!m_readForward)
                {
                    break;
                }
            }
            if (!wait)
                break;
            this_thread::sleep_for(chrono::milliseconds(5));
        }
        if (!hVfrm)
        {
            return false;
        }
        if (hVfrm->pts == INT64_MAX)
        {
            eof = true;
            return true;
        }

        if (wait && hVfrm->vmat.empty())
        {
            bool inFrmQ;
            do {
                this_thread::sleep_for(chrono::milliseconds(2));
                {
                    lock_guard<mutex> _lk(m_vfrmQLock);
                    auto iter = find(m_vfrmQ.begin(), m_vfrmQ.end(), hVfrm);
                    inFrmQ = iter != m_vfrmQ.end();
                }
            } while (hVfrm->vmat.empty() && inFrmQ && !m_quitThread);
        }
        if (hVfrm->vmat.empty())
        {
            return false;
        }

        m = hVfrm->vmat;
        m_prevReadResult = {pos, m};
        return true;
    }

    bool ReadAudioSamples(uint8_t* buf, uint32_t& size, double& pos, bool& eof, bool wait) override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method ReadAudioSamples()!");
    }

    bool ReadAudioSamples(ImGui::ImMat& m, uint32_t readSamples, bool& eof, bool wait) override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method ReadAudioSamples()!");
    }

    bool IsOpened() const override
    {
        return m_opened;
    }

    bool IsStarted() const override
    {
        return m_started;
    }

    bool IsVideoReader() const override
    {
        return true;
    }

    bool SetCacheDuration(double forwardDur, double backwardDur) override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method SetCacheDuration()!");
    }

    pair<double, double> GetCacheDuration() const override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method GetCacheDuration()!");
    }

    MediaInfo::Holder GetMediaInfo() const override
    {
        return m_hMediaInfo;
    }

    const VideoStream* GetVideoStream() const override
    {
        MediaInfo::Holder hInfo = m_hMediaInfo;
        if (!hInfo || m_vidStmIdx < 0)
            return nullptr;
        return dynamic_cast<VideoStream*>(hInfo->streams[m_vidStmIdx].get());
    }

    const AudioStream* GetAudioStream() const override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method GetAudioStream()!");
    }

    uint32_t GetVideoOutWidth() const override
    {
        const VideoStream* vidStream = GetVideoStream();
        if (!vidStream)
            return 0;
        uint32_t w = m_frmCvt.GetOutWidth();
        if (w > 0)
            return w;
        w = vidStream->width;
        return w;
    }

    uint32_t GetVideoOutHeight() const override
    {
        const VideoStream* vidStream = GetVideoStream();
        if (!vidStream)
            return 0;
        uint32_t h = m_frmCvt.GetOutHeight();
        if (h > 0)
            return h;
        h = vidStream->height;
        return h;
    }

    string GetAudioOutPcmFormat() const override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method GetAudioOutPcmFormat()!");
    }

    uint32_t GetAudioOutChannels() const override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method GetAudioOutChannels()!");
    }

    uint32_t GetAudioOutSampleRate() const override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method GetAudioOutSampleRate()!");
    }

    uint32_t GetAudioOutFrameSize() const override
    {
        throw runtime_error("VideoReader does NOT SUPPORT method GetAudioOutFrameSize()!");
    }

    bool IsHwAccelEnabled() const override
    {
        return m_vidPreferUseHw;
    }

    void EnableHwAccel(bool enable) override
    {
        m_vidPreferUseHw = enable;
    }

    void SetLogLevel(Logger::Level l) override
    {
        m_logger->SetShowLevels(l);
    }

    string GetError() const override
    {
        return m_errMsg;
    }

private:
    string FFapiFailureMessage(const string& apiName, int fferr)
    {
        ostringstream oss;
        oss << "FF api '" << apiName << "' returns error! fferr=" << fferr << ".";
        return oss.str();
    }

    int64_t CvtPtsToMts(int64_t pts)
    {
        return av_rescale_q_rnd(pts-m_vidStartTime, m_vidTimeBase, MILLISEC_TIMEBASE, AV_ROUND_DOWN);
    }

    int64_t CvtMtsToPts(int64_t mts)
    {
        return av_rescale_q_rnd(mts, MILLISEC_TIMEBASE, m_vidTimeBase, AV_ROUND_DOWN)+m_vidStartTime;
    }

    bool OpenMedia(MediaParser::Holder hParser)
    {
        // create new logger based on opened media name
        auto fileName = SysUtils::ExtractFileName(hParser->GetUrl());
        ostringstream loggerNameOss;
        loggerNameOss << "Vreader-" << fileName.substr(0, 8);
        int n;
        Level l = m_logger->GetShowLevels(n);
        auto newLoggerName = loggerNameOss.str();
        m_logger = Logger::GetLogger(newLoggerName);
        m_logger->SetShowLevels(l, n);

        // open media
        int fferr = avformat_open_input(&m_avfmtCtx, hParser->GetUrl().c_str(), nullptr, nullptr);
        if (fferr < 0)
        {
            m_avfmtCtx = nullptr;
            m_errMsg = FFapiFailureMessage("avformat_open_input", fferr);
            return false;
        }

        m_hMediaInfo = hParser->GetMediaInfo();
        m_vidStmIdx = hParser->GetBestVideoStreamIndex();
        if (m_vidStmIdx < 0)
        {
            ostringstream oss;
            oss << "No VIDEO stream can be found in '" << m_avfmtCtx->url << "'.";
            m_errMsg = oss.str();
            return false;
        }
        return true;
    }

    void ReleaseVideoResource()
    {
        WaitAllThreadsQuit();
        FlushAllQueues();

        if (m_viddecCtx)
        {
            avcodec_free_context(&m_viddecCtx);
            m_viddecCtx = nullptr;
        }
        if (m_avfmtCtx)
        {
            avformat_close_input(&m_avfmtCtx);
            m_avfmtCtx = nullptr;
        }
        m_vidAvStm = nullptr;

        m_prepared = false;
    }

    bool Prepare()
    {
        bool locked = false;
        do {
            locked = m_apiLock.try_lock();
            if (!locked)
                this_thread::sleep_for(chrono::milliseconds(5));
        } while (!locked && !m_quitThread);
        if (m_quitThread)
        {
            m_logger->Log(WARN) << "Abort 'Prepare' procedure! 'm_quitThread' is set!" << endl;
            return false;
        }

        lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
        int fferr;
        if (!m_streamInfoFound)
        {
            fferr = avformat_find_stream_info(m_avfmtCtx, nullptr);
            if (fferr < 0)
            {
                m_errMsg = FFapiFailureMessage("avformat_find_stream_info", fferr);
                return false;
            }
            m_streamInfoFound = true;
        }

        m_vidAvStm = m_avfmtCtx->streams[m_vidStmIdx];
        m_vidStartTime = m_vidAvStm->start_time != AV_NOPTS_VALUE ? m_vidAvStm->start_time : 0;
        m_vidTimeBase = m_vidAvStm->time_base;
        m_vidfrmIntvPts = av_rescale_q(1, av_inv_q(m_vidAvStm->r_frame_rate), m_vidAvStm->time_base);

        m_viddecOpenOpts.onlyUseSoftwareDecoder = !m_vidPreferUseHw;
        m_viddecOpenOpts.useHardwareType = m_vidUseHwType;
        FFUtils::OpenVideoDecoderResult res;
        if (FFUtils::OpenVideoDecoder(m_avfmtCtx, -1, &m_viddecOpenOpts, &res))
        {
            m_viddecCtx = res.decCtx;
            AVHWDeviceType hwDevType = res.hwDevType;
            m_logger->Log(INFO) << "Opened video decoder '" << 
                m_viddecCtx->codec->name << "'(" << (hwDevType==AV_HWDEVICE_TYPE_NONE ? "SW" : av_hwdevice_get_type_name(hwDevType)) << ")"
                << " for media '" << m_hParser->GetUrl() << "'." << endl;
        }
        else
        {
            ostringstream oss;
            oss << "Open video decoder FAILED! Error is '" << res.errMsg << "'.";
            m_errMsg = oss.str();
            return false;
        }

        UpdateReadPos(m_vidStartTime);
        m_prepared = true;
        return true;
    }

    void StartAllThreads()
    {
        string fileName = SysUtils::ExtractFileName(m_hParser->GetUrl());
        ostringstream thnOss;
        m_quitThread = false;
        m_dmxThdRunning = true;
        m_demuxThread = thread(&VideoReader_Impl::DemuxThreadProc, this);
        thnOss << "VrdrDmx-" << fileName;
        SysUtils::SetThreadName(m_demuxThread, thnOss.str());
        m_decThdRunning = true;
        m_decodeThread = thread(&VideoReader_Impl::DecodeThreadProc, this);
        thnOss.str(""); thnOss << "VrdrDec-" << fileName;
        SysUtils::SetThreadName(m_decodeThread, thnOss.str());
        m_cnvThdRunning = true;
        m_cnvMatThread = thread(&VideoReader_Impl::ConvertMatThreadProc, this);
        thnOss.str(""); thnOss << "VrdrCmt-" << fileName;
        SysUtils::SetThreadName(m_cnvMatThread, thnOss.str());
    }

    void WaitAllThreadsQuit(bool callFromReleaseProc = false)
    {
        m_quitThread = true;
        if (m_demuxThread.joinable())
        {
            m_demuxThread.join();
            m_demuxThread = thread();
        }
        if (m_decodeThread.joinable())
        {
            m_decodeThread.join();
            m_decodeThread = thread();
        }
        if (m_cnvMatThread.joinable())
        {
            m_cnvMatThread.join();
            m_cnvMatThread = thread();
        }
    }

    void FlushAllQueues()
    {
        m_vpktQ.clear();
        m_vfrmQ.clear();
    }

    struct VideoPacket
    {
        using Holder = shared_ptr<VideoPacket>;
        SelfFreeAVPacketPtr pktPtr;
        bool isAfterSeek{false};
        bool needFlushVfrmQ{false};
    };

    struct VideoFrame
    {
        using Holder = shared_ptr<VideoFrame>;
        SelfFreeAVFramePtr frmPtr;
        ImGui::ImMat vmat;
        double ts;
        int64_t pts;
        int64_t dur{0};
    };

    void UpdateReadPos(int64_t readPts)
    {
        lock_guard<mutex> _lk(m_cacheRangeLock);
        m_readPos = readPts;
        auto& cacheFrameCount = m_readForward ? m_forwardCacheFrameCount : m_backwardCacheFrameCount;
        m_cacheRange.first = readPts-cacheFrameCount.first*m_vidfrmIntvPts;
        m_cacheRange.second = readPts+cacheFrameCount.second*m_vidfrmIntvPts;
        if (m_vidfrmIntvPts > 1)
        {
            m_cacheRange.first--;
            m_cacheRange.second++;
        }
    }

    void DemuxThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter DemuxThreadProc()..." << endl;

        if (!m_prepared && !Prepare())
        {
            m_logger->Log(Error) << "Prepare() FAILED! Error is '" << m_errMsg << "'." << endl;
            return;
        }

        int fferr;
        bool needSeek = false;
        bool needFlushVfrmQ = false;
        bool afterSeek = false;
        bool readForward = m_readForward;
        int64_t lastPktPts = INT64_MIN, minPtsAfterSeek = INT64_MAX;
        int64_t backwardReadLimitPts;
        int64_t seekPts;
        while (!m_quitThread)
        {
            bool idleLoop = true;
            bool directionChanged = readForward != m_readForward;
            readForward = m_readForward;
            if (directionChanged)
            {
                UpdateReadPos(m_readPos);
                needSeek = true;
                if (readForward)
                {
                    seekPts = m_readPos;
                }
                else
                {
                    lock_guard<mutex> _lk(m_vfrmQLock);
                    auto iter = m_vfrmQ.begin();
                    bool firstGreaterPts = true;
                    while (iter != m_vfrmQ.end())
                    {
                        bool remove = false;
                        if ((*iter)->pts < m_cacheRange.first)
                            remove = true;
                        else if ((*iter)->pts > m_cacheRange.second)
                        {
                            if (firstGreaterPts)
                                firstGreaterPts = false;
                            else
                                remove = true;
                        }
                        if (remove)
                            iter = m_vfrmQ.erase(iter);
                        else
                            iter++;
                    }
                    if (m_vfrmQ.empty())
                        backwardReadLimitPts = m_readPos;
                    else
                    {
                        auto& vf = m_vfrmQ.front();
                        backwardReadLimitPts = vf->pts > m_readPos ? m_readPos : vf->pts-1;
                    }
                    seekPts = backwardReadLimitPts;
                }
            }

            bool seekOpTriggered = false;
            // handle seek operation
            {
                lock_guard<mutex> _lk(m_seekPosLock);
                if (m_seekPosUpdated)
                {
                    seekOpTriggered = true;
                    m_inSeeking = needSeek = needFlushVfrmQ = true;
                    seekPts = CvtMtsToPts(m_seekPosTs*1000);
                    m_seekPosUpdated = false;
                }
            }
            if (seekOpTriggered)
            {
                UpdateReadPos(seekPts);
            }
            if (needSeek)
            {
                needSeek = false;
                // clear avpacket queue
                {
                    lock_guard<mutex> _lk(m_vpktQLock);
                    m_vpktQ.clear();
                }
                // seek to the new position
                m_logger->Log(DEBUG) << "--> Seek[1]: Demux seek to " << seekPts << "(" << seekPts << ")." << endl;
                fferr = avformat_seek_file(m_avfmtCtx, m_vidStmIdx, INT64_MIN, seekPts, seekPts, 0);
                if (fferr < 0)
                {
                    double seekTs = (double)CvtPtsToMts(seekPts)/1000;
                    m_logger->Log(WARN) << "avformat_seek_file() FAILED to seek to time " << seekTs << "(" << seekPts << ")! fferr=" << fferr << "." << endl;
                }
                lastPktPts = INT64_MIN;
                minPtsAfterSeek = INT64_MAX;
                m_dmxEof = false;
                afterSeek = true;
            }

            // check read packet condition
            bool doReadPacket;
            if (m_readForward)
            {
                doReadPacket = m_vpktQ.size() < m_vpktQMaxSize;
            }
            else
            {
                doReadPacket = lastPktPts < backwardReadLimitPts;
                if (!doReadPacket && minPtsAfterSeek >= m_cacheRange.first && minPtsAfterSeek > m_vidStartTime)
                {
                    seekPts = backwardReadLimitPts = minPtsAfterSeek-1;
                    needSeek = true;
                    idleLoop = false;
                }
            }

            // read avpacket
            if (!m_dmxEof && doReadPacket)
            {
                SelfFreeAVPacketPtr pktPtr = AllocSelfFreeAVPacketPtr();
                fferr = av_read_frame(m_avfmtCtx, pktPtr.get());
                if (fferr == 0)
                {
                    if (pktPtr->stream_index == m_vidStmIdx)
                    {
                        m_logger->Log(VERBOSE) << "=== Get video packet: pts=" << pktPtr->pts << ", ts=" << (double)CvtPtsToMts(pktPtr->pts)/1000 << "." << endl;
                        if (pktPtr->pts < minPtsAfterSeek) minPtsAfterSeek = pktPtr->pts;
                        VideoPacket::Holder hVpkt(new VideoPacket({pktPtr, afterSeek, needFlushVfrmQ}));
                        afterSeek = needFlushVfrmQ = false;
                        lastPktPts = pktPtr->pts;
                        lock_guard<mutex> _lk(m_vpktQLock);
                        m_vpktQ.push_back(hVpkt);
                    }
                    idleLoop = false;
                }
                else if (fferr == AVERROR_EOF)
                {
                    m_dmxEof = true;
                    {
                        VideoPacket::Holder hVpkt(new VideoPacket({nullptr, afterSeek, needFlushVfrmQ}));
                        afterSeek = needFlushVfrmQ = false;
                        lastPktPts = INT64_MAX;
                        lock_guard<mutex> _lk(m_vpktQLock);
                        m_vpktQ.push_back(hVpkt);
                    }
                }
                else
                {
                    m_logger->Log(WARN) << "av_read_frame() FAILED! fferr=" << fferr << "." << endl;
                }
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        m_dmxThdRunning = false;
        m_logger->Log(DEBUG) << "Leave DemuxThreadProc()." << endl;
    }

    static const VideoFrame::Holder _EOF_HVFRM;

    void DecodeThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter DecodeThreadProc()..." << endl;
        while (!m_prepared && !m_quitThread)
            this_thread::sleep_for(chrono::milliseconds(5));

        int fferr;
        int64_t prevFrmPts = INT64_MIN;
        bool decoderEof = false;
        while (!m_quitThread)
        {
            bool idleLoop = true;

            // retrieve avpacket and reset decoder if needed
            VideoPacket::Holder hVpkt;
            {
                lock_guard<mutex> _lk(m_vpktQLock);
                if (!m_vpktQ.empty())
                    hVpkt = m_vpktQ.front();
            }
            if (hVpkt && hVpkt->isAfterSeek)
            {
                m_logger->Log(DEBUG) << "--> Seek[2]: Decoder reset and flush vframe queue. pts=" << hVpkt->pktPtr->pts << "." << endl;
                prevFrmPts = INT64_MIN;
                if (hVpkt->pktPtr)
                {
                    avcodec_flush_buffers(m_viddecCtx);
                    decoderEof = false;
                    if (hVpkt->needFlushVfrmQ)
                    {
                        lock_guard<mutex> _lk(m_vfrmQLock);
                        m_vfrmQ.clear();
                    }
                }
                else
                {
                    decoderEof = true;
                    {
                        lock_guard<mutex> _lk(m_vfrmQLock);
                        if (hVpkt->needFlushVfrmQ)
                            m_vfrmQ.clear();
                        m_vfrmQ.push_back(_EOF_HVFRM);
                    }
                }
                m_inSeeking = false;
            }

            // retrieve decoded frame
            bool doDecode = !decoderEof && m_pendingVidfrmCnt < m_maxPendingVidfrmCnt
                    && (prevFrmPts < m_cacheRange.second || !m_readForward);
            if (doDecode)
            {
                AVFrame* pAvfrm = av_frame_alloc();
                fferr = avcodec_receive_frame(m_viddecCtx, pAvfrm);
                if (fferr == 0)
                {
                    m_logger->Log(VERBOSE) << "======= Get video frame: pts=" << pAvfrm->pts << ", ts=" << (double)CvtPtsToMts(pAvfrm->pts)/1000 << "." << endl;
                    SelfFreeAVFramePtr frmPtr(pAvfrm, [this] (AVFrame* p) {
                        av_frame_free(&p);
                        m_pendingVidfrmCnt--;
                    });
                    m_pendingVidfrmCnt++;
                    const int64_t pts = prevFrmPts = pAvfrm->pts;
#if LIBAVUTIL_VERSION_MAJOR > 56
                    const int64_t dur = pAvfrm->duration;
#else
                    const int64_t dur = pAvfrm->pkt_duration;
#endif
                    pAvfrm = nullptr;
                    VideoFrame::Holder hVfrm(new VideoFrame({frmPtr, ImGui::ImMat(), (double)CvtPtsToMts(pts)/1000, pts, dur}));
                    lock_guard<mutex> _lk(m_vfrmQLock);
                    auto riter = find_if(m_vfrmQ.rbegin(), m_vfrmQ.rend(), [pts] (auto& vf) {
                        return vf->pts < pts;
                    });
                    auto iter = riter.base();
                    if (iter != m_vfrmQ.end() && (*iter)->pts == pts)
                        m_logger->Log(DEBUG) << "DISCARD duplicated VF@" << hVfrm->ts << "(" << hVfrm->pts << ")." << endl;
                    else
                        m_vfrmQ.insert(iter, hVfrm);
                    idleLoop = false;
                }
                else if (fferr == AVERROR_EOF)
                {
                    decoderEof = true;
                    lock_guard<mutex> _lk(m_vfrmQLock);
                    if (m_vfrmQ.empty() || m_vfrmQ.back()->pts != INT64_MAX)
                        m_vfrmQ.push_back(_EOF_HVFRM);
                }
                else if (fferr != AVERROR(EAGAIN))
                {
                    m_logger->Log(WARN) << "avcodec_receive_frame() FAILED! fferr=" << fferr << "." << endl;
                }
                if (pAvfrm) av_frame_free(&pAvfrm);
            }

            // send avpacket data to the decoder
            if (hVpkt)
            {
                fferr = avcodec_send_packet(m_viddecCtx, hVpkt->pktPtr ? hVpkt->pktPtr.get() : nullptr);
                bool popPkt = false;
                if (fferr == 0)
                {
                    popPkt = true;
                    idleLoop = false;
                }
                else if (fferr != AVERROR(EAGAIN))
                {
                    m_logger->Log(WARN) << "avcodec_send_packet() FAILED! fferr=" << fferr << "." << endl;
                    popPkt = true;
                    idleLoop = false;
                }
                if (popPkt)
                {
                    lock_guard<mutex> _lk(m_vpktQLock);
                    if (!m_vpktQ.empty() && hVpkt == m_vpktQ.front())
                        m_vpktQ.pop_front();
                }
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        m_decThdRunning = false;
        m_logger->Log(DEBUG) << "Leave DecodeThreadProc()." << endl;
    }

    void ConvertMatThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter ConvertMatThreadProc()..." << endl;
        while (!m_prepared && !m_quitThread)
            this_thread::sleep_for(chrono::milliseconds(5));

        while (!m_quitThread)
        {
            bool idleLoop = true;

            // remove unused frames and find the next frame needed to do the conversion
            VideoFrame::Holder hVfrm;
            {
                lock_guard<mutex> _lk(m_vfrmQLock);
                auto iter = m_vfrmQ.begin();
                bool firstGreaterPts = true;
                while (iter != m_vfrmQ.end())
                {
                    auto vf = *iter;
                    bool remove = false;
                    if (m_readForward && vf->pts < m_cacheRange.first)
                        remove = true;
                    else if (vf->pts > m_cacheRange.second && vf->pts != INT64_MAX)
                    {
                        if (firstGreaterPts)
                            firstGreaterPts = false;
                        else
                            remove = true;
                    }
                    if (remove)
                    {
                        m_logger->Log(VERBOSE) << "             --------- Remove video frame: pts=" << (*iter)->pts << ", ts=" << (*iter)->ts << "." << endl;
                        iter = m_vfrmQ.erase(iter);
                        continue;
                    }
                    if (!hVfrm && vf->pts != INT64_MAX && vf->vmat.empty())
                        hVfrm = vf;
                    iter++;
                }
            }

            // convert avframe to mat
            if (hVfrm)
            {
                if (!m_frmCvt.ConvertImage(hVfrm->frmPtr.get(), hVfrm->vmat, hVfrm->ts))
                {
                    m_logger->Log(Error) << "AVFrameToImMatConverter::ConvertImage() FAILED at pos " << hVfrm->ts << "(" << hVfrm->pts << ")! Discard this frame." << endl;
                    lock_guard<mutex> _lk(m_vfrmQLock);
                    auto iter = find(m_vfrmQ.begin(), m_vfrmQ.end(), hVfrm);
                    if (iter != m_vfrmQ.end()) m_vfrmQ.erase(iter);
                }
                hVfrm->frmPtr = nullptr;
                idleLoop = false;
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        m_cnvThdRunning = false;
        m_logger->Log(DEBUG) << "Leave ConvertMatThreadProc()." << endl;
    }

private:
    ALogger* m_logger;
    string m_errMsg;

    MediaParser::Holder m_hParser;
    MediaInfo::Holder m_hMediaInfo;
    bool m_opened{false};
    bool m_configured{false};
    bool m_streamInfoFound{false};
    bool m_isImage{false};
    bool m_started{false};
    bool m_prepared{false};
    bool m_close{false};
    bool m_quitThread{false};
    recursive_mutex m_apiLock;

    AVFormatContext* m_avfmtCtx{nullptr};
    int m_vidStmIdx{-1};
    AVStream* m_vidAvStm{nullptr};
    FFUtils::OpenVideoDecoderOptions m_viddecOpenOpts;
    AVCodecContext* m_viddecCtx{nullptr};
    bool m_vidPreferUseHw{true};
    AVHWDeviceType m_vidUseHwType{AV_HWDEVICE_TYPE_NONE};
    int64_t m_vidStartTime{0};
    AVRational m_vidTimeBase;

    // demuxing thread
    thread m_demuxThread;
    bool m_dmxThdRunning{false};
    list<VideoPacket::Holder> m_vpktQ;
    mutex m_vpktQLock;
    size_t m_vpktQMaxSize{8};
    bool m_dmxEof{false};
    // video decoding thread
    thread m_decodeThread;
    bool m_decThdRunning{false};
    list<VideoFrame::Holder> m_vfrmQ;
    mutex m_vfrmQLock;
    atomic_int32_t m_pendingVidfrmCnt{0};
    int32_t m_maxPendingVidfrmCnt{3};
    // convert avframe to mat thread
    thread m_cnvMatThread;
    bool m_cnvThdRunning{false};

    int64_t m_readPos{0};
    pair<int64_t, int64_t> m_cacheRange;
    pair<int32_t, int32_t> m_forwardCacheFrameCount{1, 3};
    pair<int32_t, int32_t> m_backwardCacheFrameCount{8, 1};
    mutex m_cacheRangeLock;
    pair<double, ImGui::ImMat> m_prevReadResult{0, ImGui::ImMat()};
    bool m_readForward{true};
    bool m_seekPosUpdated{false};
    double m_seekPosTs{0};
    bool m_inSeeking{false};
    mutex m_seekPosLock;
    double m_vidfrmIntvMts{0};
    int64_t m_vidfrmIntvPts{0};
    double m_vidDurTs{0};

    float m_ssWFacotr{1.f}, m_ssHFacotr{1.f};
    AVFrameToImMatConverter m_frmCvt;
};

const VideoReader_Impl::VideoFrame::Holder VideoReader_Impl::_EOF_HVFRM(new VideoReader_Impl::VideoFrame({nullptr, ImGui::ImMat(), -1, INT64_MAX}));

static const auto VIDEO_READER_HOLDER_DELETER = [] (MediaReader* p) {
    VideoReader_Impl* ptr = dynamic_cast<VideoReader_Impl*>(p);
    ptr->Close();
    delete ptr;
};

MediaReader::Holder MediaReader::CreateVideoInstance(const string& loggerName)
{
    return MediaReader::Holder(new VideoReader_Impl(loggerName), VIDEO_READER_HOLDER_DELETER);
}
}
