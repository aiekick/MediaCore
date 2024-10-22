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

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "immat.h"
#include "MediaInfo.h"
#include "Logger.h"
#include "MediaCore.h"

namespace MediaCore
{
struct MediaEncoder
{
    using Holder = std::shared_ptr<MediaEncoder>;
    static MEDIACORE_API Holder CreateInstance();
    static MEDIACORE_API Logger::ALogger* GetLogger();

    struct Option
    {
        enum ValueType
        {
            OPVT_INT = 0,
            OPVT_DOUBLE,
            OPVT_BOOL,
            OPVT_STRING,
            OPVT_FLAGS,
            OPVT_RATIO,
        };
        struct Value
        {
            ValueType type;
            union
            {
                int64_t i64;
                double dbl;
                bool bln;
            } numval;
            std::string strval;

            friend std::ostream& operator<<(std::ostream& os, const Value& val);
        };
        enum LimitationType
        {
            OPLT_NONE = 0,
            OPLT_RANGE,
            OPLT_ENUM,
        };
        struct EnumValue
        {
            std::string name;
            std::string desc;
            int32_t value;

            friend std::ostream& operator<<(std::ostream& os, const EnumValue& enumval);
        };
        struct Description
        {
            std::string name;
            std::string tag;
            std::string desc;
            std::string unit;
            ValueType valueType;
            Value defaultValue;
            LimitationType limitType;
            Value rangeMin, rangeMax;  // when 'limitType' == OPLT_RANGE, here stores the min/max option values.
            std::vector<EnumValue> enumValues;  // when 'limitType' == OPLT_ENUM, here stores all the enumeration values.

            friend std::ostream& operator<<(std::ostream& os, const Description& optdesc);
        };

        std::string name;
        Value value;
    };

    struct Description
    {
        std::string codecName;
        std::string longName;
        MediaType mediaType;
        bool isHardwareEncoder;
        std::vector<Option::Description> optDescList;

        friend std::ostream& operator<<(std::ostream& os, const Description& encdesc);
    };

    static MEDIACORE_API bool FindEncoder(const std::string& codecName, std::vector<Description>& encoderDescList);

    virtual bool Open(const std::string& url) = 0;
    virtual bool Close() = 0;
    virtual bool ConfigureVideoStream(const std::string& codecName,
            std::string& imageFormat, uint32_t width, uint32_t height,
            const Ratio& frameRate, uint64_t bitRate,
            std::vector<Option>* extraOpts = nullptr) = 0;
    virtual bool ConfigureAudioStream(const std::string& codecName,
            std::string& sampleFormat, uint32_t channels, uint32_t sampleRate, uint64_t bitRate) = 0;
    virtual bool Start() = 0;
    virtual bool FinishEncoding() = 0;
    virtual bool EncodeVideoFrame(ImGui::ImMat& vmat, bool wait = true) = 0;
    virtual bool EncodeAudioSamples(uint8_t* buf, uint32_t size, bool wait = true) = 0;
    virtual bool EncodeAudioSamples(ImGui::ImMat& amat, bool wait = true) = 0;

    virtual bool IsOpened() const = 0;
    virtual bool HasVideo() const = 0;
    virtual bool HasAudio() const = 0;
    virtual Ratio GetVideoFrameRate() const = 0;

    virtual bool IsHwAccelEnabled() const = 0;
    virtual void EnableHwAccel(bool enable) = 0;
    virtual std::string GetError() const = 0;
};
}