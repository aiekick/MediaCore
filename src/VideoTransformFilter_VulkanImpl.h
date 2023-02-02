#pragma once
#include "VideoTransformFilter_Base.h"
#include <warpAffine_vulkan.h>
// #include <Crop_vulkan.h>

namespace MediaCore
{
    class VideoTransformFilter_VulkanImpl : public VideoTransformFilter_Base
    {
    public:
        const std::string GetFilterName() const override;
        bool Initialize(uint32_t outWidth, uint32_t outHeight) override;
        VideoTransformFilterHolder Clone(uint32_t outWidth, uint32_t outHeight) override { return nullptr; }
        bool SetOutputFormat(const std::string& outputFormat) override;
        ImGui::ImMat FilterImage(const ImGui::ImMat& vmat, int64_t pos) override;

    private:
        bool _filterImage(const ImGui::ImMat& inMat, ImGui::ImMat& outMat, int64_t pos);
        void UpdatePassThrough();

    private:
        ImGui::warpAffine_vulkan m_warpAffine;
        ImGui::ImMat m_affineMat;
        double m_realScaleRatioH{1}, m_realScaleRatioV{1};
        ImPixel m_cropRect;
        ImInterpolateMode m_interpMode{IM_INTERPOLATE_BICUBIC};
        bool m_passThrough{false};
        std::string m_errMsg;
    };
}