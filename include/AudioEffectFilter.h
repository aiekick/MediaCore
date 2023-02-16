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
        virtual bool Init(const std::string& sampleFormat, uint32_t channels, uint32_t sampleRate) = 0;
        virtual bool ProcessData(const ImGui::ImMat& in, std::list<ImGui::ImMat>& out) = 0;

        virtual bool SetVolume(float v) = 0;
        virtual float GetVolume() = 0;
        virtual std::string GetError() const = 0;
    };
    using AudioEffectFilterHolder = std::shared_ptr<AudioEffectFilter>;

    AudioEffectFilterHolder CreateAudioEffectFilter(const std::string& loggerName = "");
    Logger::ALogger* GetAudioEffectFilterLogger();
}