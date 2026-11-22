// SPDX-License-Identifier: AGPL-3.0-or-later
//
// WTS-03 (docs/PLAN.md; ADR-0232 d3): goldens that check text by its place, not its pixels.
//  - GlyphBoxRecorder renders as JUCE's software renderer does and keeps the box of every run of glyphs
//    it draws, in the image's pixels;
//  - compareMasked compares two renders byte for byte outside the union of both renders' text boxes
//    (EXACT, as memcmp), so a word that moves by a pixel passes while a divider that moves fails;
//  - diffImage draws what differs: red outside the mask, amber inside it, the golden greyed;
//  - the boxes go beside a golden as text, one "x y w h" a line.
// Replacing the shell's hand-placed masks with these, and recording the goldens again with their text,
// follows the font experiment (one pinned face, each scene recorded per OS), which needs CI's legs.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <string>
#include <vector>
namespace adi::test {
class GlyphBoxRecorder final : public juce::LowLevelGraphicsSoftwareRenderer {
  public:
    explicit GlyphBoxRecorder(const juce::Image &image) : LowLevelGraphicsSoftwareRenderer(image), bounds_(image.getBounds()) {
        transforms_.push_back({});
    }
    void setOrigin(juce::Point<int> o) override {
        LowLevelGraphicsSoftwareRenderer::setOrigin(o);
        transforms_.back() = juce::AffineTransform::translation(o.toFloat()).followedBy(transforms_.back());
    }
    void addTransform(const juce::AffineTransform &t) override {
        LowLevelGraphicsSoftwareRenderer::addTransform(t);
        transforms_.back() = t.followedBy(transforms_.back());
    }
    void saveState() override {
        LowLevelGraphicsSoftwareRenderer::saveState();
        transforms_.push_back(transforms_.back());
    }
    void restoreState() override {
        LowLevelGraphicsSoftwareRenderer::restoreState();
        if (transforms_.size() > 1)
            transforms_.pop_back();
    }
    void beginTransparencyLayer(float opacity) override {
        LowLevelGraphicsSoftwareRenderer::beginTransparencyLayer(opacity);
        transforms_.push_back(transforms_.back());
    }
    void endTransparencyLayer() override {
        LowLevelGraphicsSoftwareRenderer::endTransparencyLayer();
        if (transforms_.size() > 1)
            transforms_.pop_back();
    }
    void drawGlyphs(juce::Span<const uint16_t> glyphs, juce::Span<const juce::Point<float>> positions,
                    const juce::AffineTransform &t) override {
        const auto &font = getFont();
        const auto typeface = font.getTypefacePtr();
        const float height = font.getHeightInPoints();
        const auto scale = juce::AffineTransform::scale(height * font.getHorizontalScale(), height);
        juce::Rectangle<float> run;
        for (std::size_t i = 0; i < glyphs.size() && i < positions.size(); ++i) {
            if (typeface == nullptr)
                break;
            const auto glyph = typeface->getGlyphBounds(glyphs[i]);
            if (glyph.isEmpty())
                continue;
            const auto box = glyph.transformedBy(scale.followedBy(juce::AffineTransform::translation(positions[i]))
                                                     .followedBy(t)
                                                     .followedBy(transforms_.back()));
            run = run.isEmpty() ? box : run.getUnion(box);
        }
        if (!run.isEmpty())
            if (const auto pixels = run.getSmallestIntegerContainer().getIntersection(bounds_); !pixels.isEmpty())
                boxes.push_back(pixels);
        LowLevelGraphicsSoftwareRenderer::drawGlyphs(glyphs, positions, t);
    }
    // Every run of glyphs drawn, in the image's pixels.
    std::vector<juce::Rectangle<int>> boxes;

