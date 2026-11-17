// ps5-homebrew-ui - Baked font loading and layout tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/save_file.hpp"
#include "gfx/font.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace
{

using hui::gfx::Align;
using hui::gfx::Font;
using hui::gfx::GlyphQuad;

const Font &inter()
{
    // Built in place: a Font owns its glyph caches and cannot be copied.
    static Font font;
    static const bool loaded = []
    {
        std::string data;
        EXPECT_TRUE(
            hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/inter-semibold.huifont", &data));
        EXPECT_TRUE(font.load(data)) << font.error();
        return true;
    }();
    (void)loaded;
    return font;
}

TEST(Font, LoadsAsciiAndSymbols)
{
    const Font &font = inter();
    for (char c = ' '; c < 127; ++c)
        EXPECT_TRUE(font.has_glyph(static_cast<std::uint32_t>(c))) << c;
    EXPECT_TRUE(font.has_glyph(0x2022));
    EXPECT_GT(font.ascent(40), 30.0f);
    EXPECT_GT(font.descent(40), 5.0f);
    EXPECT_GT(font.line_height(40), font.ascent(40));
}

TEST(Font, RejectsCorruptData)
{
    Font font;
    EXPECT_FALSE(font.load("short"));
    std::string data;
    ASSERT_TRUE(hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/inter-regular.huifont", &data));
    EXPECT_FALSE(font.load(data.substr(0, data.size() - 1)));
    data[0] = 'X';
    EXPECT_FALSE(font.load(data));
}

TEST(Font, MeasureScalesLinearlyAndKerns)
{
    const Font &font = inter();
    const float at20 = font.measure("Light Up", 20);
    const float at40 = font.measure("Light Up", 40);
    EXPECT_GT(at20, 0.0f);
    EXPECT_NEAR(at40, at20 * 2.0f, 0.01f);
    // stb_truetype only reads legacy/simple kerning; pairs never widen text.
    EXPECT_LE(font.measure("AV", 40), font.measure("A", 40) + font.measure("V", 40));
    EXPECT_FLOAT_EQ(font.measure("", 40), 0.0f);
}

TEST(Font, LayoutAlignsAndSkipsSpaces)
{
    const Font &font = inter();
    std::vector<GlyphQuad> quads;
    const float width = font.layout("A B", 100.0f, 50.0f, 32.0f, Align::left, quads);
    ASSERT_EQ(quads.size(), 2u);
    // Quads include the distance-field margin around each glyph.
    EXPECT_GE(quads[0].x0, 100.0f - font.sdf_range(32.0f) - 1.0f);
    EXPECT_LT(quads[0].x0, 100.0f);
    EXPECT_LT(quads[0].y0, 50.0f); // glyphs rise above the baseline
    EXPECT_GT(quads[1].x0, quads[0].x1 - 20.0f);
    for (const GlyphQuad &q : quads)
    {
        EXPECT_GE(q.u0, 0.0f);
        EXPECT_LE(q.u1, 1.0f);
        EXPECT_LT(q.u0, q.u1);
        EXPECT_LT(q.v0, q.v1);
    }
    std::vector<GlyphQuad> centred;
    font.layout("A B", 100.0f, 50.0f, 32.0f, Align::center, centred);
    EXPECT_NEAR(centred[0].x0, quads[0].x0 - width * 0.5f, 0.01f);
    std::vector<GlyphQuad> right;
    font.layout("A B", 100.0f, 50.0f, 32.0f, Align::right, right);
    EXPECT_NEAR(right[0].x0, quads[0].x0 - width, 0.01f);
}

TEST(Font, UnknownCharactersFallBackToQuestionMark)
{
    const Font &font = inter();
    EXPECT_FLOAT_EQ(font.measure("\xE2\x98\x83", 30), font.measure("?", 30)); // U+2603 snowman
    EXPECT_FLOAT_EQ(font.measure("\xFF", 30), font.measure("?", 30));         // invalid byte
}

// What the baked face lacks comes from the console's fonts; on a PC, Noto CJK
// stands in for them (docs/PRD.md 7.5).
TEST(Font, DrawsOtherScriptsFromSystemFonts)
{
    const std::string cjk = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
    std::string probe;
    if (!hui::save::read_file(cjk, &probe, 64u << 20))
        GTEST_SKIP() << "no Noto CJK on this machine";
    Font font;
    std::string data;
    ASSERT_TRUE(hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/inter-semibold.huifont", &data));
    ASSERT_TRUE(font.load(data)) << font.error();
    const int baked_rows = font.atlas_height();

    const std::string kanji = "\xE6\x97\xA5\xE6\x9C\xAC"; // two CJK ideographs
    EXPECT_FALSE(font.can_draw(kanji));
    EXPECT_FLOAT_EQ(font.measure(kanji, 30), font.measure("??", 30));

    font.use_system_fonts({cjk}, "ja-JP");
    EXPECT_TRUE(font.can_draw(kanji));
    EXPECT_GT(font.atlas_height(), baked_rows); // room for the new glyphs
    std::vector<GlyphQuad> quads;
    font.layout(kanji, 0.0f, 0.0f, 32.0f, Align::left, quads);
    ASSERT_EQ(quads.size(), 2u);
    EXPECT_FLOAT_EQ(quads[0].range, font.sdf_range(32.0f));
    EXPECT_GT(quads[0].x1 - quads[0].x0, 10.0f);

    // The glyphs were drawn below the baked ones, once.
    int first = 0;
    int last = 0;
    ASSERT_TRUE(font.take_changed_rows(&first, &last));
    EXPECT_GE(first, baked_rows);
    EXPECT_GT(last, first);
    EXPECT_FALSE(font.take_changed_rows(&first, &last));

    // Latin text measures as it did; what no font has still shows as '?'.
    EXPECT_FLOAT_EQ(font.measure("Light Up", 40), inter().measure("Light Up", 40));
}

TEST(Font, DecodesUtf8)
{
    std::size_t index = 0;
    const std::string text = "a\xC3\x97\xE2\x80\xA2";
    EXPECT_EQ(hui::gfx::next_codepoint(text, &index), 'a');
    EXPECT_EQ(hui::gfx::next_codepoint(text, &index), 0xD7u);
    EXPECT_EQ(hui::gfx::next_codepoint(text, &index), 0x2022u);
    EXPECT_EQ(index, text.size());
}

TEST(Font, WrapsAtWordsAndNewlines)
{
    const Font &font = inter();
    const auto lines =
        font.wrap("Connect all the islands with a network of bridges.\nNext", 30, 300);
    ASSERT_GE(lines.size(), 3u);
    for (const std::string &line : lines)
        EXPECT_LE(font.measure(line, 30), 300.0f + 1e-3f) << line;
    EXPECT_EQ(lines.back(), "Next");
}

TEST(Font, BreaksAWordWiderThanTheLine)
{
    const Font &font = inter();
    const std::string address = "https://example.org/a/very/long/address/without/any/space";
    const auto lines = font.wrap("See " + address + " now", 30, 300);
    ASSERT_GE(lines.size(), 3u);
    EXPECT_EQ(lines.front(), "See");
    std::string joined;
    for (const std::string &line : lines)
    {
        EXPECT_LE(font.measure(line, 30), 300.0f + 1e-3f) << line;
        for (const char c : line)
        {
            if (c != ' ')
                joined += c;
        }
    }
    // Nothing is lost or repeated where the word was cut.
    EXPECT_EQ(joined, "See" + address + "now");
    // A code point wider than the line still gets a line: the text is never dropped.
    const auto narrow = font.wrap("Wide", 30, 1.0f);
    ASSERT_EQ(narrow.size(), 4u);
    EXPECT_EQ(narrow.front(), "W");
    EXPECT_EQ(narrow.back(), "e");
}

} // namespace
