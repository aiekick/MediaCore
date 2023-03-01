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
        ReleasePanFilterGraph();
    }

    bool Init(uint32_t composeFlags, const string& sampleFormat, uint32_t channels, uint32_t sampleRate) override
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

        if (composeFlags > 0)
        {
            bool ret = CreateFilterGraph(composeFlags, smpfmt, channels, sampleRate);
            if (!ret)
                return false;
            if (CheckFilters(composeFlags, PAN))
            {
                ret = CreatePanFilterGraph(smpfmt, channels, sampleRate);
                if (!ret)
                    return false;
            }
        }
        else
        {
            m_logger->Log(DEBUG) << "This 'AudioEffectFilter' is using pass-through mode because 'composeFlags' is 0." << endl;
            m_passThrough = true;
        }

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
        if (m_passThrough)
        {
            out.push_back(in);
            return true;
        }

        SelfFreeAVFramePtr avfrm = AllocSelfFreeAVFramePtr();
        int64_t pts = (int64_t)(in.time_stamp*m_sampleRate);
        if (!m_matCvter.ConvertImMatToAVFrame(in, avfrm.get(), pts))
        {
            ostringstream oss;
            oss << "FAILED to invoke AudioImMatAVFrameConverter::ConvertImMatToAVFrame()!";
            m_errMsg = oss.str();
            return false;
        }
        m_logger->Log(DEBUG) << "Get incoming mat: ts=" << in.time_stamp << "; avfrm: pts=" << pts << endl;

        UpdateFilterParameters();

        int fferr;
        if (m_useGeneralFg)
        {
            fferr = av_buffersrc_add_frame(m_bufsrcCtx, avfrm.get());
            if (fferr < 0)
            {
                ostringstream oss;
                oss << "FAILED to invoke av_buffersrc_add_frame()! fferr = " << fferr << ".";
                m_errMsg = oss.str();
                return false;
            }
        }

        bool hasErr = false;
        while (true)
        {
            if (m_useGeneralFg)
            {
                av_frame_unref(avfrm.get());
                fferr = av_buffersink_get_frame(m_bufsinkCtx, avfrm.get());
            }
            else
            {
                fferr = 0;
            }
            if (fferr >= 0)
            {
                if (m_usePanFg)
                {
                    fferr = av_buffersrc_add_frame(m_panBufsrcCtx, avfrm.get());
                    if (fferr < 0)
                    {
                        ostringstream oss;
                        oss << "FAILED to invoke av_buffersrc_add_frame() on PAN filter-graph! fferr = " << fferr << ".";
                        m_errMsg = oss.str();
                        hasErr = true;
                        break;
                    }
                    av_frame_unref(avfrm.get());
                    fferr = av_buffersink_get_frame(m_panBufsinkCtx, avfrm.get());
                    if (fferr < 0)
                    {
                        ostringstream oss;
                        oss << "FAILED to invoke av_buffersink_get_frame() on PAN filter-graph! fferr = " << fferr << ".";
                        m_errMsg = oss.str();
                        hasErr = true;
                        break;
                    }
                }
                if (avfrm->nb_samples > 0)
                {
                    ImGui::ImMat m;
                    double ts = (double)avfrm->pts/m_sampleRate;
                    if (m_matCvter.ConvertAVFrameToImMat(avfrm.get(), m, ts))
                    {
                        m_logger->Log(DEBUG) << "Add output avfrm: pts=" << avfrm->pts << "; mat: ts=" << m.time_stamp << endl;
                        m.flags = IM_MAT_FLAGS_AUDIO_FRAME;
                        m.rate = { (int)m_sampleRate, 1 };
                        m.elempack = m_channels;
                        out.push_back(m);
                    }
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

    bool HasFilter(uint32_t composeFlags) const override
    {
        return CheckFilters(m_composeFlags, composeFlags);
    }

    bool SetVolumeParams(VolumeParams* params) override
    {
        if (!HasFilter(VOLUME))
        {
            m_errMsg = "CANNOT set 'VolumeParams' because this instance is NOT initialized with 'AudioEffectFilter::VOLUME' compose-flag!";
            return false;
        }
        m_setVolumeParams = *params;
        return true;
    }

    VolumeParams GetVolumeParams() const override
    {
        return m_setVolumeParams;
    }

    bool SetPanParams(PanParams* params) override
    {
        if (!HasFilter(PAN))
        {
            m_errMsg = "CANNOT set 'PanParams' because this instance is NOT initialized with 'AudioEffectFilter::PAN' compose-flag!";
            return false;
        }
        m_setPanParams = *params;
        return true;
    }

    PanParams GetPanParams() const override
    {
        return m_setPanParams;
    }

    bool SetLimiterParams(LimiterParams* params) override
    {
        if (!HasFilter(LIMITER))
        {
            m_errMsg = "CANNOT set 'LimiterParams' because this instance is NOT initialized with 'AudioEffectFilter::LIMITER' compose-flag!";
            return false;
        }
        m_setLimiterParams = *params;
        return true;
    }

    LimiterParams GetLimiterParams() const override
    {
        return m_setLimiterParams;
    }

    string GetError() const override
    {
        return m_errMsg;
    }

private:
    bool CreateFilterGraph(uint32_t composeFlags, const AVSampleFormat smpfmt, uint32_t channels, uint32_t sampleRate)
    {
        if (composeFlags&(~PAN) == 0)
        {
            m_useGeneralFg = false;
            return true;
        }
        else
        {
            m_useGeneralFg = true;
        }

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
        char chlytDescBuff[256] = {0};
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        int64_t chlyt = av_get_default_channel_layout(channels);
        av_get_channel_layout_string(chlytDescBuff, sizeof(chlytDescBuff), channels, (uint64_t)chlyt);
#else
        AVChannelLayout chlyt{AV_CHANNEL_ORDER_UNSPEC, 0};
        av_channel_layout_default(&chlyt, channels);
        av_channel_layout_describe(&chlyt, chlytDescBuff, sizeof(chlytDescBuff));
#endif
        abufsrcArgsOss << ":channel_layout=" << chlytDescBuff;
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
        bool isFirstFilter = true;
        if (CheckFilters(composeFlags, LIMITER))
        {
            if (!isFirstFilter) fgArgsOss << ","; else isFirstFilter = false;
            fgArgsOss << "alimiter=limit=" << m_currLimiterParams.limit << ":attack=" << m_currLimiterParams.attack << ":release=" << m_currLimiterParams.release;
        }
        if (CheckFilters(composeFlags, PAN))
        {
            m_logger->Log(Error) << "Filter 'pan' is NOT SUPPORTED yet! Ignore this setting." << endl;
        }
        if (CheckFilters(composeFlags, VOLUME))
        {
            if (!isFirstFilter) fgArgsOss << ","; else isFirstFilter = false;
            fgArgsOss << "volume=volume=" << m_currVolumeParams.volume << ":precision=float:eval=frame";
        }
        if (!isFirstFilter)
        {
            fgArgsOss << ",aformat=f=" << av_get_sample_fmt_name(smpfmt);
        }
        string fgArgs = fgArgsOss.str();
        m_logger->Log(DEBUG) << "Initialze filter-graph with arguments '" << fgArgs << "'." << endl;
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
        m_composeFlags = composeFlags;
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

    bool CreatePanFilterGraph(const AVSampleFormat smpfmt, uint32_t channels, uint32_t sampleRate)
    {
        if (m_currPanParams.x == 0.5f && m_currPanParams.y == 0.5f)
        {
            m_usePanFg = false;
            return true;
        }
        else
        {
            m_usePanFg = true;
        }

        const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
        const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
        m_panFg = avfilter_graph_alloc();
        if (!m_panFg)
        {
            m_errMsg = "FAILED to allocate new 'AVFilterGraph'(pan)!";
            return false;
        }

        ostringstream abufsrcArgsOss;
        abufsrcArgsOss << "time_base=1/" << sampleRate << ":sample_rate=" << sampleRate
            << ":sample_fmt=" << av_get_sample_fmt_name(smpfmt);
        char chlytDescBuff[256] = {0};
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
        uint64_t chlyt = (uint64_t)av_get_default_channel_layout(channels);
        av_get_channel_layout_string(chlytDescBuff, sizeof(chlytDescBuff), channels, chlyt);
#else
        AVChannelLayout chlyt{AV_CHANNEL_ORDER_UNSPEC, 0};
        av_channel_layout_default(&chlyt, channels);
        av_channel_layout_describe(&chlyt, chlytDescBuff, sizeof(chlytDescBuff));
#endif
        abufsrcArgsOss << ":channel_layout=" << chlytDescBuff;
        string bufsrcArgs = abufsrcArgsOss.str();
        int fferr;
        AVFilterContext* bufSrcCtx = nullptr;
        fferr = avfilter_graph_create_filter(&bufSrcCtx, abuffersrc, "BufferSource", bufsrcArgs.c_str(), nullptr, m_panFg);
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
        fferr = avfilter_graph_create_filter(&bufSinkCtx, abuffersink, "BufferSink", nullptr, nullptr, m_panFg);
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
        fgArgsOss << "pan=" << chlytDescBuff << "| ";
        for (int i = 0; i < channels; i++)
        {
            double xCoef = 1., yCoef = 1.;
#if !defined(FF_API_OLD_CHANNEL_LAYOUT) && (LIBAVUTIL_VERSION_MAJOR < 58)
            uint64_t ch = av_channel_layout_extract_channel(chlyt, i);
            if (ch == AV_CH_FRONT_LEFT || ch == AV_CH_BACK_LEFT || ch == AV_CH_FRONT_LEFT_OF_CENTER ||
                ch == AV_CH_SIDE_LEFT || ch == AV_CH_TOP_FRONT_LEFT || ch == AV_CH_TOP_BACK_LEFT ||
                ch == AV_CH_STEREO_LEFT || ch == AV_CH_WIDE_LEFT || ch == AV_CH_SURROUND_DIRECT_LEFT ||
                ch == AV_CH_TOP_SIDE_LEFT || ch == AV_CH_BOTTOM_FRONT_LEFT)
                xCoef *= (1-m_currPanParams.x)/0.5;
            else if (ch == AV_CH_FRONT_RIGHT || ch == AV_CH_BACK_RIGHT || ch == AV_CH_FRONT_RIGHT_OF_CENTER ||
                ch == AV_CH_SIDE_RIGHT || ch == AV_CH_TOP_FRONT_RIGHT || ch == AV_CH_TOP_BACK_RIGHT ||
                ch == AV_CH_STEREO_RIGHT || ch == AV_CH_WIDE_RIGHT || ch == AV_CH_SURROUND_DIRECT_RIGHT ||
                ch == AV_CH_TOP_SIDE_RIGHT || ch == AV_CH_BOTTOM_FRONT_RIGHT)
                xCoef *= m_currPanParams.x/0.5;
            if (ch == AV_CH_FRONT_LEFT || ch == AV_CH_FRONT_RIGHT || ch == AV_CH_FRONT_CENTER ||
                ch == AV_CH_FRONT_LEFT_OF_CENTER || ch == AV_CH_FRONT_RIGHT_OF_CENTER || ch == AV_CH_TOP_FRONT_LEFT ||
                ch == AV_CH_TOP_FRONT_CENTER || ch == AV_CH_TOP_FRONT_RIGHT || ch == AV_CH_BOTTOM_FRONT_CENTER ||
                ch == AV_CH_BOTTOM_FRONT_LEFT || ch == AV_CH_BOTTOM_FRONT_RIGHT)
                yCoef *= (1-m_currPanParams.y)/0.5;
            else if (ch == AV_CH_BACK_LEFT || ch == AV_CH_BACK_RIGHT || ch == AV_CH_BACK_CENTER ||
                ch == AV_CH_TOP_BACK_LEFT || ch == AV_CH_TOP_BACK_CENTER || ch == AV_CH_TOP_BACK_RIGHT)
                yCoef *= m_currPanParams.y/0.5;
#else
            enum AVChannel ch = av_channel_layout_channel_from_index(&chlyt, (unsigned int)i);
            if (ch == AV_CHAN_FRONT_LEFT || ch == AV_CHAN_BACK_LEFT || ch == AV_CHAN_FRONT_LEFT_OF_CENTER ||
                ch == AV_CHAN_SIDE_LEFT || ch == AV_CHAN_TOP_FRONT_LEFT || ch == AV_CHAN_TOP_BACK_LEFT ||
                ch == AV_CHAN_STEREO_LEFT || ch == AV_CHAN_WIDE_LEFT || ch == AV_CHAN_SURROUND_DIRECT_LEFT ||
                ch == AV_CHAN_TOP_SIDE_LEFT || ch == AV_CHAN_BOTTOM_FRONT_LEFT)
                xCoef *= (1-m_currPanParams.x)/0.5;
            else if (ch == AV_CHAN_FRONT_RIGHT || ch == AV_CHAN_BACK_RIGHT || ch == AV_CHAN_FRONT_RIGHT_OF_CENTER ||
                ch == AV_CHAN_SIDE_RIGHT || ch == AV_CHAN_TOP_FRONT_RIGHT || ch == AV_CHAN_TOP_BACK_RIGHT ||
                ch == AV_CHAN_STEREO_RIGHT || ch == AV_CHAN_WIDE_RIGHT || ch == AV_CHAN_SURROUND_DIRECT_RIGHT ||
                ch == AV_CHAN_TOP_SIDE_RIGHT || ch == AV_CHAN_BOTTOM_FRONT_RIGHT)
                xCoef *= m_currPanParams.x/0.5;
            if (ch == AV_CHAN_FRONT_LEFT || ch == AV_CHAN_FRONT_RIGHT || ch == AV_CHAN_FRONT_CENTER ||
                ch == AV_CHAN_FRONT_LEFT_OF_CENTER || ch == AV_CHAN_FRONT_RIGHT_OF_CENTER || ch == AV_CHAN_TOP_FRONT_LEFT ||
                ch == AV_CHAN_TOP_FRONT_CENTER || ch == AV_CHAN_TOP_FRONT_RIGHT || ch == AV_CHAN_BOTTOM_FRONT_CENTER ||
                ch == AV_CHAN_BOTTOM_FRONT_LEFT || ch == AV_CHAN_BOTTOM_FRONT_RIGHT)
                yCoef *= (1-m_currPanParams.y)/0.5;
            else if (ch == AV_CHAN_BACK_LEFT || ch == AV_CHAN_BACK_RIGHT || ch == AV_CHAN_BACK_CENTER ||
                ch == AV_CHAN_TOP_BACK_LEFT || ch == AV_CHAN_TOP_BACK_CENTER || ch == AV_CHAN_TOP_BACK_RIGHT)
                yCoef *= m_currPanParams.y/0.5;
#endif
            double finalCoef = xCoef*yCoef;
            fgArgsOss << "c" << i << "=" << finalCoef << "*" << "c" << i;
            if (i < channels-1)
                fgArgsOss << " | ";
        }
        fgArgsOss << ",aformat=f=" << av_get_sample_fmt_name(smpfmt);
        string fgArgs = fgArgsOss.str();
        m_logger->Log(DEBUG) << "Initialze PAN filter-graph with arguments '" << fgArgs << "'." << endl;
        fferr = avfilter_graph_parse_ptr(m_panFg, fgArgs.c_str(), &inputs, &outputs, nullptr);
        if (fferr < 0)
        {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            ostringstream oss;
            oss << "FAILED to invoke 'avfilter_graph_parse_ptr' with arguments string '" << fgArgs << "'! fferr=" << fferr << ".";
            m_errMsg = oss.str();
            return false;
        }

        fferr = avfilter_graph_config(m_panFg, nullptr);
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
        m_panBufsrcCtx = bufSrcCtx;
        m_panBufsinkCtx = bufSinkCtx;
        return true;
    }

    void ReleasePanFilterGraph()
    {
        if (m_panFg)
        {
            avfilter_graph_free(&m_panFg);
            m_panFg = nullptr;
        }
        m_panBufsrcCtx = nullptr;
        m_panBufsinkCtx = nullptr;
    }

    void UpdateFilterParameters()
    {
        int fferr;
        char cmdRes[256] = {0};
        if (m_setVolumeParams.volume != m_currVolumeParams.volume)
        {
            m_logger->Log(DEBUG) << "Change VolumeParams::volume: " << m_currVolumeParams.volume << " -> " << m_setVolumeParams.volume << " ... ";
            char cmdArgs[32] = {0};
            snprintf(cmdArgs, sizeof(cmdArgs)-1, "%f", m_setVolumeParams.volume);
            fferr = avfilter_graph_send_command(m_filterGraph, "volume", "volume", cmdArgs, cmdRes, sizeof(cmdRes)-1, 0);
            if (fferr >= 0)
            {
                m_currVolumeParams.volume = m_setVolumeParams.volume;
                m_logger->Log(DEBUG) << "Succeeded." << endl;
            }
            else
            {
                m_logger->Log(DEBUG) << "FAILED!" << endl;
                ostringstream oss;
                oss << "FAILED to invoke 'avfilter_graph_send_command()' with arguments: target='" << "volume" << "', cmd='" << "volume"
                    << "', arg='" << cmdArgs << "'. Returned fferr=" << fferr << ", res='" << cmdRes << "'.";
                m_errMsg = oss.str();
                m_logger->Log(WARN) << m_errMsg << endl;
            }
        }
        if (m_setLimiterParams.limit != m_currLimiterParams.limit)
        {
            m_logger->Log(DEBUG) << "Change LimiterParams::limit: " << m_currLimiterParams.limit << " -> " << m_setLimiterParams.limit << " ... ";
            char cmdArgs[32] = {0};
            snprintf(cmdArgs, sizeof(cmdArgs)-1, "%f", m_setLimiterParams.limit);
            fferr = avfilter_graph_send_command(m_filterGraph, "alimiter", "limit", cmdArgs, cmdRes, sizeof(cmdRes)-1, 0);
            if (fferr >= 0)
            {
                m_currLimiterParams.limit = m_setLimiterParams.limit;
                m_logger->Log(DEBUG) << "Succeeded." << endl;
            }
            else
            {
                m_logger->Log(DEBUG) << "FAILED!" << endl;
                ostringstream oss;
                oss << "FAILED to invoke 'avfilter_graph_send_command()' with arguments: target='" << "alimiter" << "', cmd='" << "limit"
                    << "', arg='" << cmdArgs << "'. Returned fferr=" << fferr << ", res='" << cmdRes << "'.";
                m_errMsg = oss.str();
                m_logger->Log(WARN) << m_errMsg << endl;
            }
        }
        if (m_setLimiterParams.attack != m_currLimiterParams.attack)
        {
            m_logger->Log(DEBUG) << "Change LimiterParams::attack: " << m_currLimiterParams.attack << " -> " << m_setLimiterParams.attack << " ... ";
            char cmdArgs[32] = {0};
            snprintf(cmdArgs, sizeof(cmdArgs)-1, "%f", m_setLimiterParams.attack);
            fferr = avfilter_graph_send_command(m_filterGraph, "alimiter", "attack", cmdArgs, cmdRes, sizeof(cmdRes)-1, 0);
            if (fferr >= 0)
            {
                m_currLimiterParams.attack = m_setLimiterParams.attack;
                m_logger->Log(DEBUG) << "Succeeded." << endl;
            }
            else
            {
                m_logger->Log(DEBUG) << "FAILED!" << endl;
                ostringstream oss;
                oss << "FAILED to invoke 'avfilter_graph_send_command()' with arguments: target='" << "alimiter" << "', cmd='" << "attack"
                    << "', arg='" << cmdArgs << "'. Returned fferr=" << fferr << ", res='" << cmdRes << "'.";
                m_errMsg = oss.str();
                m_logger->Log(WARN) << m_errMsg << endl;
            }
        }
        if (m_setLimiterParams.release != m_currLimiterParams.release)
        {
            m_logger->Log(DEBUG) << "Change LimiterParams::release: " << m_currLimiterParams.release << " -> " << m_setLimiterParams.release << " ... ";
            char cmdArgs[32] = {0};
            snprintf(cmdArgs, sizeof(cmdArgs)-1, "%f", m_setLimiterParams.release);
            fferr = avfilter_graph_send_command(m_filterGraph, "alimiter", "release", cmdArgs, cmdRes, sizeof(cmdRes)-1, 0);
            if (fferr >= 0)
            {
                m_currLimiterParams.release = m_setLimiterParams.release;
                m_logger->Log(DEBUG) << "Succeeded." << endl;
            }
            else
            {
                m_logger->Log(DEBUG) << "FAILED!" << endl;
                ostringstream oss;
                oss << "FAILED to invoke 'avfilter_graph_send_command()' with arguments: target='" << "alimiter" << "', cmd='" << "release"
                    << "', arg='" << cmdArgs << "'. Returned fferr=" << fferr << ", res='" << cmdRes << "'.";
                m_errMsg = oss.str();
                m_logger->Log(WARN) << m_errMsg << endl;
            }
        }
        if (m_setPanParams.x != m_currPanParams.x || m_setPanParams.y != m_currPanParams.y)
        {
            m_logger->Log(DEBUG) << "Change PanParams (" << m_currPanParams.x << ", " << m_currPanParams.y << ") -> (" << m_setPanParams.x << ", " << m_setPanParams.y << ")." << endl;
            m_currPanParams = m_setPanParams;
            ReleasePanFilterGraph();
            if (!CreatePanFilterGraph(m_smpfmt, m_channels, m_sampleRate))
            {
                m_logger->Log(Error) << "FAILED to re-create PAN filter-graph during updating the parameters! Error is '" << m_errMsg << "'." << endl;
                m_usePanFg = false;
            }
        }
    }

    bool CheckFilters(uint32_t composeFlags, uint32_t checkFlags) const
    {
        return (composeFlags&checkFlags) == checkFlags;
    }

private:
    ALogger* m_logger;
    uint32_t m_composeFlags{0};
    bool m_inited{false};
    bool m_passThrough{false};
    AVSampleFormat m_smpfmt{AV_SAMPLE_FMT_NONE};
    uint32_t m_channels{0};
    uint32_t m_sampleRate{0};
    uint32_t m_blockAlign{0};
    bool m_isPlanar{false};
    bool m_useGeneralFg{false};
    AVFilterGraph* m_filterGraph{nullptr};
    AVFilterContext* m_bufsrcCtx{nullptr};
    AVFilterContext* m_bufsinkCtx{nullptr};
    bool m_usePanFg{false};
    AVFilterGraph* m_panFg{nullptr};
    AVFilterContext* m_panBufsrcCtx{nullptr};
    AVFilterContext* m_panBufsinkCtx{nullptr};

    VolumeParams m_setVolumeParams, m_currVolumeParams;
    PanParams m_setPanParams, m_currPanParams;
    LimiterParams m_setLimiterParams, m_currLimiterParams;

    AudioImMatAVFrameConverter m_matCvter;
    string m_errMsg;
};

const uint32_t AudioEffectFilter::VOLUME        = 0x1;
const uint32_t AudioEffectFilter::PAN           = 0x2;
const uint32_t AudioEffectFilter::LIMITER       = 0x4;

AudioEffectFilterHolder CreateAudioEffectFilter(const string& loggerName)
{
    return AudioEffectFilterHolder(new AudioEffectFilter_FFImpl(loggerName));
}

ALogger* GetAudioEffectFilterLogger()
{
    return GetLogger("AEFilter");
}
}
