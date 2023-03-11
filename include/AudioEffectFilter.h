#pragma once
#include <memory>
#include <string>
#include <list>
#include "Logger.h"
#include "MediaCore.h"
#include "immat.h"

namespace MediaCore
{
    struct MEDIACORE_API AudioEffectFilter
    {
        static const uint32_t VOLUME;
        static const uint32_t PAN;
        static const uint32_t GATE;
        static const uint32_t LIMITER;
        static const uint32_t EQUALIZER;
        static const uint32_t COMPRESSOR;

        virtual bool Init(uint32_t composeFlags, const std::string& sampleFormat, uint32_t channels, uint32_t sampleRate) = 0;
        virtual bool ProcessData(const ImGui::ImMat& in, std::list<ImGui::ImMat>& out) = 0;
        virtual bool HasFilter(uint32_t composeFlags) const = 0;

        struct VolumeParams
        {
            float volume{1.f};
        };
        virtual bool SetVolumeParams(VolumeParams* params) = 0;
        virtual VolumeParams GetVolumeParams() const = 0;

        struct PanParams
        {
            float x{0.5f};
            float y{0.5f};
        };
        virtual bool SetPanParams(PanParams* params) = 0;
        virtual PanParams GetPanParams() const = 0;

        struct LimiterParams
        {
            float limit{1.f};
            float attack{5};
            float release{50};
        };
        virtual bool SetLimiterParams(LimiterParams* params) = 0;
        virtual LimiterParams GetLimiterParams() const = 0;

        struct GateParams
        {
            float threshold{0.125f};
            float range{0.06125f};
            float ratio{2};
            float attack{20};
            float release{250};
            float makeup{1};
            float knee{2.82843f};
        };
        virtual bool SetGateParams(GateParams* params) = 0;
        virtual GateParams GetGateParams() const = 0;

        struct CompressorParams
        {
            float threshold{0.125f};
            float ratio{2};
            float knee{2.82843f};
            float mix{1};
            float attack{20};
            float release{250};
            float makeup{1};
            float level_sc{1};
        };
        virtual bool SetCompressorParams(CompressorParams* params) = 0;
        virtual CompressorParams GetCompressorParams() const = 0;

        // struct EqualizerParams
        // {
        //     uint32_t centerFreq;  // in Hz
        //     uint32_t bandWidth;  // in Hz
        //     int32_t gain;  // in db
        // };
        // audio_band_config mBandCfg[10];

        virtual std::string GetError() const = 0;
    };
    using AudioEffectFilterHolder = std::shared_ptr<AudioEffectFilter>;

    MEDIACORE_API AudioEffectFilterHolder CreateAudioEffectFilter(const std::string& loggerName = "");
    MEDIACORE_API Logger::ALogger* GetAudioEffectFilterLogger();
}