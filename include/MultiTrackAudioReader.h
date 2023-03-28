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
#include <list>
#include "immat.h"
#include "AudioTrack.h"
#include "AudioEffectFilter.h"
#include "Logger.h"
#include "MediaCore.h"

struct MultiTrackAudioReader
{
    virtual bool Configure(uint32_t outChannels, uint32_t outSampleRate, uint32_t outSamplesPerFrame = 1024) = 0;
    virtual MultiTrackAudioReader* CloneAndConfigure(uint32_t outChannels, uint32_t outSampleRate, uint32_t outSamplesPerFrame) = 0;
    virtual bool Start() = 0;
    virtual void Close() = 0;
    virtual MediaCore::AudioTrackHolder AddTrack(int64_t trackId) = 0;
    virtual MediaCore::AudioTrackHolder RemoveTrackByIndex(uint32_t index) = 0;
    virtual MediaCore::AudioTrackHolder RemoveTrackById(int64_t trackId) = 0;
    virtual bool SetDirection(bool forward) = 0;
    virtual bool SeekTo(int64_t pos) = 0;
    virtual bool ReadAudioSamplesEx(std::vector<MediaCore::CorrelativeFrame>& amats, bool& eof) = 0;
    virtual bool ReadAudioSamples(ImGui::ImMat& amat, bool& eof) = 0;
    virtual void UpdateDuration() = 0;
    virtual bool Refresh() = 0;
    virtual int64_t SizeToDuration(uint32_t sizeInByte) = 0;

    virtual int64_t Duration() const = 0;
    virtual int64_t ReadPos() const = 0;

    virtual uint32_t TrackCount() const = 0;
    virtual std::list<MediaCore::AudioTrackHolder>::iterator TrackListBegin() = 0;
    virtual std::list<MediaCore::AudioTrackHolder>::iterator TrackListEnd() = 0;
    virtual MediaCore::AudioTrackHolder GetTrackByIndex(uint32_t idx) = 0;
    virtual MediaCore::AudioTrackHolder GetTrackById(int64_t trackId, bool createIfNotExists = false) = 0;
    virtual MediaCore::AudioClipHolder GetClipById(int64_t clipId) = 0;
    virtual MediaCore::AudioOverlapHolder GetOverlapById(int64_t ovlpId) = 0;
    virtual MediaCore::AudioEffectFilterHolder GetAudioEffectFilter() = 0;

    virtual std::string GetError() const = 0;

    friend std::ostream& operator<<(std::ostream& os, MultiTrackAudioReader& mtaReader)
    {
        os << ">>> MultiTrackAudioReader :" << std::endl;
        auto trackIter = mtaReader.TrackListBegin();
        while (trackIter != mtaReader.TrackListEnd())
        {
            auto& track = *trackIter;
            os << "\t Track#" << track->Id() << " : " << track << std::endl;
            trackIter++;
        }
        os << "<<< [END]MultiTrackAudioReader";
        return os;
    }
};

MEDIACORE_API MultiTrackAudioReader* CreateMultiTrackAudioReader();
MEDIACORE_API void ReleaseMultiTrackAudioReader(MultiTrackAudioReader** mreader);

MEDIACORE_API Logger::ALogger* GetMultiTrackAudioReaderLogger();