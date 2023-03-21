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
#include <immat.h>

namespace MediaCore
{
    struct VideoBlender
    {
        virtual bool Init() = 0;
        virtual ImGui::ImMat Blend(ImGui::ImMat& baseImage, ImGui::ImMat& overlayImage, int32_t x, int32_t y) = 0;
        virtual bool Init(const std::string& inputFormat, uint32_t w1, uint32_t h1, uint32_t w2, uint32_t h2, int32_t x, int32_t y) = 0;
        virtual ImGui::ImMat Blend(ImGui::ImMat& baseImage, ImGui::ImMat& overlayImage) = 0;

        virtual bool EnableUseVulkan(bool enable) = 0;
        virtual std::string GetError() const = 0;
    };

    using VideoBlenderHolder = std::shared_ptr<VideoBlender>;

    VideoBlenderHolder CreateVideoBlender();
}