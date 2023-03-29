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
#include <string>
#include <memory>
#include "immat.h"
#include "MediaParser.h"
#include "Logger.h"
#include "MediaCore.h"

namespace MediaCore
{
namespace Snapshot
{
    using TextureHolder = std::shared_ptr<ImTextureID>;
    struct Image
    {
        using Holder = std::shared_ptr<Image>;

        bool mTextureReady{false};
        TextureHolder mTextureHolder;
        ImVec2 mSize{0, 0};
        int64_t mTimestampMs{0};
        ImGui::ImMat mImgMat;
    };

    struct Viewer
    {
        using Holder = std::shared_ptr<Viewer>;

        virtual bool Seek(double pos) = 0;
        virtual double GetCurrWindowPos() const = 0;
        virtual bool GetSnapshots(double startPos, std::vector<Image::Holder>& snapshots) = 0;
        virtual bool UpdateSnapshotTexture(std::vector<Image::Holder>& snapshots) = 0;

        virtual Holder CreateViewer(double pos = 0) = 0;
        virtual void Release() = 0;
        virtual MediaParser::Holder GetMediaParser() const = 0;

        virtual std::string GetError() const = 0;
    };

    struct Generator
    {
        using Holder = std::shared_ptr<Generator>;
        static MEDIACORE_API Holder CreateInstance();

        virtual bool Open(const std::string& url) = 0;
        virtual bool Open(MediaParser::Holder hParser) = 0;
        virtual MediaParser::Holder GetMediaParser() const = 0;
        virtual void Close() = 0;

        virtual Viewer::Holder CreateViewer(double pos = 0) = 0;
        virtual void ReleaseViewer(Viewer::Holder& viewer) = 0;

        virtual bool IsOpened() const = 0;
        virtual bool HasVideo() const = 0;
        virtual bool HasAudio() const = 0;
        virtual bool ConfigSnapWindow(double& windowSize, double frameCount, bool forceRefresh = false) = 0;
        virtual bool SetCacheFactor(double cacheFactor) = 0;
        virtual double GetMinWindowSize() const = 0;
        virtual double GetMaxWindowSize() const = 0;

        virtual bool SetSnapshotSize(uint32_t width, uint32_t height) = 0;
        virtual bool SetSnapshotResizeFactor(float widthFactor, float heightFactor) = 0;
        virtual bool SetOutColorFormat(ImColorFormat clrfmt) = 0;
        virtual bool SetResizeInterpolateMode(ImInterpolateMode interp) = 0;

        virtual MediaInfo::Holder GetMediaInfo() const = 0;
        virtual const VideoStream* GetVideoStream() const = 0;
        virtual const AudioStream* GetAudioStream() const = 0;

        virtual uint32_t GetVideoWidth() const = 0;
        virtual uint32_t GetVideoHeight() const = 0;
        virtual int64_t GetVideoMinPos() const = 0;
        virtual int64_t GetVideoDuration() const = 0;
        virtual int64_t GetVideoFrameCount() const = 0;

        virtual bool IsHwAccelEnabled() const = 0;
        virtual void EnableHwAccel(bool enable) = 0;
        virtual std::string GetError() const = 0;
    };

    MEDIACORE_API Logger::ALogger* GetLogger();
}
}
