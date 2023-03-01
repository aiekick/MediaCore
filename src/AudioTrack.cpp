#include <sstream>
#include <algorithm>
#include "AudioTrack.h"
#include "FFUtils.h"
extern "C"
{
    #include "libavutil/samplefmt.h"
}

using namespace std;
using namespace Logger;

namespace MediaCore
{
class AudioTrack_Impl : public AudioTrack
{
public:
    AudioTrack_Impl(int64_t id, uint32_t outChannels, uint32_t outSampleRate, const string& outSampleFormat)
        : m_id(id), m_outChannels(outChannels), m_outSampleRate(outSampleRate), m_outSampleFormat(outSampleFormat)
    {
        AVSampleFormat m_outAvSmpfmt = av_get_sample_fmt(outSampleFormat.c_str());
        if (m_outAvSmpfmt == AV_SAMPLE_FMT_NONE)
        {
            ostringstream oss;
            oss << "'" << outSampleFormat << "' is NOT a VALID pcm SAMPLE FORMAT!";
            throw runtime_error(oss.str());
        }
        m_logger = GetAudioTrackLogger();
        m_readClipIter = m_clips.begin();
        m_bytesPerSample = (uint8_t)av_get_bytes_per_sample(m_outAvSmpfmt);
        m_frameSize = outChannels*m_bytesPerSample;
        m_pcmSizePerSec = m_frameSize*m_outSampleRate;
        ostringstream loggerNameOss;
        loggerNameOss << "AEFilter#" << id;
        m_aeFilter = CreateAudioEffectFilter(loggerNameOss.str());
        if (!m_aeFilter->Init(
            AudioEffectFilter::VOLUME|AudioEffectFilter::LIMITER|AudioEffectFilter::PAN,
            outSampleFormat, outChannels, outSampleRate))
            throw runtime_error(m_aeFilter->GetError());
    }

    virtual ~AudioTrack_Impl() {}

    AudioTrackHolder Clone(uint32_t outChannels, uint32_t outSampleRate, const string& outSampleFormat) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        AudioTrack_Impl* newInstance = new AudioTrack_Impl(m_id, outChannels, outSampleRate, outSampleFormat);
        // duplicate the clips
        for (auto clip : m_clips)
        {
            auto newClip = clip->Clone(outChannels, outSampleRate, outSampleFormat);
            newInstance->m_clips.push_back(newClip);
            newClip->SetTrackId(m_id);
            AudioClipHolder lastClip = newInstance->m_clips.back();
            newInstance->m_duration = lastClip->Start()+lastClip->Duration();
            newInstance->UpdateClipOverlap(newClip);
        }
        return AudioTrackHolder(newInstance);
    }

