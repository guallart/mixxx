#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>
#include <QVector2D>
#include <QVector4D>

#include <algorithm>

#include "control/controlproxy.h"
#include "engine/engine.h"
#include "moc_waveformrenderbeat.cpp"
#include "rendergraph/geometry.h"
#include "rendergraph/material/rgbamaterial.h"
#include "rendergraph/vertexupdaters/rgbavertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/wskincolor.h"

using namespace rendergraph;

namespace allshader {

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type)
        : ::WaveformRendererAbstract(waveformWidget),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip) {
    initForRectangles<RGBAMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& skinContext) {
    m_color = QColor(skinContext.selectString(node, QStringLiteral("BeatColor")));
    m_color = WSkinColor::getCorrectColor(m_color).toRgb();
}

void WaveformRenderBeat::draw(QPainter* painter, QPaintEvent* event) {
    Q_UNUSED(painter);
    Q_UNUSED(event);
    DEBUG_ASSERT(false);
}

void WaveformRenderBeat::preprocess() {
    if (!preprocessInner()) {
        geometry().allocate(0);
        markDirtyGeometry();
    }
}

bool WaveformRenderBeat::preprocessInner() {
    const TrackPointer trackInfo = m_waveformRenderer->getTrackInfo();

    if (!trackInfo || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return false;
    }

    const bool isStemTrack = trackInfo && trackInfo->hasStem() &&
            trackInfo->getWaveform() && trackInfo->getWaveform()->hasStem();
    const bool splitStemTracks = isStemTrack && WaveformWidgetFactory::isCreated() &&
            WaveformWidgetFactory::instance()->isStemSplitTracks();

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    mixxx::BeatsPointer trackBeats = trackInfo->getBeats();
    if (!trackBeats) {
        return false;
    }

#ifndef __SCENEGRAPH__
    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return false;
    }
    m_color.setAlphaF(alpha / 100.0f);
