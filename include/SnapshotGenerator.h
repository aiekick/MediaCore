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

struct SnapshotGenerator
{
    using TextureHolder = std::shared_ptr<ImTextureID>;
    struct Image
    {
        bool mTextureReady{false};
        TextureHolder mTextureHolder;
        ImVec2 mSize{0, 0};
        int64_t mTimestampMs{0};
        ImGui::ImMat mImgMat;
    };
    using ImageHolder = std::shared_ptr<Image>;

    virtual ~SnapshotGenerator() {}
    virtual bool Open(const std::string& url) = 0;
    virtual bool Open(MediaParserHolder hParser) = 0;
    virtual MediaParserHolder GetMediaParser() const = 0;
    virtual void Close() = 0;

    struct Viewer;
    using ViewerHolder = std::shared_ptr<Viewer>;

    struct Viewer
    {
        virtual ~Viewer() {}
        virtual bool Seek(double pos) = 0;
        virtual double GetCurrWindowPos() const = 0;
        virtual bool GetSnapshots(double startPos, std::vector<ImageHolder>& snapshots) = 0;
        virtual bool UpdateSnapshotTexture(std::vector<ImageHolder>& snapshots) = 0;

        virtual ViewerHolder CreateViewer(double pos = 0) = 0;
        virtual void Release() = 0;
        virtual MediaParserHolder GetMediaParser() const = 0;

        virtual std::string GetError() const = 0;
    };

    virtual ViewerHolder CreateViewer(double pos = 0) = 0;
    virtual void ReleaseViewer(ViewerHolder& viewer) = 0;

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

    virtual MediaInfo::InfoHolder GetMediaInfo() const = 0;
    virtual const MediaInfo::VideoStream* GetVideoStream() const = 0;
    virtual const MediaInfo::AudioStream* GetAudioStream() const = 0;

    virtual uint32_t GetVideoWidth() const = 0;
    virtual uint32_t GetVideoHeight() const = 0;
    virtual int64_t GetVideoMinPos() const = 0;
    virtual int64_t GetVideoDuration() const = 0;
    virtual int64_t GetVideoFrameCount() const = 0;

    virtual bool IsHwAccelEnabled() const = 0;
    virtual void EnableHwAccel(bool enable) = 0;
    virtual std::string GetError() const = 0;
};
using SnapshotGeneratorHolder = std::shared_ptr<SnapshotGenerator>;

MEDIACORE_API SnapshotGeneratorHolder CreateSnapshotGenerator();
MEDIACORE_API Logger::ALogger* GetSnapshotGeneratorLogger();