    AudioClipHolder AddNewClip(int64_t clipId, MediaParserHolder hParser, int64_t start, int64_t startOffset, int64_t endOffset) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        AudioClipHolder hClip = AudioClip::CreateInstance(clipId, hParser, m_outChannels, m_outSampleRate, m_outSampleFormat, start, startOffset, endOffset);
        InsertClip(hClip);
        return hClip;
    }

    void InsertClip(AudioClipHolder hClip) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (!CheckClipRangeValid(hClip->Id(), hClip->Start(), hClip->End()))
            throw invalid_argument("Invalid argument for inserting clip!");

        // add this clip into clip list
        hClip->SetDirection(m_readForward);
        m_clips.push_back(hClip);
        hClip->SetTrackId(m_id);
        m_clips.sort(CLIP_SORT_CMP);
        // update track duration
        AudioClipHolder lastClip = m_clips.back();
        m_duration = lastClip->Start()+lastClip->Duration();
        // update overlap
        UpdateClipOverlap(hClip);
    }

    void MoveClip(int64_t id, int64_t start) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        AudioClipHolder hClip = GetClipById(id);
        if (!hClip)
            throw invalid_argument("Invalid value for argument 'id'!");

        if (hClip->Start() == start)
            return;
        else
            hClip->SetStart(start);

        if (!CheckClipRangeValid(id, hClip->Start(), hClip->End()))
            throw invalid_argument("Invalid argument for moving clip!");

        // update clip order
        m_clips.sort(CLIP_SORT_CMP);
        // update track duration
        AudioClipHolder lastClip = m_clips.back();
        m_duration = lastClip->Start()+lastClip->Duration();
        // update overlap
        UpdateClipOverlap(hClip);
    }

    void ChangeClipRange(int64_t id, int64_t startOffset, int64_t endOffset) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        AudioClipHolder hClip = GetClipById(id);
        if (!hClip)
            throw invalid_argument("Invalid value for argument 'id'!");

        bool rangeChanged = false;
        if (startOffset != hClip->StartOffset())
        {
            int64_t bias = startOffset-hClip->StartOffset();
            hClip->ChangeStartOffset(startOffset);
            hClip->SetStart(hClip->Start()+bias);
            rangeChanged = true;
        }
        if (endOffset != hClip->EndOffset())
        {
            hClip->ChangeEndOffset(endOffset);
            rangeChanged = true;
        }
        if (!rangeChanged)
            return;

        if (!CheckClipRangeValid(id, hClip->Start(), hClip->End()))
            throw invalid_argument("Invalid argument for changing clip range!");

        // update clip order
        m_clips.sort(CLIP_SORT_CMP);
        // update track duration
        AudioClipHolder lastClip = m_clips.back();
        m_duration = lastClip->Start()+lastClip->Duration();
        // update overlap
        UpdateClipOverlap(hClip);
    }

    AudioClipHolder RemoveClipById(int64_t clipId) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        auto iter = find_if(m_clips.begin(), m_clips.end(), [clipId](const AudioClipHolder& clip) {
            return clip->Id() == clipId;
        });
        if (iter == m_clips.end())
            return nullptr;

        AudioClipHolder hClip = (*iter);
        m_clips.erase(iter);
        hClip->SetTrackId(-1);
        UpdateClipOverlap(hClip, true);

        if (m_clips.empty())
            m_duration = 0;
        else
        {
            AudioClipHolder lastClip = m_clips.back();
            m_duration = lastClip->Start()+lastClip->Duration();
        }
        return hClip;
    }

    AudioClipHolder RemoveClipByIndex(uint32_t index) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (index >= m_clips.size())
            throw invalid_argument("Argument 'index' exceeds the count of clips!");

        auto iter = m_clips.begin();
        while (index > 0)
        {
            iter++; index--;
        }

        AudioClipHolder hClip = (*iter);
        m_clips.erase(iter);
        hClip->SetTrackId(-1);
        UpdateClipOverlap(hClip, true);

        if (m_clips.empty())
            m_duration = 0;
        else
        {
            AudioClipHolder lastClip = m_clips.back();
            m_duration = lastClip->Start()+lastClip->Duration();
        }
        return hClip;
    }

    void SeekTo(int64_t pos) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (pos < 0)
            throw invalid_argument("Argument 'pos' can NOT be NEGATIVE!");

        if (m_readForward)
        {
            // update read clip iterator
            m_readClipIter = m_clips.end();
            {
                auto iter = m_clips.begin();
                while (iter != m_clips.end())
                {
                    const AudioClipHolder& hClip = *iter;
                    int64_t clipPos = pos-hClip->Start();
                    hClip->SeekTo(clipPos);
                    if (m_readClipIter == m_clips.end() && clipPos < hClip->Duration())
                        m_readClipIter = iter;
                    iter++;
                }
            }
            // update read overlap iterator
            m_readOverlapIter = m_overlaps.end();
            {
                auto iter = m_overlaps.begin();
                while (iter != m_overlaps.end())
                {
                    const AudioOverlapHolder& hOverlap = *iter;
                    int64_t overlapPos = pos-hOverlap->Start();
                    if (m_readOverlapIter == m_overlaps.end() && overlapPos < hOverlap->Duration())
                    {
                        m_readOverlapIter = iter;
                        break;
                    }
                    iter++;
                }
            }
        }
        else
        {
            m_readClipIter = m_clips.end();
            {
                auto riter = m_clips.rbegin();
                while (riter != m_clips.rend())
                {
                    const AudioClipHolder& hClip = *riter;
                    int64_t clipPos = pos-hClip->Start();
                    hClip->SeekTo(clipPos);
                    if (m_readClipIter == m_clips.end() && clipPos >= 0)
                        m_readClipIter = riter.base();
                    riter++;
                }
            }
            m_readOverlapIter = m_overlaps.end();
            {
                auto riter = m_overlaps.rbegin();
                while (riter != m_overlaps.rend())
                {
                    const AudioOverlapHolder& hOverlap = *riter;
                    int64_t overlapPos = pos-hOverlap->Start();
                    if (m_readOverlapIter == m_overlaps.end() && overlapPos >= 0)
                        m_readOverlapIter = riter.base();
                    riter++;
                }
            }
        }

        m_readSamples = pos*m_outSampleRate/1000;
    }

    AudioEffectFilterHolder GetAudioEffectFilter() override
    {
        return m_aeFilter;
    }

    void ReadAudioSamples(uint8_t* buf, uint32_t& size, double& pos)
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        pos = (double)m_readSamples/m_outSampleRate;
        uint32_t readSamples = 0, toReadSamples = size/m_frameSize;
        unique_ptr<uint8_t*[]> planbuf(new uint8_t* [m_outChannels]);
        for (int i = 0; i < m_outChannels; i++)
            planbuf[i] = buf+i*toReadSamples*m_bytesPerSample;
        if (m_overlaps.empty())
        {
            readSamples = ReadClipData(planbuf.get(), toReadSamples);
            size = readSamples*m_frameSize;
            return;
        }

        uint32_t toReadSamples2, readSamples2;
        if (m_readForward)
        {
            int64_t readPosBegin = m_readSamples*1000/m_outSampleRate;
            int64_t readPosEnd = (m_readSamples+size/m_frameSize)*1000/m_outSampleRate;
            while (readSamples < toReadSamples && m_readOverlapIter != m_overlaps.end() && (*m_readOverlapIter)->Start() < readPosEnd)
            {
                auto& ovlp = *m_readOverlapIter;
                if (ovlp->Start() > readPosBegin)
                {
                    toReadSamples2 = (ovlp->Start()-readPosBegin)*m_outSampleRate/1000;
                    if (toReadSamples2 > toReadSamples-readSamples)
                        toReadSamples2 = toReadSamples-readSamples;
                    readSamples2 = ReadClipData(planbuf.get(), toReadSamples2);
                    readSamples += readSamples2;
                    if (m_isPlanar)
                    {
                        for (int i = 0; i < m_outChannels; i++)
                            planbuf[i] += readSamples2*m_bytesPerSample;
                    }
                    else
                    {
                        planbuf[0] += readSamples2*m_frameSize;
                    }
                }
                if (readSamples >= toReadSamples)
                    break;

                bool eof = false;
                toReadSamples2 = toReadSamples-readSamples;
                ImGui::ImMat amat = ovlp->ReadAudioSamples(toReadSamples2, eof);
                if (!amat.empty())
                {
                    CopyMatData(planbuf.get(), 0, amat);
                    readSamples += amat.w;
                    if (m_isPlanar)
                    {
                        for (int i = 0; i < m_outChannels; i++)
                            planbuf[i] += amat.w*m_bytesPerSample;
                    }
                    else
                    {
                        planbuf[0] += amat.w*m_frameSize;
                    }
                    m_readSamples += amat.w;
                }
                if (eof)
                {
                    m_readOverlapIter++;
                    if (m_readOverlapIter == m_overlaps.end())
                        break;
                }
            }
            if (readSamples < toReadSamples)
            {
                toReadSamples2 = toReadSamples-readSamples;
                readSamples2 = ReadClipData(planbuf.get(), toReadSamples2);
                readSamples += readSamples2;
            }
        }
        else
        {
            if (m_readOverlapIter == m_overlaps.end()) m_readOverlapIter--;
            int64_t readPosBegin = m_readSamples*1000/m_outSampleRate;
            int64_t readPosEnd = (m_readSamples-size/m_frameSize)*1000/m_outSampleRate;
            while (readSamples < toReadSamples && (m_readOverlapIter != m_overlaps.begin() || readPosBegin > (*m_readOverlapIter)->Start()))
            {
                auto& ovlp = *m_readOverlapIter;
                if (readPosBegin > ovlp->End())
                {
                    toReadSamples2 = (readPosBegin-ovlp->End())*m_outSampleRate/1000;
                    if (toReadSamples2 > toReadSamples-readSamples)
                        toReadSamples2 = toReadSamples-readSamples;
                    readSamples2 = ReadClipData(planbuf.get(), toReadSamples2);
                    readSamples += readSamples2;
                    if (m_isPlanar)
                    {
                        for (int i = 0; i < m_outChannels; i++)
                            planbuf[i] += readSamples2*m_bytesPerSample;
                    }
                    else
                    {
                        planbuf[0] += readSamples2*m_frameSize;
                    }
                }
                if (readSamples >= toReadSamples)
                    break;

                bool eof = false;
                toReadSamples2 = toReadSamples-readSamples;
                ImGui::ImMat amat = ovlp->ReadAudioSamples(toReadSamples2, eof);
                if (!amat.empty())
                {
                    CopyMatData(planbuf.get(), 0, amat);
                    readSamples += amat.w;
                    if (m_isPlanar)
                    {
                        for (int i = 0; i < m_outChannels; i++)
                            planbuf[i] += amat.w*m_bytesPerSample;
                    }
                    else
                    {
                        planbuf[0] += amat.w*m_frameSize;
                    }
                    m_readSamples += amat.w;
                }
                if (eof)
                {
                    if (m_readOverlapIter != m_overlaps.begin())
                        m_readOverlapIter--;
                    else
                        break;
                }
            }
            if (readSamples < toReadSamples)
            {
                toReadSamples2 = toReadSamples-readSamples;
                readSamples2 = ReadClipData(planbuf.get(), toReadSamples2);
                readSamples += readSamples2;
            }
        }
        size = readSamples*m_frameSize;
    }

    ImGui::ImMat ReadAudioSamples(uint32_t readSamples) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        ImGui::ImMat amat;
        amat.create((int)readSamples, 1, (int)OutChannels(), (size_t)m_bytesPerSample);
        uint32_t bufSize = amat.total()*amat.elemsize;
        while (m_cachedSamples-m_readCacheOffsetSamples < readSamples)
        {
            double pos = 0;
            uint32_t readSize = bufSize;
            ReadAudioSamples((uint8_t*)amat.data, readSize, pos);
            amat.elempack = 1;
            amat.rate = { (int)OutSampleRate(), 1 };
            amat.time_stamp = pos;
            if (readSize < bufSize)
            {
                if (m_isPlanar)
                {
                    uint8_t* bufPtr = (uint8_t*)amat.data+readSize/m_outChannels;
                    int lineSize = readSamples*m_bytesPerSample;
                    int sizeToZero = (bufSize-readSize)/m_outChannels;
                    for (int i = 0; i < m_outChannels; i++)
                    {
                        memset(bufPtr, 0, sizeToZero);
                        bufPtr += lineSize;
                    }
                }
                else
                {
                    uint8_t* bufPtr = (uint8_t*)amat.data+readSize;
                    memset(bufPtr, 0, bufSize-readSize);
                }
            }

            // apply audio effect(s)
            list<ImGui::ImMat> aeOutMats;
            if (!m_aeFilter->ProcessData(amat, aeOutMats))
            {
                m_logger->Log(Error) << "ID#" << m_id << " FAILED to invoke AudioEffectFilter::ProcessData()! Error is '" << m_aeFilter->GetError() << "'." << endl;
            }
            auto iter = aeOutMats.begin();
            while (iter != aeOutMats.end())
            {
                auto& m = *iter++;
                if (!m.empty() && m.w > 0)
                {
                    m_cachedMats.push_back(m);
                    m_cachedSamples += (uint32_t)m.w;
                }                         
            }
        }

        uint32_t copiedSamples = 0;
        unique_ptr<uint8_t*[]> dstbufs(new uint8_t* [m_outChannels]);
        if (m_isPlanar)
        {
            int dstLineSize = readSamples*m_bytesPerSample;
            for (int i = 0; i < m_outChannels; i++)
                dstbufs[i] = (uint8_t*)amat.data+dstLineSize*i;
        }
        else
        {
            dstbufs[0] = (uint8_t*)amat.data;
        }
        while (copiedSamples < readSamples)
        {
            auto& srcmat = m_cachedMats.front();
            unique_ptr<const uint8_t*[]> srcbufs(new const uint8_t* [m_outChannels]);
            if (m_isPlanar)
            {
                int srcLineSize = srcmat.w*m_bytesPerSample;
                for (int i = 0; i < m_outChannels; i++)
                    srcbufs[i] = (const uint8_t*)srcmat.data+srcLineSize*i;
            }
            else
            {
                srcbufs[0] = (const uint8_t*)srcmat.data;
            }
            uint32_t toCopySamples = readSamples-copiedSamples;
            if (toCopySamples > (uint32_t)srcmat.w-m_readCacheOffsetSamples)
                toCopySamples = (uint32_t)srcmat.w-m_readCacheOffsetSamples;
            uint32_t copied = FFUtils::CopyPcmDataEx((uint8_t)m_outChannels, m_bytesPerSample, toCopySamples,
                m_isPlanar, dstbufs.get(), copiedSamples, m_isPlanar, srcbufs.get(), m_readCacheOffsetSamples);
            copiedSamples += copied;
            m_readCacheOffsetSamples += copied;
            if (m_readCacheOffsetSamples >= (uint32_t)srcmat.w)
            {
                m_cachedSamples -= (uint32_t)srcmat.w;
                m_cachedMats.pop_front();
                m_readCacheOffsetSamples = 0;
            }
        }

        return amat;
    }

    void SetDirection(bool forward) override
    {
        if (m_readForward == forward)
            return;
        m_readForward = forward;
        for (auto& clip : m_clips)
            clip->SetDirection(forward);
    }

    AudioClipHolder GetClipByIndex(uint32_t index) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        if (index >= m_clips.size())
            return nullptr;
        auto iter = m_clips.begin();
        while (index > 0)
        {
            iter++; index--;
        }
        return *iter;
    }

    AudioClipHolder GetClipById(int64_t id) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        auto iter = find_if(m_clips.begin(), m_clips.end(), [id] (const AudioClipHolder& clip) {
            return clip->Id() == id;
        });
        if (iter != m_clips.end())
            return *iter;
        return nullptr;
    }

    AudioOverlapHolder GetOverlapById(int64_t id) override
    {
        lock_guard<recursive_mutex> lk(m_apiLock);
        auto iter = find_if(m_overlaps.begin(), m_overlaps.end(), [id] (const AudioOverlapHolder& ovlp) {
            return ovlp->Id() == id;
        });
        if (iter != m_overlaps.end())
            return *iter;
        return nullptr;
    }

    uint32_t ClipCount() const override
    {
        return m_clips.size();
    }

    list<AudioClipHolder>::iterator ClipListBegin() override
    {
        return m_clips.begin();
    }

    list<AudioClipHolder>::iterator ClipListEnd() override
    {
        return m_clips.end();
    }

    uint32_t OverlapCount() const override
    {
        return m_overlaps.size();
    }

    list<AudioOverlapHolder>::iterator OverlapListBegin() override
    {
        return m_overlaps.begin();
    }

    list<AudioOverlapHolder>::iterator OverlapListEnd() override
    {
        return m_overlaps.end();
    }

    int64_t Id() const override
    {
        return m_id;
    }

    int64_t Duration() const override
    {
        return m_duration;
    }

    uint32_t OutChannels() const override
    {
        return m_outChannels;
    }

    uint32_t OutSampleRate() const override
    {
        return m_outSampleRate;
    }

    string OutSampleFormat() const override
    {
        return m_outSampleFormat;
    }

    uint32_t OutFrameSize() const override
    {
        return m_frameSize;
    }

