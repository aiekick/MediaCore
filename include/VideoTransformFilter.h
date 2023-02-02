#pragma once
#include <string>
#include <memory>
#include "imgui_extra_widget.h"
#include "imgui_curve.h"

namespace MediaCore
{
    enum ScaleType
    {
        SCALE_TYPE__FIT = 0,
        SCALE_TYPE__CROP,
        SCALE_TYPE__FILL,
        SCALE_TYPE__STRETCH,
    };

    struct VideoTransformFilter;
    using VideoTransformFilterHolder = std::shared_ptr<VideoTransformFilter>;

    struct VideoTransformFilter
    {
        virtual bool Initialize(uint32_t outWidth, uint32_t outHeight) = 0;
        virtual VideoTransformFilterHolder Clone(uint32_t outWidth, uint32_t outHeight) = 0;
        virtual bool SetOutputFormat(const std::string& outputFormat) = 0;
        virtual bool SetScaleType(ScaleType type) = 0;
        virtual bool SetPositionOffset(int32_t offsetH, int32_t offsetV) = 0;
        virtual bool SetPositionOffsetH(int32_t value) = 0;
        virtual bool SetPositionOffsetV(int32_t value) = 0;
        virtual bool SetCropMargin(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) = 0;
        virtual bool SetCropMarginL(uint32_t value) = 0;
        virtual bool SetCropMarginT(uint32_t value) = 0;
        virtual bool SetCropMarginR(uint32_t value) = 0;
        virtual bool SetCropMarginB(uint32_t value) = 0;
        virtual bool SetRotationAngle(double angle) = 0;
        virtual bool SetScaleH(double scale) = 0;
        virtual bool SetScaleV(double scale) = 0;
        virtual bool SetKeyPoint(ImGui::KeyPointEditor &keypoint) = 0;
        virtual ImGui::ImMat FilterImage(const ImGui::ImMat& vmat, int64_t pos) = 0;

        virtual const std::string GetFilterName() const = 0;
        virtual std::string GetOutputFormat() const = 0;
        virtual uint32_t GetInWidth() const = 0;
        virtual uint32_t GetInHeight() const = 0;
        virtual uint32_t GetOutWidth() const = 0;
        virtual uint32_t GetOutHeight() const = 0;
        virtual ScaleType GetScaleType() const = 0;
        virtual int32_t GetPositionOffsetH() const = 0;
        virtual int32_t GetPositionOffsetV() const = 0;
        virtual uint32_t GetCropMarginL() const = 0;
        virtual uint32_t GetCropMarginT() const = 0;
        virtual uint32_t GetCropMarginR() const = 0;
        virtual uint32_t GetCropMarginB() const = 0;
        virtual double GetRotationAngle() const = 0;
        virtual double GetScaleH() const = 0;
        virtual double GetScaleV() const = 0;
        virtual ImGui::KeyPointEditor* GetKeyPoint() = 0;

        virtual std::string GetError() const = 0;
    };

    VideoTransformFilterHolder CreateVideoTransformFilter();
}