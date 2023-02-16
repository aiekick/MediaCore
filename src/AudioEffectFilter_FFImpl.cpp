#include <sstream>
#include "AudioEffectFilter.h"
#include "FFUtils.h"
extern "C"
{
    #include "libavutil/avutil.h"
    #include "libavfilter/avfilter.h"
    #include "libavfilter/buffersrc.h"
    #include "libavfilter/buffersink.h"
    #include "libswresample/swresample.h"
}

using namespace std;
using namespace Logger;

namespace MediaCore
{
class AudioEffectFilter_FFImpl : public AudioEffectFilter
{
public:
    AudioEffectFilter_FFImpl(const string& loggerName = "")
    {
        if (loggerName.empty())
        {
            m_logger = GetAudioEffectFilterLogger();
        }
        else
        {
            m_logger = GetLogger(loggerName);
            m_logger->SetShowLevels(DEBUG);
        }
    }

    virtual ~AudioEffectFilter_FFImpl()
    {
        ReleaseFilterGraph();
    }

    bool Init(const string& sampleFormat, uint32_t channels, uint32_t sampleRate) override
    {
        AVSampleFormat smpfmt = av_get_sample_fmt(sampleFormat.c_str());
        if (smpfmt == AV_SAMPLE_FMT_NONE)
        {
            ostringstream oss;
            oss << "Invalid argument 'sampleFormat' for AudioEffectFilter::Init()! Value '" << sampleFormat << "' is NOT a VALID sample format.";
            m_errMsg = oss.str();
            return false;
        }
        if (channels == 0)
        {
            ostringstream oss;
            oss << "Invalid argument 'channels' for AudioEffectFilter::Init()! Value " << channels << " is a bad value.";
            m_errMsg = oss.str();
            return false;
        }
        if (sampleRate == 0)
        {
            ostringstream oss;
            oss << "Invalid argument 'sampleRate' for AudioEffectFilter::Init()! Value " << sampleRate << " is a bad value.";
            m_errMsg = oss.str();
            return false;
        }

        bool ret = CreateFilterGraph(smpfmt, channels, sampleRate);
        if (!ret)
            return false;

        m_smpfmt = smpfmt;
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_blockAlign = channels*av_get_bytes_per_sample(smpfmt);
        m_isPlanar = av_sample_fmt_is_planar(smpfmt);

        m_inited = true;
        return true;
    }

    bool ProcessData(const ImGui::ImMat& in, list<ImGui::ImMat>& out) override
    {
        out.clear();
        if (!m_inited)
        {
            m_errMsg = "This 'AudioEffectFilter' instance is NOT INITIALIZED!";
            return false;
        }
        if (in.empty())
            return true;

        SelfFreeAVFramePtr avfrm = AllocSelfFreeAVFramePtr();
        if (!m_matCvter.ConvertImMatToAVFrame(in, avfrm.get(), m_nbSamples))
        {
            ostringstream oss;
            oss << "FAILED to invoke AudioImMatAVFrameConverter::ConvertImMatToAVFrame()!";
            m_errMsg = oss.str();
            return false;
        }

        UpdateFilterParameters();

        int fferr;
        fferr = av_buffersrc_add_frame(m_bufsrcCtx, avfrm.get());
        if (fferr < 0)
        {
            ostringstream oss;
            oss << "FAILED to invoke av_buffersrc_add_frame()! fferr = " << fferr << ".";
            m_errMsg = oss.str();
            return false;
        }

        bool hasErr = false;
        while (true)
        {
            av_frame_unref(avfrm.get());
            fferr = av_buffersink_get_frame(m_bufsinkCtx, avfrm.get());
            if (fferr >= 0)
            {
                if (avfrm->nb_samples > 0)
                {
                    ImGui::ImMat m;
                    double ts = (double)avfrm->pts/m_sampleRate;
                    if (m_matCvter.ConvertAVFrameToImMat(avfrm.get(), m, ts))
                        out.push_back(m);
                    else
                    {
                        ostringstream oss;
                        oss << "FAILED to invoke AudioImMatAVFrameConverter::ConvertAVFrameToImMat()!";
                        m_errMsg = oss.str();
                        hasErr = true;
                        break;
                    }
                }
                else
                {
                    Log(WARN) << "av_buffersink_get_frame() returns INVALID number of samples! nb_samples=" << avfrm->nb_samples << "." << endl;
                }
            }
            else if (fferr == AVERROR(EAGAIN))
                break;
            else
            {
                ostringstream oss;
                oss << "FAILED to invoke av_buffersink_get_frame()! fferr = " << fferr << ".";
                m_errMsg = oss.str();
                hasErr = true;
                break;
            }
        }
        return !hasErr;
    }

    bool SetVolume(float v) override
    {
        m_volume = v;
        return true;
    }

    float GetVolume() override
    {
        return m_volume;
    }