private:
    static function<bool(const AudioClipHolder&, const AudioClipHolder&)> CLIP_SORT_CMP;
    static function<bool(const AudioOverlapHolder&, const AudioOverlapHolder&)> OVERLAP_SORT_CMP;

    bool CheckClipRangeValid(int64_t clipId, int64_t start, int64_t end)
    {
        for (auto& overlap : m_overlaps)
        {
            if (clipId == overlap->FrontClip()->Id() || clipId == overlap->RearClip()->Id())
                continue;
            if ((start > overlap->Start() && start < overlap->End()) ||
                (end > overlap->Start() && end < overlap->End()))
                return false;
        }
        return true;
    }

    void UpdateClipOverlap(AudioClipHolder hUpdateClip, bool remove = false)
    {
        const int64_t id1 = hUpdateClip->Id();
        // remove invalid overlaps
        auto ovIter = m_overlaps.begin();
        while (ovIter != m_overlaps.end())
        {
            auto& hOverlap = *ovIter;
            if (hOverlap->FrontClip()->TrackId() != m_id || hOverlap->RearClip()->TrackId() != m_id)
            {
                ovIter = m_overlaps.erase(ovIter);
                continue;
            }
            if (hOverlap->FrontClip()->Id() == id1 || hOverlap->RearClip()->Id() == id1)
            {
                hOverlap->Update();
                if (hOverlap->Duration() <= 0)
                {
                    ovIter = m_overlaps.erase(ovIter);
                    continue;
                }
            }
            ovIter++;
        }
        if (!remove)
        {
            // add new overlaps
            for (auto& clip : m_clips)
            {
                if (hUpdateClip == clip)
                    continue;
                if (AudioOverlap::HasOverlap(hUpdateClip, clip))
                {
                    const int64_t id2 = clip->Id();
                    auto iter = find_if(m_overlaps.begin(), m_overlaps.end(), [id1, id2] (const AudioOverlapHolder& overlap) {
                        const int64_t idf = overlap->FrontClip()->Id();
                        const int64_t idr = overlap->RearClip()->Id();
                        return (id1 == idf && id2 == idr) || (id1 == idr && id2 == idf);
                    });
                    if (iter == m_overlaps.end())
                    {
                        AudioOverlapHolder hOverlap(new AudioOverlap(0, hUpdateClip, clip));
                        m_overlaps.push_back(hOverlap);
                    }
                }
            }
        }

        // sort overlap by 'Start' time
        m_overlaps.sort(OVERLAP_SORT_CMP);
    }

    uint32_t ReadClipData(uint8_t** buf, uint32_t toReadSamples)
    {
        uint32_t readSamples = 0;
        if (m_readForward)
        {
            if (m_readClipIter == m_clips.end())
                return 0;

            do {
                int64_t readPos = m_readSamples*1000/m_outSampleRate;
                if (readPos < (*m_readClipIter)->Start())
                {
                    int64_t skipSamples = (*m_readClipIter)->Start()*m_outSampleRate/1000-m_readSamples;
                    if (skipSamples > 0)
                    {
                        if (skipSamples > toReadSamples-readSamples)
                            skipSamples = toReadSamples-readSamples;
                        uint32_t skipSize = m_isPlanar ? (uint32_t)(skipSamples*m_bytesPerSample) : (uint32_t)(skipSamples*m_frameSize);
                        if (m_isPlanar)
                        {
                            for (int i = 0; i < m_outChannels; i++)
                                memset(buf[i]+readSamples*m_bytesPerSample, 0, skipSize);
                        }
                        else
                        {
                            memset(buf[0]+readSamples*m_frameSize, 0, skipSize);
                        }
                        readSamples += skipSamples;
                        m_readSamples += skipSamples;
                    }
                    if (readSamples >= toReadSamples)
                        break;
                }

                bool eof = false;
                while (readPos >= (*m_readClipIter)->End())
                {
                    m_readClipIter++;
                    if (m_readClipIter == m_clips.end())
                    {
                        eof = true;
                        break;
                    }
                }
                if (eof)
                    break;

                uint32_t readClipSamples = toReadSamples-readSamples;
                eof = false;
                ImGui::ImMat amat = (*m_readClipIter)->ReadAudioSamples(readClipSamples, eof);
                if (!amat.empty())
                {
                    uint32_t dstOffset = m_isPlanar ? readSamples*m_bytesPerSample : readSamples*m_frameSize;
                    CopyMatData(buf, dstOffset, amat);
                    readSamples += readClipSamples;
                    m_readSamples += readClipSamples;
                }
                if (eof)
                    m_readClipIter++;
            }
            while (readSamples < toReadSamples && m_readClipIter != m_clips.end());
        }
        else
        {
            if (m_readSamples <= 0 || m_clips.empty())
                return 0;
            if (m_readClipIter == m_clips.end())
                m_readClipIter--;

            do
            {
                int64_t readPos = m_readSamples*1000/m_outSampleRate;
                if (readPos > (*m_readClipIter)->End())
                {
                    int64_t skipSamples = m_readSamples-(*m_readClipIter)->End()*m_outSampleRate/1000;
                    if (skipSamples > 0)
                    {
                        if (skipSamples > toReadSamples-readSamples)
                            skipSamples = toReadSamples-readSamples;
                        uint32_t skipSize = m_isPlanar ? (uint32_t)(skipSamples*m_bytesPerSample) : (uint32_t)(skipSamples*m_frameSize);
                        if (m_isPlanar)
                        {
                            for (int i = 0; i < m_outChannels; i++)
                                memset(buf[i]+readSamples*m_bytesPerSample, 0, skipSize);
                        }
                        else
                        {
                            memset(buf[0]+readSamples*m_frameSize, 0, skipSize);
                        }
                        readSamples += skipSamples;
                        m_readSamples -= skipSamples;
                    }
                    if (readSamples >= toReadSamples || m_readSamples <= 0)
                        break;
                }

                bool eof = false;
                while (readPos <= (*m_readClipIter)->Start())
                {
                    if (m_readClipIter != m_clips.begin())
                        m_readClipIter--;
                    else
                    {
                        eof = true;
                        break;
                    }
                }
                if (eof)
                    break;

                uint32_t readClipSamples = toReadSamples-readSamples;
                eof = false;
                ImGui::ImMat amat = (*m_readClipIter)->ReadAudioSamples(readClipSamples, eof);
                if (!amat.empty())
                {
                    uint32_t dstOffset = m_isPlanar ? readSamples*m_bytesPerSample : readSamples*m_frameSize;
                    CopyMatData(buf, dstOffset, amat);
                    readSamples += readClipSamples;
                    m_readSamples += readClipSamples;
                }
                if (eof)
                {
                    if (m_readClipIter != m_clips.begin())
                        m_readClipIter--;
                    else
                        break;
                }
            }
            while (readSamples < toReadSamples && m_readSamples > 0);
        }
        return readSamples;
    }

    void CopyMatData(uint8_t** dstbuf, uint32_t dstOffset, ImGui::ImMat& srcmat)
    {
        if (m_isPlanar)
        {
            if (srcmat.elempack == 1 || m_outChannels == 1)
            {
                uint32_t lineSize = srcmat.w*m_bytesPerSample;
                uint8_t* srcptr = (uint8_t*)srcmat.data;
                for (int i = 0; i < m_outChannels; i++)
                {
                    memcpy(dstbuf[i]+dstOffset, srcptr, lineSize);
                    srcptr += lineSize;
                }
            }
            else
            {
                unique_ptr<uint8_t*[]> dstlinebuf(new uint8_t* [m_outChannels]);
                for (int i = 0; i < m_outChannels; i++)
                    dstlinebuf[i] = dstbuf[i]+dstOffset;
                uint8_t* srcptr = (uint8_t*)srcmat.data;
                for (int j = 0; j < srcmat.w; j++)
                {
                    for (int i = 0; i < m_outChannels; i++)
                    {
                        memcpy(dstlinebuf[i], srcptr, m_bytesPerSample);
                        dstlinebuf[i] += m_bytesPerSample;
                        srcptr += m_bytesPerSample;
                    }
                }
            }
        }
        else
        {
            if (srcmat.elempack == 1 && m_outChannels != 1)
            {
                uint8_t* dstptr = dstbuf[0]+dstOffset;
                unique_ptr<uint8_t*[]> srclinebuf(new uint8_t* [m_outChannels]);
                for (int i = 0; i < m_outChannels; i++)
                    srclinebuf[i] = (uint8_t*)srcmat.data+i*srcmat.w*m_bytesPerSample;
                for (int j = 0; j < srcmat.w; j++)
                {
                    for (int i = 0; i < m_outChannels; i++)
                    {
                        memcpy(dstptr, srclinebuf[i], m_bytesPerSample);
                        dstptr += m_bytesPerSample;
                        srclinebuf[i] += m_bytesPerSample;
                    }
                }
            }
            else
            {
                memcpy(dstbuf[0]+dstOffset, srcmat.data, srcmat.w*m_frameSize);
            }
        }
    }

