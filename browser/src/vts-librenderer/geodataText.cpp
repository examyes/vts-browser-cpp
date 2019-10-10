/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <list>
#include <map>

#include "geodata.hpp"
#include "font.hpp"

extern "C"
{
#include <SheenBidi.h>
}

namespace vts { namespace renderer
{

namespace
{

struct TmpGlyph
{
    std::shared_ptr<Font> font;
    vec2f position; // screen units
    vec2f size; // screen units
    vec2f offset; // font units
    float advance; // font units
    uint16 glyphIndex;

    TmpGlyph() : position(0, 0), offset(0, 0), advance(0), glyphIndex(0)
    {}
};

struct TmpLine
{
    std::vector<TmpGlyph> glyphs;
    float width; // screen units

    TmpLine() : width(0)
    {}
};

struct AlgorithmHolder
{
    SBAlgorithmRef algorithm;

    AlgorithmHolder(SBCodepointSequence *codepoints) : algorithm(nullptr)
    {
        algorithm = SBAlgorithmCreate(codepoints);
    }

    ~AlgorithmHolder()
    {
        SBAlgorithmRelease(algorithm);
    }

    SBAlgorithmRef operator () () { return algorithm; };
};

struct ParagraphHolder
{
    SBParagraphRef paragraph;

    ParagraphHolder(SBAlgorithmRef bidiAlgorithm, SBUInteger paragraphStart)
        : paragraph(nullptr)
    {
        paragraph = SBAlgorithmCreateParagraph(bidiAlgorithm,
                paragraphStart, INT32_MAX, SBLevelDefaultLTR);
    }

    ~ParagraphHolder()
    {
        SBParagraphRelease(paragraph);
    }

    SBParagraphRef operator () () { return paragraph; };
};

struct LineHolder
{
    SBLineRef paragraphLine;

    LineHolder(SBParagraphRef paragraph, SBUInteger lineStart,
        SBUInteger paragraphLength) : paragraphLine(nullptr)
    {
        paragraphLine = SBParagraphCreateLine(paragraph,
            lineStart, paragraphLength);
    }

    ~LineHolder()
    {
        SBLineRelease(paragraphLine);
    }

    SBLineRef operator () () { return paragraphLine; };
};

struct BufferHolder
{
    hb_buffer_t *buffer;

    BufferHolder() : buffer(nullptr)
    {
        buffer = hb_buffer_create();
    }

    ~BufferHolder()
    {
        hb_buffer_destroy(buffer);
    }

