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

#include <sstream>
#include "AudioClip.h"
#include "Logger.h"

using namespace std;
using namespace Logger;

static string GetFileNameFromPath(const string& path)
{
    auto pos = path.rfind("/");
    if (pos == string::npos)
        pos = path.rfind("\\");
    auto fileName = pos==string::npos ? path : path.substr(pos+1);
    return std::move(fileName);
}

namespace MediaCore
{
    ///////////////////////////////////////////////////////////////////////////////////////////
    // AudioClip
    ///////////////////////////////////////////////////////////////////////////////////////////
    class AudioClip_AudioImpl : public AudioClip
    {
    public:
        AudioClip_AudioImpl(
            int64_t id, MediaParserHolder hParser,
            uint32_t outChannels, uint32_t outSampleRate, const string& outSampleFormat,
            int64_t start, int64_t startOffset, int64_t endOffset, bool exclusiveLogger = false)
            : m_id(id), m_start(start)
        {
            m_hInfo = hParser->GetMediaInfo();
            if (hParser->GetBestAudioStreamIndex() < 0)
                throw invalid_argument("Argument 'hParser' has NO AUDIO stream!");
            string loggerName = "";
            if (exclusiveLogger)
            {
                auto fileName = GetFileNameFromPath(hParser->GetUrl());
                ostringstream oss;
                oss << "AUD@" << fileName << "";
                loggerName = oss.str();
            }
            m_srcReader = CreateMediaReader(loggerName);
            if (!m_srcReader->Open(hParser))
                throw runtime_error(m_srcReader->GetError());
            if (!m_srcReader->ConfigAudioReader(outChannels, outSampleRate, outSampleFormat))
                throw runtime_error(m_srcReader->GetError());
            m_srcDuration = (int64_t)(m_srcReader->GetAudioStream()->duration*1000);
            if (startOffset < 0)
                throw invalid_argument("Argument 'startOffset' can NOT be NEGATIVE!");
            if (endOffset < 0)
                throw invalid_argument("Argument 'endOffset' can NOT be POSITIVE!");
            if (startOffset+endOffset >= m_srcDuration)
                throw invalid_argument("Argument 'startOffset/endOffset', clip duration is NOT LARGER than 0!");
            m_start = start;
            m_startOffset = startOffset;
            m_endOffset = endOffset;
            m_totalSamples = Duration()*outSampleRate/1000;
            if (!m_srcReader->Start())
                throw runtime_error(m_srcReader->GetError());
        }

        ~AudioClip_AudioImpl()
        {
            ReleaseMediaReader(&m_srcReader);
        }

        AudioClipHolder Clone(uint32_t outChannels, uint32_t outSampleRate, const string& outSampleFormat) const override
        {
            AudioClipHolder newInstance = AudioClipHolder(new AudioClip_AudioImpl(
                m_id, m_srcReader->GetMediaParser(), outChannels, outSampleRate, outSampleFormat, m_start, m_startOffset, m_endOffset));
            return newInstance;
        }

        MediaParserHolder GetMediaParser() const override
        {
            return m_srcReader->GetMediaParser();
        }

        int64_t Id() const override
        {
            return m_id;
        }

        int64_t TrackId() const override
        {
            return m_trackId;
        }

        int64_t Start() const override
        {
            return m_start;
        }

        int64_t End() const override
        {
            return m_start+Duration();
        }

        int64_t StartOffset() const override
        {
            return m_startOffset;
        }

        int64_t EndOffset() const override
        {
            return m_endOffset;
        }

        int64_t Duration() const override
        {
            return m_srcDuration-m_startOffset-m_endOffset;
        }

        int64_t ReadPos() const override
        {
            return m_readSamples*1000/m_srcReader->GetAudioOutSampleRate()+m_start;
        }

        uint32_t OutChannels() const override
        {
            return m_srcReader->GetAudioOutChannels();
        }

        uint32_t OutSampleRate() const override
        {
            return m_srcReader->GetAudioOutSampleRate();
        }

        uint32_t LeftSamples() const override
        {
            bool isForward = m_srcReader->IsDirectionForward();
            if (isForward)
                return m_totalSamples > m_readSamples ? (uint32_t)(m_totalSamples-m_readSamples) : 0;
            else
                return m_readSamples > m_totalSamples ? 0 : (m_readSamples >= 0 ? (uint32_t)m_readSamples : 0);
        }

        void SetTrackId(int64_t trackId) override
        {
            m_trackId = trackId;
        }

