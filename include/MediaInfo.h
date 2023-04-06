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
#include <memory>
#include <vector>
#include "MediaCore.h"

namespace MediaCore
{
    enum class MediaType
    {
        UNKNOWN = 0,
        VIDEO,
        AUDIO,
        SUBTITLE,
    };

    struct Ratio
    {
        int32_t num{0};
        int32_t den{0};

        static inline bool IsValid(const Ratio& r)
        { return r.num != 0 && r.den != 0; }
    };

    struct Stream
    {
        using Holder = std::shared_ptr<Stream>;
        virtual ~Stream() {}

        MediaType type{MediaType::UNKNOWN};
        uint64_t bitRate{0};
        double startTime;
        double duration;
        Ratio timebase;
    };

    struct VideoStream : public Stream
    {
        VideoStream() { type = MediaType::VIDEO; }
        uint32_t width{0};
        uint32_t height{0};
        std::string format;
        std::string codec;
        Ratio sampleAspectRatio;
        Ratio avgFrameRate;
        Ratio realFrameRate;
        uint64_t frameNum{0};
        bool isImage{false};
        bool isHdr{false};
        uint8_t bitDepth{0};
    };

    struct AudioStream : public Stream
    {
        AudioStream() { type = MediaType::AUDIO; }
        uint32_t channels{0};
        uint32_t sampleRate{0};
        std::string format;
        std::string codec;
        uint8_t bitDepth{0};
    };

    struct SubtitleStream : public Stream
    {
        SubtitleStream() { type = MediaType::SUBTITLE; }
    };

    struct MediaInfo
    {
        using Holder = std::shared_ptr<MediaInfo>;
        virtual ~MediaInfo() {}

        std::string url;
        std::vector<Stream::Holder> streams;
        double startTime{0};
        double duration{-1};
        bool isComplete{true};
    };
}
