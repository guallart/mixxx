#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>
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
    auto isBarStartIndex = [beatsPerBar, barStartOffset](ptrdiff_t index) {
        if (beatsPerBar <= 0) {
            return false;
        }
        // Shift the bars, keeping the modulo non-negative
        return ((index - barStartOffset) % beatsPerBar + beatsPerBar) % beatsPerBar == 0;
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
    // Bar starts additionally get two triangles (top and bottom).
    const int numVerticesPerBarMarker = 6;
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
    // Reddish color for bar starts
    const QVector4D barColor{0.9f, 0.15f, 0.15f, std::max(a, 0.9f)};

    constexpr float kBarLineWidth = 3.f;
    constexpr float kTriHalfWidth = 6.f;
    constexpr float kTriHeight = 8.f;

    int usedVertices = 0;
    auto addLine = [&](float x1, float y1, float x2, float y2, bool isBarStart) {
        if (!isBarStart) {
            vertexUpdater.addRectangle({x1, y1}, {x2, y2}, normalColor);
            usedVertices += numVerticesPerLine;
            return;
        }
        vertexUpdater.addRectangle({x1, y1}, {x2, y2}, barColor);
        const float cx = (x1 + x2) / 2.f;
        // Top triangle, pointing down
        vertexUpdater.addTriangle({cx - kTriHalfWidth, y1},
                {cx + kTriHalfWidth, y1},
                {cx, y1 + kTriHeight},
                barColor);
        // Bottom triangle, pointing up
        vertexUpdater.addTriangle({cx - kTriHalfWidth, y2},
                {cx + kTriHalfWidth, y2},
                {cx, y2 - kTriHeight},
                barColor);
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

        const bool isBarStart = isBarStartIndex(it - firstBeat);
        const float x1 = static_cast<float>(xBeatPoint) - (isBarStart ? 1.f : 0.f);
        const float x2 = x1 + (isBarStart ? kBarLineWidth : 1.f);

        if (m_isSlipRenderer && splitStemTracks) {
            for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; ++stemIdx) {
                const float posy1 = stemIdx * boxBreadth;
                const float posy2 = posy1 + boxBreadth / 2.f;
                addLine(x1, posy1, x2, posy2, isBarStart);
            }
        } else {
            addLine(x1, 0.f, x2, m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth, isBarStart);
        }
    }
    markDirtyGeometry();
    markDirtyMaterial();

    DEBUG_ASSERT(reserved == usedVertices);

    return true;
}

} // namespace allshader
