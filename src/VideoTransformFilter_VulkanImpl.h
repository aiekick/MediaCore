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
#include "VideoTransformFilter_Base.h"
#include <warpAffine_vulkan.h>

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