    hb_buffer_t *operator () () { return buffer; };
};

std::vector<TmpLine> textToGlyphs(
    const std::string &s,
    const std::vector<std::shared_ptr<Font>> &fontCascade)
{
    std::vector<TmpLine> lines;

    SBCodepointSequence codepoints
        = { SBStringEncodingUTF8, (void*)s.data(), s.length() };
    AlgorithmHolder bidiAlgorithm(&codepoints);
    SBUInteger paragraphStart = 0;
    while (true)
    {
        ParagraphHolder paragraph(bidiAlgorithm(), paragraphStart);
        if (!paragraph())
            break;
        SBUInteger paragraphLength = SBParagraphGetLength(paragraph());
        SBUInteger lineStart = paragraphStart;
        paragraphStart += paragraphLength;

        while (true)
        {
            LineHolder paragraphLine(paragraph(), lineStart, paragraphLength);
            if (!paragraphLine())
                break;
            SBUInteger lineLength = SBLineGetLength(paragraphLine());
            lineStart += lineLength;
            paragraphLength -= lineLength;

            SBUInteger runCount = SBLineGetRunCount(paragraphLine());
            const SBRun *runArray = SBLineGetRunsPtr(paragraphLine());

            TmpLine line;
            line.glyphs.reserve(lineLength);

            for (uint32 runIndex = 0; runIndex < runCount; runIndex++)
            {
                const SBRun *run = runArray + runIndex;

                // bidi is running, harf is buzzing

                struct Buzz
                {
                    SBUInteger offset;
                    SBUInteger length;
                    uint32 font;
                    Buzz(const SBRun *run, uint32 o, uint32 l, uint32 f)
                        : offset(o), length(l), font(f)
                    {
                        (void)run;
                        assert(offset >= run->offset);
                        assert(offset + length <= run->offset + run->length);
                    }
                };

                // the text needs to be split into parts with different font

                std::list<Buzz> runs;

                // harfbuzz cannot handle new lines
                runs.emplace_back(run, run->offset,
                    s[run->offset + run->length - 1] == '\n'
                    ? run->length - 1 : run->length, 0);

                while (!runs.empty())
                {
                    Buzz r = runs.front();
                    runs.pop_front();
                    if (r.length == 0)
                        continue;

                    bool terminal = r.font >= fontCascade.size();
                    std::shared_ptr<Font> fnt = fontCascade
                                    [terminal ? 0 : r.font];

                    BufferHolder buffer;
                    hb_buffer_add_utf8(buffer(),
                        (char*)codepoints.stringBuffer,
                        codepoints.stringLength,
                        r.offset, r.length);
                    hb_buffer_set_direction(buffer(), (run->level % 2) == 0
                        ? HB_DIRECTION_LTR : HB_DIRECTION_RTL);
                    hb_buffer_guess_segment_properties(buffer());
                    hb_shape(fnt->font, buffer(), nullptr, 0);

                    uint32 len = hb_buffer_get_length(buffer());
                    hb_glyph_info_t *info
                        = hb_buffer_get_glyph_infos(buffer(), nullptr);
                    hb_glyph_position_t *pos
                        = hb_buffer_get_glyph_positions(buffer(), nullptr);

                    if (!terminal)
                    {
                        // we have another font available to use
                        // so we search for tofus
                        bool hasTofu = false;
                        for (uint32 i = 0; i < len; i++)
                        {
                            if (info[i].codepoint == 0)
                            {
                                hasTofu = true;
                                break;
                            }
                        }

                        // if any tofu was found, split the text into parts
                        // each part is either all tofu or no tofu
                        if (hasTofu)
                        {
                            // RTL text has clusters in reverse order
                            //   reverse it back to simplify the algorithm
                            if ((run->level % 2) == 1)
                                std::reverse(info, info + len);

                            runs.reverse();
                            bool lastTofu = info[0].codepoint == 0;
                            bool hadTofu = lastTofu;
                            uint32 lastCluster = info[0].cluster;
                            for (uint32 i = 1; i < len; i++)
                            {
                                // there are rare situations where harfbuzz
                                //   puts some tofus and non-tofus
                                //   into same cluster,
                                //   which prevents me to split them
                                // for a lack of better ideas
                                //   we send these sequences to next font
                                //   without proper splitting
                                bool currentTofu = info[i].codepoint == 0;
                                if (currentTofu != lastTofu)
                                {
                                    uint32 currentCluster
                                        = info[i].cluster;
                                    if (currentCluster != lastCluster)
                                    {
                                        runs.emplace_back(run, lastCluster,
                                            currentCluster - lastCluster,
                                            r.font + hadTofu);
                                        lastCluster = currentCluster;
                                        hadTofu = currentTofu;
                                    }
                                    lastTofu = currentTofu;
                                    hadTofu = hadTofu || currentTofu;
                                }
                            }
                            {
                                uint32 currentCluster
                                    = r.offset + r.length;
                                runs.emplace_back(run, lastCluster,
                                    currentCluster - lastCluster,
                                    r.font + hadTofu);
                            }
                            runs.reverse();
                            continue;
                        }
                    }

                    // add glyphs information to output
                    for (uint32 i = 0; i < len; i++)
                    {
                        assert(pos[i].y_advance == 0);
                        uint16 gi = info[i].codepoint;
                        assert(gi < fnt->glyphs.size());
                        TmpGlyph g;
                        g.font = fnt;
                        g.glyphIndex = gi;
                        g.advance = pos[i].x_advance / 64.f;
                        g.offset = vec2f(pos[i].x_offset,
                                pos[i].y_offset) / 64;
                        line.glyphs.push_back(g);
                    }
                }
            }

            lines.push_back(std::move(line));
        }
    }

    // erase empty lines
    lines.erase(std::remove_if(lines.begin(), lines.end(),
        [](const TmpLine &l) { return l.glyphs.empty(); }), lines.end());

    return lines;
}

vec2f textLayout(float size, float align,
    std::vector<TmpLine> &lines)
{
    // find glyph positions
    vec2f res = vec2f(0, 0);
    {
        vec2f o = vec2f(0, 0);
        for (TmpLine &line : lines)
        {
            for (TmpGlyph &g : line.glyphs)
            {
                const auto &f = g.font->glyphs[g.glyphIndex];
                float s = size / g.font->size;
                g.position = o + (g.offset + f.offset) * s;
                g.size = f.size * s;
                o[0] += g.advance * s;
            }
            line.width = o[0];
            res[0] = std::max(res[0], o[0]);
            o[0] = 0;
            o[1] += size * -1.5; // 1.5 line spacing
        }
        res[1] = -o[1];
    }

    // center the entire text
    vec2f off = res.cwiseProduct(vec2f(-0.5, 0.5)) + vec2f(0, -size);
    for (TmpLine &line : lines)
        for (TmpGlyph &g : line.glyphs)
            g.position += off;

    // align (move each line independently)
    for (TmpLine &line : lines)
    {
        float dx = (res[0] - line.width) * align;
        for (TmpGlyph &g : line.glyphs)
            g.position[0] += dx;
    }

    return res;
}

Rect textCollision(const std::vector<TmpLine> &lines)
{
    Rect collision;
    for (const TmpLine &line : lines)
    {
        for (const TmpGlyph &g : line.glyphs)
            collision = Rect::merge(collision,
                Rect(g.position, g.position + g.size));
    }
    return collision;
}

Text generateTexts(std::vector<TmpLine> &lines)
{
    // sort into groups
    struct WordCompare
    {
        bool operator () (const Subtext &a, const Subtext &b) const
        {
            if (a.font == b.font)
                return a.fileIndex < b.fileIndex;
            return a.font < b.font;
        }
    };
    std::map<Subtext, std::vector<TmpGlyph>, WordCompare> groups;
    for (TmpLine &line : lines)
    {
        for (const TmpGlyph &g : line.glyphs)
        {
            Subtext w;
            w.font = g.font;
            w.fileIndex = g.font->glyphs[g.glyphIndex].fileIndex;
            groups[w].push_back(g);
        }
    }

    // generate subtexts
    Text text;
    text.coordinates.reserve(1020);
    uint32 vertexIndex = 0;
    for (auto &grp : groups)
    {
        Subtext w(grp.first);
        w.indicesStart = vertexIndex;
        for (const TmpGlyph &tg : grp.second)
        {
            const Glyph &g = tg.font->glyphs[tg.glyphIndex];
            // 2--3
            // |  |
            // 0--1
            for (int i = 0; i < 4; i++)
            {
                vec4f c;
                c[0] = tg.position[0] + ((i % 2) == 1 ? tg.size[0] : 0);
                c[1] = tg.position[1] + ((i / 2) == 1 ? tg.size[1] : 0);
                c[2] = g.uvs[0 + 2 * (i % 2)];
                c[3] = g.uvs[1 + 2 * (i / 2)];
                c[2] += g.plane * 2;
                text.coordinates.push_back(c);
            }
            w.indicesCount += 6;
            vertexIndex += 6;

            // todo better handle shader limit
            if (text.coordinates.size() >= 480)
            {
                text.subtexts.push_back(std::move(w));
                return text;
            }
        }
        text.subtexts.push_back(std::move(w));
    }
    text.coordinates.shrink_to_fit();
    return text;
}

float numericAlign(GpuGeodataSpec::TextAlign a)
{
    float align = 0.5;
    switch (a)
    {
    case GpuGeodataSpec::TextAlign::Left:
        align = 0;
        break;
    case GpuGeodataSpec::TextAlign::Right:
        align = 1;
        break;
    default:
        break;
    }
    return align;
}

template<class T>
T interpFactor(T what, T before, T after)
{
    return (what - before) / (after - before);
}

template<class T>
bool arrayPosition(const std::vector<T> &v, T p, uint32 &index, T &factor)
{
    assert(v.size() > 1);
    if (p <= v[0])
    {
        index = 0;
        factor = 0;
        return false;
    }
    if (p >= v[v.size() - 1])
    {
        index = v.size() - 2;
        factor = 1;
        return false;
    }
    assert(index < v.size() - 1);
    while (v[index] >= p)
        index--;
    while (v[index + 1] <= p)
        index++;
    factor = interpFactor(p, v[index], v[index + 1]);
    return true;
}

void textLinePositions(GeodataTile *g,
    const std::vector<std::array<float, 3>> &positions, Text &t)
{
    mat4 model = rawToMat4(g->spec.model);
    const auto &m2w = [&](const std::array<float, 3> &pos)
    {
        return vec4to3(vec4(model
            * vec3to4(rawToVec3(pos.data()), 1).cast<double>()));
    };

    // precompute absolute distances
    std::vector<double> dists;
    dists.reserve(positions.size());
    double totalDist = 0;
    {
        vec3 lastPos = m2w(positions[0]);
        for (const auto &it : positions)
        {
            vec3 p = m2w(it);
            totalDist += length(vec3(lastPos - p));
            dists.push_back(totalDist);
            lastPos = p;
        }
        assert(totalDist > 1e-7);
    }

    // normalize distance-along-the-line into -1..1 range
    std::vector<float> &lvp = t.lineVertPositions;
    lvp.reserve(positions.size());
    for (uint32 i = 0, e = positions.size(); i != e; i++)
        lvp.push_back(2 * dists[i] / totalDist - 1);
    assert(std::abs(lvp[0] + 1) < 1e-7);
    assert(std::abs(lvp[lvp.size() - 1] - 1) < 1e-7);

    // find line-positions for each glyph
    uint32 gc = t.coordinates.size() / 4;
    t.lineGlyphPositions.reserve(gc);
    for (uint32 i = 0; i < gc; i++)
    {
        float x = 0;
        for (uint32 j = 0; j < 4; j++)
            x += t.coordinates[i * 4 + j][0];
        x *= 0.25;
        for (uint32 j = 0; j < 4; j++)
            t.coordinates[i * 4 + j][0] -= x;
        t.lineGlyphPositions.push_back(2 * (x + totalDist * 0.5) / totalDist - 1);
    }

    // find center of the line
    {
        uint32 i = 0;
        float f;
        arrayPosition<float>(lvp, 0, i, f);
        Point t;
        t.modelPosition = interpolate(rawToVec3(positions[i].data()),
            rawToVec3(positions[i + 1].data()), (float)f);
        t.worldPosition = interpolate(m2w(positions[i]),
            m2w(positions[i + 1]), (double)f);
        t.worldUp = normalize(t.worldPosition).cast<float>();
        g->points.push_back(t);
    }
}

float labelFlatScale(const RenderViewImpl *rv, const GeodataJob &j)
{
    float scale = rv->draws->camera.viewExtent / rv->height;
    return scale * rv->options.textScale;
}

} // namespace

void GeodataTile::copyFonts()
{
    fontCascade.reserve(spec.fontCascade.size());
    for (auto &i : spec.fontCascade)
        fontCascade.push_back(std::static_pointer_cast<Font>(i));
}

void GeodataTile::loadLabelScreens()
{
    assert(spec.texts.size() == spec.positions.size());
    copyPoints();
    copyFonts();

    float align = numericAlign(spec.unionData.labelScreen.textAlign);
    for (uint32 i = 0, e = spec.texts.size(); i != e; i++)
    {
        std::vector<TmpLine> lines = textToGlyphs(
            spec.texts[i], fontCascade);
        vec2f originSize = textLayout(
            spec.unionData.labelScreen.size,
            align, lines);
        Text t = generateTexts(lines);
        t.collision = textCollision(lines);
        t.originSize = originSize;
        info->ramMemoryCost += t.coordinates.size() * sizeof(vec4f);
        info->ramMemoryCost += t.subtexts.size() * sizeof(Subtext);
        texts.push_back(std::move(t));
    }
    info->ramMemoryCost += texts.size() * sizeof(decltype(texts[0]));
}

void GeodataTile::loadLabelFlats()
{
    assert(spec.texts.size() == spec.positions.size());
    copyFonts();
    points.reserve(spec.positions.size());
    for (uint32 i = 0, e = spec.texts.size(); i != e; i++)
    {
        assert(spec.positions[i].size() > 1); // line must have at least two points
        std::vector<TmpLine> lines = textToGlyphs(
            spec.texts[i], fontCascade);
        textLayout(spec.unionData.labelFlat.size, 0.5, lines);
        Text t = generateTexts(lines);
        textLinePositions(this, spec.positions[i], t);
        info->ramMemoryCost += t.coordinates.size() * sizeof(vec4f);
        info->ramMemoryCost += t.subtexts.size() * sizeof(Subtext);
        texts.push_back(std::move(t));
    }
    info->ramMemoryCost += texts.size() * sizeof(decltype(texts[0]));
    assert(points.size() == spec.positions.size());
}

bool GeodataTile::checkTextures()
{
    bool ok = true;
    for (auto &t : texts)
    {
        for (auto &w : t.subtexts)
        {
            if (w.texture)
                continue;
            w.texture = std::static_pointer_cast<Texture>(
                w.font->fontHandle->requestTexture(w.fileIndex));
            ok = ok && !!w.texture;
        }
    }
    return ok;
}

bool regenerateJobLabelFlat(const RenderViewImpl *rv, GeodataJob &j)
{
    const auto &g = j.g;
    auto &t = g->texts[j.itemIndex];
    const auto &mps = g->spec.positions[j.itemIndex];
    const mat4f mvp = (rv->viewProj * g->model).cast<float>();
    t.tmpGlyphCentersClip.clear();
    t.tmpGlyphCentersClip.reserve(t.lineGlyphPositions.size());
    float scale = labelFlatScale(rv, j);
    bool ok = true;
    uint32 vi = 0;
    for (uint32 i = 0, e = t.lineGlyphPositions.size(); i != e; i++)
    {
        float lp = t.lineGlyphPositions[i] * scale;
        float f;
        ok = arrayPosition(t.lineVertPositions, lp, vi, f) && ok;
        vec3f mpa = rawToVec3(mps[vi].data());
        vec3f mpb = rawToVec3(mps[vi + 1].data());
        vec3f mp = interpolate(mpa, mpb, f);
        vec4f cp = mvp * vec3to4(mp, 1);
        t.tmpGlyphCentersClip.push_back(vec4to2(cp, true));
    }
    return ok;
}

void preDrawJobLabelFlat(const RenderViewImpl *rv, const GeodataJob &j,
    std::vector<vec3> &worldPos, float &scale)
{
    const auto &g = j.g;
    auto &t = g->texts[j.itemIndex];
    const auto &mps = g->spec.positions[j.itemIndex];
    worldPos.clear();
    worldPos.reserve(t.lineGlyphPositions.size());
    scale = labelFlatScale(rv, j);
    uint32 vi = 0;
    for (uint32 i = 0, e = t.lineGlyphPositions.size(); i != e; i++)
    {
        float lp = t.lineGlyphPositions[i] * scale;
        float f;
        arrayPosition(t.lineVertPositions, lp, vi, f);
        vec3f mpa = rawToVec3(mps[vi].data());
        vec3f mpb = rawToVec3(mps[vi + 1].data());
        vec3f mp = interpolate(mpa, mpb, f);
        vec3 wp = vec4to3(vec4(g->model * (vec3to4(mp, 1).cast<double>())));
        worldPos.push_back(wp);
    }
}

} } // namespace vts renderer