#endif

    if (!m_color.alpha()) {
        // Don't render the beatgrid lines is there are fully transparent
        return false;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0.0) {
        return false;
    }

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition(positionType);
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition(positionType);

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);

    if (!startPosition.isValid() || !endPosition.isValid()) {
        return false;
    }

    const float rendererBreadth = m_waveformRenderer->getBreadth();

    const int numVerticesPerLine = 6; // 2 triangles

    // Count the number of beats in the range to reserve space in the m_vertices vector.
    // Note that we could also use
    //   int numBearsInRange = trackBeats->numBeatsInRange(startPosition, endPosition);
    // for this, but there have been reports of that method failing with a DEBUG_ASSERT.
    // Beats that start a bar are drawn as thicker lines.
    const int beatsPerBar = WaveformWidgetFactory::isCreated()
            ? WaveformWidgetFactory::instance()->getBeatsPerBar()
            : 0;
    if (!m_pBarStartOffset) {
        m_pBarStartOffset = std::make_unique<ControlProxy>(
                m_waveformRenderer->getGroup(), QStringLiteral("bar_start_offset"));
    }
    const int barStartOffset = static_cast<int>(m_pBarStartOffset->get());
    const auto firstBeat = trackBeats->cbegin();
    // Every 4th bar starts a phrase, which gets a distinct marker.
    constexpr ptrdiff_t kBarsPerPhrase = 4;
    // Returns 0 for a regular beat, 1 for a bar start and 2 for a phrase start.
    auto markerLevel = [beatsPerBar, barStartOffset](ptrdiff_t index) {
        if (beatsPerBar <= 0) {
            return 0;
        }
        // Shift the bars, keeping the modulo non-negative
        const ptrdiff_t shifted = index - barStartOffset;
        if ((shifted % beatsPerBar + beatsPerBar) % beatsPerBar != 0) {
            return 0;
        }
        const ptrdiff_t barNumber = shifted / beatsPerBar;
        return ((barNumber % kBarsPerPhrase + kBarsPerPhrase) % kBarsPerPhrase == 0) ? 2 : 1;
    };
    auto isBarStartIndex = [&markerLevel](ptrdiff_t index) {
        return markerLevel(index) > 0;
    };

    int numBeatsInRange = 0;
    int numBarStartsInRange = 0;
    for (auto it = trackBeats->iteratorFrom(startPosition);
            it != trackBeats->cend() && *it <= endPosition;
            ++it) {
        numBeatsInRange++;
        if (isBarStartIndex(it - firstBeat)) {
            numBarStartsInRange++;
        }
    }

    const int numBoxesPerBeat = (m_isSlipRenderer && splitStemTracks)
            ? mixxx::kMaxSupportedStems
            : 1;
    // Bar starts additionally get a soft glow (1 rectangle) and two small
    // caps (top and bottom), i.e. 3 rectangles.
    const int numVerticesPerBarMarker = 18;
    const int reserved = numBoxesPerBeat *
            (numBeatsInRange * numVerticesPerLine +
                    numBarStartsInRange * numVerticesPerBarMarker);
    geometry().allocate(reserved);

    RGBAVertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::RGBAColoredPoint2D>()};

    const float boxBreadth = splitStemTracks
            ? rendererBreadth / static_cast<float>(mixxx::kMaxSupportedStems)
            : rendererBreadth;

    const float r = static_cast<float>(m_color.redF());
    const float g = static_cast<float>(m_color.greenF());
    const float b = static_cast<float>(m_color.blueF());
    const float a = static_cast<float>(m_color.alphaF());
    const QVector4D normalColor{r, g, b, a};
    // Modern coral red for bar starts, with a translucent glow behind the line
    const QVector4D barColor{1.0f, 0.30f, 0.34f, std::max(a, 0.95f)};
    const QVector4D barGlowColor{1.0f, 0.30f, 0.34f, 0.25f};
    // Electric violet for phrase starts (every 4 bars), clearly distinct from
    // the coral bar markers and from typical waveform colors
    const QVector4D phraseColor{0.62f, 0.36f, 1.0f, std::max(a, 0.98f)};
    const QVector4D phraseGlowColor{0.62f, 0.36f, 1.0f, 0.35f};

    constexpr float kBarLineWidth = 2.f;
    constexpr float kGlowHalfWidth = 3.f;
    constexpr float kCapHalfWidth = 5.f;
    constexpr float kCapHeight = 3.f;
    constexpr float kPhraseLineWidth = 3.f;
    constexpr float kPhraseGlowHalfWidth = 5.f;
    // Phrase markers end in diamonds instead of flat caps
    constexpr float kPhraseDiamondHalfSize = 7.f;

    int usedVertices = 0;
    auto addLine = [&](float x1, float y1, float x2, float y2, int level) {
        if (level == 0) {
            vertexUpdater.addRectangle({x1, y1}, {x2, y2}, normalColor);
            usedVertices += numVerticesPerLine;
            return;
        }
        const bool isPhrase = level == 2;
        const QVector4D& color = isPhrase ? phraseColor : barColor;
        const QVector4D& glowColor = isPhrase ? phraseGlowColor : barGlowColor;
        const float glowHalfWidth = isPhrase ? kPhraseGlowHalfWidth : kGlowHalfWidth;
        const float capHalfWidth = kCapHalfWidth;
        const float capHeight = kCapHeight;
        const float cx = (x1 + x2) / 2.f;
        // Soft glow behind the line
        vertexUpdater.addRectangle({cx - glowHalfWidth, y1},
                {cx + glowHalfWidth, y2},
                glowColor);
        // The marker line itself
        vertexUpdater.addRectangle({x1, y1}, {x2, y2}, color);
        if (isPhrase) {
            // Diamonds (2 triangles each) at both ends
            auto addDiamond = [&](float centerY) {
                const float h = kPhraseDiamondHalfSize;
                const QVector2D left{cx - h, centerY};
                const QVector2D right{cx + h, centerY};
                const QVector2D top{cx, centerY - h};
                const QVector2D bottom{cx, centerY + h};
                vertexUpdater.addTriangle({left.x(), left.y()},
                        {right.x(), right.y()},
                        {top.x(), top.y()},
                        color);
                vertexUpdater.addTriangle({left.x(), left.y()},
                        {right.x(), right.y()},
                        {bottom.x(), bottom.y()},
                        color);
            };
            addDiamond(y1 + kPhraseDiamondHalfSize);
            addDiamond(y2 - kPhraseDiamondHalfSize);
        } else {
            // Top cap
            vertexUpdater.addRectangle({cx - capHalfWidth, y1},
                    {cx + capHalfWidth, y1 + capHeight},
                    color);
            // Bottom cap
            vertexUpdater.addRectangle({cx - capHalfWidth, y2 - capHeight},
                    {cx + capHalfWidth, y2},
                    color);
        }
        usedVertices += numVerticesPerLine + numVerticesPerBarMarker;
    };

    for (auto it = trackBeats->iteratorFrom(startPosition);
            it != trackBeats->cend() && *it <= endPosition;
            ++it) {
        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        beatPosition, positionType);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const int level = markerLevel(it - firstBeat);
        const float lineWidth = level == 2 ? kPhraseLineWidth
                : level == 1               ? kBarLineWidth
                                           : 1.f;
        const float x1 = static_cast<float>(xBeatPoint) - (level > 0 ? 1.f : 0.f);
        const float x2 = x1 + lineWidth;

        if (m_isSlipRenderer && splitStemTracks) {
            for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; ++stemIdx) {
                const float posy1 = stemIdx * boxBreadth;
                const float posy2 = posy1 + boxBreadth / 2.f;
                addLine(x1, posy1, x2, posy2, level);
            }
        } else {
            addLine(x1, 0.f, x2, m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth, level);
        }
    }
    markDirtyGeometry();
    markDirtyMaterial();

    DEBUG_ASSERT(reserved == usedVertices);

    return true;
}

} // namespace allshader
