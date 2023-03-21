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

#include <list>
#include <fontconfig/fontconfig.h>
#include "FontManager.h"

using namespace std;
using namespace FM;

int convertWeight(FontWeight weight)
{
    switch (weight)
    {
    case FontWeightThin:
        return FC_WEIGHT_THIN;
    case FontWeightUltraLight:
        return FC_WEIGHT_ULTRALIGHT;
    case FontWeightLight:
        return FC_WEIGHT_LIGHT;
    case FontWeightNormal:
        return FC_WEIGHT_REGULAR;
    case FontWeightMedium:
        return FC_WEIGHT_MEDIUM;
    case FontWeightSemiBold:
        return FC_WEIGHT_SEMIBOLD;
    case FontWeightBold:
        return FC_WEIGHT_BOLD;
    case FontWeightUltraBold:
        return FC_WEIGHT_EXTRABOLD;
    case FontWeightHeavy:
        return FC_WEIGHT_ULTRABLACK;
    default:
        return FC_WEIGHT_REGULAR;
    }
}

FontWeight convertWeight(int weight)
{
    switch (weight)
    {
    case FC_WEIGHT_THIN:
        return FontWeightThin;
    case FC_WEIGHT_ULTRALIGHT:
        return FontWeightUltraLight;
    case FC_WEIGHT_LIGHT:
        return FontWeightLight;
    case FC_WEIGHT_REGULAR:
        return FontWeightNormal;
    case FC_WEIGHT_MEDIUM:
        return FontWeightMedium;
    case FC_WEIGHT_SEMIBOLD:
        return FontWeightSemiBold;
    case FC_WEIGHT_BOLD:
        return FontWeightBold;
    case FC_WEIGHT_EXTRABOLD:
        return FontWeightUltraBold;
    case FC_WEIGHT_ULTRABLACK:
        return FontWeightHeavy;
    default:
        return FontWeightNormal;
    }
}

int convertWidth(FontWidth width)
{
    switch (width)
    {
    case FontWidthUltraCondensed:
        return FC_WIDTH_ULTRACONDENSED;
    case FontWidthExtraCondensed:
        return FC_WIDTH_EXTRACONDENSED;
    case FontWidthCondensed:
        return FC_WIDTH_CONDENSED;
    case FontWidthSemiCondensed:
        return FC_WIDTH_SEMICONDENSED;
    case FontWidthNormal:
        return FC_WIDTH_NORMAL;
    case FontWidthSemiExpanded:
        return FC_WIDTH_SEMIEXPANDED;
    case FontWidthExpanded:
        return FC_WIDTH_EXPANDED;
    case FontWidthExtraExpanded:
        return FC_WIDTH_EXTRAEXPANDED;
    case FontWidthUltraExpanded:
        return FC_WIDTH_ULTRAEXPANDED;
    default:
        return FC_WIDTH_NORMAL;
    }
}

FontWidth convertWidth(int width)
{
    switch (width)
    {
    case FC_WIDTH_ULTRACONDENSED:
        return FontWidthUltraCondensed;
    case FC_WIDTH_EXTRACONDENSED:
        return FontWidthExtraCondensed;
    case FC_WIDTH_CONDENSED:
        return FontWidthCondensed;
    case FC_WIDTH_SEMICONDENSED:
        return FontWidthSemiCondensed;
    case FC_WIDTH_NORMAL:
        return FontWidthNormal;
    case FC_WIDTH_SEMIEXPANDED:
        return FontWidthSemiExpanded;
    case FC_WIDTH_EXPANDED:
        return FontWidthExpanded;
    case FC_WIDTH_EXTRAEXPANDED:
        return FontWidthExtraExpanded;
    case FC_WIDTH_ULTRAEXPANDED:
        return FontWidthUltraExpanded;
    default:
        return FontWidthNormal;
    }
}

static FontDescriptorHolder CreateFontDescriptor(FcPattern *pattern)
{
    FcChar8 *path, *psName, *family, *style;
    int weight, width, slant, spacing;

    FcPatternGetString(pattern, FC_FILE, 0, &path);
    FcPatternGetString(pattern, FC_POSTSCRIPT_NAME, 0, &psName);
    FcPatternGetString(pattern, FC_FAMILY, 0, &family);
    FcPatternGetString(pattern, FC_STYLE, 0, &style);

    FcPatternGetInteger(pattern, FC_WEIGHT, 0, &weight);
    FcPatternGetInteger(pattern, FC_WIDTH, 0, &width);
    FcPatternGetInteger(pattern, FC_SLANT, 0, &slant);
    FcPatternGetInteger(pattern, FC_SPACING, 0, &spacing);

    return NewFontDescriptor(
        (char *)path,
        (char *)psName,
        (char *)family,
        (char *)style,
        convertWeight(weight),
        convertWidth(width),
        slant == FC_SLANT_ITALIC,
        spacing == FC_MONO);
}

vector<FontDescriptorHolder> FM::GetAvailableFonts()
{
    FcInit();

    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_POSTSCRIPT_NAME, FC_FAMILY, FC_STYLE, FC_WEIGHT, FC_WIDTH, FC_SLANT, FC_SPACING, nullptr);
    FcFontSet *fs = FcFontList(NULL, pattern, os);

    vector<FontDescriptorHolder> result;
    if (fs)
    {
        result.reserve(fs->nfont);
        for (int i = 0; i < fs->nfont; i++)
        {
            FontDescriptorHolder fc = CreateFontDescriptor(fs->fonts[i]);
            // skip all the fonts those start with '.'
            if (fc->Family().c_str()[0] == '.')
                continue;
            result.push_back(fc);
        }
    }

    return std::move(result);

    FcPatternDestroy(pattern);
    FcObjectSetDestroy(os);
    FcFontSetDestroy(fs);

    return std::move(result);
}

unordered_map<string, vector<FontDescriptorHolder>> FM::GroupFontsByFamily(const vector<FontDescriptorHolder>& fonts)
{
    unordered_map<string, vector<FontDescriptorHolder>> result;
    for (auto hFD : fonts)
    {
        auto iter = result.find(hFD->Family());
        if (iter == result.end())
        {
            vector<FontDescriptorHolder> group;
            group.push_back(hFD);
            result[hFD->Family()] = std::move(group);
        }
        else
        {
            iter->second.push_back(hFD);
        }
    }
    return result;
}