private:
    ALogger* m_logger;
    int64_t m_id;
    recursive_mutex m_apiLock;
    uint32_t m_outChannels;
    uint32_t m_outSampleRate;
    string m_outSampleFormat;
    AVSampleFormat m_outAvSmpfmt;
    uint8_t m_bytesPerSample;
    uint32_t m_frameSize;
    uint32_t m_pcmSizePerSec;
    list<AudioClipHolder> m_clips;
    list<AudioClipHolder>::iterator m_readClipIter;
    list<AudioOverlapHolder> m_overlaps;
    list<AudioOverlapHolder>::iterator m_readOverlapIter;
    int64_t m_readSamples{0};
    int64_t m_duration{0};
    list<ImGui::ImMat> m_cachedMats;
    int64_t m_cachedSamples{0};
    uint32_t m_readCacheOffsetSamples{0};
    bool m_readForward{true};
    bool m_isPlanar{true};
    AudioEffectFilterHolder m_aeFilter;
};

function<bool(const AudioClipHolder&, const AudioClipHolder&)> AudioTrack_Impl::CLIP_SORT_CMP =
    [](const AudioClipHolder& a, const AudioClipHolder& b)
    {
        return a->Start() < b->Start();
    };

function<bool(const AudioOverlapHolder&, const AudioOverlapHolder&)> AudioTrack_Impl::OVERLAP_SORT_CMP =
    [](const AudioOverlapHolder& a, const AudioOverlapHolder& b)
    {
        return a->Start() < b->Start();
    };

AudioTrackHolder CreateAudioTrack(int64_t id, uint32_t outChannels, uint32_t outSampleRate, const std::string& outSampleFormat)
{
    return AudioTrackHolder(new AudioTrack_Impl(id, outChannels, outSampleRate, outSampleFormat));
}

ALogger* GetAudioTrackLogger()
{
    return GetLogger("AudioTrack");
}
}
