#include "textStyleBuilder.h"

#include "data/propertyItem.h" // Include wherever Properties is used!
#include "scene/drawRule.h"
#include "text/lineWrapper.h"
#include "tile/tile.h"
#include "util/geom.h"
#include "util/mapProjection.h"
#include "view/view.h"

#include <cmath>
#include <locale>
#include <mutex>

namespace Tangram {

TextStyleBuilder::TextStyleBuilder(const TextStyle& _style)
    : StyleBuilder(_style),
      m_style(_style),
      m_batch(_style.context()->m_atlas, m_scratch) {}

void TextStyleBuilder::setup(const Tile& _tile){
    m_tileSize = _tile.getProjection()->TileSize();
    m_scratch.clear();

    m_textLabels = std::make_unique<TextLabels>(m_style);
}

std::unique_ptr<StyledMesh> TextStyleBuilder::build() {
    if (!m_scratch.labels.empty()) {
        m_textLabels->setLabels(m_scratch.labels);
        m_textLabels->setQuads(m_scratch.quads);
    }

    m_scratch.clear();

    return std::move(m_textLabels);
}

bool TextStyleBuilder::checkRule(const DrawRule& _rule) const {
    return true;
}

void TextStyleBuilder::addPoint(const Point& _point, const Properties& _props, const DrawRule& _rule) {

    TextStyle::Parameters params = applyRule(_rule, _props);

    if (!prepareLabel(params, Label::Type::point)) { return; }

    addLabel(params, Label::Type::point, { glm::vec2(_point), glm::vec2(_point) });
}

void TextStyleBuilder::addLine(const Line& _line, const Properties& _props, const DrawRule& _rule) {

    TextStyle::Parameters params = applyRule(_rule, _props);

    if (!prepareLabel(params, Label::Type::line)) { return; }

    float pixel = 2.0 / (m_tileSize * m_style.pixelScale());

    float minLength = m_scratch.bbox.x * pixel * 0.2;

    for (size_t i = 0; i < _line.size() - 1; i++) {
        glm::vec2 p1 = glm::vec2(_line[i]);
        glm::vec2 p2 = glm::vec2(_line[i + 1]);
        if (glm::length(p1-p2) > minLength) {
            addLabel(params, Label::Type::line, { p1, p2 });
        }
    }
}

void TextStyleBuilder::addPolygon(const Polygon& _polygon, const Properties& _props, const DrawRule& _rule) {
    Point p = glm::vec3(centroid(_polygon), 0.0);
    addPoint(p, _props, _rule);
}

std::string TextStyleBuilder::applyTextTransform(const TextStyle::Parameters& _params,
                                                 const std::string& _string) {
    std::locale loc;
    std::string text = _string;

    switch (_params.transform) {
        case TextLabelProperty::Transform::capitalize:
            text[0] = toupper(text[0], loc);
            if (text.size() > 1) {
                for (size_t i = 1; i < text.length(); ++i) {
                    if (text[i - 1] == ' ') {
                        text[i] = std::toupper(text[i], loc);
                    }
                }
            }
            break;
        case TextLabelProperty::Transform::lowercase:
            for (size_t i = 0; i < text.length(); ++i) {
                text[i] = std::tolower(text[i], loc);
            }
            break;
        case TextLabelProperty::Transform::uppercase:
            // TODO : use to wupper when any wide character is detected
            for (size_t i = 0; i < text.length(); ++i) {
                text[i] = std::toupper(text[i], loc);
            }
            break;
        default:
            break;
    }

    return text;
}

bool TextStyleBuilder::prepareLabel(TextStyle::Parameters& _params, Label::Type _type) {

    if (_params.text.empty() || _params.fontSize <= 0.f) {
        LOGD("invalid params: '%s' %f", _params.text.c_str(), _params.fontSize);
        return false;
    }

    m_scratch.reset();

    // Apply text transforms
    const std::string* renderText;
    std::string text;

    if (_params.transform == TextLabelProperty::Transform::none) {
        renderText = &_params.text;
    } else {
        text = applyTextTransform(_params, _params.text);
        renderText = &text;
    }

    // Scale factor by which the texture glyphs are scaled to match fontSize
    float fontScale = (_params.fontSize * m_style.pixelScale()) / _params.font->size();

    // Stroke width is normalized by the distance of the SDF spread, then
    // scaled to a char, then packed into the "alpha" channel of stroke.
    // Maximal strokeWidth is 3px, attribute is normalized to 0-1 range.
    float strokeWidth = _params.strokeWidth * m_style.pixelScale();

#if 0
    // HACK - need to use a smaller font in this case
    // to have enough sdf-radius for the stroke!
    // if (strokeWidth > 1.5 * fontScale) {
    //     strokeWidth = 1.5 * fontScale;
    // }

    //// see point.vs and sdf.fs
    float sdf_radius = 3.0;

    // (rate of change within one pixel)
    float sdf_pixel = 0.5 / sdf_radius;

    // scale strokeWidth to sdf_pixel
    float stroke_width = strokeWidth * sdf_pixel;

    // scale sdf (texture is scaled depeding on font size)
    stroke_width /= fontScale;

    float v_sdf_threshold = 0.5 - stroke_width;

    // 0.1245 antialiasing filter width of unscaled glyph
    float filter_width = sdf_pixel / fontScale * (0.5 + 0.25);

    if (v_sdf_threshold - filter_width < 0) {
        // modify stroke width to be within sdf_radius
        LOG("size:%f scale:%f stroke:%f att:%f thres:%f filter:%f ==> %f",
            _params.fontSize, fontScale, _params.strokeWidth,
            stroke_width, v_sdf_threshold, filter_width,
            v_sdf_threshold - filter_width);
    }
#endif
    auto ctx = m_style.context();

    uint32_t strokeAttrib = std::max(std::min(strokeWidth / ctx->maxStrokeWidth() * 255.f, 255.f), 0.f);

    m_scratch.stroke = (_params.strokeColor & 0x00ffffff) + (strokeAttrib << 24);
    m_scratch.fill = _params.fill;
    m_scratch.fontScale = std::min(fontScale * 64.f, 255.f);

    {
        std::lock_guard<std::mutex> lock(ctx->m_mutex);
        auto line = m_shaper.shape(_params.font, *renderText);

        if (line.shapes().size() == 0) {
            LOGD("Empty text line");
            return false;
        }

        //LOG("fontScale %d", m_scratch.fontScale);
        line.setScale(fontScale);

        LineWrap wrap;

        if (_type == Label::Type::point && _params.wordWrap) {
            wrap = drawWithLineWrapping(line, m_batch, _params.maxLineWidth, 4, _params.align,
                m_style.pixelScale());
        } else {
            m_batch.draw(line, glm::vec2(0.0), wrap.metrics);
        }

        m_scratch.bbox.x = fabsf(wrap.metrics.aabb.x) + (wrap.metrics.aabb.z);
        m_scratch.bbox.y = fabsf(wrap.metrics.aabb.y) + (wrap.metrics.aabb.w);

        m_scratch.numLines = m_scratch.bbox.y / line.height();

        m_scratch.metrics.descender = -line.descent();
        m_scratch.metrics.ascender = line.ascent();
        m_scratch.metrics.lineHeight = line.height();

        m_scratch.quadsLocalOrigin = {wrap.metrics.aabb.x, wrap.metrics.aabb.y};
    }

    return true;
}

void TextStyleBuilder::addLabel(const TextStyle::Parameters& _params, Label::Type _type,
                                Label::Transform _transform)
{
    int numQuads = m_scratch.numQuads;
    int quadOffset = m_scratch.quads.size() - numQuads;

    m_scratch.labels.emplace_back(new TextLabel(_transform, _type, _params.labelOptions, _params.anchor,
        {m_scratch.fill, m_scratch.stroke, m_scratch.fontScale }, m_scratch.bbox, m_scratch.metrics,
        m_scratch.numLines, m_scratch.quadsLocalOrigin, *m_textLabels, { quadOffset, numQuads }));
}

TextStyle::Parameters TextStyleBuilder::applyRule(const DrawRule& _rule,
                                                  const Properties& _props) const
{
    const static std::string key_name("name");
    const static std::string defaultWeight("400");
    const static std::string defaultStyle("normal");
    const static std::string defaultFamily("default");

    TextStyle::Parameters p;
    glm::vec2 offset;

    _rule.get(StyleParamKey::text_source, p.text);
    if (!_rule.isJSFunction(StyleParamKey::text_source)) {
        if (p.text.empty()) {
            p.text = _props.getString(key_name);
        } else {
            p.text = _props.getString(p.text);
        }
    }
    if (p.text.empty()) { return p; }

    auto fontFamily = _rule.get<std::string>(StyleParamKey::font_family);
    fontFamily = (!fontFamily) ? &defaultFamily : fontFamily;

    auto fontWeight = _rule.get<std::string>(StyleParamKey::font_weight);
    fontWeight = (!fontWeight) ? &defaultWeight : fontWeight;

    auto fontStyle = _rule.get<std::string>(StyleParamKey::font_style);
    fontStyle = (!fontStyle) ? &defaultStyle : fontStyle;

    _rule.get(StyleParamKey::font_size, p.fontSize);
    // TODO - look font from fontManager
    p.font = m_style.context()->getFont(*fontFamily, *fontStyle, *fontWeight,
                                        p.fontSize * m_style.pixelScale());

    _rule.get(StyleParamKey::font_fill, p.fill);
    _rule.get(StyleParamKey::offset, p.labelOptions.offset);
    _rule.get(StyleParamKey::font_stroke_color, p.strokeColor);
    _rule.get(StyleParamKey::font_stroke_width, p.strokeWidth);
    _rule.get(StyleParamKey::priority, p.labelOptions.priority);
    _rule.get(StyleParamKey::collide, p.labelOptions.collide);
    _rule.get(StyleParamKey::transition_hide_time, p.labelOptions.hideTransition.time);
    _rule.get(StyleParamKey::transition_selected_time, p.labelOptions.selectTransition.time);
    _rule.get(StyleParamKey::transition_show_time, p.labelOptions.showTransition.time);
    _rule.get(StyleParamKey::text_wrap, p.maxLineWidth);

    size_t repeatGroupHash = 0;
    std::string repeatGroup;
    if (_rule.get(StyleParamKey::repeat_group, repeatGroup)) {
        hash_combine(repeatGroupHash, repeatGroup);
    } else {
        repeatGroupHash = _rule.getParamSetHash();
    }

    StyleParam::Width repeatDistance;
    if (_rule.get(StyleParamKey::repeat_distance, repeatDistance)) {
        p.labelOptions.repeatDistance = repeatDistance.value;
    } else {
        p.labelOptions.repeatDistance = View::s_pixelsPerTile;
    }

    hash_combine(repeatGroupHash, p.text);
    p.labelOptions.repeatGroup = repeatGroupHash;
    p.labelOptions.repeatDistance *= m_style.pixelScale();

    if (_rule.get(StyleParamKey::interactive, p.interactive) && p.interactive) {
        p.labelOptions.properties = std::make_shared<Properties>(_props);
    }

    if (auto* anchor = _rule.get<std::string>(StyleParamKey::anchor)) {
        LabelProperty::anchor(*anchor, p.anchor);
    }

    if (auto* transform = _rule.get<std::string>(StyleParamKey::transform)) {
        TextLabelProperty::transform(*transform, p.transform);
    }

    if (auto* align = _rule.get<std::string>(StyleParamKey::align)) {
        bool res = TextLabelProperty::align(*align, p.align);
        if (!res) {
            switch(p.anchor) {
            case LabelProperty::Anchor::top_left:
            case LabelProperty::Anchor::left:
            case LabelProperty::Anchor::bottom_left:
                p.align = TextLabelProperty::Align::right;
                break;
            case LabelProperty::Anchor::top_right:
            case LabelProperty::Anchor::right:
            case LabelProperty::Anchor::bottom_right:
                p.align = TextLabelProperty::Align::left;
                break;
            case LabelProperty::Anchor::top:
            case LabelProperty::Anchor::bottom:
            case LabelProperty::Anchor::center:
                break;
            }
        }
    }

    /* Global operations done for fontsize and sdfblur */
    p.fontSize *= m_pixelScale;
    p.labelOptions.offset *= m_pixelScale;
    float emSize = p.fontSize / 16.f;
    p.blurSpread = m_sdf ? emSize * 5.0f : 0.0f;

    float boundingBoxBuffer = -p.fontSize / 2.f;
    p.labelOptions.buffer = boundingBoxBuffer;

    std::hash<TextStyle::Parameters> hash;
    p.labelOptions.paramHash = hash(p);

    return p;
}

void TextStyleBuilder::ScratchBuffer::reset() {
    yMin = std::numeric_limits<float>::max();
    xMin = std::numeric_limits<float>::max();
    bbox = glm::vec2(0);
    numLines = 1;
    numQuads = 0;
}

void TextStyleBuilder::ScratchBuffer::drawGlyph(const alf::Rect& q, const alf::AtlasGlyph& atlasGlyph) {
    numQuads++;

    auto& g = *atlasGlyph.glyph;
    quads.push_back({
            atlasGlyph.atlas,
            {{glm::vec2{q.x1, q.y1} * position_scale, {g.u1, g.v1}},
             {glm::vec2{q.x1, q.y2} * position_scale, {g.u1, g.v2}},
             {glm::vec2{q.x2, q.y1} * position_scale, {g.u2, g.v1}},
             {glm::vec2{q.x2, q.y2} * position_scale, {g.u2, g.v2}}}});
}

void TextStyleBuilder::ScratchBuffer::clear() {
    quads.clear();
    labels.clear();
}

}