        void SetStart(int64_t start) override
        {
            int64_t bias = start-m_start;
            m_start = start;
        }

        void ChangeStartOffset(int64_t startOffset) override
        {
            if (startOffset == m_startOffset)
                return;
            if (startOffset < 0)
                throw invalid_argument("Argument 'startOffset' can NOT be NEGATIVE!");
            if (startOffset+m_endOffset >= m_srcDuration)
                throw invalid_argument("Argument 'startOffset/endOffset', clip duration is NOT LARGER than 0!");
            m_startOffset = startOffset;
            const int64_t newTotalSamples = Duration()*m_srcReader->GetAudioOutSampleRate()/1000;
            m_readSamples += newTotalSamples-m_totalSamples;
            m_totalSamples = newTotalSamples;
        }

        void ChangeEndOffset(int64_t endOffset) override
        {
            if (endOffset == m_endOffset)
                return;
            if (endOffset < 0)
                throw invalid_argument("Argument 'endOffset' can NOT be POSITIVE!");
            if (m_startOffset+endOffset >= m_srcDuration)
                throw invalid_argument("Argument 'startOffset/endOffset', clip duration is NOT LARGER than 0!");
            m_endOffset = endOffset;
            m_totalSamples = Duration()*m_srcReader->GetAudioOutSampleRate()/1000;
        }

        void SeekTo(int64_t pos) override
        {
            if (pos < 0)
                pos = 0;
            else if (pos > Duration())
                pos = Duration()-1;
            const double p = (double)(pos+m_startOffset)/1000;
            if (!m_srcReader->SeekTo(p))
                throw runtime_error(m_srcReader->GetError());
            m_readSamples = pos*m_srcReader->GetAudioOutSampleRate()/1000;
            m_eof = false;
        }

        ImGui::ImMat ReadAudioSamples(uint32_t& readSamples, bool& eof) override
        {
            const uint32_t leftSamples = LeftSamples();
            if (m_eof || leftSamples == 0)
            {
                readSamples = 0;
                m_eof = eof = true;
                return ImGui::ImMat();
            }

            uint32_t sampleRate = m_srcReader->GetAudioOutSampleRate();
            if (m_pcmFrameSize == 0)
            {
                m_pcmFrameSize = m_srcReader->GetAudioOutFrameSize();
                m_pcmSizePerSec = sampleRate*m_pcmFrameSize;
            }

            if (readSamples > leftSamples)
                readSamples = leftSamples;
            int channels = m_srcReader->GetAudioOutChannels();
            ImGui::ImMat amat;
            bool srceof{false};
            if (!m_srcReader->ReadAudioSamples(amat, readSamples, srceof))
                throw runtime_error(m_srcReader->GetError());
            double srcpos = amat.time_stamp;
            amat.time_stamp = (double)m_readSamples/sampleRate+(double)m_start/1000.;
            readSamples = amat.w;
            // Log(WARN) << "~~~ srcpos=" << srcpos << ", ts=" << amat.time_stamp << ", delta=" << (srcpos-amat.time_stamp) << endl;
            const bool isForward = m_srcReader->IsDirectionForward();
            if (isForward)
                m_readSamples += readSamples;
            else
                m_readSamples -= readSamples;
            const uint32_t leftSamples2 = LeftSamples();
            if (leftSamples2 == 0)
                m_eof = eof = true;

            if (m_filter)
                amat = m_filter->FilterPcm(amat, (int64_t)(amat.time_stamp*1000) - m_start);
            return amat;
        }

        void SetDirection(bool forward) override
        {
            m_srcReader->SetDirection(forward);
        }

        void SetFilter(AudioFilterHolder filter) override
        {
            if (filter)
            {
                filter->ApplyTo(this);
                m_filter = filter;
            }
            else
            {
                m_filter = nullptr;
            }
        }

        AudioFilterHolder GetFilter() const override
        {
            return m_filter;
        }


    private:
        int64_t m_id;
        int64_t m_trackId{-1};
        MediaInfo::InfoHolder m_hInfo;
        MediaReader* m_srcReader;
        AudioFilterHolder m_filter;
        int64_t m_srcDuration;
        int64_t m_start;
        int64_t m_startOffset;
        int64_t m_endOffset;
        uint32_t m_pcmSizePerSec{0};
        uint32_t m_pcmFrameSize{0};
        int64_t m_readSamples{0};
        int64_t m_totalSamples;
        bool m_eof{false};
    };

