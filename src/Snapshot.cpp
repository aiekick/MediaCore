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
#include <iostream>
#include <sstream>
#include <iomanip>
#include <list>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <cmath>
#include <algorithm>
#include "imgui_helper.h"
#include "Snapshot.h"
#include "FFUtils.h"
#include "SysUtils.h"
#include "DebugHelper.h"
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

static AVPixelFormat get_hw_format(AVCodecContext *ctx, const AVPixelFormat *pix_fmts);

namespace MediaCore
{
namespace Snapshot
{
class Generator_Impl : public Snapshot::Generator
{
public:
    Generator_Impl()
    {
        m_logger = Snapshot::GetLogger();
    }

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
        hParser->EnableParseInfo(MediaParser::VIDEO_SEEK_POINTS);

        if (!OpenMedia(hParser))
        {
            Close();
            return false;
        }
        m_hParser = hParser;

        m_opened = true;
        m_logger->Log(INFO) << "Create SnapshotGenerator for file '" << hParser->GetUrl() << "'. Output image resolution=" <<
            m_frmCvt.GetOutWidth() << "x" << m_frmCvt.GetOutHeight() << "." << endl;
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
        hParser->EnableParseInfo(MediaParser::VIDEO_SEEK_POINTS);

        if (IsOpened())
            Close();

        if (!OpenMedia(hParser))
        {
            Close();
            return false;
        }
        m_hParser = hParser;

        m_opened = true;
        m_logger->Log(INFO) << "Create SnapshotGenerator for file '" << hParser->GetUrl() << "'. Output image resolution=" <<
            m_frmCvt.GetOutWidth() << "x" << m_frmCvt.GetOutHeight() << "." << endl;
        return true;
    }

    void Close() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        WaitAllThreadsQuit();
        FlushAllQueues();

        m_deprecatedTextures.clear();

        if (m_viddecCtx)
        {
            avcodec_free_context(&m_viddecCtx);
            m_viddecCtx = nullptr;
        }
        if (m_viddecHwDevCtx)
        {
            av_buffer_unref(&m_viddecHwDevCtx);
            m_viddecHwDevCtx = nullptr;
        }
        m_vidHwPixFmt = AV_PIX_FMT_NONE;
        m_viddecDevType = AV_HWDEVICE_TYPE_NONE;
        if (m_avfmtCtx)
        {
            avformat_close_input(&m_avfmtCtx);
            m_avfmtCtx = nullptr;
        }
        m_vidStmIdx = -1;
        m_audStmIdx = -1;
        m_vidStream = nullptr;
        m_audStream = nullptr;
        m_viddec = nullptr;
        m_hParser = nullptr;
        m_hMediaInfo = nullptr;

        m_vidStartMts = 0;
        m_vidStartPts = 0;
        m_vidDurMts = 0;
        m_vidFrmCnt = 0;
        m_vidMaxIndex = 0;
        m_maxCacheSize = 0;

        m_hSeekPoints = nullptr;
        m_prepared = false;
        m_opened = false;

        m_errMsg = "";
    }

    bool GetSnapshots(double startPos, vector<Image::Holder>& images)
    {
        images.clear();
        if (!IsOpened())
        {
            m_errMsg = "NOT OPENED yet!";
            return false;
        }
        if (!m_prepared)
            return true;
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!IsOpened())  // double check within the lock protected range, in case another thread closed this instance
        {
            m_errMsg = "NOT OPENED yet!";
            return false;
        }

        int32_t idx0 = CalcSsIndexFromTs(startPos);
        if (idx0 < 0) idx0 = 0; if (idx0 > m_vidMaxIndex) idx0 = m_vidMaxIndex;
        int32_t idx1 = CalcSsIndexFromTs(startPos+m_snapWindowSize);
        if (idx1 < 0) idx1 = 0; if (idx1 > m_vidMaxIndex) idx1 = m_vidMaxIndex;
        if (idx0 > idx1)
            return true;

        // init 'images' with blank image
        images.reserve(idx1-idx0+1);
        for (int32_t i = idx0; i <= idx1; i++)
        {
            Image::Holder hIamge(new Image());
            hIamge->mTimestampMs = CalcSnapshotMts(i);
            images.push_back(hIamge);
        }

        lock_guard<mutex> readLock(m_goptskListReadLocks[0]);
        for (auto& goptsk : m_goptskList)
        {
            if (idx0 >= goptsk->TaskRange().SsIdx().second || idx1 < goptsk->TaskRange().SsIdx().first)
                continue;
            auto ssIter = goptsk->ssImgList.begin();
            while (ssIter != goptsk->ssImgList.end())
            {
                auto& ss = *ssIter++;
                if (ss->index < idx0 || ss->index > idx1)
                    continue;
                images[ss->index-idx0] = ss->img;
            }
        }
        return true;
    }

    MediaParser::Holder GetMediaParser() const override
    {
        return m_hParser;
    }

    Viewer::Holder CreateViewer(double pos) override;

    void ReleaseViewer(Viewer::Holder& viewer) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        {
            lock_guard<mutex> lk(m_viewerListLock);
            auto iter = find(m_viewers.begin(), m_viewers.end(), viewer);
            if (iter != m_viewers.end())
                m_viewers.erase(iter);
        }
        viewer = nullptr;
    }

    void ReleaseViewer(Viewer* viewer)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        {
            lock_guard<mutex> lk(m_viewerListLock);
            auto iter = find_if(m_viewers.begin(), m_viewers.end(), [viewer] (const Viewer::Holder& hViewer) {
                return hViewer.get() == viewer;
            });
            if (iter != m_viewers.end())
                m_viewers.erase(iter);
        }
    }

    bool IsOpened() const override
    {
        return m_opened;
    }

    bool HasVideo() const override
    {
        return m_vidStmIdx >= 0;
    }

    bool HasAudio() const override
    {
        return m_audStmIdx >= 0;
    }

    bool ConfigSnapWindow(double& windowSize, double frameCount, bool forceRefresh) override
    {
        // AutoSection _as("CfgSsWnd");
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (frameCount < 1)
        {
            m_errMsg = "Argument 'frameCount' must be greater than 1!";
            return false;
        }
        double minWndSize = CalcMinWindowSize(frameCount);
        if (windowSize < minWndSize)
            windowSize = minWndSize;
        double maxWndSize = GetMaxWindowSize();
        if (windowSize > maxWndSize)
            windowSize = maxWndSize;
        if (m_snapWindowSize == windowSize && m_wndFrmCnt == frameCount && !forceRefresh)
            return true;

        WaitAllThreadsQuit();
        FlushAllQueues();
        if (m_viddecCtx)
            avcodec_flush_buffers(m_viddecCtx);

        m_snapWindowSize = windowSize;
        m_wndFrmCnt = frameCount;

        if (m_prepared)
        {
            CalcWindowVariables();
            ResetGopDecodeTaskList();
            for (auto& hViewer : m_viewers)
            {
                Viewer_Impl* viewer = dynamic_cast<Viewer_Impl*>(hViewer.get());
                viewer->UpdateSnapwnd(viewer->GetCurrWindowPos(), true);
            }
        }

        StartAllThreads();

        m_logger->Log(DEBUG) << ">>>> Config window: m_snapWindowSize=" << m_snapWindowSize << ", m_wndFrmCnt=" << m_wndFrmCnt
            << ", m_vidMaxIndex=" << m_vidMaxIndex << ", m_maxCacheSize=" << m_maxCacheSize << ", m_prevWndCacheSize=" << m_prevWndCacheSize << endl;
        return true;
    }

    bool SetCacheFactor(double cacheFactor) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (cacheFactor < 1.)
        {
            m_errMsg = "Argument 'cacheFactor' must be greater or equal than 1.0!";
            return false;
        }
        m_cacheFactor = cacheFactor;
        m_maxCacheSize = (uint32_t)ceil(m_wndFrmCnt*m_cacheFactor);
        if (m_prepared)
            ResetGopDecodeTaskList();
        return true;
    }

    double GetMinWindowSize() const override
    {
        return CalcMinWindowSize(m_wndFrmCnt);
    }

    double GetMaxWindowSize() const override
    {
        return (double)m_vidDurMts/1000.;
    }

    bool SetSnapshotSize(uint32_t width, uint32_t height) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        m_useRszFactor = false;
        if (m_frmCvt.GetOutWidth() == width && m_frmCvt.GetOutHeight() == height)
            return true;
        if (!m_frmCvt.SetOutSize(width, height))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }
        if (m_prepared)
            ResetGopDecodeTaskList();
        return true;
    }

    bool SetSnapshotResizeFactor(float widthFactor, float heightFactor) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (widthFactor <= 0.f || heightFactor <= 0.f)
        {
            m_errMsg = "Resize factor must be a positive number!";
            return false;
        }
        if (!m_ssSizeChanged && m_useRszFactor && m_ssWFacotr == widthFactor && m_ssHFacotr == heightFactor)
            return true;

        m_ssWFacotr = widthFactor;
        m_ssHFacotr = heightFactor;
        m_useRszFactor = true;
        if (HasVideo())
        {
            if (widthFactor == 1.f && heightFactor == 1.f)
                return SetSnapshotSize(0, 0);

            auto vidStream = GetVideoStream();
            uint32_t outWidth = (uint32_t)ceil(vidStream->width*widthFactor);
            if ((outWidth&0x1) == 1)
                outWidth++;
            uint32_t outHeight = (uint32_t)ceil(vidStream->height*heightFactor);
            if ((outHeight&0x1) == 1)
                outHeight++;
            if (!SetSnapshotSize(outWidth, outHeight))
                return false;
            m_useRszFactor = true;
        }
        m_ssSizeChanged = false;
        return true;
    }

    bool SetOutColorFormat(ImColorFormat clrfmt) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (m_frmCvt.GetOutColorFormat() == clrfmt)
            return true;
        if (!m_frmCvt.SetOutColorFormat(clrfmt))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }
        if (m_prepared)
            ResetGopDecodeTaskList();
        return true;
    }

    bool SetResizeInterpolateMode(ImInterpolateMode interp) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (m_frmCvt.GetResizeInterpolateMode() == interp)
            return true;
        if (!m_frmCvt.SetResizeInterpolateMode(interp))
        {
            m_errMsg = m_frmCvt.GetError();
            return false;
        }
        if (m_prepared)
            ResetGopDecodeTaskList();
        return true;
    }

    MediaInfo::Holder GetMediaInfo() const override
    {
        return m_hMediaInfo;
    }

    const VideoStream* GetVideoStream() const override
    {
        MediaInfo::Holder hInfo = m_hMediaInfo;
        if (!hInfo || !HasVideo())
            return nullptr;
        return dynamic_cast<VideoStream*>(hInfo->streams[m_vidStmIdx].get());
    }

    const AudioStream* GetAudioStream() const override
    {
        MediaInfo::Holder hInfo = m_hMediaInfo;
        if (!hInfo || !HasAudio())
            return nullptr;
        return dynamic_cast<AudioStream*>(hInfo->streams[m_audStmIdx].get());
    }

    uint32_t GetVideoWidth() const override
    {
        const VideoStream* vidStream = GetVideoStream();
        if (vidStream)
            return vidStream->width;
        return 0;
    }

    uint32_t GetVideoHeight() const override
    {
        const VideoStream* vidStream = GetVideoStream();
        if (vidStream)
            return vidStream->height;
        return 0;
    }

    int64_t GetVideoMinPos() const override
    {
        return 0;
    }

    int64_t GetVideoDuration() const override
    {
        return m_vidDurMts;
    }

    int64_t GetVideoFrameCount() const override
    {
        return m_vidFrmCnt;
    }

    bool IsHwAccelEnabled() const override
    {
        return m_vidPreferUseHw;
    }

    void EnableHwAccel(bool enable) override
    {
        m_vidPreferUseHw = enable;
    }

    string GetError() const override
    {
        return m_errMsg;
    }

    bool CheckHwPixFmt(AVPixelFormat pixfmt)
    {
        return pixfmt == m_vidHwPixFmt;
    }

