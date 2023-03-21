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
#include "MediaCore.h"
#include "MediaInfo.h"
#include "Logger.h"

struct MediaParser
{
    virtual ~MediaParser() {};

    virtual bool Open(const std::string& url) = 0;
    virtual void Close() = 0;

    enum InfoType
    {
        MEDIA_INFO = 0,
        VIDEO_SEEK_POINTS,
    };
    virtual bool EnableParseInfo(InfoType infoType) = 0;
    virtual bool CheckInfoReady(InfoType infoType) = 0;

    virtual std::string GetUrl() const = 0;

    virtual MediaInfo::InfoHolder GetMediaInfo(bool wait = true) = 0;
    virtual bool HasVideo() = 0;
    virtual bool HasAudio() = 0;
    virtual int GetBestVideoStreamIndex() = 0;
    virtual int GetBestAudioStreamIndex() = 0;
    virtual MediaInfo::VideoStream* GetBestVideoStream() = 0;
    virtual MediaInfo::AudioStream* GetBestAudioStream() = 0;

    using SeekPointsHolder = std::shared_ptr<std::vector<int64_t>>;
    virtual SeekPointsHolder GetVideoSeekPoints(bool wait = true) = 0;

    virtual bool IsOpened() const = 0;

    virtual std::string GetError() const = 0;
};

using MediaParserHolder = std::shared_ptr<MediaParser>;

MEDIACORE_API MediaParserHolder CreateMediaParser();

MEDIACORE_API Logger::ALogger* GetMediaParserLogger();