    AudioClipHolder AudioClip::CreateInstance(
        int64_t id, MediaParserHolder hParser,
        uint32_t outChannels, uint32_t outSampleRate, const string& outSampleFormat,
        int64_t start, int64_t startOffset, int64_t endOffset)
    {
        return AudioClipHolder(new AudioClip_AudioImpl(id, hParser, outChannels, outSampleRate, outSampleFormat, start, startOffset, endOffset));
    }

    ostream& operator<<(ostream& os, AudioClip& clip)
    {
            os << "{'id':" << clip.Id() << ", 'start':" << clip.Start() << ", 'dur':" << clip.Duration()
                << ", 'soff':" << clip.StartOffset() << ", 'eoff':" << clip.EndOffset() << "}";
        return os;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // DefaultAudioTransition_Impl
    ///////////////////////////////////////////////////////////////////////////////////////////
    class DefaultAudioTransition_Impl : public AudioTransition
    {
    public:
        void ApplyTo(AudioOverlap* overlap) override
        {
            m_overlapPtr = overlap;
        }

        ImGui::ImMat MixTwoAudioMats(const ImGui::ImMat& amat1, const ImGui::ImMat& amat2, int64_t pos) override
        {
            return amat2;
        }

    private:
        AudioOverlap* m_overlapPtr{nullptr};
    };

    ///////////////////////////////////////////////////////////////////////////////////////////
    // AudioOverlap
    ///////////////////////////////////////////////////////////////////////////////////////////
    AudioOverlap::AudioOverlap(int64_t id, AudioClipHolder hClip1, AudioClipHolder hClip2)
        : m_id(id), m_frontClip(hClip1), m_rearClip(hClip2), m_transition(new DefaultAudioTransition_Impl())
    {
        Update();
        m_transition->ApplyTo(this);
    }

    void AudioOverlap::Update()
    {
        AudioClipHolder hClip1 = m_frontClip;
        AudioClipHolder hClip2 = m_rearClip;
        if (hClip1->Start() <= hClip2->Start())
        {
            m_frontClip = hClip1;
            m_rearClip = hClip2;
        }
        else
        {
            m_frontClip = hClip2;
            m_rearClip = hClip1;
        }
        if (m_frontClip->End() <= m_rearClip->Start())
        {
            m_start = m_end = 0;
        }
        else
        {
            m_start = m_rearClip->Start();
            m_end = m_frontClip->End() <= m_rearClip->End() ? m_frontClip->End() : m_rearClip->End();
        }
    }

    void AudioOverlap::SetTransition(AudioTransitionHolder transition)
    {
        if (transition)
        {
            transition->ApplyTo(this);
            m_transition = transition;
        }
        else
        {
            AudioTransitionHolder defaultTrans(new DefaultAudioTransition_Impl());
            defaultTrans->ApplyTo(this);
            m_transition = defaultTrans;
        }
    }

    void AudioOverlap::SeekTo(int64_t pos)
    {
        if (pos > Duration())
            return;
        if (pos < 0)
            pos = 0;
        int64_t pos1 = pos+(Start()-m_frontClip->Start());
        m_frontClip->SeekTo(pos1);
        int64_t pos2 = pos+(Start()-m_rearClip->Start());
        m_rearClip->SeekTo(pos2);
    }

    ImGui::ImMat AudioOverlap::ReadAudioSamples(uint32_t& readSamples, bool& eof)
    {
        const uint32_t leftSamples1 = m_frontClip->LeftSamples();
        if (leftSamples1 < readSamples)
            readSamples = leftSamples1;
        const uint32_t leftSamples2 = m_rearClip->LeftSamples();
        if (leftSamples2 < readSamples)
            readSamples = leftSamples2;
        if (readSamples == 0)
        {
            eof = true;
            return ImGui::ImMat();
        }

        bool eof1{false};
        ImGui::ImMat amat1 = m_frontClip->ReadAudioSamples(readSamples, eof1);
        bool eof2{false};
        ImGui::ImMat amat2 = m_rearClip->ReadAudioSamples(readSamples, eof2);
        AudioTransitionHolder transition = m_transition;
        ImGui::ImMat amat = transition->MixTwoAudioMats(amat1, amat2, (int64_t)(amat1.time_stamp*1000));
        eof = eof1 || eof2;
        return amat;
    }

    std::ostream& operator<<(std::ostream& os, AudioOverlap& overlap)
    {
        os << "{'id':" << overlap.Id() << ", 'start':" << overlap.Start() << ", 'dur':" << overlap.Duration() << "}";
        return os;
    }
}