  private:
    juce::Rectangle<int> bounds_;
    std::vector<juce::AffineTransform> transforms_;
};
// A component painted through the recorder: the image, and its text boxes.
struct TextRender {
    juce::Image image;
    std::vector<juce::Rectangle<int>> boxes;
};
inline TextRender renderWithText(juce::Component &c) {
    TextRender out{juce::Image(juce::Image::ARGB, std::max(1, c.getWidth()), std::max(1, c.getHeight()), true,
                               juce::SoftwareImageType{}),
                   {}};
    GlyphBoxRecorder recorder(out.image);
    {
        juce::Graphics g(recorder);
        c.paintEntireComponent(g, true);
    }
    out.boxes = recorder.boxes;
    return out;
}
struct Comparison {
    bool sameSize = false;
    std::size_t differing = 0; // pixels that differ outside the mask
    std::size_t masked = 0;    // pixels that differ inside it
    bool exact() const noexcept { return sameSize && differing == 0; }
};
// Pixel by pixel, outside the union of both renders' text boxes grown by `pad` pixels.
inline Comparison compareMasked(const juce::Image &golden, const std::vector<juce::Rectangle<int>> &goldenText,
                                const juce::Image &render, const std::vector<juce::Rectangle<int>> &renderText, int pad = 1) {
    Comparison out;
    if (golden.getBounds() != render.getBounds())
        return out;
    out.sameSize = true;
    juce::RectangleList<int> mask;
    for (const auto *list : {&goldenText, &renderText})
        for (const auto &b : *list)
            mask.addWithoutMerging(b.expanded(pad));
    const juce::Image::BitmapData a(golden, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData b(render, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < golden.getHeight(); ++y)
        for (int x = 0; x < golden.getWidth(); ++x)
            if (a.getPixelColour(x, y) != b.getPixelColour(x, y))
                ++(mask.containsPoint({x, y}) ? out.masked : out.differing);
    return out;
}
// What differs, for a person: red outside the mask, amber inside it, over the golden greyed.
inline juce::Image diffImage(const juce::Image &golden, const std::vector<juce::Rectangle<int>> &goldenText,
                             const juce::Image &render, const std::vector<juce::Rectangle<int>> &renderText, int pad = 1) {
    juce::Image out(juce::Image::ARGB, golden.getWidth(), golden.getHeight(), true, juce::SoftwareImageType{});
    if (golden.getBounds() != render.getBounds())
        return out;
    juce::RectangleList<int> mask;
    for (const auto *list : {&goldenText, &renderText})
        for (const auto &b : *list)
            mask.addWithoutMerging(b.expanded(pad));
    for (int y = 0; y < golden.getHeight(); ++y)
        for (int x = 0; x < golden.getWidth(); ++x) {
            const auto g = golden.getPixelAt(x, y), r = render.getPixelAt(x, y);
            if (g != r)
                out.setPixelAt(x, y, mask.containsPoint({x, y}) ? juce::Colour(0xffffb000) : juce::Colour(0xffff2020));
            else
                out.setPixelAt(x, y, juce::Colour::greyLevel(g.getPerceivedBrightness() * 0.5f));
        }
    return out;
}
// The boxes beside a golden: one "x y w h" a line.
inline juce::String boxesText(const std::vector<juce::Rectangle<int>> &boxes) {
    juce::String out;
    for (const auto &b : boxes)
        out << b.getX() << ' ' << b.getY() << ' ' << b.getWidth() << ' ' << b.getHeight() << '\n';
    return out;
}
inline std::vector<juce::Rectangle<int>> boxesFrom(const juce::String &text) {
    std::vector<juce::Rectangle<int>> out;
    for (const auto &line : juce::StringArray::fromLines(text)) {
        juce::StringArray t;
        t.addTokens(line, " ", "");
        t.removeEmptyStrings();
        if (t.size() == 4)
            out.emplace_back(t[0].getIntValue(), t[1].getIntValue(), t[2].getIntValue(), t[3].getIntValue());
    }
    return out;
}
} // namespace adi::test
