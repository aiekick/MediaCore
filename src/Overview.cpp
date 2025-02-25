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
#include <algorithm>
#include <list>
#include "Overview.h"
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
class Overview_Impl : public Overview
{
public:
    Overview_Impl()
    {
        m_logger = Overview::GetLogger();
    }

    Overview_Impl(const Overview_Impl&) = delete;
    Overview_Impl(Overview_Impl&&) = delete;
    Overview_Impl& operator=(const Overview_Impl&) = delete;

    virtual ~Overview_Impl() {}

    bool Open(const string& url, uint32_t snapshotCount) override
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
        m_ssCount = snapshotCount;
        if (m_vidFrmCnt > 0 && m_vidFrmCnt < snapshotCount)
            m_ssCount = m_vidFrmCnt;
        m_ssIntvMts = (double)m_vidDurMts/m_ssCount;

        BuildSnapshots();
        m_opened = true;
        return true;
    }

    bool Open(MediaParser::Holder hParser, uint32_t snapshotCount) override
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
        m_ssCount = snapshotCount;
        m_ssIntvMts = (double)m_vidDurMts/m_ssCount;

        BuildSnapshots();
        m_opened = true;
        return true;
    }

    MediaParser::Holder GetMediaParser() const override
    {
        return m_hParser;
    }

    void Close() override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        ReleaseResources();

        m_vidStmIdx = -1;
        m_audStmIdx = -1;
        m_hParser = nullptr;
        m_hMediaInfo = nullptr;
        m_opened = false;
        m_errMsg = "";
    }

    bool GetSnapshots(std::vector<ImGui::ImMat>& snapshots) override
    {
        if (!IsOpened())
            return false;

        snapshots.clear();
        for (auto& ss : m_snapshots)
        {
            if (ss.sameFrame)
                snapshots.push_back(m_snapshots[ss.sameAsIndex].img);
            else
                snapshots.push_back(ss.img);
        }

        return true;
    }

    Waveform::Holder GetWaveform() const override
    {
        return m_hWaveform;
    }

    bool SetSingleFramePixels(uint32_t pixels) override
    {
        m_singleFramePixels = pixels;
        return true;
    }

    bool SetFixedAggregateSamples(double aggregateSamples) override
    {
        if (aggregateSamples < 1)
        {
            m_errMsg = "Argument 'aggregateSamples' must be larger than 1!";
            return false;
        }
        m_fixedAggregateSamples = aggregateSamples;
        return true;
    }

    bool IsOpened() const override
    {
        return m_opened;
    }

    bool IsDone() const override
    {
        return m_genSsEof;
    }

    bool HasVideo() const override
    {
        return m_vidStmIdx >= 0;
    }

    bool HasAudio() const override
    {
        return m_audStmIdx >= 0;
    }

    uint32_t GetSnapshotCount() const override
    {
        if (!IsOpened())
            return 0;
        return m_ssCount;
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
        RebuildSnapshots();
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

            uint32_t outWidth = (uint32_t)ceil(m_vidAvStm->codecpar->width*widthFactor);
            if ((outWidth&0x1) == 1)
                outWidth++;
            uint32_t outHeight = (uint32_t)ceil(m_vidAvStm->codecpar->height*heightFactor);
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
        RebuildSnapshots();
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
        RebuildSnapshots();
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
        if (m_vidAvStm)
        {
            return m_vidAvStm->codecpar->width;
        }
        return 0;
    }

    uint32_t GetVideoHeight() const override
    {
        if (m_vidAvStm)
        {
            return m_vidAvStm->codecpar->height;
        }
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

    uint32_t GetAudioChannel() const override
    {
        if (!HasAudio())
            return 0;
        int ch;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        ch = m_audAvStm->codecpar->channels;
#else
        ch = m_audAvStm->codecpar->ch_layout.nb_channels;
#endif
        return ch >= 0 ? (uint32_t)ch : 0;
    }

    uint32_t GetAudioSampleRate() const override
    {
        if (!HasAudio())
            return 0;
        return m_audAvStm->codecpar->sample_rate;
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

private:
    struct Snapshot
    {
        uint32_t index{0};
        bool sameFrame{false};
        uint32_t sameAsIndex{0};
        int64_t ssFrmPts{INT64_MIN};
        ImGui::ImMat img;
    };

    string FFapiFailureMessage(const string& apiName, int fferr)
    {
        ostringstream oss;
        oss << "FF api '" << apiName << "' returns error! fferr=" << fferr << ".";
        return oss.str();
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
        if (m_vidStmIdx < 0 && m_audStmIdx < 0)
        {
            ostringstream oss;
            oss << "Neither video nor audio stream can be found in '" << m_avfmtCtx->url << "'.";
            m_errMsg = oss.str();
            return false;
        }

        m_vidfrmIntvTs = 0;
        if (HasVideo())
        {
            VideoStream* vidStream = dynamic_cast<VideoStream*>(m_hMediaInfo->streams[m_vidStmIdx].get());
            m_isImage = vidStream->isImage;
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
            m_vidfrmIntvTs = av_q2d(av_inv_q(frameRate));

            if (m_isImage)
                m_frmCvt.SetUseVulkanConverter(false);
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
        }

        if (HasAudio())
        {
            AudioStream* audStream = dynamic_cast<AudioStream*>(m_hMediaInfo->streams[m_audStmIdx].get());
            Waveform::Holder hWaveform(new Waveform);
            // default 'm_aggregateSamples' value is 9.6, which is calculated as
            // 48kHz audio & 25fps video, 200 pixels in the UI between two adjacent snapshots.
            // video-frame-interval = 40 ms,
            // in 40ms there are 48000*0.04 = 1920 pcm samples,
            // 200 pixels for displaying 1920 samples,
            // so aggregate 1920 samples to 200 results in 1920/200 = 9.6
            double vidfrmIntvTs = m_vidfrmIntvTs > 0 ? m_vidfrmIntvTs : 0.04;
            if (m_fixedAggregateSamples > 0)
                hWaveform->aggregateSamples = m_fixedAggregateSamples;
            else
            {
                hWaveform->aggregateSamples = audStream->sampleRate*vidfrmIntvTs/m_singleFramePixels;
                if (hWaveform->aggregateSamples < m_minAggregateSamples)
                    hWaveform->aggregateSamples = m_minAggregateSamples;
            }
            hWaveform->aggregateDuration = hWaveform->aggregateSamples/audStream->sampleRate;
            uint32_t waveformSamples = (uint32_t)ceil(audStream->duration/hWaveform->aggregateDuration);
            if (audStream->channels > 1)
                hWaveform->pcm.resize(2);
            else
                hWaveform->pcm.resize(1);
            for (auto& chpcm : hWaveform->pcm)
                chpcm.resize(waveformSamples, 0);
            m_hWaveform = hWaveform;
        }

        return true;
    }

    bool Prepare()
    {
        int fferr;
        fferr = avformat_find_stream_info(m_avfmtCtx, nullptr);
        if (fferr < 0)
        {
            m_errMsg = FFapiFailureMessage("avformat_open_input", fferr);
            return false;
        }

        bool openVideoFailed = true;
        if (HasVideo())
        {
            m_vidAvStm = m_avfmtCtx->streams[m_vidStmIdx];

            m_viddecOpenOpts.onlyUseSoftwareDecoder = !m_vidPreferUseHw;
            FFUtils::OpenVideoDecoderResult res;
            if (FFUtils::OpenVideoDecoder(m_avfmtCtx, -1, &m_viddecOpenOpts, &res))
            {
                m_viddecCtx = res.decCtx;
                openVideoFailed = false;
            }
            else
            {
                ostringstream oss;
                oss << "Open video decoder FAILED! Error is '" << res.errMsg << "'.";
                m_errMsg = oss.str();
                m_vidStmIdx = -1;
            }
        }
        m_decodeVideo = !openVideoFailed;

        bool openAudioFailed = true;
        if (HasAudio())
        {
            m_audAvStm = m_avfmtCtx->streams[m_audStmIdx];

            m_auddec = avcodec_find_decoder(m_audAvStm->codecpar->codec_id);
            if (m_auddec == nullptr)
            {
                ostringstream oss;
                oss << "Can not find audio decoder by codec_id " << m_audAvStm->codecpar->codec_id << "!";
                if (openVideoFailed)
                    m_errMsg = m_errMsg+" "+oss.str();
                else
                    m_errMsg = oss.str();
            }
            else if (OpenAudioDecoder())
                openAudioFailed = false;
        }
        m_decodeAudio = !openAudioFailed;

        if (openVideoFailed && openAudioFailed)
            return false;

        m_prepared = true;
        return true;
    }

    bool OpenAudioDecoder()
    {
        m_auddecCtx = avcodec_alloc_context3(m_auddec);
        if (!m_auddecCtx)
        {
            m_errMsg = "FAILED to allocate new AVCodecContext!";
            return false;
        }

        int fferr;
        fferr = avcodec_parameters_to_context(m_auddecCtx, m_audAvStm->codecpar);
        if (fferr < 0)
        {
            m_errMsg = FFapiFailureMessage("avcodec_parameters_to_context", fferr);
            return false;
        }

        fferr = avcodec_open2(m_auddecCtx, m_auddec, nullptr);
        if (fferr < 0)
        {
            m_errMsg = FFapiFailureMessage("avcodec_open2", fferr);
            return false;
        }
        m_logger->Log(DEBUG) << "Audio decoder '" << m_auddec->name << "' opened." << endl;

        // setup sw resampler
        int inSampleRate = m_audAvStm->codecpar->sample_rate;
        AVSampleFormat inSmpfmt = (AVSampleFormat)m_audAvStm->codecpar->format;
        m_swrOutSampleRate = inSampleRate;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        int inChannels = m_audAvStm->codecpar->channels;
        uint64_t inChnLyt = m_audAvStm->codecpar->channel_layout;
        m_swrOutChannels = inChannels > 2 ? 2 : inChannels;
        m_swrOutChnLyt = av_get_default_channel_layout(m_swrOutChannels);
        if (inChnLyt <= 0)
            inChnLyt = av_get_default_channel_layout(inChannels);
        if (m_swrOutChnLyt != inChnLyt || m_swrOutSmpfmt != inSmpfmt || m_swrOutSampleRate != inSampleRate)
        {
            m_swrCtx = swr_alloc_set_opts(NULL, m_swrOutChnLyt, m_swrOutSmpfmt, m_swrOutSampleRate, inChnLyt, inSmpfmt, inSampleRate, 0, nullptr);
            if (!m_swrCtx)
#else
        auto& inChlyt = m_audAvStm->codecpar->ch_layout;
        if (inChlyt.nb_channels <= 2)
            m_swrOutChlyt = inChlyt;
        else
            av_channel_layout_default(&m_swrOutChlyt, 2);
        if (av_channel_layout_compare(&m_swrOutChlyt, &inChlyt) || m_swrOutSmpfmt != inSmpfmt || m_swrOutSampleRate != inSampleRate)
        {
            fferr = swr_alloc_set_opts2(&m_swrCtx, &m_swrOutChlyt, m_swrOutSmpfmt, m_swrOutSampleRate, &inChlyt, inSmpfmt, inSampleRate, 0, nullptr);
            if (fferr < 0)
#endif
            {
                m_errMsg = "FAILED to invoke 'swr_alloc_set_opts()' to create 'SwrContext'!";
                return false;
            }
            int fferr = swr_init(m_swrCtx);
            if (fferr < 0)
            {
                m_errMsg = FFapiFailureMessage("swr_init", fferr);
                return false;
            }
            m_swrPassThrough = false;
        }
        else
        {
            m_swrPassThrough = true;
        }
        return true;
    }

    void BuildSnapshots()
    {
        m_snapshots.clear();
        for (uint32_t i = 0; i < m_ssCount; i++)
        {
            Snapshot ss;
            ss.index = i;
            ss.img.time_stamp = (m_ssIntvMts*i+m_vidStartMts)/1000.;
            m_snapshots.push_back(ss);
        }
        StartAllThreads();
    }

    void StartAllThreads()
    {
        string fileName = SysUtils::ExtractFileName(m_hParser->GetUrl());
        ostringstream thnOss;
        m_quit = false;
        if (HasVideo())
        {
            m_demuxVidThread = thread(&Overview_Impl::DemuxVideoThreadProc, this);
            thnOss.str(""); thnOss << "OvwVdmx-" << fileName;
            SysUtils::SetThreadName(m_demuxVidThread, thnOss.str());
            m_viddecThread = thread(&Overview_Impl::VideoDecodeThreadProc, this);
            thnOss.str(""); thnOss << "OvwVdc-" << fileName;
            SysUtils::SetThreadName(m_viddecThread, thnOss.str());
            m_genSsThread = thread(&Overview_Impl::GenerateSsThreadProc, this);
            thnOss.str(""); thnOss << "OvwGss-" << fileName;
            SysUtils::SetThreadName(m_genSsThread, thnOss.str());
        }
        if (HasAudio())
        {
            m_demuxAudThread = thread(&Overview_Impl::DemuxAudioThreadProc, this);
            thnOss.str(""); thnOss << "OvwAdmx-" << fileName;
            SysUtils::SetThreadName(m_demuxAudThread, thnOss.str());
            m_auddecThread = thread(&Overview_Impl::AudioDecodeThreadProc, this);
            thnOss.str(""); thnOss << "OvwAdc-" << fileName;
            SysUtils::SetThreadName(m_auddecThread, thnOss.str());
            m_genWfThread = thread(&Overview_Impl::GenWaveformThreadProc, this);
            thnOss.str(""); thnOss << "OvwGwf-" << fileName;
            SysUtils::SetThreadName(m_genWfThread, thnOss.str());
        }
        m_releaseThread = thread(&Overview_Impl::ReleaseResourceProc, this);
    }

    void WaitAllThreadsQuit(bool callFromReleaseProc = false)
    {
        m_quit = true;
        if (!callFromReleaseProc && m_releaseThread.joinable())
        {
            m_releaseThread.join();
            m_releaseThread = thread();
        }
        if (m_demuxVidThread.joinable())
        {
            m_demuxVidThread.join();
            m_demuxVidThread = thread();
        }
        if (m_viddecThread.joinable())
        {
            m_viddecThread.join();
            m_viddecThread = thread();
        }
        if (m_genSsThread.joinable())
        {
            m_genSsThread.join();
            m_genSsThread = thread();
        }
        if (m_demuxAudThread.joinable())
        {
            m_demuxAudThread.join();
            m_demuxAudThread = thread();
        }
        if (m_auddecThread.joinable())
        {
            m_auddecThread.join();
            m_auddecThread = thread();
        }
        if (m_genWfThread.joinable())
        {
            m_genWfThread.join();
            m_genWfThread = thread();
        }
    }

    void FlushAllQueues()
    {
        for (AVPacket* avpkt : m_vidpktQ)
            av_packet_free(&avpkt);
        m_vidpktQ.clear();
        for (AVPacket* avpkt : m_audpktQ)
            av_packet_free(&avpkt);
        m_audpktQ.clear();
        for (AVFrame* avfrm : m_vidfrmQ)
            av_frame_free(&avfrm);
        m_vidfrmQ.clear();
        for (AVFrame* avfrm : m_audfrmQ)
            av_frame_free(&avfrm);
        m_audfrmQ.clear();
    }

    void RebuildSnapshots()
    {
        if (!IsOpened())
            return;

        WaitAllThreadsQuit();
        FlushAllQueues();
        if (m_viddecCtx)
            avcodec_flush_buffers(m_viddecCtx);
        if (m_auddecCtx)
            avcodec_flush_buffers(m_auddecCtx);
        BuildSnapshots();
    }

    void DemuxVideoThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter DemuxVideoThreadProc()..." << endl;

        if (!m_prepared && !Prepare())
        {
            m_logger->Log(Error) << "Prepare() FAILED for url '" << m_hParser->GetUrl() << "'! Error is '" << m_errMsg << "'." << endl;
            return;
        }
        if (!m_decodeVideo)
        {
            m_demuxVidEof = true;
            return;
        }

        AVPacket avpkt = {0};
        bool avpktLoaded = false;
        while (!m_quit)
        {
            bool idleLoop = true;

            if (HasVideo())
            {
                auto iter = find_if(m_snapshots.begin(), m_snapshots.end(), [](const Snapshot& ss) {
                    return ss.ssFrmPts == INT64_MIN;
                });
                if (iter == m_snapshots.end())
                    break;

                Snapshot& ss = *iter;
                int fferr;
                if (!m_isImage)
                {
                    int64_t seekTargetPts = ss.ssFrmPts != INT64_MIN ? ss.ssFrmPts :
                        av_rescale_q((int64_t)(m_ssIntvMts*ss.index+m_vidStartMts), MILLISEC_TIMEBASE, m_vidAvStm->time_base);
                    fferr = avformat_seek_file(m_avfmtCtx, m_vidStmIdx, INT64_MIN, seekTargetPts, seekTargetPts, 0);
                    if (fferr < 0)
                    {
                        m_logger->Log(Error) << "avformat_seek_file() FAILED for seeking to pts(" << seekTargetPts << ")! fferr = " << fferr << "!" << endl;
                        break;
                    }
                }

                bool enqDone = false;
                while (!m_quit && !enqDone)
                {
                    bool idleLoop2 = true;
                    if (!avpktLoaded)
                    {
                        int fferr = av_read_frame(m_avfmtCtx, &avpkt);
                        if (fferr == 0)
                        {
                            avpktLoaded = true;
                            idleLoop = idleLoop2 = false;
                            ss.ssFrmPts = avpkt.pts;
                            auto iter2 = iter;
                            if (avpkt.stream_index == m_vidStmIdx && iter2 != m_snapshots.begin())
                            {
                                iter2--;
                                if (iter2->ssFrmPts == ss.ssFrmPts)
                                {
                                    ss.sameFrame = true;
                                    ss.sameAsIndex = iter2->sameFrame ? iter2->sameAsIndex : iter2->index;
                                    av_packet_unref(&avpkt);
                                    avpktLoaded = false;
                                    enqDone = true;
                                }
                            }
                        }
                        else
                        {
                            if (fferr != AVERROR_EOF)
                                m_logger->Log(Error) << "Demuxer ERROR! 'av_read_frame()' returns " << fferr << "." << endl;
                            break;
                        }
                    }

                    if (avpktLoaded)
                    {
                        if (avpkt.stream_index == m_vidStmIdx)
                        {
                            if (m_vidpktQ.size() < m_vidpktQMaxSize)
                            {
                                AVPacket* enqpkt = av_packet_clone(&avpkt);
                                if (!enqpkt)
                                {
                                    m_logger->Log(Error) << "FAILED to invoke 'av_packet_clone(DemuxVideoThreadProc)'!" << endl;
                                    break;
                                }
                                {
                                    lock_guard<mutex> lk(m_vidpktQLock);
                                    m_vidpktQ.push_back(enqpkt);
                                }
                                av_packet_unref(&avpkt);
                                avpktLoaded = false;
                                idleLoop = idleLoop2 = false;
                                enqDone = true;
                            }
                        }
                        else
                        {
                            av_packet_unref(&avpkt);
                            avpktLoaded = false;
                        }
                    }
                    if (idleLoop2)
                        this_thread::sleep_for(chrono::milliseconds(5));
                }
            }
            else
            {
                m_logger->Log(Error) << "Demux procedure to non-video media is NOT IMPLEMENTED yet!" << endl;
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        if (avpktLoaded)
            av_packet_unref(&avpkt);
        m_demuxVidEof = true;
        m_logger->Log(DEBUG) << "Leave DemuxVideoThreadProc()." << endl;
    }

    void VideoDecodeThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter VideoDecodeThreadProc()..." << endl;

        while (!m_prepared && !m_quit)
            this_thread::sleep_for(chrono::milliseconds(5));
        if (m_quit || !m_decodeVideo)
        {
            m_viddecEof = true;
            return;
        }

        AVFrame avfrm = {0};
        bool avfrmLoaded = false;
        bool inputEof = false;
        while (!m_quit)
        {
            bool idleLoop = true;
            bool quitLoop = false;

            // retrieve output frame
            bool hasOutput;
            do{
                bool idleLoop2 = true;
                if (!avfrmLoaded)
                {
                    int fferr = avcodec_receive_frame(m_viddecCtx, &avfrm);
                    if (fferr == 0)
                    {
                        // m_logger->Log(DEBUG) << "<<< Get video frame pts=" << avfrm.pts << "(" << MillisecToString(av_rescale_q(avfrm.pts, m_vidAvStm->time_base, MILLISEC_TIMEBASE)) << ")." << endl;
                        avfrmLoaded = true;
                        idleLoop = idleLoop2 = false;
                    }
                    else if (fferr != AVERROR(EAGAIN))
                    {
                        if (fferr != AVERROR_EOF)
                        {
                            m_logger->Log(Error) << "FAILED to invoke 'avcodec_receive_frame'(VideoDecodeThreadProc)! return code is "
                                << fferr << "." << endl;
                        }
                        else
                        {
                            m_logger->Log(VERBOSE) << "---> EOF received" << endl;
                        }
                        quitLoop = true;
                    }
                }

                hasOutput = avfrmLoaded;
                if (avfrmLoaded && m_vidfrmQ.size() < m_vidfrmQMaxSize)
                {
                    AVFrame* enqfrm = av_frame_clone(&avfrm);
                    {
                        lock_guard<mutex> lk(m_vidfrmQLock);
                        m_vidfrmQ.push_back(enqfrm);
                    }
                    av_frame_unref(&avfrm);
                    avfrmLoaded = false;
                    idleLoop = idleLoop2 = false;
                }

                if (idleLoop2)
                    this_thread::sleep_for(chrono::milliseconds(5));
            } while (hasOutput && !m_quit);
            if (quitLoop)
                break;

            // input packet to decoder
            if (!inputEof)
            {
                if (!m_vidpktQ.empty())
                {
                    AVPacket* avpkt = m_vidpktQ.front();
                    int fferr = avcodec_send_packet(m_viddecCtx, avpkt);
                    if (fferr == 0)
                    {
                        // m_logger->Log(DEBUG) << ">>> Send video packet pts=" << avpkt->pts << "(" << MillisecToString(av_rescale_q(avpkt->pts, m_vidAvStm->time_base, MILLISEC_TIMEBASE))
                        //     << "), size=" << avpkt->size << "." << endl;
                        {
                            lock_guard<mutex> lk(m_vidpktQLock);
                            m_vidpktQ.pop_front();
                        }
                        av_packet_free(&avpkt);
                        idleLoop = false;
                    }
                    else if (fferr != AVERROR(EAGAIN))
                    {
                        m_logger->Log(WARN) << "FAILED to invoke 'avcodec_send_packet'(VideoDecodeThreadProc)! return code is "
                            << fferr << ". url = '" << m_hParser->GetUrl() << "'." << endl;
                        {
                            lock_guard<mutex> lk(m_vidpktQLock);
                            m_vidpktQ.pop_front();
                        }
                        av_packet_free(&avpkt);
                    }
                }
                else if (m_demuxVidEof)
                {
                    m_logger->Log(VERBOSE) << "---------------------------> send nullptr <--------------------------" << endl;
                    avcodec_send_packet(m_viddecCtx, nullptr);
                    inputEof = true;
                }
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        m_viddecEof = true;
        m_logger->Log(DEBUG) << "Leave VideoDecodeThreadProc()." << endl;
    }

    void GenerateSsThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter GenerateSsThreadProc()." << endl;
        while (!m_quit)
        {
            bool idleLoop = true;

            if (!m_vidfrmQ.empty())
            {
                AVFrame* frm = m_vidfrmQ.front();
                {
                    lock_guard<mutex> lk(m_vidfrmQLock);
                    m_vidfrmQ.pop_front();
                }

                double ts = (double)av_rescale_q(frm->pts, m_vidAvStm->time_base, MILLISEC_TIMEBASE)/1000.;
                auto iter = find_if(m_snapshots.begin(), m_snapshots.end(), [frm](const Snapshot& ss){
                    return ss.ssFrmPts == frm->pts;
                });
                if (iter != m_snapshots.end())
                {
                    if (!m_frmCvt.ConvertImage(frm, iter->img, ts))
                        m_logger->Log(Error) << "FAILED to convert AVFrame to ImGui::ImMat! Message is '" << m_frmCvt.GetError() << "'." << endl;
                    // else
                    //     m_logger->Log(DEBUG) << "Add SS#" << iter->index << "." << endl;
                }
                else
                {
                    m_logger->Log(WARN) << "Discard AVFrame with pts=" << frm->pts << "(ts=" << ts << ")!";
                }

                av_frame_free(&frm);
                idleLoop = false;
            }
            else if (m_viddecEof)
                break;

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(5));
        }

        auto iter = find_if(m_snapshots.begin(), m_snapshots.end(), [](const Snapshot& ss) {
            return ss.ssFrmPts == INT64_MIN;
        });
        if (iter != m_snapshots.begin())
        {
            auto iter2 = iter;
            iter2--;
            while (iter != m_snapshots.end())
            {
                iter->sameFrame = true;
                iter->sameAsIndex = iter2->sameFrame ? iter2->sameAsIndex : iter2->index;
                iter++;
                iter2++;
            }
        }
        else
        {
            iter++;
            while (iter != m_snapshots.end())
            {
                iter->sameFrame = true;
                iter->sameAsIndex = 0;
                iter++;
            }
        }
        m_genSsEof = true;
        m_logger->Log(DEBUG) << "Leave GenerateSsThreadProc()." << endl;
    }

    void DemuxAudioThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter DemuxAudioThreadProc()..." << endl;

        if (!HasVideo() && !m_prepared && !Prepare())
        {
            m_logger->Log(Error) << "Prepare() FAILED! Error is '" << m_errMsg << "'." << endl;
            return;
        }
        else
        {
            while (!m_prepared && !m_quit)
                this_thread::sleep_for(chrono::milliseconds(5));
        }
        if (m_quit || !m_decodeAudio)
        {
            m_demuxAudEof = true;
            return;
        }

        int fferr;
        AVFormatContext* avfmtCtx = nullptr;
        fferr = avformat_open_input(&avfmtCtx, m_hParser->GetUrl().c_str(), nullptr, nullptr);
        if (fferr)
        {
            m_logger->Log(Error) << "'avformat_open_input' FAILED with return code " << fferr << "! Quit Waveform demux thread." << endl;
            return;
        }

        AVPacket avpkt = {0};
        bool avpktLoaded = false;
        while (!m_quit)
        {
            bool idleLoop = true;

            if (!avpktLoaded)
            {
                int fferr = av_read_frame(avfmtCtx, &avpkt);
                if (fferr == 0)
                {
                    avpktLoaded = true;
                    idleLoop = false;
                }
                else
                {
                    if (fferr != AVERROR_EOF)
                        m_logger->Log(Error) << "Demuxer ERROR! 'av_read_frame(DemuxAudioThreadProc)' returns " << fferr << "." << endl;
                    break;
                }
            }

            if (avpktLoaded)
            {
                if (avpkt.stream_index == m_audStmIdx)
                {
                    if (m_audpktQ.size() < m_audpktQMaxSize)
                    {
                        AVPacket* enqpkt = av_packet_clone(&avpkt);
                        if (!enqpkt)
                        {
                            m_logger->Log(Error) << "FAILED to invoke 'av_packet_clone(DemuxAudioThreadProc)'!" << endl;
                            break;
                        }
                        {
                            lock_guard<mutex> lk(m_audpktQLock);
                            m_audpktQ.push_back(enqpkt);
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

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(1));
        }
        if (avpktLoaded)
            av_packet_unref(&avpkt);
        if (avfmtCtx)
            avformat_close_input(&avfmtCtx);
        m_demuxAudEof = true;
        m_logger->Log(DEBUG) << "Leave DemuxAudioThreadProc()." << endl;
    }

    void AudioDecodeThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter AudioDecodeThreadProc()..." << endl;

        while (!m_prepared && !m_quit)
            this_thread::sleep_for(chrono::milliseconds(5));
        if (m_quit || !m_decodeAudio)
        {
            m_auddecEof = true;
            return;
        }

        AVFrame avfrm = {0};
        bool avfrmLoaded = false;
        bool inputEof = false;
        while (!m_quit)
        {
            bool idleLoop = true;
            bool quitLoop = false;

            // retrieve output frame
            bool hasOutput;
            do{
                if (!avfrmLoaded)
                {
                    int fferr = avcodec_receive_frame(m_auddecCtx, &avfrm);
                    if (fferr == 0)
                    {
                        avfrmLoaded = true;
                        idleLoop = false;
                        // update average audio frame duration, for calculating audio queue size
                        double frmDur = (double)avfrm.nb_samples/m_audAvStm->codecpar->sample_rate;
                        m_audfrmAvgDur = (m_audfrmAvgDur*(m_audfrmAvgDurCalcCnt-1)+frmDur)/m_audfrmAvgDurCalcCnt;
                        m_audfrmQMaxSize = (int)ceil(m_audQDuration/m_audfrmAvgDur);
                    }
                    else if (fferr != AVERROR(EAGAIN))
                    {
                        if (fferr != AVERROR_EOF)
                            m_logger->Log(Error) << "FAILED to invoke 'avcodec_receive_frame'(AudioDecodeThreadProc)! return code is "
                                << fferr << "." << endl;
                        quitLoop = true;
                        break;
                    }
                }

                hasOutput = avfrmLoaded;
                if (avfrmLoaded)
                {
                    if (m_audfrmQ.size() < m_audfrmQMaxSize)
                    {
                        lock_guard<mutex> lk(m_audfrmQLock);
                        AVFrame* enqfrm = av_frame_clone(&avfrm);
                        m_audfrmQ.push_back(enqfrm);
                        av_frame_unref(&avfrm);
                        avfrmLoaded = false;
                        idleLoop = false;
                    }
                    else
                        break;
                }
            } while (hasOutput);
            if (quitLoop)
                break;

            // input packet to decoder
            if (!inputEof)
            {
                if (!m_audpktQ.empty())
                {
                    while (!m_audpktQ.empty())
                    {
                        AVPacket* avpkt = m_audpktQ.front();
                        int fferr = avcodec_send_packet(m_auddecCtx, avpkt);
                        if (fferr == 0)
                        {
                            lock_guard<mutex> lk(m_audpktQLock);
                            m_audpktQ.pop_front();
                            av_packet_free(&avpkt);
                            idleLoop = false;
                        }
                        else
                        {
                            if (fferr != AVERROR(EAGAIN))
                            {
                                m_logger->Log(Error) << "FAILED to invoke 'avcodec_send_packet'(AudioDecodeThreadProc)! return code is "
                                    << fferr << "." << endl;
                                quitLoop = true;
                            }
                            break;
                        }
                    }
                    if (quitLoop)
                        break;
                }
                else
                {
                    if (m_demuxAudEof)
                    {
                        avcodec_send_packet(m_auddecCtx, nullptr);
                        idleLoop = false;
                        inputEof = true;
                    }
                    // m_logger->Log(DEBUG) << "Audio pkt Q is empty!" << endl;
                }
            }

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(1));
        }
        m_auddecEof = true;
        if (avfrmLoaded)
            av_frame_unref(&avfrm);
        m_logger->Log(DEBUG) << "Leave AudioDecodeThreadProc()." << endl;
    }

    void GenWaveformThreadProc()
    {
        m_logger->Log(DEBUG) << "Enter GenWaveformThreadProc()..." << endl;

        while (!m_prepared && !m_quit)
            this_thread::sleep_for(chrono::milliseconds(5));
        if (m_quit)
            return;

        double wfStep = 0;
        double wfAggsmpCnt = m_hWaveform->aggregateSamples;
        uint32_t wfIdx = 0;
        uint32_t wfSize = m_hWaveform->pcm[0].size();
        vector<float>* wf1 = &m_hWaveform->pcm[0];
        vector<float>* wf2 = nullptr;
        float minSmp{1.f}, maxSmp{-1.f};
        if (m_hWaveform->pcm.size() > 1)
            wf2 = &m_hWaveform->pcm[1];
        while (!m_quit && wfIdx < wfSize)
        {
            bool idleLoop = true;
            if (!m_audfrmQ.empty())
            {
                AVFrame* srcfrm = m_audfrmQ.front();
                AVFrame* dstfrm = nullptr;
                if (m_swrPassThrough)
                {
                    dstfrm = srcfrm;
                }
                else
                {
                    dstfrm = av_frame_alloc();
                    if (!dstfrm)
                    {
                        m_logger->Log(Error) << "FAILED to allocate new AVFrame for 'swr_convert()'!" << endl;
                        break;
                    }
                    dstfrm->format = (int)m_swrOutSmpfmt;
                    dstfrm->sample_rate = m_swrOutSampleRate;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
                    dstfrm->channels = m_swrOutChannels;
                    dstfrm->channel_layout = m_swrOutChnLyt;
#else
                    dstfrm->ch_layout = m_swrOutChlyt;
#endif
                    dstfrm->nb_samples = swr_get_out_samples(m_swrCtx, srcfrm->nb_samples);
                    int fferr = av_frame_get_buffer(dstfrm, 0);
                    if (fferr < 0)
                    {
                        m_logger->Log(Error) << "av_frame_get_buffer(GenWaveformThreadProc) FAILED with return code " << fferr << endl;
                        break;
                    }
                    av_frame_copy_props(dstfrm, srcfrm);
                    dstfrm->pts = swr_next_pts(m_swrCtx, srcfrm->pts);
                    fferr = swr_convert(m_swrCtx, dstfrm->data, dstfrm->nb_samples, (const uint8_t **)srcfrm->data, srcfrm->nb_samples);
                    if (fferr < 0)
                    {
                        m_logger->Log(Error) << "swr_convert(GenWaveformThreadProc) FAILED with return code " << fferr << endl;
                        break;
                    }
                }
                {
                    lock_guard<mutex> lk(m_audfrmQLock);
                    m_audfrmQ.pop_front();
                }

                float* ch1ptr = (float*)dstfrm->data[0];
                float chMaxWf, chMinWf;
                chMaxWf = -1.f; chMinWf = 1.f;
                double currWfStep = wfStep;
                uint32_t currWfIdx = wfIdx;
                for (int i = 0; i < dstfrm->nb_samples; i++)
                {
                    float chVal = *ch1ptr++;
                    if (chMaxWf < chVal)
                    {
                        chMaxWf = chVal;
                        if (maxSmp < chVal)
                            maxSmp = chVal;
                    }
                    if (chMinWf > chVal)
                    {
                        chMinWf = chVal;
                        if (minSmp > chVal)
                            minSmp = chVal;
                    }

                    currWfStep++;
                    if (currWfStep >= wfAggsmpCnt)
                    {
                        currWfStep -= wfAggsmpCnt;
                        (*wf1)[currWfIdx] = abs(chMaxWf) > abs(chMinWf) ? chMaxWf : chMinWf;
                        currWfIdx++;
                        if (currWfIdx >= wfSize)
                            break;
                        chMaxWf = -1.f; chMinWf = 1.f;
                    }
                }
                int dstCh;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
                dstCh = dstfrm->channels;
#else
                dstCh = dstfrm->ch_layout.nb_channels;
#endif
                float* ch2ptr = dstCh > 1 && wf2 ? (float*)dstfrm->data[1] : nullptr;
                if (ch2ptr)
                {
                    chMaxWf = -1.f; chMinWf = 1.f;
                    currWfStep = wfStep;
                    currWfIdx = wfIdx;
                    for (int i = 0; i < dstfrm->nb_samples; i++)
                    {
                        float chVal = *ch2ptr++;
                        if (chMaxWf < chVal)
                        {
                            chMaxWf = chVal;
                            if (maxSmp < chVal)
                                maxSmp = chVal;
                        }
                        if (chMinWf > chVal)
                        {
                            chMinWf = chVal;
                            if (minSmp > chVal)
                                minSmp = chVal;
                        }

                        currWfStep++;
                        if (currWfStep >= wfAggsmpCnt)
                        {
                            currWfStep -= wfAggsmpCnt;
                            (*wf2)[currWfIdx] = abs(chMaxWf) > abs(chMinWf) ? chMaxWf : chMinWf;
                            currWfIdx++;
                            if (currWfIdx >= wfSize)
                                break;
                            chMaxWf = -1.f; chMinWf = 1.f;
                        }
                    }
                }
                wfStep = currWfStep;
                wfIdx = currWfIdx;
                m_hWaveform->maxSample = maxSmp;
                m_hWaveform->minSample = minSmp;

                if (dstfrm != srcfrm)
                    av_frame_free(&dstfrm);
                av_frame_free(&srcfrm);
                idleLoop = false;
            }
            else if (m_auddecEof)
                break;

            if (idleLoop)
                this_thread::sleep_for(chrono::milliseconds(1));
        }
        m_genWfEof = true;
        m_logger->Log(DEBUG) << "Leave GenWaveformThreadProc(), " << wfIdx << " samples generated." << endl;
    }

    void ReleaseResources(bool callFromReleaseProc = false)
    {
        WaitAllThreadsQuit(callFromReleaseProc);
        FlushAllQueues();

        if (m_swrCtx)
        {
            swr_free(&m_swrCtx);
            m_swrCtx = nullptr;
        }
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        m_swrOutChannels = 0;
        m_swrOutChnLyt = 0;
#else
        m_swrOutChlyt = {AV_CHANNEL_ORDER_UNSPEC, 0};
#endif
        m_swrOutSampleRate = 0;
        m_swrPassThrough = false;
        if (m_auddecCtx)
        {
            avcodec_free_context(&m_auddecCtx);
            m_auddecCtx = nullptr;
        }
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
        m_vidAvStm = nullptr;
        m_audAvStm = nullptr;
        m_auddec = nullptr;

        m_demuxVidEof = false;
        m_viddecEof = false;
        m_genSsEof = false;
        m_demuxAudEof = false;
        m_auddecEof = false;
        m_genWfEof = false;
        m_prepared = false;
    }

    void ReleaseResourceProc()
    {
        while (!m_quit)
        {
            if (!m_prepared || (m_viddecCtx && !m_genSsEof) || (m_auddecCtx && !m_genWfEof))
                this_thread::sleep_for(chrono::milliseconds(100));
            else
                break;
        }
        if (!m_quit)
        {
            bool lockAquired = false;
            while (!(lockAquired = m_apiLock.try_lock()) && !m_quit)
                this_thread::sleep_for(chrono::milliseconds(5));
            if (m_quit)
            {
                if (lockAquired) m_apiLock.unlock();
                return;
            }
            lock_guard<recursive_mutex> lk(m_apiLock, adopt_lock);
            m_logger->Log(DEBUG) << "AUTO RELEASE decoding resources." << endl;
            ReleaseResources(true);
        }
    }

private:
    ALogger* m_logger;
    string m_errMsg;
    bool m_opened{false};
    bool m_vidPreferUseHw{true};
    AVHWDeviceType m_vidUseHwType{AV_HWDEVICE_TYPE_NONE};

    MediaParser::Holder m_hParser;
    MediaInfo::Holder m_hMediaInfo;

    AVFormatContext* m_avfmtCtx{nullptr};
    bool m_prepared{false};
    int m_vidStmIdx{-1};
    int m_audStmIdx{-1};
    bool m_isImage{false};
    AVStream* m_vidAvStm{nullptr};
    AVStream* m_audAvStm{nullptr};
    bool m_decodeVideo{false};
    bool m_decodeAudio{false};
    AVCodecPtr m_auddec{nullptr};
    FFUtils::OpenVideoDecoderOptions m_viddecOpenOpts;
    AVCodecContext* m_viddecCtx{nullptr};
    AVCodecContext* m_auddecCtx{nullptr};
    AVPixelFormat m_vidHwPixFmt{AV_PIX_FMT_NONE};
    AVHWDeviceType m_viddecDevType{AV_HWDEVICE_TYPE_NONE};
    AVBufferRef* m_viddecHwDevCtx{nullptr};
    SwrContext* m_swrCtx{nullptr};
    AVSampleFormat m_swrOutSmpfmt{AV_SAMPLE_FMT_FLTP};
    int m_swrOutSampleRate;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
    int m_swrOutChannels;
    int64_t m_swrOutChnLyt;
#else
    AVChannelLayout m_swrOutChlyt{AV_CHANNEL_ORDER_UNSPEC, 0};
#endif

    // demux video thread
    thread m_demuxVidThread;
    list<AVPacket*> m_vidpktQ;
    int m_vidpktQMaxSize{8};
    mutex m_vidpktQLock;
    bool m_demuxVidEof{false};
    // video decoding thread
    thread m_viddecThread;
    list<AVFrame*> m_vidfrmQ;
    int m_vidfrmQMaxSize{4};
    mutex m_vidfrmQLock;
    bool m_viddecEof{false};
    // generate snapshots thread
    thread m_genSsThread;
    bool m_genSsEof{false};
    // demux audio thread
    thread m_demuxAudThread;
    list<AVPacket*> m_audpktQ;
    int m_audpktQMaxSize{64};
    mutex m_audpktQLock;
    bool m_demuxAudEof{false};
    // audio decoding thread
    thread m_auddecThread;
    list<AVFrame*> m_audfrmQ;
    int m_audfrmQMaxSize{25};
    double m_audfrmAvgDur{0.021};
    uint32_t m_audfrmAvgDurCalcCnt{10};
    float m_audQDuration{5.f};
    mutex m_audfrmQLock;
    bool m_auddecEof{false};
    // generate waveform samples thread
    thread m_genWfThread;
    bool m_swrPassThrough{false};
    bool m_genWfEof{false};
    // thread to release computer resources after all snapshots are finished
    thread m_releaseThread;

    recursive_mutex m_apiLock;
    bool m_quit{false};

    // video snapshots
    vector<Snapshot> m_snapshots;
    uint32_t m_ssCount;
    int64_t m_vidStartMts{0};
    int64_t m_vidDurMts{0};
    int64_t m_vidFrmCnt{0};
    double m_ssIntvMts;
    double m_vidfrmIntvTs;

    // audio waveform
    Waveform::Holder m_hWaveform;
    uint32_t m_singleFramePixels{200};
    double m_minAggregateSamples{5};
    double m_fixedAggregateSamples{0};

    // AVFrame -> ImMat
    bool m_useRszFactor{false};
    bool m_ssSizeChanged{false};
    float m_ssWFacotr{1.f}, m_ssHFacotr{1.f};
    AVFrameToImMatConverter m_frmCvt;
};

static const auto OVERVIEW_HOLDER_DELETER = [] (Overview* p) {
    Overview_Impl* ptr = dynamic_cast<Overview_Impl*>(p);
    ptr->Close();
    delete ptr;
};

Overview::Holder Overview::CreateInstance()
{
    return Overview::Holder(new Overview_Impl(), OVERVIEW_HOLDER_DELETER);
}

ALogger* Overview::GetLogger()
{
    return Logger::GetLogger("MOverview");
}
}