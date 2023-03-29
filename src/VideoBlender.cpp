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

#include "VideoBlender.h"
#include <imconfig.h>
#if IMGUI_VULKAN_SHADER
#include <imvk_mat.h>
#include <AlphaBlending_vulkan.h>
#endif
#include "FFUtils.h"
#include "Logger.h"

using namespace std;
using namespace Logger;

namespace MediaCore
{
class VideoBlender_Impl : public VideoBlender
{
public:
    VideoBlender_Impl()
    {
#if IMGUI_VULKAN_SHADER
        m_useVulkan = true;
#else
        m_useVulkan = false;
#endif
    }

    bool Init() override
    {
        bool success = m_ffBlender.Init();
        if (!success)
            m_errMsg = m_ffBlender.GetError();
        return success;
    }

    ImGui::ImMat Blend(ImGui::ImMat& baseImage, ImGui::ImMat& overlayImage, int32_t x, int32_t y) override
    {
        ImGui::ImMat res;
        if (m_useVulkan)
        {
#if IMGUI_VULKAN_SHADER
            ImGui::VkMat vkmat;
            vkmat.type = IM_DT_INT8;
            m_vulkanBlender.blend(overlayImage, baseImage, vkmat, x, y);
            if (!vkmat.empty())
            {
                res = vkmat;
                res.time_stamp = baseImage.time_stamp;
                res.duration = baseImage.time_stamp;
                res.color_space = baseImage.color_space;
                res.color_range = baseImage.color_range;
            }
            else
            {
                res = baseImage;
            }
#endif
        }
        else
        {
            res = m_ffBlender.Blend(baseImage, overlayImage, x, y, overlayImage.w, overlayImage.h);
        }
        return res;
    }

    bool Init(const std::string& inputFormat, uint32_t w1, uint32_t h1, uint32_t w2, uint32_t h2, int32_t x, int32_t y) override
    {
        m_ovlyX = x;
        m_ovlyY = y;
        bool success = m_ffBlender.Init(inputFormat, w1, h1, w2, h2, x, y, false);
        if (!success)
            m_errMsg = m_ffBlender.GetError();
        return success;
    }

    ImGui::ImMat Blend(ImGui::ImMat& baseImage, ImGui::ImMat& overlayImage) override
    {
        ImGui::ImMat res;
        if (m_useVulkan)
        {
#if IMGUI_VULKAN_SHADER
            ImGui::VkMat vkmat;
            vkmat.type = IM_DT_INT8;
            m_vulkanBlender.blend(overlayImage, baseImage, vkmat, m_ovlyX, m_ovlyY);
            if (!vkmat.empty())
            {
                res = vkmat;
                res.time_stamp = baseImage.time_stamp;
                res.duration = baseImage.time_stamp;
                res.color_space = baseImage.color_space;
                res.color_range = baseImage.color_range;
            }
            else
            {
                res = baseImage;
            }
#endif
        }
        else
        {
            res = m_ffBlender.Blend(baseImage, overlayImage);
        }
        return res;
    }

    bool EnableUseVulkan(bool enable) override
    {
        if (m_useVulkan == enable)
            return true;
#if !IMGUI_VULKAN_SHADER
        if (enable)
        {
            m_errMsg = "Vulkan code is DISABLED!";
            return false;
        }
#endif
        m_useVulkan = enable;
        return true;
    }

    std::string GetError() const override
    {
        return m_errMsg;
    }

private:
    bool m_useVulkan;
    int32_t m_ovlyX{0}, m_ovlyY{0};
#if IMGUI_VULKAN_SHADER
    ImGui::AlphaBlending_vulkan m_vulkanBlender;
#endif
    FFOverlayBlender m_ffBlender;
    string m_errMsg;
};

VideoBlender::Holder VideoBlender::CreateInstance()
{
    return VideoBlender::Holder(new VideoBlender_Impl(), [] (VideoBlender* p) {
        VideoBlender_Impl* ptr = dynamic_cast<VideoBlender_Impl*>(p);
        delete ptr;
    });
}
}