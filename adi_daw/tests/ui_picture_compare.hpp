// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_graphics/juce_graphics.h>
#include <cstdint>

namespace adi::uitest {

struct ComparisonPicture {
    juce::Image raster;
    juce::Image withoutWords;
};

// Use the same software renderer and layout in both passes. Only glyph ink
// disappears, so the mask follows transformed and clipped text as drawn.
class WordlessRenderer final : public juce::LowLevelGraphicsSoftwareRenderer {
  public:
    using juce::LowLevelGraphicsSoftwareRenderer::LowLevelGraphicsSoftwareRenderer;

    void drawGlyphs(juce::Span<const std::uint16_t>, juce::Span<const juce::Point<float>>,
                    const juce::AffineTransform &) override {}
};

template <typename Paint>
ComparisonPicture pictureForComparison(juce::Image::PixelFormat format, int width, int height, Paint paint) {
    ComparisonPicture picture{juce::Image(format, width, height, true, juce::SoftwareImageType{}),
                              juce::Image(format, width, height, true, juce::SoftwareImageType{})};
    {
        juce::Graphics graphics(picture.raster);
        paint(graphics);
    }
    {
        WordlessRenderer renderer(picture.withoutWords);
        juce::Graphics graphics(renderer);
        paint(graphics);
    }
    return picture;
}

inline bool samePicturePixels(const ComparisonPicture &a, const ComparisonPicture &b) {
    if (a.raster.getBounds() != b.raster.getBounds())
        return false;
    using Pixels = juce::Image::BitmapData;
    const Pixels left(a.raster, Pixels::readOnly), right(b.raster, Pixels::readOnly);
    const Pixels leftWordless(a.withoutWords, Pixels::readOnly), rightWordless(b.withoutWords, Pixels::readOnly);
    // Black out the union of both text masks on both pictures. A shorter
    // label must not expose its counterpart's glyph pixels as differences.
    for (int y = 0; y < left.height; ++y)
        for (int x = 0; x < left.width; ++x) {
            const auto l = left.getPixelColour(x, y), r = right.getPixelColour(x, y);
            if (l == r)
                continue;
            const bool masked = l != leftWordless.getPixelColour(x, y) || r != rightWordless.getPixelColour(x, y);
            if ((masked ? juce::Colours::black : l) != (masked ? juce::Colours::black : r))
                return false;
        }
    return true;
}

} // namespace adi::uitest