private:
    string FFapiFailureMessage(const string& apiName, int fferr)
    {
        ostringstream oss;
        oss << "FF api '" << apiName << "' returns error! fferr=" << fferr << ".";
        return oss.str();
    }

    double CalcMinWindowSize(double windowFrameCount) const
    {
        return m_vidfrmIntvMts*windowFrameCount/1000.;
    }

    int64_t CvtVidPtsToMts(int64_t pts)
    {
        return av_rescale_q(pts-m_vidStartPts, m_vidStream->time_base, MILLISEC_TIMEBASE);
    }

    int64_t CvtVidMtsToPts(int64_t mts)
    {
        return av_rescale_q(mts, MILLISEC_TIMEBASE, m_vidStream->time_base)+m_vidStartPts;
    }

    void CalcWindowVariables()
    {
        // AutoSection _as("CalcWnd");
        m_ssIntvMts = m_snapWindowSize*1000./m_wndFrmCnt;
        if (m_ssIntvMts < m_vidfrmIntvMts)
            m_ssIntvMts = m_vidfrmIntvMts;
        else if (m_ssIntvMts-m_vidfrmIntvMts <= 0.5)
            m_ssIntvMts = m_vidfrmIntvMts;
        m_ssIntvPts = m_ssIntvMts*m_vidStream->time_base.den/(1000.*m_vidStream->time_base.num); //av_rescale_q(m_ssIntvMts*1000, MICROSEC_TIMEBASE, m_vidStream->time_base);
        m_vidMaxIndex = (uint32_t)floor(((double)m_vidDurMts-m_vidfrmIntvMts)/m_ssIntvMts);
        m_maxCacheSize = (uint32_t)ceil(m_wndFrmCnt*m_cacheFactor);
        uint32_t intWndFrmCnt = (uint32_t)ceil(m_wndFrmCnt);
        if (m_maxCacheSize < intWndFrmCnt)
            m_maxCacheSize = intWndFrmCnt;
        m_prevWndCacheSize = (m_maxCacheSize-intWndFrmCnt)/2;
    }

    bool IsSsIdxValid(int32_t idx) const
    {
        return idx >= 0 && idx <= (int32_t)m_vidMaxIndex;
    }

    bool OpenMedia(MediaParser::Holder hParser)
    {
        int fferr = avformat_open_input(&m_avfmtCtx, hParser->GetUrl().c_str(), nullptr, nullptr);
        if (fferr < 0)
        {
            m_avfmtCtx = nullptr;
            m_errMsg = FFapiFailureMessage("avformat_open_input", fferr);
            return false;
        }

        m_hMediaInfo = hParser->GetMediaInfo();
        m_vidStmIdx = hParser->GetBestVideoStreamIndex();
        m_audStmIdx = hParser->GetBestAudioStreamIndex();
        if (m_vidStmIdx < 0)
        {
            ostringstream oss;
            oss << "No video stream can be found in '" << m_avfmtCtx->url << "'.";
            m_errMsg = oss.str();
            return false;
        }

        VideoStream* vidStream = dynamic_cast<VideoStream*>(m_hMediaInfo->streams[m_vidStmIdx].get());
        m_vidStartMts = (int64_t)(vidStream->startTime*1000);
        m_vidDurMts = (int64_t)(vidStream->duration*1000);
        m_vidFrmCnt = vidStream->frameNum;
        AVRational timebase = { vidStream->timebase.num, vidStream->timebase.den };
        AVRational frameRate;
        if (Ratio::IsValid(vidStream->avgFrameRate))
            frameRate = { vidStream->avgFrameRate.num, vidStream->avgFrameRate.den };
        else if (Ratio::IsValid(vidStream->realFrameRate))
            frameRate = { vidStream->realFrameRate.num, vidStream->realFrameRate.den };
        else
            frameRate = av_inv_q(timebase);
        m_vidfrmIntvMts = av_q2d(av_inv_q(frameRate))*1000.;
        m_vidfrmIntvMtsHalf = ceil(m_vidfrmIntvMts)/2;
        m_vidfrmIntvPts = av_rescale_q(1, av_inv_q(frameRate), timebase);
        m_vidfrmIntvPtsHalf = m_vidfrmIntvPts/2;

        if (m_useRszFactor)
        {
            uint32_t outWidth = (uint32_t)ceil(vidStream->width*m_ssWFacotr);
            if ((outWidth&0x1) == 1)
                outWidth++;
            uint32_t outHeight = (uint32_t)ceil(vidStream->height*m_ssHFacotr);
            if ((outHeight&0x1) == 1)
                outHeight++;
            if (!m_frmCvt.SetOutSize(outWidth, outHeight))
            {
                m_errMsg = m_frmCvt.GetError();
                return false;
            }
        }
        return true;
    }

    bool Prepare()
    {
        bool lockAquired;
        while (!(lockAquired = m_apiLock.try_lock()) && !m_quit)
            this_thread::sleep_for(chrono::milliseconds(5));
        if (m_quit)
        {
            if (lockAquired) m_apiLock.unlock();
            return false;
        }

        lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
        m_hParser->EnableParseInfo(MediaParser::VIDEO_SEEK_POINTS);
        m_hSeekPoints = m_hParser->GetVideoSeekPoints();
        if (!m_hSeekPoints)
        {
            m_errMsg = "FAILED to retrieve video seek points!";
            m_logger->Log(Error) << m_errMsg << endl;
            return false;
        }

        int fferr;
        fferr = avformat_find_stream_info(m_avfmtCtx, nullptr);
        if (fferr < 0)
        {
            m_errMsg = FFapiFailureMessage("avformat_open_input", fferr);
            return false;
        }

        if (HasVideo())
        {
            m_vidStream = m_avfmtCtx->streams[m_vidStmIdx];
            m_vidStartPts = m_vidStream->start_time;

            m_viddec = avcodec_find_decoder(m_vidStream->codecpar->codec_id);
            if (m_viddec == nullptr)
            {
                ostringstream oss;
                oss << "Can not find video decoder by codec_id " << m_vidStream->codecpar->codec_id << "!";
                m_errMsg = oss.str();
                return false;
            }

            if (m_vidPreferUseHw)
            {
                if (!OpenHwVideoDecoder())
                    if (!OpenVideoDecoder())
                        return false;
            }
            else if (!OpenVideoDecoder())
                return false;
            m_logger->Log(INFO) << "SnapshotGenerator for file '" << m_hParser->GetUrl() << "' opened video decoder '" << 
                m_viddecCtx->codec->name << "'(" << (m_viddecDevType==AV_HWDEVICE_TYPE_NONE ? "SW" : av_hwdevice_get_type_name(m_viddecDevType)) << ")." << endl;

            CalcWindowVariables();
            ResetGopDecodeTaskList();
        }

        if (HasAudio())
        {
            m_audStream = m_avfmtCtx->streams[m_audStmIdx];

            // wyvern: disable opening audio decoder because we don't use it now
            // if (!OpenAudioDecoder())
            //     return false;
        }

        {
            lock_guard<mutex> lk(m_viewerListLock);
            for (auto& hViewer : m_viewers)
            {
                Viewer_Impl* viewer = dynamic_cast<Viewer_Impl*>(hViewer.get());
                viewer->UpdateSnapwnd(viewer->GetCurrWindowPos(), true);
            }
        }

        m_logger->Log(DEBUG) << ">>>> Prepared: m_snapWindowSize=" << m_snapWindowSize << ", m_wndFrmCnt=" << m_wndFrmCnt
            << ", m_vidMaxIndex=" << m_vidMaxIndex << ", m_maxCacheSize=" << m_maxCacheSize << ", m_prevWndCacheSize=" << m_prevWndCacheSize << endl;
        m_prepared = true;
        return true;
    }

    bool OpenVideoDecoder()
    {
        m_viddecCtx = avcodec_alloc_context3(m_viddec);
        if (!m_viddecCtx)
        {
            m_errMsg = "FAILED to allocate new AVCodecContext!";
            return false;
        }
        m_viddecCtx->opaque = this;

        int fferr;
        fferr = avcodec_parameters_to_context(m_viddecCtx, m_vidStream->codecpar);
        if (fferr < 0)
        {
            m_errMsg = FFapiFailureMessage("avcodec_parameters_to_context", fferr);
            return false;
        }

        m_viddecCtx->thread_count = 8;
        // m_viddecCtx->thread_type = FF_THREAD_FRAME;
        fferr = avcodec_open2(m_viddecCtx, m_viddec, nullptr);
        if (fferr < 0)
        {
            m_errMsg = FFapiFailureMessage("avcodec_open2", fferr);
            return false;
        }
        m_logger->Log(DEBUG) << "Video decoder '" << m_viddec->name << "' opened." << " thread_count=" << m_viddecCtx->thread_count
            << ", thread_type=" << m_viddecCtx->thread_type << endl;
        return true;
    }

    bool OpenHwVideoDecoder()
    {
        int fferr;
        AVHWDeviceType hwDevType = AV_HWDEVICE_TYPE_NONE;
        AVCodecContext* hwDecCtx = nullptr;
        AVBufferRef* devCtx;
        for (int i = 0; ; i++)
        {
            const AVCodecHWConfig* config = avcodec_get_hw_config(m_viddec, i);
            if (!config)
            {
                m_vidHwPixFmt = AV_PIX_FMT_NONE;
                ostringstream oss;
                oss << "Decoder '" << m_viddec->name << "' does NOT support hardware acceleration.";
                m_errMsg = oss.str();
                return false;
            }
            if ((config->methods&AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0)
            {
                if (m_vidUseHwType == AV_HWDEVICE_TYPE_NONE || m_vidUseHwType == config->device_type)
                {
                    m_vidHwPixFmt = config->pix_fmt;
                    hwDevType = config->device_type;
                    hwDecCtx = avcodec_alloc_context3(m_viddec);
                    if (!hwDecCtx)
                        continue;
                    hwDecCtx->opaque = this;
                    fferr = avcodec_parameters_to_context(hwDecCtx, m_vidStream->codecpar);
                    if (fferr < 0)
                    {
                        avcodec_free_context(&hwDecCtx);
                        continue;
                    }
                    hwDecCtx->get_format = get_hw_format;
                    devCtx = nullptr;
                    fferr = av_hwdevice_ctx_create(&devCtx, hwDevType, nullptr, nullptr, 0);
                    if (fferr < 0)
                    {
                        avcodec_free_context(&hwDecCtx);
                        if (devCtx) av_buffer_unref(&devCtx);
                        continue;
                    }
                    hwDecCtx->hw_device_ctx = av_buffer_ref(devCtx);
                    fferr = avcodec_open2(hwDecCtx, m_viddec, nullptr);
                    if (fferr < 0)
                    {
                        avcodec_free_context(&hwDecCtx);
                        av_buffer_unref(&devCtx);
                        continue;
                    }
                    break;
                }
            }
        }

        m_viddecDevType = hwDevType;
        m_viddecCtx = hwDecCtx;
        m_viddecHwDevCtx = devCtx;
        m_logger->Log(DEBUG) << "Use hardware device type '" << av_hwdevice_get_type_name(m_viddecDevType) << "'." << endl;
        m_logger->Log(DEBUG) << "Video decoder(HW) '" << m_viddecCtx->codec->name << "' opened." << endl;
        return true;
    }

    void DemuxThreadProc()
    {
        m_logger->Log(VERBOSE) << "Enter DemuxThreadProc()..." << endl;

        if (!m_prepared && !Prepare())
        {
            if (!m_quit)
                m_logger->Log(Error) << "Prepare() FAILED! Error is '" << m_errMsg << "'." << endl;
            return;
        }

        AVPacket avpkt = {0};
        bool avpktLoaded = false;
        GopDecodeTaskHolder currTask = nullptr;
        int64_t lastGopSsPts;
        bool demuxEof = false;
        while (!m_quit)
        {
            bool idleLoop = true;

            UpdateGopDecodeTaskList();

            if (HasVideo())
            {
                bool taskChanged = false;
                if (!currTask || currTask->cancel || currTask->demuxerEof)
                {
                    if (currTask && currTask->cancel)
                        m_logger->Log(VERBOSE) << "~~~~ Current demux task canceled" << endl;
                    currTask = FindNextDemuxTask();
                    if (currTask)
                    {
                        currTask->demuxing = true;
                        taskChanged = true;
                        lastGopSsPts = INT64_MAX;
                        m_logger->Log(DEBUG) << "--> Change demux task, ssIdxPair=[" << currTask->TaskRange().SsIdx().first << ", " << currTask->TaskRange().SsIdx().second
                            << "), seekPtsPair=[" << currTask->TaskRange().SeekPts().first << "{" << MillisecToString(CvtVidPtsToMts(currTask->TaskRange().SeekPts().first)) << "}"
                            << ", " << currTask->TaskRange().SeekPts().second << "{" << MillisecToString(CvtVidPtsToMts(currTask->TaskRange().SeekPts().second)) << "}" << endl;
                    }
                }

                if (currTask)
                {
                    if (taskChanged)
                    {
                        if (!avpktLoaded || avpkt.pts != currTask->TaskRange().SeekPts().first)
                        {
                            if (avpktLoaded)
                            {
                                av_packet_unref(&avpkt);
                                avpktLoaded = false;
                            }
                            const int64_t seekPts0 = currTask->TaskRange().SeekPts().first;
                            m_logger->Log(DEBUG) << "--> Seek to pts=" << seekPts0 << endl;
                            int fferr = avformat_seek_file(m_avfmtCtx, m_vidStmIdx, INT64_MIN, seekPts0, seekPts0, 0);
                            if (fferr < 0)
                            {
                                m_logger->Log(Error) << "avformat_seek_file() FAILED for seeking to 'currTask->startPts'(" << seekPts0 << ")! fferr = " << fferr << "!" << endl;
                                break;
                            }
                            demuxEof = false;
                            int64_t ptsAfterSeek = INT64_MIN;
                            if (!ReadNextStreamPacket(m_vidStmIdx, &avpkt, &avpktLoaded, &ptsAfterSeek))
                                break;
                            if (ptsAfterSeek == INT64_MAX)
                                demuxEof = true;
                            else if (ptsAfterSeek != seekPts0)
                            {
                                m_logger->Log(VERBOSE) << "'ptsAfterSeek'(" << ptsAfterSeek << ") != 'ssTask->startPts'(" << seekPts0 << ")!" << endl;
                            }
                        }
                    }

                    if (!demuxEof && !avpktLoaded)
                    {
                        int fferr = av_read_frame(m_avfmtCtx, &avpkt);
                        if (fferr == 0)
                        {
                            avpktLoaded = true;
                            idleLoop = false;
                        }
                        else
                        {
                            if (fferr == AVERROR_EOF)
                            {
                                currTask->demuxerEof = true;
                                demuxEof = true;
                            }
                            else
                                m_logger->Log(Error) << "Demuxer ERROR! av_read_frame() returns " << fferr << "." << endl;
                        }
                    }

                    if (avpktLoaded)
                    {
                        if (avpkt.stream_index == m_vidStmIdx)
                        {
                            if (avpkt.pts >= currTask->TaskRange().SeekPts().second || avpkt.pts > lastGopSsPts)
                            {
                                bool canReadMore = avpkt.pts < currTask->TaskRange().SeekPts().second+CvtVidMtsToPts(200);
                                if (!canReadMore)
                                    currTask->demuxerEof = true;
                            }

                            if (!currTask->demuxerEof)
                            {
                                uint32_t bias{0};
                                int32_t ssIdx = CheckFrameSsBias(avpkt.pts, bias);
                                // update SS candidates frame
                                auto candIter = currTask->ssCandidates.find(ssIdx);
                                if (candIter != currTask->ssCandidates.end())
                                {
                                    if (candIter->second.pts == INT64_MIN || candIter->second.bias > bias)
                                        candIter->second = { avpkt.pts, bias, false };
                                }
                                else
                                {
                                    m_logger->Log(DEBUG) << ">> Extra SS candidate << SS candidate #" << ssIdx << ": pts=" << avpkt.pts << "(ts="
                                            << MillisecToString(CvtVidPtsToMts(avpkt.pts)) << "), bias=" << bias << endl;
                                    currTask->ssCandidates[ssIdx] = { avpkt.pts, bias, false };
                                }
                                if (ssIdx == currTask->m_range.SsIdx().second-1 && bias <= m_vidfrmIntvPtsHalf)
                                    lastGopSsPts = avpkt.pts;

                                m_logger->Log(VERBOSE) << "--> Queuing video packet, pts=" << avpkt.pts << ", isKey=" << ((avpkt.flags&AV_PKT_FLAG_KEY) != 0) << endl;
                                AVPacket* enqpkt = av_packet_clone(&avpkt);
                                if (!enqpkt)
                                {
                                    m_logger->Log(Error) << "FAILED to invoke [DEMUX]av_packet_clone()!" << endl;
                                    break;
                                }
                                {
                                    lock_guard<mutex> lk(currTask->avpktQLock);
                                    if (!currTask->demuxerEof)
                                    {
                                        // decoding thread may have finished decoding of all the SS in this GOP task,
                                        // then there is no need to continue the demuxing task
                                        currTask->avpktQ.push_back(enqpkt);
                                    }
                                }
                                av_packet_unref(&avpkt);
                                avpktLoaded = false;
                                idleLoop = false;
                            }
                        }
                        else
                        {
                            av_packet_unref(&avpkt);
                            avpktLoaded = false;
                        }
                    }
                }
            }
            else
            {
                m_logger->Log(Error) << "Demux procedure to non-video media is NOT IMPLEMENTED yet!" << endl;
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        if (currTask && !currTask->demuxerEof)
            currTask->demuxerEof = true;
        if (avpktLoaded)
            av_packet_unref(&avpkt);
        m_logger->Log(VERBOSE) << "Leave DemuxThreadProc()." << endl;
    }

    bool ReadNextStreamPacket(int stmIdx, AVPacket* avpkt, bool* avpktLoaded, int64_t* pts)
    {
        *avpktLoaded = false;
        int fferr;
        do {
            fferr = av_read_frame(m_avfmtCtx, avpkt);
            if (fferr == 0)
            {
                if (avpkt->stream_index == stmIdx)
                {
                    if (pts) *pts = avpkt->pts;
                    *avpktLoaded = true;
                    break;
                }
                av_packet_unref(avpkt);
            }
            else
            {
                if (fferr == AVERROR_EOF)
                {
                    if (pts) *pts = INT64_MAX;
                    break;
                }
                else
                {
                    m_logger->Log(Error) << "av_read_frame() FAILED! fferr = " << fferr << "." << endl;
                    return false;
                }
            }
        } while (fferr >= 0 && !m_quit);
        if (m_quit)
            return false;
        return true;
    }

    void VideoDecodeThreadProc()
    {
        m_logger->Log(VERBOSE) << "Enter VideoDecodeThreadProc()..." << endl;

        while (!m_prepared && !m_quit)
            this_thread::sleep_for(chrono::milliseconds(5));

        GopDecodeTaskHolder currTask;
        AVFrame avfrm = {0};
        bool avfrmLoaded = false;
        bool needResetDecoder = false;
        bool sentNullPacket = false;
        while (!m_quit)
        {
            bool idleLoop = true;
            bool quitLoop = false;

            if (!currTask || currTask->cancel || currTask->redoDecoding || currTask->decoderEof)
            {
                GopDecodeTaskHolder oldTask = currTask;
                currTask = FindNextDecoderTask();
                if (currTask)
                {
                    currTask->decoding = true;
                    m_logger->Log(DEBUG) << "==> Change decoding task to build SS ["
                        << currTask->m_range.SsIdx().first << ", " << currTask->m_range.SsIdx().second << "), pts=["
                        << currTask->m_range.SeekPts().first << "(" << MillisecToString(CvtVidPtsToMts(currTask->m_range.SeekPts().first)) << "), "
                        << currTask->m_range.SeekPts().second << "(" << MillisecToString(CvtVidPtsToMts(currTask->m_range.SeekPts().second)) << ")]" << endl;
                }
                if (oldTask)
                {
                    if (oldTask->cancel || oldTask->redoDecoding)
                    {
                        m_logger->Log(DEBUG) << "~~~~ Old video task canceled (or redo-decoding), SS range ["
                            << oldTask->m_range.SsIdx().first << ", " << oldTask->m_range.SsIdx().second << ")." << endl;
                        if (avfrmLoaded)
                        {
                            av_frame_unref(&avfrm);
                            avfrmLoaded = false;
                        }
                        needResetDecoder = true;
                    }
                    else
                    {
                        m_logger->Log(DEBUG) << ">>>--->>> Sending NULL ptr to video decoder <<<---<<<" << endl;
                        avcodec_send_packet(m_viddecCtx, nullptr);
                        sentNullPacket = true;
                    }
                }
            }

            if (needResetDecoder)
            {
                avcodec_flush_buffers(m_viddecCtx);
                needResetDecoder = false;
                sentNullPacket = false;
            }

            // retrieve output frame
            bool hasOutput;
            do{
                if (!avfrmLoaded)
                {
                    int fferr = avcodec_receive_frame(m_viddecCtx, &avfrm);
                    if (fferr == 0)
                    {
                        m_logger->Log(VERBOSE) << "<<< avcodec_receive_frame() pts=" << avfrm.pts << "(" << MillisecToString(CvtVidPtsToMts(avfrm.pts)) << ")." << endl;
                        avfrmLoaded = true;
                        idleLoop = false;
                    }
                    else if (fferr != AVERROR(EAGAIN))
                    {
                        if (fferr != AVERROR_EOF)
                        {
                            m_logger->Log(Error) << "FAILED to invoke avcodec_receive_frame()! return code is " << fferr << "." << endl;
                            quitLoop = true;
                            break;
                        }
                        else
                        {
                            idleLoop = false;
                            needResetDecoder = true;
                            m_logger->Log(DEBUG) << "Video decoder current task reaches EOF!" << endl;
                        }
                    }
                }

                hasOutput = avfrmLoaded;
                if (avfrmLoaded)
                {
                    int32_t ssIdx{-1};
                    uint32_t bias{UINT32_MAX};
                    list<GopDecodeTaskHolder> ssGopTasks = FindFrameSsPosition(avfrm.pts, ssIdx, bias);
                    if (ssGopTasks.empty())
                    {
                        m_logger->Log(VERBOSE) << "Drop video frame pts=" << avfrm.pts << ", ssIdx=" << ssIdx << ". No corresponding GopDecoderTask can be found." << endl;
                        av_frame_unref(&avfrm);
                        avfrmLoaded = false;
                        idleLoop = false;
                    }
                    else
                    {
                        while (!m_quit)
                        {
                            if (m_pendingVidfrmCnt < m_maxPendingVidfrmCnt)
                            {
                                for (auto& t : ssGopTasks)
                                {
                                    m_logger->Log(DEBUG) << "Enqueue SS#" << ssIdx << ", pts=" << avfrm.pts << "(ts=" << MillisecToString(CvtVidPtsToMts(avfrm.pts))
                                        << ") to _GopDecodeTask: ssIdxPair=[" << t->m_range.SsIdx().first << ", " << t->m_range.SsIdx().second
                                        << "), ptsPair=[" << t->m_range.SeekPts().first << ", " << t->m_range.SeekPts().second << ")." << endl;
                                }
                                if (!EnqueueSnapshotAVFrame(ssGopTasks, &avfrm, ssIdx, bias))
                                    m_logger->Log(WARN) << "FAILED to enqueue SS#" << ssIdx << ", pts=" << avfrm.pts << "(ts=" << MillisecToString(CvtVidPtsToMts(avfrm.pts)) << ")." << endl;
                                av_frame_unref(&avfrm);
                                avfrmLoaded = false;
                                idleLoop = false;
                                break;
                            }
                            else
                            {
                                this_thread::sleep_for(chrono::milliseconds(5));
                            }
                        }
                    }
                }
            } while (hasOutput && !m_quit);
            if (quitLoop)
                break;
            if (currTask && (currTask->decoderEof || currTask->cancel || currTask->redoDecoding))
                continue;

            if (currTask && !sentNullPacket)
            {
                // input packet to decoder
                if (!currTask->avpktQ.empty())
                {
                    bool popAvpkt = false;
                    AVPacket* avpkt = currTask->avpktQ.front();
                    int fferr = avcodec_send_packet(m_viddecCtx, avpkt);
                    if (fferr == 0)
                    {
                        m_logger->Log(VERBOSE) << ">>> avcodec_send_packet() pts=" << avpkt->pts << "(" << MillisecToString(CvtVidPtsToMts(avpkt->pts)) << ")." << endl;
                        popAvpkt = true;
                    }
                    else if (fferr != AVERROR(EAGAIN) && fferr != AVERROR_INVALIDDATA)
                    {
                        m_logger->Log(Error) << "FAILED to invoke avcodec_send_packet()! return code is " << fferr << "." << endl;
                        quitLoop = true;
                    }
                    else if (fferr == AVERROR_INVALIDDATA)
                    {
                        popAvpkt = true;
                    }
                    if (popAvpkt)
                    {
                        {
                            lock_guard<mutex> lk(currTask->avpktQLock);
                            currTask->avpktQ.pop_front();
                            currTask->avpktBkupQ.push_back(avpkt);
                        }
                        idleLoop = false;
                    }
                }
                else if (currTask->demuxerEof)
                {
                    currTask->decoderEof = true;
                    idleLoop = false;
                }
                if (quitLoop)
                    break;
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        if (currTask && !currTask->decoderEof)
            currTask->decoderEof = true;
        if (avfrmLoaded)
            av_frame_unref(&avfrm);
        m_logger->Log(VERBOSE) << "Leave VideoDecodeThreadProc()." << endl;
    }

    void UpdateSnapshotThreadProc()
    {
        m_logger->Log(VERBOSE) << "Enter UpdateSnapshotThreadProc()." << endl;
        GopDecodeTaskHolder currTask;
        while (!m_quit)
        {
            bool idleLoop = true;

            if (!currTask || currTask->ssAvfrmList.empty() || currTask->cancel || currTask->redoDecoding)
            {
                currTask = FindNextSsUpdateTask();
            }

            if (currTask)
            {
                while (!currTask->ssAvfrmList.empty())
                {
                    _Picture::Holder ss;
                    {
                        lock_guard<mutex> lk(currTask->ssAvfrmListLock);
                        ss = currTask->ssAvfrmList.front();
                        currTask->ssAvfrmList.pop_front();
                    }
                    if (ss->avfrm)
                    {
                        double ts = (double)CvtVidPtsToMts(ss->avfrm->pts)/1000.;
                        if (!m_frmCvt.ConvertImage(ss->avfrm, ss->img->mImgMat, ts))
                        {
                            m_logger->Log(WARN) << "FAILED to convert AVFrame(pts=" << ss->avfrm->pts << ", mts=" << CvtVidPtsToMts(ss->avfrm->pts)
                                    << ") to ImGui::ImMat! Message is '" << m_frmCvt.GetError() << "'. REDO-decoding on this task." << endl;
                            av_frame_free(&ss->avfrm);
                            m_pendingVidfrmCnt--;
                            currTask->redoDecoding = true;
                            idleLoop = false;
                            break;
                        }

                        av_frame_free(&ss->avfrm);
                        ss->avfrm = nullptr;
                        m_pendingVidfrmCnt--;
                        if (m_pendingVidfrmCnt < 0)
                            m_logger->Log(Error) << "Pending video AVFrame ptr count is NEGATIVE! " << m_pendingVidfrmCnt << endl;
                        ss->img->mTimestampMs = CalcSnapshotMts(ss->index);
                        idleLoop = false;
                    }
                    if (!ss->img->mImgMat.empty())
                    {
                        auto imgIter = find_if(currTask->ssImgList.begin(), currTask->ssImgList.end(), [ss] (auto& elem) {
                            return ss->index == elem->index;
                        });
                        if (imgIter != currTask->ssImgList.end())
                        {
                            if (ss->bias < (*imgIter)->bias)
                                *imgIter = ss;
                            else if (ss->bias > (*imgIter)->bias)
                                m_logger->Log(WARN) << "DISCARD SS Image #" << ss->index << ", pts=" << ss->pts << "(" << MillisecToString(CvtVidPtsToMts(ss->pts))
                                    << ") due to an EXISTING BETTER SS Image, pts=" << (*imgIter)->pts << "(" << MillisecToString(CvtVidPtsToMts((*imgIter)->pts))
                                    << "), bias " << ss->bias << "(new) >= " << (*imgIter)->bias << "." << endl;
                        }
                        else
                        {
                            currTask->ssImgList.push_back(ss);
                        }
                        idleLoop = false;
                    }
                }
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        m_logger->Log(VERBOSE) << "Leave UpdateSnapshotThreadProc()." << endl;
    }

    void FreeGopTaskProc()
    {
        m_logger->Log(VERBOSE) << "Enter FreeGopTaskProc()." << endl;
        while (!m_quit)
        {
            bool idleLoop = true;

            list<GopDecodeTaskHolder> clearList;
            {
                lock_guard<mutex> _lk(m_goptskFreeLock);
                if (!m_goptskToFree.empty())
                    clearList.splice(clearList.end(), m_goptskToFree);
            }
            if (!clearList.empty())
            {
                m_logger->Log(VERBOSE) << "Clear " << clearList.size() << " gop tasks." << endl;
                clearList.clear();
                idleLoop = false;
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(20));
        }
        m_logger->Log(VERBOSE) << "Leave FreeGopTaskProc()." << endl;
    }

    void StartAllThreads()
    {
        string fileName = SysUtils::ExtractFileName(m_hParser->GetUrl());
        ostringstream thnOss;
        m_quit = false;
        m_demuxThread = thread(&Generator_Impl::DemuxThreadProc, this);
        thnOss << "SsgDmx-" << fileName;
        SysUtils::SetThreadName(m_demuxThread, thnOss.str());
        m_viddecThread = thread(&Generator_Impl::VideoDecodeThreadProc, this);
        thnOss.str(""); thnOss << "SsgVdc-" << fileName;
        SysUtils::SetThreadName(m_viddecThread, thnOss.str());
        m_updateSsThread = thread(&Generator_Impl::UpdateSnapshotThreadProc, this);
        thnOss.str(""); thnOss << "SsgUss-" << fileName;
        SysUtils::SetThreadName(m_updateSsThread, thnOss.str());
        m_freeGoptskThread = thread(&Generator_Impl::FreeGopTaskProc, this);
        thnOss.str(""); thnOss << "SsgFgt-" << fileName;
        SysUtils::SetThreadName(m_freeGoptskThread, thnOss.str());    }

    void WaitAllThreadsQuit()
    {
        // AutoSection _as("WATQ");
        m_quit = true;
        if (m_demuxThread.joinable())
        {
            m_demuxThread.join();
            m_demuxThread = thread();
        }
        if (m_viddecThread.joinable())
        {
            m_viddecThread.join();
            m_viddecThread = thread();
        }
        if (m_updateSsThread.joinable())
        {
            m_updateSsThread.join();
            m_updateSsThread = thread();
        }
        if (m_freeGoptskThread.joinable())
        {
            m_freeGoptskThread.join();
            m_freeGoptskThread = thread();
        }
    }

    void FlushAllQueues()
    {
        // AutoSection _as("FAQ");
        {
            lock_guard<mutex> lk(m_deprecatedTextureLock);
            for (auto tsk : m_goptskPrepareList)
            {
                for (auto p: tsk->ssAvfrmList)
                    if (p->img->mTextureHolder)
                        m_deprecatedTextures.push_back(std::move(p->img->mTextureHolder));
                for (auto p: tsk->ssImgList)
                    if (p->img->mTextureHolder)
                        m_deprecatedTextures.push_back(std::move(p->img->mTextureHolder));
            }
            for (auto tsk : m_goptskList)
            {
                for (auto p: tsk->ssAvfrmList)
                    if (p->img->mTextureHolder)
                        m_deprecatedTextures.push_back(std::move(p->img->mTextureHolder));
                for (auto p: tsk->ssImgList)
                    if (p->img->mTextureHolder)
                        m_deprecatedTextures.push_back(std::move(p->img->mTextureHolder));
            }
        }

        {
            lock_guard<mutex> _lk(m_goptskFreeLock);
            if (!m_goptskPrepareList.empty())
                m_goptskToFree.splice(m_goptskToFree.end(), m_goptskPrepareList);
            if (!m_goptskList.empty())
                m_goptskToFree.splice(m_goptskToFree.end(), m_goptskList);
        }
    }

    struct _Picture
    {
        using Holder = shared_ptr<_Picture>;

        _Picture(Generator_Impl* owner, int32_t _index, AVFrame* _avfrm, uint32_t _bias)
            : m_owner(owner), img(new Image()), index(_index), avfrm(_avfrm), bias(_bias)
        {
            pts = _avfrm->pts;
        }

        ~_Picture()
        {
            if (avfrm)
            {
                av_frame_free(&avfrm);
                m_owner->m_pendingVidfrmCnt--;
            }
            if (img->mTextureHolder)
            {
                lock_guard<mutex> lk(m_owner->m_deprecatedTextureLock);
                m_owner->m_deprecatedTextures.push_back(img->mTextureHolder);
            }
        }

        Generator_Impl* m_owner;
        Image::Holder img;
        int32_t index;
        AVFrame* avfrm;
        int64_t pts;
        int64_t bias;
        bool fixed{false};
    };

    struct _SnapWindow
    {
        double wndpos;
        int32_t viewIdx0;
        int32_t viewIdx1;
        int32_t cacheIdx0;
        int32_t cacheIdx1;
        int64_t seekPos00;
        int64_t seekPos10;

        bool IsInView(int32_t idx) const
        { return idx >= viewIdx0 && idx <= viewIdx1; }
        bool IsInCache(int32_t idx) const
        { return idx >= cacheIdx0 && idx <= cacheIdx1; }
        bool IsInCache(int64_t pts) const
        { return pts >= seekPos00 && pts <= seekPos10; }
    };

    struct _SnapshotCandidate
    {
        int64_t pts{INT64_MIN};
        uint32_t bias{UINT32_MAX};
        bool frmEnqueued{false};
    };

    struct _GopDecodeTask
    {
        class Range
        {
        public:
            Range(const pair<int64_t, int64_t>& seekPts, const pair<int32_t, int32_t>& ssIdx, bool isInView, int32_t distanceToViewWnd)
                : m_seekPts(seekPts), m_ssIdx(ssIdx), m_isInView(isInView), m_distanceToViewWnd(distanceToViewWnd)
            {}
            Range(const Range&) = default;
            Range(Range&&) = default;
            Range& operator=(const Range&) = default;

            const pair<int64_t, int64_t>& SeekPts() const { return m_seekPts; }
            const pair<int32_t, int32_t>& SsIdx() const { return m_ssIdx; }
            bool IsInView() const { return m_isInView; }
            void SetInView(bool isInView) { m_isInView = isInView; }
            int32_t DistanceToViewWindow() const { return m_distanceToViewWnd; }

            friend bool operator==(const Range& oprnd1, const Range& oprnd2)
            {
                if (oprnd1.m_seekPts.first == INT64_MIN && oprnd2.m_seekPts.second == INT64_MIN)
                    return false;
                bool e1 = oprnd1.m_seekPts.first == oprnd2.m_seekPts.first;
                bool e2 = oprnd1.m_seekPts.second == oprnd2.m_seekPts.second;
                if (e1^e2)
                    GetLogger()->Log(Error) << "!!! _GopDecodeTask::Range compare ABNORMAL! ("
                        << oprnd1.m_seekPts.first << ", " << oprnd1.m_seekPts.second << ") VS ("
                        << oprnd2.m_seekPts.first << ", " << oprnd2.m_seekPts.second << ")." << endl;
                return e1 && e2;
            }

        private:
            pair<int64_t, int64_t> m_seekPts{INT64_MIN, INT64_MIN};
            pair<int32_t, int32_t> m_ssIdx{-1, -1};
            int32_t m_distanceToViewWnd{0};
            bool m_isInView{false};
        };

        _GopDecodeTask(Generator_Impl* owner, const Range& range)
            : m_owner(owner), m_range(range)
        {
            int32_t idxBegin = range.SsIdx().first < 0 ? 0 : range.SsIdx().first;
            int32_t idxEnd = range.SsIdx().second > owner->m_vidMaxIndex+1 ? owner->m_vidMaxIndex+1 : range.SsIdx().second;
            for (int32_t ssIdx = idxBegin; ssIdx < idxEnd; ssIdx++)
                ssCandidates[ssIdx] = _SnapshotCandidate();
        }

        ~_GopDecodeTask()
        {
            for (AVPacket* avpkt : avpktQ)
                av_packet_free(&avpkt);
            for (AVPacket* avpkt : avpktBkupQ)
                av_packet_free(&avpkt);
        }

        const Range& TaskRange() const { return m_range; }
        bool IsInView() const { return m_range.IsInView(); }
        int32_t DistanceToViewWnd() const { return m_range.DistanceToViewWindow(); }

        Generator_Impl* m_owner;
        Range m_range;
        unordered_map<int32_t, _SnapshotCandidate> ssCandidates;
        bool isEndOfGop{true};
        list<_Picture::Holder> ssAvfrmList;
        mutex ssAvfrmListLock;
        list<_Picture::Holder> ssImgList;
        list<AVPacket*> avpktQ;
        list<AVPacket*> avpktBkupQ;
        mutex avpktQLock;
        bool demuxing{false};
        bool demuxerEof{false};
        bool decoding{false};
        bool redoDecoding{false};
        bool allCandDecoded{false};
        bool decoderEof{false};
        bool cancel{false};
    };
    using GopDecodeTaskHolder = shared_ptr<_GopDecodeTask>;

    _SnapWindow CreateSnapWindow(double wndpos)
    {
        if (!m_prepared)
            return { wndpos, -1, -1, -1, -1, INT64_MIN, INT64_MIN };
        int32_t index0 = CalcSsIndexFromTs(wndpos);
        int32_t index1 = CalcSsIndexFromTs(wndpos+m_snapWindowSize);
        int32_t cacheIdx0 = index0-(int32_t)m_prevWndCacheSize;
        int32_t cacheIdx1 = cacheIdx0+(int32_t)m_maxCacheSize-1;
        pair<int64_t, int64_t> seekPos0 = GetSeekPosBySsIndex(cacheIdx0);
        pair<int64_t, int64_t> seekPos1 = GetSeekPosBySsIndex(cacheIdx1);
        return { wndpos, index0, index1, cacheIdx0, cacheIdx1, seekPos0.first, seekPos1.first };
    }

    list<GopDecodeTaskHolder> FindFrameSsPosition(int64_t pts, int32_t& ssIdx, uint32_t& bias)
    {
        ssIdx = (int32_t)round((double)pts/m_ssIntvPts);
        bias = (uint32_t)floor(abs(m_ssIntvPts*ssIdx-pts));
        bool noEntry = true;
        _SnapshotCandidate bestCandTime;
        list<GopDecodeTaskHolder> tasks;
        {
            lock_guard<mutex> lk(m_goptskListReadLocks[0]);
            auto iter = m_goptskList.begin();
            while (iter != m_goptskList.end())
            {
                auto& t = *iter++;
                auto candIter = t->ssCandidates.find(ssIdx);
                if (candIter == t->ssCandidates.end())
                    continue;
                tasks.push_back(t);
                auto& candTime = candIter->second;
                if (candTime.bias < bias && candTime.bias < bestCandTime.bias)
                    bestCandTime = candTime;
            }
            if (tasks.empty())
            {
                m_logger->Log(DEBUG) << ">>> CANNOT find SS candidate entry for #" << ssIdx << ", pts=" << pts
                        << "(mts=" << MillisecToString(CvtVidPtsToMts(pts)) << ")." << endl;
            }
            else if (bestCandTime.bias < UINT32_MAX)
            {
                for (auto& t : tasks)
                {
                    auto candIter = t->ssCandidates.find(ssIdx);
                    auto& candTime = candIter->second;
                    if (candTime.bias > bestCandTime.bias)
                    {
                        candTime.pts = bestCandTime.pts;
                        candTime.bias = bestCandTime.bias;
                    }
                }
                tasks.clear();
            }
        }
        return std::move(tasks);
    }

    int32_t CheckFrameSsBias(int64_t pts, uint32_t& bias)
    {
        int32_t index = (int32_t)round((double)pts/m_ssIntvPts);
        bias = (uint32_t)floor(abs(m_ssIntvPts*index-pts));
        return index;
    }

    // bool IsSpecificSnapshotFrame(uint32_t index, int64_t mts)
    // {
    //     double diff = abs(index*m_ssIntvMts-mts);
    //     return diff <= m_vidfrmIntvMtsHalf;
    // }

    int64_t CalcSnapshotMts(int32_t index)
    {
        if (m_ssIntvPts > 0)
            return CvtVidPtsToMts(floor(index*m_ssIntvPts+m_vidStartPts));
        return 0;
    }

    // double CalcSnapshotTimestamp(uint32_t index)
    // {
    //     return (double)CalcSnapshotMts(index)/1000.;
    // }

    int32_t CalcSsIndexFromTs(double ts)
    {
        return (int32_t)floor(ts*1000/m_ssIntvMts);
    }

    pair<int64_t, int64_t> GetSeekPosByMts(int64_t mts)
    {
        if (mts < 0)
            return { INT64_MIN, INT64_MIN };
        if (mts > m_vidDurMts)
            return { INT64_MAX, INT64_MAX };
        int64_t targetPts = CvtVidMtsToPts(mts);
        int64_t offsetCompensation = m_vidfrmIntvPtsHalf;
        auto iter = find_if(m_hSeekPoints->begin(), m_hSeekPoints->end(),
            [targetPts, offsetCompensation](int64_t keyPts) { return keyPts-offsetCompensation > targetPts; });
        if (iter != m_hSeekPoints->begin())
            iter--;
        int64_t first = *iter++;
        int64_t second = iter == m_hSeekPoints->end() ? INT64_MAX : *iter;
        if (targetPts >= second || (second-targetPts) < offsetCompensation)
        {
            first = second;
            iter++;
            second = iter == m_hSeekPoints->end() ? INT64_MAX : *iter;
        }
        return { first, second };
    }

    pair<int64_t, int64_t> GetSeekPosBySsIndex(int32_t index)
    {
        auto ptsPair = GetSeekPosByMts(CalcSnapshotMts(index));
        if (index == m_vidMaxIndex && ptsPair.first == INT64_MAX && ptsPair.second == INT64_MAX)
            ptsPair.first = m_hSeekPoints->back();
        return ptsPair;
    }

    pair<int32_t, int32_t> CalcSsIndexPairFromPtsPair(const pair<int64_t, int64_t>& ptsPair, int32_t startIdx)
    {
        int32_t idx0 = (int32_t)ceil((double)(ptsPair.first-m_vidStartPts-m_vidfrmIntvPtsHalf)/m_ssIntvPts);
        if (idx0 > startIdx) idx0 = startIdx;
        int32_t idx1;
        if (ptsPair.second == INT64_MAX)
            idx1 = m_vidMaxIndex+1;
        else
            idx1 = (int32_t)ceil((double)(ptsPair.second-m_vidStartPts-m_vidfrmIntvPtsHalf)/m_ssIntvPts);
        if (idx1 == idx0) idx1++;
        return { idx0, idx1 };
    }

    void ResetGopDecodeTaskList()
    {
        // AutoSection _as("RstGop");
        {
            lock(m_goptskListReadLocks[0], m_goptskListReadLocks[1], m_goptskListReadLocks[2]);
            lock_guard<mutex> lk0(m_goptskListReadLocks[0], adopt_lock);
            lock_guard<mutex> lk1(m_goptskListReadLocks[1], adopt_lock);
            lock_guard<mutex> lk2(m_goptskListReadLocks[2], adopt_lock);
            m_goptskList.clear();
            m_goptskPrepareList.clear();
        }

        UpdateGopDecodeTaskList();
    }

    void UpdateGopDecodeTaskList()
    {
        list<Viewer::Holder> viewers;
        {
            lock_guard<mutex> lk(m_viewerListLock);
            viewers = m_viewers;
        }
        // Check if view window changed
        bool taskRangeChanged = false;
        for (auto& hViewer : viewers)
        {
            Viewer_Impl* viewer = dynamic_cast<Viewer_Impl*>(hViewer.get());
            if (viewer->IsTaskRangeChanged())
            {
                taskRangeChanged = true;
                break;
            }
        }
        if (!taskRangeChanged)
            return;

        // Aggregate all _GopDecodeTask::Range(s) from all the Viewer(s)
        list<_GopDecodeTask::Range> totalTaskRanges;
        for (auto& hViewer : viewers)
        {
            Viewer_Impl* viewer = dynamic_cast<Viewer_Impl*>(hViewer.get());
            list<_GopDecodeTask::Range> taskRanges = viewer->CheckTaskRanges();
            for (auto& tskrng : taskRanges)
            {
                auto iter = find(totalTaskRanges.begin(), totalTaskRanges.end(), tskrng);
                if (iter == totalTaskRanges.end())
                    totalTaskRanges.push_back(tskrng);
                else if (tskrng.IsInView())
                    iter->SetInView(true);
            }
        }
        m_logger->Log(DEBUG) << ">>>>> Aggregated task ranges <<<<<<<" << endl << "\t";
        for (auto& range : totalTaskRanges)
            m_logger->Log(DEBUG) << "[" << range.SsIdx().first << ", " << range.SsIdx().second << "), ";
        m_logger->Log(DEBUG) << endl;

        // Update _GopDecodeTask prepare list
        bool updated = false;
        // 1. remove the tasks that are no longer in the cache range
        auto taskIter = m_goptskPrepareList.begin();
        while (taskIter != m_goptskPrepareList.end())
        {
            auto& task = *taskIter;
            auto iter = find_if(totalTaskRanges.begin(), totalTaskRanges.end(), [task] (const _GopDecodeTask::Range& range) {
                return task->m_range == range;
            });
            if (iter == totalTaskRanges.end())
            {
                m_logger->Log(DEBUG) << "~~~~> Erase UNUSED task range [" << (*taskIter)->TaskRange().SsIdx().first << ", " << (*taskIter)->TaskRange().SsIdx().second << ")" << endl;
                task->cancel = true;
                taskIter = m_goptskPrepareList.erase(taskIter);
                updated = true;
            }
            else
            {
                m_logger->Log(DEBUG) << "~~~~> Remove DUPLICATED task range [" << (*taskIter)->TaskRange().SsIdx().first << ", " << (*taskIter)->TaskRange().SsIdx().second << ")" << endl;
                task->m_range.SetInView(iter->IsInView());
                totalTaskRanges.erase(iter);
                taskIter++;
            }
        }
        // 2. add the tasks with newly created ranges
        for (auto& range : totalTaskRanges)
        {
            GopDecodeTaskHolder hTask(new _GopDecodeTask(this, range));
            m_goptskPrepareList.push_back(hTask);
            updated = true;
        }
        m_logger->Log(DEBUG) << ">>>>> GopTask list task ranges <<<<<<<" << endl << "\t";
        for (auto& goptsk : m_goptskPrepareList)
        {
            auto& range = goptsk->TaskRange();
            m_logger->Log(DEBUG) << "[" << range.SsIdx().first << ", " << range.SsIdx().second << "), ";
        }
        m_logger->Log(DEBUG) << "updated=" << updated << endl;

        // Update _GopDecodeTask list
        if (updated)
        {
            lock(m_goptskListReadLocks[0], m_goptskListReadLocks[1], m_goptskListReadLocks[2]);
            lock_guard<mutex> lk0(m_goptskListReadLocks[0], adopt_lock);
            lock_guard<mutex> lk1(m_goptskListReadLocks[1], adopt_lock);
            lock_guard<mutex> lk2(m_goptskListReadLocks[2], adopt_lock);
            m_goptskList = m_goptskPrepareList;
        }
    }

    GopDecodeTaskHolder FindNextDemuxTask()
    {
        GopDecodeTaskHolder candidateTask = nullptr;
        uint32_t pendingDecodingTaskCnt = 0;
        int32_t shortestDistanceToViewWnd = INT32_MAX;
        for (auto& task : m_goptskList)
        {
            if (!task->cancel && !task->demuxing)
            {
                if (task->IsInView())
                {
                    candidateTask = task;
                    break;
                }
                else if (shortestDistanceToViewWnd > task->DistanceToViewWnd())
                {
                    candidateTask = task;
                    shortestDistanceToViewWnd = task->DistanceToViewWnd();
                }
            }
            else if (!task->decoding)
            {
                pendingDecodingTaskCnt++;
                if (pendingDecodingTaskCnt > m_maxPendingTaskCountForDecoding)
                {
                    candidateTask = nullptr;
                    break;
                }
            }
        }
        return candidateTask;
    }

    GopDecodeTaskHolder FindNextDecoderTask()
    {
        lock_guard<mutex> lk(m_goptskListReadLocks[1]);
        GopDecodeTaskHolder candidateTask = nullptr;
        int32_t shortestDistanceToViewWnd = INT32_MAX;
        for (auto& task : m_goptskList)
        {
            if (!task->cancel && task->demuxing && (!task->decoding || task->redoDecoding))
            {
                if (task->IsInView())
                {
                    candidateTask = task;
                    break;
                }
                else if (shortestDistanceToViewWnd > task->DistanceToViewWnd())
                {
                    candidateTask = task;
                    shortestDistanceToViewWnd = task->DistanceToViewWnd();
                }
            }
        }
        if (candidateTask && candidateTask->redoDecoding)
        {
            m_logger->Log(DEBUG) << "---> REDO decoding on _GopDecodeTask, ssIdxPair=["
                    << candidateTask->m_range.SsIdx().first << ", " << candidateTask->m_range.SsIdx().second << "), ptsPair=["
                    << candidateTask->m_range.SeekPts().first << ", " << candidateTask->m_range.SeekPts().second << ")." << endl;
            for (auto& elem : candidateTask->ssCandidates)
                elem.second.frmEnqueued = false;
            candidateTask->allCandDecoded = false;
            candidateTask->redoDecoding = false;
            candidateTask->decoderEof = false;
            while (!candidateTask->avpktQ.empty())
            {
                candidateTask->avpktBkupQ.push_back(candidateTask->avpktQ.front());
                candidateTask->avpktQ.pop_front();
            }
            while (!candidateTask->avpktBkupQ.empty())
            {
                candidateTask->avpktQ.push_back(candidateTask->avpktBkupQ.front());
                candidateTask->avpktBkupQ.pop_front();
            }
        }
        return candidateTask;
    }

    GopDecodeTaskHolder FindNextSsUpdateTask()
    {
        lock_guard<mutex> lk(m_goptskListReadLocks[2]);
        GopDecodeTaskHolder nxttsk = nullptr;
        for (auto& tsk : m_goptskList)
            if (!tsk->ssAvfrmList.empty() && !tsk->cancel && !tsk->redoDecoding)
            {
                nxttsk = tsk;
                break;
            }
        return nxttsk;
    }

    bool EnqueueSnapshotAVFrame(list<GopDecodeTaskHolder> ssGopTasks, AVFrame* frm, int32_t ssIdx, uint32_t bias)
    {
        if (ssGopTasks.empty())
            return false;

        AVFrame* _avfrm = av_frame_clone(frm);
        if (!_avfrm)
        {
            m_logger->Log(Error) << "FAILED to invoke 'av_frame_clone()' to allocate new AVFrame for SS!" << endl;
            return false;
        }
        _Picture::Holder ss(new _Picture(this, ssIdx, _avfrm, bias));

        bool ssAdopted = false;
        for (auto& t : ssGopTasks)
        {
            lock_guard<mutex> lk(t->ssAvfrmListLock);
            bool ssAdopt = false;
            // m_logger->Log(DEBUG) << "Adding SS#" << ssIdx << "." << endl;
            auto ssIter = find_if(t->ssAvfrmList.begin(), t->ssAvfrmList.end(), [ssIdx] (auto& elem) {
                return elem->index == ssIdx;
            });
            if (ssIter == t->ssAvfrmList.end() || (*ssIter)->bias > bias)
                ssAdopt = true;
            auto ssIter2 = find_if(t->ssImgList.begin(), t->ssImgList.end(), [ssIdx] (auto& elem) {
                return elem->index == ssIdx;
            });
            if (ssIter2 != t->ssImgList.end() && (*ssIter2)->bias <= bias)
                ssAdopt = false;

            if (ssAdopt)
            {
                if (ssIter == t->ssAvfrmList.end())
                    t->ssAvfrmList.push_back(ss);
                else
                    *ssIter = ss;
                ssAdopted = true;
            }
            // check if all the candidate SS of this task has been decoded, if so then stop current decoding task.
            auto candIter = t->ssCandidates.find(ssIdx);
            if (candIter != t->ssCandidates.end())
            {
                candIter->second.frmEnqueued = true;
                bool allCandDecoded = true;
                for (auto& elem : t->ssCandidates)
                {
                    if (!elem.second.frmEnqueued)
                    {
                        allCandDecoded = false;
                        break;
                    }
                }
                t->allCandDecoded = allCandDecoded;
                if (allCandDecoded)
                {
                    m_logger->Log(DEBUG) << "--> Set 'allCandDecoded' of _GopDecodeTask:{ ssidx=[" << t->TaskRange().SsIdx().first << ", "
                        << t->TaskRange().SsIdx().second << "). Also set 'decoderEof'." << endl;
                    t->decoderEof = true;
                    t->demuxerEof = true;  // also stop demuxing task if it's not stopped already
                }
            }
        }
        if (ssAdopted)
            m_pendingVidfrmCnt++;
        else if (ss->avfrm)
        {
            av_frame_free(&ss->avfrm);
            ss->avfrm = nullptr;
        }
        return true;
    }

public:
    class Viewer_Impl : public Viewer
    {
    public:
        Viewer_Impl(Generator_Impl* owner, double wndpos)
            : m_owner(owner)
        {
            m_logger = owner->m_logger;
            UpdateSnapwnd(wndpos, true);
        }

        bool Seek(double pos) override
        {
            UpdateSnapwnd(pos);
            return true;
        }

        double GetCurrWindowPos() const override
        {
            return m_snapwnd.wndpos;
        }

        bool GetSnapshots(double startPos, vector<Image::Holder>& snapshots) override
        {
            // AutoSection _as("GetSs");
            UpdateSnapwnd(startPos);
            auto res = m_owner->GetSnapshots(startPos, snapshots);
            return res;
        }

        Viewer::Holder CreateViewer(double pos) override
        {
            return m_owner->CreateViewer(pos);
        }

        void Release() override
        {
            return m_owner->ReleaseViewer(this);
        }

        MediaParser::Holder GetMediaParser() const override
        {
            return m_owner->GetMediaParser();
        }

        string GetError() const override
        {
            return m_owner->GetError();
        }

        bool UpdateSnapshotTexture(vector<Image::Holder>& snapshots) override
        {
            // AutoSection _as("UpdSsTx");
            // free deprecated textures
            {
                lock_guard<mutex> lktid(m_owner->m_deprecatedTextureLock);
                m_owner->m_deprecatedTextures.clear();
            }

            for (auto& img : snapshots)
            {
                if (img->mTextureReady)
                    continue;
                if (!img->mImgMat.empty())
                {
                    img->mTextureHolder = TextureHolder(new ImTextureID(0), [this] (ImTextureID* pTid) {
                        if (*pTid)
                            ImGui::ImDestroyTexture(*pTid);
                        delete pTid;
                    });
                    ImMatToTexture(img->mImgMat, *(img->mTextureHolder));
                    img->mTextureReady = true;
                }
            }
            return true;
        }

        bool IsTaskRangeChanged() const { return m_taskRangeChanged; }

        list<_GopDecodeTask::Range> CheckTaskRanges()
        {
            lock_guard<mutex> lk(m_taskRangeLock);
            list<_GopDecodeTask::Range> taskRanges(m_taskRanges);
            m_taskRangeChanged = false;
            return std::move(taskRanges);
        }

        void UpdateSnapwnd(double wndpos, bool force = false)
        {
            // AutoSection _as("UpdSnapWnd");
            _SnapWindow snapwnd = m_owner->CreateSnapWindow(wndpos);
            list<_GopDecodeTask::Range> taskRanges;
            bool taskRangeChanged = false;
            if ((force || snapwnd.viewIdx0 != m_snapwnd.viewIdx0 || snapwnd.viewIdx1 != m_snapwnd.viewIdx1) &&
                (snapwnd.seekPos00 != INT64_MIN || snapwnd.seekPos10 != INT64_MIN))
            {
                int32_t buildIdx0 = snapwnd.cacheIdx0 >= 0 ? snapwnd.cacheIdx0 : 0;
                int32_t buildIdx1 = snapwnd.cacheIdx1 <= m_owner->m_vidMaxIndex ? snapwnd.cacheIdx1 : m_owner->m_vidMaxIndex;
                list<GopDecodeTaskHolder> goptskList;
                while (buildIdx0 <= buildIdx1)
                {
                    auto ptsPair = m_owner->GetSeekPosBySsIndex(buildIdx0);
                    auto ssIdxPair = m_owner->CalcSsIndexPairFromPtsPair(ptsPair, buildIdx0);
                    if (ssIdxPair.second <= buildIdx0)
                    {
                        m_logger->Log(WARN) << "Snap window DOESN'T PROCEED! 'buildIdx0'(" << buildIdx0 << ") is NOT INCLUDED in the next 'ssIdxPair'["
                            << ssIdxPair.first << ", " << ssIdxPair.second << ")." << endl;
                        buildIdx0++;
                        continue;
                    }
                    bool isInView = (snapwnd.IsInView(ssIdxPair.first) && m_owner->IsSsIdxValid(ssIdxPair.first)) ||
                                    (snapwnd.IsInView(ssIdxPair.second) && m_owner->IsSsIdxValid(ssIdxPair.second));
                    int32_t distanceToViewWnd = isInView ? 0 : (ssIdxPair.second <= snapwnd.viewIdx0 ?
                            snapwnd.viewIdx0-ssIdxPair.second : ssIdxPair.first-snapwnd.viewIdx1);
                    if (distanceToViewWnd < 0) distanceToViewWnd = -distanceToViewWnd;
                    taskRanges.push_back(_GopDecodeTask::Range(ptsPair, ssIdxPair, isInView, distanceToViewWnd));
                    buildIdx0 = ssIdxPair.second;
                }
                taskRangeChanged = true;
            }
            else if (snapwnd.seekPos00 == INT64_MIN && snapwnd.seekPos10 == INT64_MIN && !m_taskRanges.empty())
            {
                taskRangeChanged = true;
            }
            if (taskRangeChanged || snapwnd.wndpos != m_snapwnd.wndpos)
            {
                m_snapwnd = snapwnd;
                m_logger->Log(DEBUG) << ">>>>> Snapwnd updated: { wndpos=" << snapwnd.wndpos
                    << ", viewIdx=[" << snapwnd.viewIdx0 << ", " << snapwnd.viewIdx1
                    << "], cacheIdx=[" << snapwnd.cacheIdx0 << ", " << snapwnd.cacheIdx1 << "] } <<<<<<<" << endl;
            }
            if (taskRangeChanged)
            {
                m_logger->Log(DEBUG) << ">>>>> Task range list CHANGED <<<<<<<<" << endl << "\t";
                for (auto& range : taskRanges)
                    m_logger->Log(DEBUG) << "[" << range.SsIdx().first << ", " << range.SsIdx().second << "), ";
                m_logger->Log(DEBUG) << endl;
                lock_guard<mutex> lk(m_taskRangeLock);
                m_taskRanges = taskRanges;
                m_taskRangeChanged = true;
            }
        }

    private:
        ALogger* m_logger;
        Generator_Impl* m_owner;
        _SnapWindow m_snapwnd;
        list<_GopDecodeTask::Range> m_taskRanges;
        mutex m_taskRangeLock;
        bool m_taskRangeChanged{false};
    };

private:
    ALogger* m_logger;
    string m_errMsg;

    MediaParser::Holder m_hParser;
    MediaInfo::Holder m_hMediaInfo;
    MediaParser::SeekPointsHolder m_hSeekPoints;
    bool m_opened{false};
    bool m_prepared{false};
    recursive_mutex m_apiLock;
    bool m_quit{false};

    AVFormatContext* m_avfmtCtx{nullptr};
    int m_vidStmIdx{-1};
    int m_audStmIdx{-1};
    AVStream* m_vidStream{nullptr};
    AVStream* m_audStream{nullptr};
    AVCodecPtr m_viddec{nullptr};
    AVCodecContext* m_viddecCtx{nullptr};
    bool m_vidPreferUseHw{true};
    AVHWDeviceType m_vidUseHwType{AV_HWDEVICE_TYPE_NONE};
    AVPixelFormat m_vidHwPixFmt{AV_PIX_FMT_NONE};
    AVHWDeviceType m_viddecDevType{AV_HWDEVICE_TYPE_NONE};
    AVBufferRef* m_viddecHwDevCtx{nullptr};

    // demuxing thread
    thread m_demuxThread;
    uint32_t m_maxPendingTaskCountForDecoding = 8;
    // video decoding thread
    thread m_viddecThread;
    // update snapshots thread
    thread m_updateSsThread;
    // free gop task thread
    thread m_freeGoptskThread;

    int64_t m_vidStartMts{0};
    int64_t m_vidStartPts{0};
    int64_t m_vidDurMts{0};
    int64_t m_vidFrmCnt{0};
    uint32_t m_vidMaxIndex;
    double m_snapWindowSize;
    double m_wndFrmCnt;
    double m_vidfrmIntvMts{0};
    double m_vidfrmIntvMtsHalf{0};
    int64_t m_vidfrmIntvPts{0};
    int64_t m_vidfrmIntvPtsHalf{0};
    double m_ssIntvMts{0};
    double m_ssIntvPts{0};
    double m_cacheFactor{10.0};
    uint32_t m_maxCacheSize{0};
    uint32_t m_prevWndCacheSize;
    list<Viewer::Holder> m_viewers;
    mutex m_viewerListLock;
    list<GopDecodeTaskHolder> m_goptskPrepareList;
    list<GopDecodeTaskHolder> m_goptskList;
    mutex m_goptskListReadLocks[3];
    list<GopDecodeTaskHolder> m_goptskToFree;
    mutex m_goptskFreeLock;
    atomic_int32_t m_pendingVidfrmCnt{0};
    int32_t m_maxPendingVidfrmCnt{2};
    // textures
    list<TextureHolder> m_deprecatedTextures;
    mutex m_deprecatedTextureLock;

    bool m_useRszFactor{false};
    bool m_ssSizeChanged{false};
    float m_ssWFacotr{1.f}, m_ssHFacotr{1.f};
    AVFrameToImMatConverter m_frmCvt;
};

static const auto SNAPSHOT_VIEWER_HOLDER_DELETER = [] (Viewer* p) {
    Generator_Impl::Viewer_Impl* ptr = dynamic_cast<Generator_Impl::Viewer_Impl*>(p);
    delete ptr;
};

Viewer::Holder Generator_Impl::CreateViewer(double pos)
{
    lock_guard<recursive_mutex> lk(m_apiLock);
    Viewer::Holder hViewer(static_cast<Viewer*>(new Viewer_Impl(this, pos)), SNAPSHOT_VIEWER_HOLDER_DELETER);
    {
        lock_guard<mutex> lk(m_viewerListLock);
        m_viewers.push_back(hViewer);
    }
    return hViewer;
}

Generator::Holder Generator::CreateInstance()
{
    return Generator::Holder(static_cast<Generator*>(new Generator_Impl()), [] (Generator* p) {
        Generator_Impl* ptr = dynamic_cast<Generator_Impl*>(p);
        ptr->Close();
        delete ptr;
    });
}

ALogger* GetLogger()
{
    return Logger::GetLogger("Snapshot");
}
}
}

static AVPixelFormat get_hw_format(AVCodecContext *ctx, const AVPixelFormat *pix_fmts)
{
    MediaCore::Snapshot::Generator_Impl* ms = reinterpret_cast<MediaCore::Snapshot::Generator_Impl*>(ctx->opaque);
    const AVPixelFormat *p;
    AVPixelFormat candidateSwfmt = AV_PIX_FMT_NONE;
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL) && candidateSwfmt == AV_PIX_FMT_NONE)
        {
            // save this software format as candidate
            candidateSwfmt = *p;
        }
        if (ms->CheckHwPixFmt(*p))
            return *p;
    }
    return candidateSwfmt;
}