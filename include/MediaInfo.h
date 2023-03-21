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

namespace MediaInfo
{
    enum Type
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
    };

    struct Stream
    {
        virtual ~Stream() {}
        Type type{UNKNOWN};
        uint64_t bitRate{0};
        double startTime;
        double duration;
        Ratio timebase;
    };

    using StreamHolder = std::shared_ptr<Stream>;

    struct VideoStream : public Stream
    {
        VideoStream() { type = VIDEO; }
        uint32_t width{0};
        uint32_t height{0};
        std::string format;
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
        AudioStream() { type = AUDIO; }
        uint32_t channels{0};
        uint32_t sampleRate{0};
        std::string format;
        uint8_t bitDepth{0};
    };

    struct SubtitleStream : public Stream
    {
        SubtitleStream() { type = SUBTITLE; }
    };

    struct Info
    {
        std::string url;
        std::vector<StreamHolder> streams;
        double startTime{0};
        double duration{-1};
        bool isComplete{true};
    };

    using InfoHolder = std::shared_ptr<Info>;
}