    string GetError() const override
    {
        return m_errMsg;
    }

private:
    bool CreateFilterGraph(const AVSampleFormat smpfmt, uint32_t channels, uint32_t sampleRate)
    {
        const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
        const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
        m_filterGraph = avfilter_graph_alloc();
        if (!m_filterGraph)
        {
            m_errMsg = "FAILED to allocate new 'AVFilterGraph'!";
            return false;
        }

        ostringstream abufsrcArgsOss;
        abufsrcArgsOss << "time_base=1/" << sampleRate << ":sample_rate=" << sampleRate
            << ":sample_fmt=" << av_get_sample_fmt_name(smpfmt);
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        abufsrcArgsOss << ":channel_layout=" << av_get_default_channel_layout(channels);
#else
        AVChannelLayout chlyt{AV_CHANNEL_ORDER_UNSPEC, 0};
        av_channel_layout_default(&chlyt, channels);
        char chlytDescBuff[256] = {0};
        av_channel_layout_describe(&chlyt, chlytDescBuff, sizeof(chlytDescBuff));
        abufsrcArgsOss << ":channel_layout=" << chlytDescBuff;
#endif
        string bufsrcArgs = abufsrcArgsOss.str();
        int fferr;
        AVFilterContext* bufSrcCtx = nullptr;
        fferr = avfilter_graph_create_filter(&bufSrcCtx, abuffersrc, "BufferSource", bufsrcArgs.c_str(), nullptr, m_filterGraph);
        if (fferr < 0)
        {
            ostringstream oss;
            oss << "FAILED when invoking 'avfilter_graph_create_filter' for source buffer! fferr=" << fferr << ".";
            m_errMsg = oss.str();
            return false;
        }
        AVFilterInOut* outputs = avfilter_inout_alloc();
        if (!outputs)
        {
            m_errMsg = "FAILED to allocate 'AVFilterInOut' instance!";
            return false;
        }
        outputs->name       = av_strdup("in");
        outputs->filter_ctx = bufSrcCtx;
        outputs->pad_idx    = 0;
        outputs->next       = nullptr;

        AVFilterContext* bufSinkCtx = nullptr;
        fferr = avfilter_graph_create_filter(&bufSinkCtx, abuffersink, "BufferSink", nullptr, nullptr, m_filterGraph);
        if (fferr < 0)
        {
            ostringstream oss;
            oss << "FAILED when invoking 'avfilter_graph_create_filter' for sink buffer! fferr=" << fferr << ".";
            m_errMsg = oss.str();
            return false;
        }
        AVFilterInOut* inputs = avfilter_inout_alloc();
        if (!inputs)
        {
            avfilter_inout_free(&outputs);
            m_errMsg = "FAILED to allocate 'AVFilterInOut' instance!";
            return false;
        }
        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = bufSinkCtx;
        inputs->pad_idx     = 0;
        inputs->next        = nullptr;

        ostringstream fgArgsOss;
        fgArgsOss << "volume=volume=" << m_currVolume << ":precision=float:eval=frame";
        string fgArgs = fgArgsOss.str();
        fferr = avfilter_graph_parse_ptr(m_filterGraph, fgArgs.c_str(), &inputs, &outputs, nullptr);
        if (fferr < 0)
        {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            ostringstream oss;
            oss << "FAILED to invoke 'avfilter_graph_parse_ptr' with arguments string '" << fgArgs << "'! fferr=" << fferr << ".";
            m_errMsg = oss.str();
            return false;
        }

        fferr = avfilter_graph_config(m_filterGraph, nullptr);
        if (fferr < 0)
        {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            ostringstream oss;
            oss << "FAILED to invoke 'avfilter_graph_config'! fferr=" << fferr << ".";
            m_errMsg = oss.str();
            return false;
        }

        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        m_bufsrcCtx = bufSrcCtx;
        m_bufsinkCtx = bufSinkCtx;
        return true;
    }

    void ReleaseFilterGraph()
    {
        if (m_filterGraph)
        {
            avfilter_graph_free(&m_filterGraph);
            m_filterGraph = nullptr;
        }
        m_bufsrcCtx = nullptr;
        m_bufsinkCtx = nullptr;
    }

    void UpdateFilterParameters()
    {
        int fferr;
        char cmdRes[256] = {0};
        if (m_volume != m_currVolume)
        {
            m_logger->Log(DEBUG) << "Change volume: " << m_currVolume << " -> " << m_volume << " ... ";
            char cmdArgs[32] = {0};
            snprintf(cmdArgs, sizeof(cmdArgs)-1, "%f", m_volume);
            fferr = avfilter_graph_send_command(m_filterGraph, "volume", "volume", cmdArgs, cmdRes, sizeof(cmdRes)-1, 0);
            if (fferr >= 0)
            {
                m_currVolume = m_volume;
                m_logger->Log(DEBUG) << "Succeeded." << endl;
            }
            else
            {
                m_logger->Log(DEBUG) << "FAILED!" << endl;
                ostringstream oss;
                oss << "FAILED to invoke 'avfilter_graph_send_command()' with arguments: target='" << "volume" << "', arg='" << cmdArgs
                    << "'. Returned fferr=" << fferr << ", res='" << cmdRes << "'.";
                m_errMsg = oss.str();
                m_logger->Log(WARN) << m_errMsg << endl;
            }
        }
    }

private:
    ALogger* m_logger;
    bool m_inited{false};
    AVSampleFormat m_smpfmt{AV_SAMPLE_FMT_NONE};
    uint32_t m_channels{0};
    uint32_t m_sampleRate{0};
    uint32_t m_blockAlign{0};
    bool m_isPlanar{false};
    AVFilterGraph* m_filterGraph{nullptr};
    AVFilterContext* m_bufsrcCtx{nullptr};
    AVFilterContext* m_bufsinkCtx{nullptr};
    float m_volume{1.f}, m_currVolume{1.f};
    AudioImMatAVFrameConverter m_matCvter;
    int64_t m_nbSamples{0};
    string m_errMsg;
};

AudioEffectFilterHolder CreateAudioEffectFilter(const string& loggerName)
{
    return AudioEffectFilterHolder(new AudioEffectFilter_FFImpl(loggerName));
}

ALogger* GetAudioEffectFilterLogger()
{
    return GetLogger("AEFilter");
}
}
