// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 vibesnake

#include "SpectrumWaterfallItem.hpp"

#include "ApplicationModel.hpp"
#include "SpectrumAmplitudeScale.hpp"
#include "WaterfallPalette.hpp"

#include <QColor>
#include <QDebug>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QSGFlatColorMaterial>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTextureMaterial>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr float waterfallMinimumDbfsDefault = -120.0F;
constexpr float waterfallMaximumDbfsDefault = -20.0F;
constexpr float waterfallMinimumDbfsLimit = -140.0F;
constexpr float waterfallMaximumDbfsLimit = 0.0F;
constexpr float waterfallMinimumSeparationDb = 5.0F;
constexpr auto waterfallMinimumDbfsSetting = "waterfall/minimumDbfs";
constexpr auto waterfallMaximumDbfsSetting = "waterfall/maximumDbfs";
constexpr auto waterfallAggregationSetting = "waterfall/aggregation";
constexpr auto waterfallOriginalAggregation = "original";
constexpr auto waterfallAverageAggregation = "average";
constexpr auto maximumHoldEnabledSetting = "spectrum/maximumHoldEnabled";

void updateHoldStrokeGeometry(
    QSGGeometryNode& underStroke,
    QSGGeometryNode& whiteStroke,
    const QVector<float>& dbfs,
    bool enabled,
    float width,
    float height,
    float minimumDbfs,
    float maximumDbfs)
{
    const int vertexCount = enabled ? static_cast<int>(dbfs.size()) : 0;
    for (auto* node : {&underStroke, &whiteStroke}) {
        if (node->geometry()->vertexCount() != vertexCount) {
            node->geometry()->allocate(vertexCount);
        }
    }
    if (vertexCount < 2) {
        underStroke.markDirty(QSGNode::DirtyGeometry);
        whiteStroke.markDirty(QSGNode::DirtyGeometry);
        return;
    }

    auto* underVertices =
        underStroke.geometry()->vertexDataAsPoint2D();
    auto* whiteVertices =
        whiteStroke.geometry()->vertexDataAsPoint2D();
    const float divisor = static_cast<float>(vertexCount - 1);
    for (int index = 0; index < vertexCount; ++index) {
        const float x = static_cast<float>(index) * width / divisor;
        const float y = sdr::gui::spectrumYForDbfs(
            dbfs[index], height, minimumDbfs, maximumDbfs);
        underVertices[index].set(x, y);
        whiteVertices[index].set(x, y);
    }
    underStroke.markDirty(QSGNode::DirtyGeometry);
    whiteStroke.markDirty(QSGNode::DirtyGeometry);
}

class FilterIndicatorNode final : public QSGNode
{
public:
    FilterIndicatorNode()
    {
        m_lowerLine = createQuad(QColor(QStringLiteral("#f6dc72")));
        m_listeningLine = createQuad(QColor(QStringLiteral("#f6ad55")));
        m_upperLine = createQuad(QColor(QStringLiteral("#f6dc72")));
        appendChildNode(m_lowerLine);
        appendChildNode(m_listeningLine);
        appendChildNode(m_upperLine);
        for (std::size_t index = 0; index < m_markerOutlines.size(); ++index) {
            m_markerOutlines[index] = createTriangle(QColor(Qt::black));
            m_markerFills[index] = createTriangle(QColor(QStringLiteral("#ffd84d")));
            appendChildNode(m_markerOutlines[index]);
            appendChildNode(m_markerFills[index]);
        }
    }

    void update(const sdr::gui::FilterGate& gate, float height, float dpr)
    {
        updateEdgeLine(*m_lowerLine, gate.lowerEdge, height, dpr,
                       gate.edgeLinePattern, gate.edgeLineOpacity);
        updateListeningLine(*m_listeningLine, gate.listening, height);
        updateEdgeLine(*m_upperLine, gate.upperEdge, height, dpr,
                       gate.edgeLinePattern, gate.edgeLineOpacity);
        const float pixel = 1.0F / std::max(1.0F, dpr);
        for (std::size_t index = 0; index < gate.markers.size(); ++index) {
            updateTriangle(*m_markerOutlines[index], gate.markers[index], 0.0F);
            updateTriangle(*m_markerFills[index], gate.markers[index], pixel);
        }
    }

    void hide()
    {
        for (auto* node : {m_lowerLine, m_listeningLine, m_upperLine}) {
            node->geometry()->allocate(0);
            node->markDirty(QSGNode::DirtyGeometry);
        }
        for (auto* node : m_markerOutlines) {
            node->geometry()->allocate(0);
            node->markDirty(QSGNode::DirtyGeometry);
        }
        for (auto* node : m_markerFills) {
            node->geometry()->allocate(0);
            node->markDirty(QSGNode::DirtyGeometry);
        }
    }

private:
    static QSGGeometryNode* createQuad(const QColor& color)
    {
        auto* node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 6);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* material = new QSGFlatColorMaterial;
        material->setColor(color);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    }

    static QSGGeometryNode* createTriangle(const QColor& color)
    {
        auto* node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 3);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* material = new QSGFlatColorMaterial;
        material->setColor(color);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    }

    static void updateListeningLine(
        QSGGeometryNode& node, const sdr::gui::FilterGateLine& line, float height)
    {
        if (node.geometry()->vertexCount() != 6) {
            node.geometry()->allocate(6);
        }
        auto* vertices = node.geometry()->vertexDataAsPoint2D();
        const float left = static_cast<float>(line.centerX) - line.width / 2.0F;
        const float right = static_cast<float>(line.centerX) + line.width / 2.0F;
        vertices[0].set(left, 0.0F);
        vertices[1].set(left, height);
        vertices[2].set(right, 0.0F);
        vertices[3].set(right, 0.0F);
        vertices[4].set(left, height);
        vertices[5].set(right, height);
        static_cast<QSGFlatColorMaterial*>(node.material())->setColor(
            QColor(QStringLiteral("#f6ad55")));
        node.markDirty(QSGNode::DirtyMaterial);
        node.markDirty(QSGNode::DirtyGeometry);
    }

    static void updateEdgeLine(
        QSGGeometryNode& node,
        const sdr::gui::FilterGateLine& line,
        float height,
        float dpr,
        sdr::gui::FilterGateLinePattern pattern,
        float opacity)
    {
        const float pixel = 1.0F / std::max(1.0F, dpr);
        const int physicalHeight = static_cast<int>(std::ceil(height * dpr));
        const auto dashCount = static_cast<std::size_t>(
            std::max(0, (physicalHeight + 5) / 6));
        const std::size_t count = pattern == sdr::gui::FilterGateLinePattern::Full
                                      ? 1U
                                      : (pattern == sdr::gui::FilterGateLinePattern::Dashed
                                             ? dashCount
                                             : 2U);
        const int verticesNeeded = static_cast<int>(count * 6U);
        if (node.geometry()->vertexCount() != verticesNeeded) {
            node.geometry()->allocate(verticesNeeded);
        }
        auto* vertices = node.geometry()->vertexDataAsPoint2D();
        const float left = static_cast<float>(line.centerX) - line.width / 2.0F;
        const float right = static_cast<float>(line.centerX) + line.width / 2.0F;
        const auto setSegment = [&](std::size_t index, float top, float bottom) {
            auto* quad = vertices + static_cast<std::ptrdiff_t>(index * 6U);
            quad[0].set(left, top);
            quad[1].set(left, bottom);
            quad[2].set(right, top);
            quad[3].set(right, top);
            quad[4].set(left, bottom);
            quad[5].set(right, bottom);
        };
        if (pattern == sdr::gui::FilterGateLinePattern::Full) {
            setSegment(0, 0.0F, height);
        } else if (pattern == sdr::gui::FilterGateLinePattern::Dashed) {
            for (std::size_t index = 0; index < count; ++index) {
                const float top = static_cast<float>(index * 6U) * pixel;
                setSegment(index, top, std::min(height, top + 2.0F * pixel));
            }
        } else {
            const float stub = std::min(height / 2.0F, 7.0F * pixel);
            setSegment(0, 0.0F, stub);
            setSegment(1, height - stub, height);
        }
        QColor color(QStringLiteral("#f6dc72"));
        color.setAlphaF(opacity);
        static_cast<QSGFlatColorMaterial*>(node.material())->setColor(color);
        node.markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    }

    static void updateTriangle(
        QSGGeometryNode& node, const sdr::gui::FilterGateTriangle& marker, float inset)
    {
        if (node.geometry()->vertexCount() != 3) {
            node.geometry()->allocate(3);
        }
        const float halfWidth = std::max(0.0F, marker.width / 2.0F - inset);
        const float baseY = marker.pointsDown ? marker.baseY + inset
                                              : marker.baseY - inset;
        const float tipY = marker.pointsDown ? marker.tipY - inset
                                             : marker.tipY + inset;
        auto* vertices = node.geometry()->vertexDataAsPoint2D();
        vertices[0].set(static_cast<float>(marker.tipX), tipY);
        vertices[1].set(static_cast<float>(marker.tipX) - halfWidth, baseY);
        vertices[2].set(static_cast<float>(marker.tipX) + halfWidth, baseY);
        node.markDirty(QSGNode::DirtyGeometry);
    }

    QSGGeometryNode* m_lowerLine = nullptr;
    QSGGeometryNode* m_listeningLine = nullptr;
    QSGGeometryNode* m_upperLine = nullptr;
    std::array<QSGGeometryNode*, 4> m_markerOutlines{};
    std::array<QSGGeometryNode*, 4> m_markerFills{};
};

class WaterfallTextureNode final : public QSGSimpleTextureNode
{
public:
    WaterfallTextureNode()
    {
        setOwnsTexture(true);
    }
};

const QImage& slopSpectrumPaletteImage()
{
    static const QImage image = [] {
        QImage result(256, 1, QImage::Format_RGBA8888);
        const auto& palette = sdr::gui::slopSpectrumPalette();
        for (int index = 0; index < result.width(); ++index) {
            result.setPixel(index, 0, palette[static_cast<std::size_t>(index)]);
        }
        return result;
    }();
    return image;
}

float paletteTextureCoordinate(int paletteIndex) noexcept
{
    return (static_cast<float>(std::clamp(paletteIndex, 0, 255)) + 0.5F) /
           256.0F;
}

class SpectrumNode final : public QSGNode
{
public:
    SpectrumNode()
    {
        m_background = createBackground();
        m_fill = createFill();
        m_trace = createTrace();
        m_maximumHoldUnderStroke = createHoldStroke(QColor(0, 0, 0, 190), 3.0F);
        m_maximumHoldWhiteStroke = createHoldStroke(Qt::white, 1.0F);
        appendChildNode(m_background);
        appendChildNode(m_fill);
        appendChildNode(m_trace);
        appendChildNode(m_maximumHoldUnderStroke);
        appendChildNode(m_maximumHoldWhiteStroke);
    }

    ~SpectrumNode() override
    {
        if (m_paletteTexture) {
            static_cast<QSGTextureMaterial*>(m_fill->material())
                ->setTexture(nullptr);
            delete m_paletteTexture;
        }
    }

    void ensurePaletteTexture(QQuickWindow* window)
    {
        if (m_paletteTexture || !window) {
            return;
        }
        m_paletteTexture = window->createTextureFromImage(
            slopSpectrumPaletteImage());
        if (!m_paletteTexture) {
            return;
        }
        auto* material = static_cast<QSGTextureMaterial*>(m_fill->material());
        material->setTexture(m_paletteTexture);
        material->setFiltering(QSGTexture::Nearest);
        material->setMipmapFiltering(QSGTexture::None);
        material->setHorizontalWrapMode(QSGTexture::ClampToEdge);
        material->setVerticalWrapMode(QSGTexture::ClampToEdge);
    }

    [[nodiscard]] QSGGeometryNode* background() const noexcept
    {
        return m_background;
    }

    [[nodiscard]] QSGGeometryNode* fill() const noexcept
    {
        return m_fill;
    }

    [[nodiscard]] QSGGeometryNode* trace() const noexcept
    {
        return m_trace;
    }

    [[nodiscard]] QSGGeometryNode* maximumHoldUnderStroke() const noexcept
    {
        return m_maximumHoldUnderStroke;
    }

    [[nodiscard]] QSGGeometryNode* maximumHoldWhiteStroke() const noexcept
    {
        return m_maximumHoldWhiteStroke;
    }

private:
    static QSGGeometryNode* createBackground()
    {
        auto* node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(
            QSGGeometry::defaultAttributes_Point2D(), 4);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* material = new QSGFlatColorMaterial;
        material->setColor(Qt::black);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    }

    static QSGGeometryNode* createFill()
    {
        auto* node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(
            QSGGeometry::defaultAttributes_TexturedPoint2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* material = new QSGTextureMaterial;
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    }

    static QSGGeometryNode* createTrace()
    {
        auto* node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(
            QSGGeometry::defaultAttributes_Point2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawLineStrip);
        geometry->setLineWidth(1.0F);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* material = new QSGFlatColorMaterial;
        material->setColor(QColor(QStringLiteral("#77e3ff")));
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    }

    static QSGGeometryNode* createHoldStroke(
        const QColor& color, float width)
    {
        auto* node = new QSGGeometryNode;
        auto* geometry = new QSGGeometry(
            QSGGeometry::defaultAttributes_Point2D(), 0);
        geometry->setDrawingMode(QSGGeometry::DrawLineStrip);
        geometry->setLineWidth(width);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        auto* material = new QSGFlatColorMaterial;
        material->setColor(color);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial);
        return node;
    }

    QSGGeometryNode* m_background = nullptr;
    QSGGeometryNode* m_fill = nullptr;
    QSGGeometryNode* m_trace = nullptr;
    QSGGeometryNode* m_maximumHoldUnderStroke = nullptr;
    QSGGeometryNode* m_maximumHoldWhiteStroke = nullptr;
    QSGTexture* m_paletteTexture = nullptr;
};

class SpectrumWaterfallRenderNode final : public QSGNode
{
public:
    SpectrumWaterfallRenderNode()
    {
        appendChildNode(m_filterIndicator);
    }

    [[nodiscard]] QSGNode* content() const noexcept
    {
        return m_content;
    }

    void setContent(QSGNode* content)
    {
        if (m_content == content) {
            return;
        }
        if (m_content) {
            removeChildNode(m_content);
            delete m_content;
        }
        m_content = content;
        if (m_content) {
            prependChildNode(m_content);
        }
    }

    [[nodiscard]] FilterIndicatorNode* filterIndicator() const noexcept
    {
        return m_filterIndicator;
    }

private:
    QSGNode* m_content = nullptr;
    FilterIndicatorNode* m_filterIndicator = new FilterIndicatorNode;
};

bool validWaterfallRange(float minimumDbfs, float maximumDbfs) noexcept
{
    return std::isfinite(minimumDbfs) && std::isfinite(maximumDbfs) &&
           minimumDbfs >= waterfallMinimumDbfsLimit &&
           maximumDbfs <= waterfallMaximumDbfsLimit &&
           maximumDbfs - minimumDbfs >= waterfallMinimumSeparationDb;
}

std::uint64_t steadyTimestampNanoseconds() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

SpectrumWaterfallItem::SpectrumWaterfallItem(QQuickItem* parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setClip(true);
    QSettings settings;
    bool minimumValid = false;
    bool maximumValid = false;
    const float storedMinimum = settings.value(waterfallMinimumDbfsSetting).toFloat(&minimumValid);
    const float storedMaximum = settings.value(waterfallMaximumDbfsSetting).toFloat(&maximumValid);
    if (minimumValid && maximumValid &&
        validWaterfallRange(storedMinimum, storedMaximum)) {
        m_waterfallMinimumDbfs = std::round(storedMinimum);
        m_waterfallMaximumDbfs = std::round(storedMaximum);
    } else {
        settings.setValue(waterfallMinimumDbfsSetting, waterfallMinimumDbfsDefault);
        settings.setValue(waterfallMaximumDbfsSetting, waterfallMaximumDbfsDefault);
    }
    const QString storedAggregation = settings.value(
        waterfallAggregationSetting, waterfallOriginalAggregation).toString();
    if (storedAggregation == QLatin1String(waterfallAverageAggregation)) {
        m_waterfallAggregation = sdr::gui::WaterfallAggregation::Average;
    } else if (storedAggregation != QLatin1String(waterfallOriginalAggregation)) {
        settings.setValue(
            waterfallAggregationSetting, waterfallOriginalAggregation);
    } else if (!settings.contains(waterfallAggregationSetting)) {
        settings.setValue(
            waterfallAggregationSetting, waterfallOriginalAggregation);
    }
    settings.remove(QStringLiteral("spectrum/minimumHoldEnabled"));
    m_maximumHoldEnabled =
        settings.value(maximumHoldEnabledSetting, false).toBool();
    m_scrollTimer.setInterval(16);
    m_scrollTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_scrollTimer, &QTimer::timeout, this, [this] {
        if (m_waterfall && !m_paused && m_waterfallHistory.size() > 0) {
            m_scrollRasterDirty = true;
            update();
        }
    });
    m_resizeCoalesceTimer.setInterval(16);
    m_resizeCoalesceTimer.setSingleShot(true);
    m_resizeCoalesceTimer.setTimerType(Qt::PreciseTimer);
    connect(
        &m_resizeCoalesceTimer,
        &QTimer::timeout,
        this,
        &SpectrumWaterfallItem::commitRasterResize);
    m_waterfallHistory.setRetentionDurationSeconds(
        m_retainedHistoryDurationSeconds);
    m_waterfallHistory.setCapacity(
        static_cast<std::size_t>(std::ceil(
            m_effectiveRowsPerSecond * m_visibleHistorySeconds)) +
            2);
    connect(
        this,
        &QQuickItem::windowChanged,
        this,
        [this](QQuickWindow* itemWindow) {
            QObject::disconnect(m_windowScreenConnection);
            if (itemWindow) {
                m_windowScreenConnection = connect(
                    itemWindow,
                    &QQuickWindow::screenChanged,
                    this,
                    &SpectrumWaterfallItem::refreshRasterScreenConnection);
            }
            refreshRasterScreenConnection();
        });
}

QObject* SpectrumWaterfallItem::applicationModel() const noexcept
{
    return m_applicationModel;
}

void SpectrumWaterfallItem::setApplicationModel(QObject* applicationModel)
{
    auto* typedModel = qobject_cast<ApplicationModel*>(applicationModel);
    if (m_applicationModel == typedModel) {
        return;
    }

    QObject::disconnect(m_frameConnection);
    QObject::disconnect(m_waterfallFrameConnection);
    QObject::disconnect(m_resetConnection);
    QObject::disconnect(m_waterfallResetConnection);
    QObject::disconnect(m_centerFrequencyConnection);
    QObject::disconnect(m_frequencyViewportConnection);
    QObject::disconnect(m_listeningFrequencyConnection);
    QObject::disconnect(m_filterMarkerConnection);
    QObject::disconnect(m_filterWidthConnection);
    QObject::disconnect(m_demodulationModeConnection);
    QObject::disconnect(m_scannerConnection);
    QObject::disconnect(m_requestedGainConnection);
    QObject::disconnect(m_effectiveGainConnection);
    QObject::disconnect(m_effectiveSampleRateConnection);
    QObject::disconnect(m_spectrumFftSizeConnection);
    QObject::disconnect(m_receiverRunningConnection);
    QObject::disconnect(m_deviceStateConnection);
    resetSpectrumHolds();
    m_applicationModel = typedModel;
    m_waterfallClearedForScannerPause = false;
    if (m_applicationModel) {
        m_observedRequestedGainDb = m_applicationModel->requestedGain();
        m_observedEffectiveGainDb = m_applicationModel->gain();
        m_observedReceiverRunning = m_applicationModel->receiverRunning();
        m_observedSelectedDeviceIndex =
            m_applicationModel->selectedDeviceIndex();
        m_observedBackendReady = m_applicationModel->backendReady();
        m_frameConnection = connect(
            m_applicationModel,
            &ApplicationModel::spectrumFrameReady,
            this,
            &SpectrumWaterfallItem::receiveSpectrumFrame);
        m_waterfallFrameConnection = connect(
            m_applicationModel,
            &ApplicationModel::waterfallFrameReady,
            this,
            &SpectrumWaterfallItem::receiveWaterfallFrame);
        m_resetConnection = connect(
            m_applicationModel,
            &ApplicationModel::spectrumReset,
            this,
            &SpectrumWaterfallItem::clearSpectrumFrame);
        m_waterfallResetConnection = connect(
            m_applicationModel,
            &ApplicationModel::waterfallReset,
            this,
            &SpectrumWaterfallItem::clearWaterfallFrames);
        m_centerFrequencyConnection = connect(
            m_applicationModel,
            &ApplicationModel::centerFrequencyChanged,
            this,
            [this] {
                resetSpectrumHolds();
                update();
            });
        m_frequencyViewportConnection = connect(
            m_applicationModel,
            &ApplicationModel::frequencyViewportChanged,
            this,
            &SpectrumWaterfallItem::frequencyAxisChanged);
        m_listeningFrequencyConnection = connect(
            m_applicationModel,
            &ApplicationModel::listeningFrequencyChanged,
            this,
            &SpectrumWaterfallItem::update);
        m_filterMarkerConnection = connect(
            m_applicationModel,
            &ApplicationModel::filterMarkerChanged,
            this,
            &SpectrumWaterfallItem::update);
        m_filterWidthConnection = connect(
            m_applicationModel,
            &ApplicationModel::filterWidthChanged,
            this,
            &SpectrumWaterfallItem::update);
        m_demodulationModeConnection = connect(
            m_applicationModel,
            &ApplicationModel::demodulationModeChanged,
            this,
            &SpectrumWaterfallItem::update);
        m_scannerConnection = connect(
            m_applicationModel,
            &ApplicationModel::scannerChanged,
            this,
            [this] {
                if (m_waterfall && m_paused &&
                    m_applicationModel->scannerOwnsTuning() &&
                    !m_waterfallClearedForScannerPause) {
                    clearWaterfallFrames();
                    m_waterfallClearedForScannerPause = true;
                }
            });
        m_requestedGainConnection = connect(
            m_applicationModel,
            &ApplicationModel::requestedGainChanged,
            this,
            [this] {
                const double requestedGain = m_applicationModel->requestedGain();
                if (std::abs(
                        requestedGain - m_observedRequestedGainDb) > 1.0e-9) {
                    m_observedRequestedGainDb = requestedGain;
                    resetSpectrumHolds();
                }
            });
        m_effectiveGainConnection = connect(
            m_applicationModel,
            &ApplicationModel::gainChanged,
            this,
            [this] {
                const double effectiveGain = m_applicationModel->gain();
                if (std::abs(
                        effectiveGain - m_observedEffectiveGainDb) > 1.0e-9) {
                    m_observedEffectiveGainDb = effectiveGain;
                    resetSpectrumHolds();
                }
            });
        m_effectiveSampleRateConnection = connect(
            m_applicationModel,
            &ApplicationModel::effectiveSampleRateChanged,
            this,
            &SpectrumWaterfallItem::resetSpectrumHolds);
        m_spectrumFftSizeConnection = connect(
            m_applicationModel,
            &ApplicationModel::spectrumFftSizeChanged,
            this,
            &SpectrumWaterfallItem::resetSpectrumHolds);
        m_receiverRunningConnection = connect(
            m_applicationModel,
            &ApplicationModel::receiverRunningChanged,
            this,
            [this] {
                const bool running = m_applicationModel->receiverRunning();
                if (running != m_observedReceiverRunning) {
                    m_observedReceiverRunning = running;
                    resetSpectrumHolds();
                }
            });
        m_deviceStateConnection = connect(
            m_applicationModel,
            &ApplicationModel::deviceStateChanged,
            this,
            [this] {
                const int selectedDeviceIndex =
                    m_applicationModel->selectedDeviceIndex();
                const bool backendReady = m_applicationModel->backendReady();
                if (selectedDeviceIndex != m_observedSelectedDeviceIndex ||
                    backendReady != m_observedBackendReady) {
                    m_observedSelectedDeviceIndex = selectedDeviceIndex;
                    m_observedBackendReady = backendReady;
                    resetSpectrumHolds();
                }
            });
        if (m_waterfall && m_paused &&
            m_applicationModel->scannerOwnsTuning()) {
            clearWaterfallFrames();
            m_waterfallClearedForScannerPause = true;
        }
    }
    frequencyAxisChanged();
    emit applicationModelChanged();
}

bool SpectrumWaterfallItem::waterfall() const noexcept
{
    return m_waterfall;
}

QString SpectrumWaterfallItem::waterfallPaletteName() const
{
    return QString::fromLatin1(sdr::gui::slopSpectrumPaletteName);
}

void SpectrumWaterfallItem::setWaterfall(bool waterfall)
{
    if (m_waterfall == waterfall) {
        return;
    }
    m_waterfall = waterfall;
    m_modeChanged = true;
    m_frameDirty = !m_latestFrame.normalizedMagnitudes.empty();
    m_projectionDirty = m_waterfallHistory.size() > 0;
    if (m_waterfall) {
        updateHistoryConfiguration(m_historySourceBins);
        scheduleRasterResize();
        if (!m_paused) {
            m_scrollTimer.start();
        }
    } else {
        m_scrollTimer.stop();
    }
    update();
    emit waterfallChanged();
}

bool SpectrumWaterfallItem::paused() const noexcept
{
    return m_paused;
}

void SpectrumWaterfallItem::setPaused(bool paused)
{
    if (m_paused == paused) {
        return;
    }
    m_paused = paused;
    if (m_waterfall) {
        if (m_paused) {
            m_scrollTimer.stop();
            if (m_applicationModel &&
                m_applicationModel->scannerOwnsTuning()) {
                clearWaterfallFrames();
                m_waterfallClearedForScannerPause = true;
            }
        } else {
            m_waterfallClearedForScannerPause = false;
            m_scrollTimer.start();
        }
    }
    emit pausedChanged();
}

float SpectrumWaterfallItem::spectrumMinimumDbfs() const noexcept
{
    return m_spectrumMinimumDbfs;
}

float SpectrumWaterfallItem::waterfallMinimumDbfs() const noexcept
{
    return m_waterfallMinimumDbfs;
}

void SpectrumWaterfallItem::setWaterfallMinimumDbfs(float minimumDbfs)
{
    if (!std::isfinite(minimumDbfs)) {
        return;
    }
    const float bounded = std::clamp(
        std::round(minimumDbfs),
        waterfallMinimumDbfsLimit,
        m_waterfallMaximumDbfs - waterfallMinimumSeparationDb);
    if (qFuzzyCompare(m_waterfallMinimumDbfs, bounded)) {
        return;
    }
    m_waterfallMinimumDbfs = bounded;
    QSettings().setValue(waterfallMinimumDbfsSetting, bounded);
    m_projectionDirty = true;
    update();
    emit waterfallRangeChanged();
}

float SpectrumWaterfallItem::waterfallMaximumDbfs() const noexcept
{
    return m_waterfallMaximumDbfs;
}

void SpectrumWaterfallItem::setWaterfallMaximumDbfs(float maximumDbfs)
{
    if (!std::isfinite(maximumDbfs)) {
        return;
    }
    const float bounded = std::clamp(
        std::round(maximumDbfs),
        m_waterfallMinimumDbfs + waterfallMinimumSeparationDb,
        waterfallMaximumDbfsLimit);
    if (qFuzzyCompare(m_waterfallMaximumDbfs, bounded)) {
        return;
    }
    m_waterfallMaximumDbfs = bounded;
    QSettings().setValue(waterfallMaximumDbfsSetting, bounded);
    m_projectionDirty = true;
    update();
    emit waterfallRangeChanged();
}

void SpectrumWaterfallItem::setSpectrumMinimumDbfs(float minimumDbfs)
{
    if (!std::isfinite(minimumDbfs) ||
        minimumDbfs >= m_spectrumMaximumDbfs - 1.0F ||
        qFuzzyCompare(m_spectrumMinimumDbfs, minimumDbfs)) {
        return;
    }
    m_spectrumMinimumDbfs = minimumDbfs;
    update();
    emit spectrumRangeChanged();
    emit noiseFloorChanged();
}

float SpectrumWaterfallItem::spectrumMaximumDbfs() const noexcept
{
    return m_spectrumMaximumDbfs;
}

void SpectrumWaterfallItem::setSpectrumMaximumDbfs(float maximumDbfs)
{
    if (!std::isfinite(maximumDbfs) ||
        maximumDbfs <= m_spectrumMinimumDbfs + 1.0F ||
        qFuzzyCompare(m_spectrumMaximumDbfs, maximumDbfs)) {
        return;
    }
    m_spectrumMaximumDbfs = maximumDbfs;
    update();
    emit spectrumRangeChanged();
    emit noiseFloorChanged();
}

QVariantList SpectrumWaterfallItem::majorDbfsTicks() const
{
    QVariantList ticks;
    for (const float tick : sdr::gui::majorDbfsTicks(
             m_spectrumMinimumDbfs, m_spectrumMaximumDbfs)) {
        ticks.push_back(tick);
    }
    return ticks;
}

QVariantList SpectrumWaterfallItem::minorDbfsTicks() const
{
    QVariantList ticks;
    for (const float tick : sdr::gui::minorDbfsTicks(
             m_spectrumMinimumDbfs, m_spectrumMaximumDbfs)) {
        ticks.push_back(tick);
    }
    return ticks;
}

float SpectrumWaterfallItem::noiseFloorDbfs() const noexcept
{
    return m_noiseFloorDbfs;
}

bool SpectrumWaterfallItem::noiseFloorAvailable() const noexcept
{
    return m_noiseFloorAvailable;
}

bool SpectrumWaterfallItem::maximumHoldEnabled() const noexcept
{
    return m_maximumHoldEnabled;
}

void SpectrumWaterfallItem::setMaximumHoldEnabled(bool enabled)
{
    if (m_maximumHoldEnabled == enabled) {
        return;
    }
    m_maximumHoldEnabled = enabled;
    QSettings().setValue(maximumHoldEnabledSetting, enabled);
    update();
    emit maximumHoldEnabledChanged();
}

bool SpectrumWaterfallItem::filterWidthAdjustmentActive() const noexcept
{
    return m_filterWidthAdjustmentActive;
}

void SpectrumWaterfallItem::setFilterWidthAdjustmentActive(bool active)
{
    if (m_filterWidthAdjustmentActive == active) {
        return;
    }
    m_filterWidthAdjustmentActive = active;
    update();
    emit filterWidthAdjustmentActiveChanged();
}

const QVector<float>& SpectrumWaterfallItem::maximumHoldDbfs() const noexcept
{
    return m_maximumHoldDbfs;
}

bool SpectrumWaterfallItem::spectrumHoldsAvailable() const noexcept
{
    return m_maximumHoldDbfs.size() >= 2;
}

QString SpectrumWaterfallItem::waterfallAggregation() const
{
    return m_waterfallAggregation == sdr::gui::WaterfallAggregation::Average
               ? QString::fromLatin1(waterfallAverageAggregation)
               : QString::fromLatin1(waterfallOriginalAggregation);
}

void SpectrumWaterfallItem::setWaterfallAggregation(const QString& aggregation)
{
    if (aggregation != QLatin1String(waterfallOriginalAggregation) &&
        aggregation != QLatin1String(waterfallAverageAggregation)) {
        return;
    }
    const auto parsed = aggregation == QLatin1String(waterfallAverageAggregation)
                            ? sdr::gui::WaterfallAggregation::Average
                            : sdr::gui::WaterfallAggregation::Original;
    if (parsed == m_waterfallAggregation) {
        return;
    }
    m_waterfallAggregation = parsed;
    QSettings().setValue(waterfallAggregationSetting, waterfallAggregation());
    m_projectedRowCache.clear();
    m_projectedViewportRowCache.clear();
    m_projectionDirty = m_waterfallHistory.size() > 0;
    update();
    emit waterfallAggregationChanged();
}

QRgb SpectrumWaterfallItem::waterfallColorForNormalizedMagnitude(
    float magnitude) const noexcept
{
    return sdr::gui::slopSpectrumColor(
        waterfallDbfs(magnitude), m_waterfallMinimumDbfs, m_waterfallMaximumDbfs);
}

QRgb SpectrumWaterfallItem::spectrumColorForDbfs(float dbfs) const noexcept
{
    return sdr::gui::slopSpectrumColor(
        dbfs, m_spectrumMinimumDbfs, m_spectrumMaximumDbfs);
}

QRgb SpectrumWaterfallItem::emptyWaterfallColor() const noexcept
{
    return sdr::gui::slopSpectrumColor(
        m_waterfallMinimumDbfs, m_waterfallMinimumDbfs, m_waterfallMaximumDbfs);
}

float SpectrumWaterfallItem::recommendedAmplitudeScaleMargin(
    float panelWidth,
    float devicePixelRatio) const noexcept
{
    return sdr::gui::amplitudeScaleMarginForPanel(panelWidth, devicePixelRatio);
}

float SpectrumWaterfallItem::yForDbfs(float dbfs, float displayHeight) const noexcept
{
    return sdr::gui::spectrumYForDbfs(
        dbfs,
        displayHeight,
        m_spectrumMinimumDbfs,
        m_spectrumMaximumDbfs);
}

float SpectrumWaterfallItem::xForFrequency(quint64 frequency) const noexcept
{
    return static_cast<float>(frequencyAxis()
                                  .positionForFrequency(
                                      static_cast<double>(frequency))
                                  .value_or(0.0));
}

QRectF SpectrumWaterfallItem::frequencyPlotRect() const noexcept
{
    return boundingRect();
}

std::uint64_t SpectrumWaterfallItem::waterfallReprojectionCount() const noexcept
{
    return m_waterfallReprojectionCount;
}

void SpectrumWaterfallItem::geometryChange(
    const QRectF& newGeometry,
    const QRectF& oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        scheduleRasterResize();
    }
}

void SpectrumWaterfallItem::scheduleRasterResize()
{
    if (!m_resizeCoalesceTimer.isActive()) {
        m_resizeCoalesceTimer.start();
    }
}

void SpectrumWaterfallItem::commitRasterResize()
{
    const qreal devicePixelRatio = window() ? window()->devicePixelRatio() : 1.0;
    const auto geometry = sdr::gui::waterfallRasterGeometry(
        width(),
        height(),
        devicePixelRatio,
        m_effectiveRowsPerSecond,
        m_visibleHistorySeconds);
    if (geometry.physicalWidth == m_rasterGeometry.physicalWidth &&
        geometry.visiblePixelRows == m_rasterGeometry.visiblePixelRows &&
        geometry.stagingPixelRows == m_rasterGeometry.stagingPixelRows) {
        return;
    }
    m_rasterGeometry = geometry;
    updateHistoryConfiguration();
    frequencyAxisChanged();
}

void SpectrumWaterfallItem::refreshRasterScreenConnection()
{
    QObject::disconnect(m_screenDpiConnection);
    if (window() && window()->screen()) {
        m_screenDpiConnection = connect(
            window()->screen(),
            &QScreen::physicalDotsPerInchChanged,
            this,
            &SpectrumWaterfallItem::scheduleRasterResize);
    }
    scheduleRasterResize();
}

quint64 SpectrumWaterfallItem::historyMemoryBudgetBytes() const noexcept
{
    return static_cast<quint64>(m_waterfallHistory.memoryBudgetBytes());
}

void SpectrumWaterfallItem::setHistoryMemoryBudgetBytes(quint64 bytes)
{
    if (bytes == 0 || bytes == historyMemoryBudgetBytes() ||
        bytes > static_cast<quint64>(std::numeric_limits<std::size_t>::max())) {
        return;
    }
    m_waterfallHistory.setMemoryBudgetBytes(static_cast<std::size_t>(bytes));
    if (!m_waterfall) {
        return;
    }
    updateHistoryConfiguration();
    m_projectionDirty = true;
    update();
    reportHistoryMetrics();
}

quint64 SpectrumWaterfallItem::viewportHistoryMemoryBudgetBytes() const noexcept
{
    return static_cast<quint64>(
        m_viewportWaterfallHistory.memoryBudgetBytes());
}

void SpectrumWaterfallItem::setViewportHistoryMemoryBudgetBytes(quint64 bytes)
{
    if (bytes == 0 || bytes == viewportHistoryMemoryBudgetBytes() ||
        bytes > static_cast<quint64>(std::numeric_limits<std::size_t>::max())) {
        return;
    }
    m_viewportWaterfallHistory.setMemoryBudgetBytes(
        static_cast<std::size_t>(bytes));
    m_projectedRowCache.clear();
    m_projectedViewportRowCache.clear();
    m_projectionDirty = m_waterfallHistory.size() > 0;
    update();
    reportHistoryMetrics();
}

double SpectrumWaterfallItem::effectiveRowsPerSecond() const noexcept
{
    return m_effectiveRowsPerSecond;
}

void SpectrumWaterfallItem::setEffectiveRowsPerSecond(double rowsPerSecond)
{
    if (!std::isfinite(rowsPerSecond) || rowsPerSecond <= 0.0 ||
        qFuzzyCompare(rowsPerSecond, m_effectiveRowsPerSecond)) {
        return;
    }
    m_effectiveRowsPerSecond = rowsPerSecond;
    if (!m_waterfall) {
        return;
    }
    m_projectionDirty = true;
    scheduleRasterResize();
    updateHistoryConfiguration(m_historySourceBins);
    update();
    reportHistoryMetrics();
}

double SpectrumWaterfallItem::visibleHistorySeconds() const noexcept
{
    return m_visibleHistorySeconds;
}

void SpectrumWaterfallItem::setVisibleHistorySeconds(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 1.0 ||
        qFuzzyCompare(seconds + 1.0, m_visibleHistorySeconds + 1.0)) {
        return;
    }
    const double previousSeconds = m_visibleHistorySeconds;
    const std::uint64_t now = steadyTimestampNanoseconds();
    double fractionalPhase = 0.0;
    if (m_renderClockOriginNanoseconds != 0 &&
        m_rasterGeometry.visiblePixelRows > 0) {
        const std::uint64_t currentRenderTimestamp =
            sdr::gui::clampWaterfallRenderTimestamp(
                sdr::gui::waterfallRenderTimestamp(
                    m_initialRenderTimestampNanoseconds,
                    m_renderClockOriginNanoseconds,
                    now),
                m_waterfallHistory.rows());
        const std::uint64_t elapsed =
            currentRenderTimestamp -
            std::min(
                currentRenderTimestamp,
                m_initialRenderTimestampNanoseconds);
        const double scrollPixels = sdr::gui::waterfallFractionalScrollPixels(
            elapsed,
            static_cast<double>(previousSeconds),
            static_cast<double>(m_rasterGeometry.visiblePixelRows));
        fractionalPhase = scrollPixels - std::floor(scrollPixels);
    }
    m_visibleHistorySeconds = seconds;
    m_retainedHistoryDurationSeconds =
        m_waterfallHistory.size() == 0
            ? static_cast<double>(seconds)
            : std::max(
                  m_retainedHistoryDurationSeconds,
                  static_cast<double>(seconds));
    if (!m_waterfall) {
        return;
    }
    m_resizeCoalesceTimer.stop();
    const qreal devicePixelRatio = window() ? window()->devicePixelRatio() : 1.0;
    m_rasterGeometry = sdr::gui::waterfallRasterGeometry(
        width(),
        height(),
        devicePixelRatio,
        m_effectiveRowsPerSecond,
        m_visibleHistorySeconds);
    updateHistoryConfiguration();
    rebaseWaterfallRenderAnchor(fractionalPhase, now);
    m_projectionDirty = m_waterfallHistory.size() > 0;
    update();
    reportHistoryMetrics();
}

quint64 SpectrumWaterfallItem::historyMemoryUsageBytes() const noexcept
{
    return static_cast<quint64>(m_waterfallHistory.memoryUsageBytes());
}

quint64 SpectrumWaterfallItem::viewportHistoryMemoryUsageBytes() const noexcept
{
    return static_cast<quint64>(
        m_viewportWaterfallHistory.memoryUsageBytes());
}

quint64 SpectrumWaterfallItem::storedHistoryBins() const noexcept
{
    return static_cast<quint64>(m_historyPlan.storedBins);
}

double SpectrumWaterfallItem::retainedHistoryCapacitySeconds() const noexcept
{
    return m_historyPlan.retainedCapacitySeconds;
}

double SpectrumWaterfallItem::retainedHistorySeconds() const noexcept
{
    return m_waterfallHistory.retainedDurationSeconds();
}

bool SpectrumWaterfallItem::historyConfigurationFitsMemoryBudget() const noexcept
{
    return m_historySourceBins < 2 || m_historyPlan.fitsMemoryBudget;
}

QSGNode* SpectrumWaterfallItem::updatePaintNode(
    QSGNode* oldNode, UpdatePaintNodeData*)
{
    auto* node = static_cast<SpectrumWaterfallRenderNode*>(oldNode);
    if (!node) {
        node = new SpectrumWaterfallRenderNode;
    }
    if (m_modeChanged) {
        node->setContent(nullptr);
        m_modeChanged = false;
    }
    QSGNode* content = m_waterfall ? updateWaterfallNode(node->content())
                                   : updateSpectrumNode(node->content());
    node->setContent(content);
    updateFilterIndicatorNode(node->filterIndicator());
    return node;
}

void SpectrumWaterfallItem::receiveFrame(
    const QVector<float>& normalizedMagnitudes)
{
    if (normalizedMagnitudes.size() < 2) {
        return;
    }
    const quint64 centerFrequency = m_applicationModel
                                        ? m_applicationModel->centerFrequency()
                                        : 100'000'000;
    const quint64 sampleRate = m_applicationModel
                                   ? m_applicationModel->effectiveSampleRate()
                                   : 2'000'000;
    if (m_waterfall && !m_paused) {
        receiveWaterfallFrame(
            normalizedMagnitudes,
            centerFrequency,
            sampleRate,
            static_cast<quint64>(normalizedMagnitudes.size()),
            m_fallbackSequence++,
            steadyTimestampNanoseconds(),
            0);
    } else {
        receiveSpectrumFrame(
            normalizedMagnitudes,
            centerFrequency,
            sampleRate,
            static_cast<quint64>(normalizedMagnitudes.size()),
            m_fallbackSequence++,
            steadyTimestampNanoseconds(),
            0);
    }
}

void SpectrumWaterfallItem::receiveSpectrumFrame(
    const QVector<float>& normalizedMagnitudes,
    quint64 centerFrequency,
    quint64 sampleRate,
    quint64 fftSize,
    quint64 sequence,
    quint64 timestampNanoseconds,
    quint64 tuningGeneration)
{
    if (!m_waterfall && !m_paused) {
        updateSpectrumHolds(
            normalizedMagnitudes,
            centerFrequency,
            sampleRate,
            fftSize,
            sequence,
            timestampNanoseconds);
        setLatestFrame(
            normalizedMagnitudes,
            centerFrequency,
            sampleRate,
            fftSize,
            sequence,
            timestampNanoseconds,
            tuningGeneration);
        if (!sdr::radio::hasConsistentMetadata(m_latestFrame)) {
            return;
        }
        updateNoiseFloor();
        m_frameDirty = true;
        update();
    }
}

void SpectrumWaterfallItem::receiveWaterfallFrame(
    const QVector<float>& normalizedMagnitudes,
    quint64 centerFrequency,
    quint64 sampleRate,
    quint64 fftSize,
    quint64 sequence,
    quint64 timestampNanoseconds,
    quint64 tuningGeneration)
{
    if (m_waterfall && !m_paused) {
        setLatestFrame(
            normalizedMagnitudes,
            centerFrequency,
            sampleRate,
            fftSize,
            sequence,
            timestampNanoseconds,
            tuningGeneration);
        updateHistoryConfiguration(m_latestFrame.fftSize);
        synchronizeWaterfallViewport(m_latestFrame);
        if (!m_waterfallHistory.append(m_latestFrame)) {
            return;
        }
        if (auto viewportRow =
                sdr::gui::projectFrameToWaterfallViewport(
                    m_latestFrame, m_waterfallViewport);
            viewportRow.has_value()) {
            [[maybe_unused]] const bool cached =
                m_viewportWaterfallHistory.append(
                    std::move(*viewportRow));
        }
        m_pendingWaterfallRows = std::min(
            static_cast<int>(m_waterfallHistory.capacity()),
            m_pendingWaterfallRows + 1);
        if (m_waterfallImage.isNull()) {
            m_projectionDirty = true;
        }
        update();
        reportHistoryMetrics();
    }
}

void SpectrumWaterfallItem::setLatestFrame(
    const QVector<float>& normalizedMagnitudes,
    quint64 centerFrequency,
    quint64 sampleRate,
    quint64 fftSize,
    quint64 sequence,
    quint64 timestampNanoseconds,
    quint64 tuningGeneration)
{
    m_latestFrame = {
        .sequence = sequence,
        .timestampNanoseconds = timestampNanoseconds,
        .centerFrequency = centerFrequency,
        .sampleRate = sampleRate,
        .captureSpan = sampleRate,
        .fftSize = static_cast<std::size_t>(fftSize),
        .tuningGeneration = tuningGeneration,
        .normalizedMagnitudes = std::vector<float>(
            normalizedMagnitudes.begin(), normalizedMagnitudes.end()),
    };
}

bool SpectrumWaterfallItem::frameMatchesCurrentSpectrumGeometry(
    quint64 centerFrequency,
    quint64 sampleRate,
    quint64 fftSize) const noexcept
{
    return !m_applicationModel ||
           (centerFrequency == m_applicationModel->centerFrequency() &&
            sampleRate == m_applicationModel->effectiveSampleRate() &&
            fftSize == m_applicationModel->effectiveSpectrumFftSize());
}

void SpectrumWaterfallItem::updateSpectrumHolds(
    const QVector<float>& normalizedMagnitudes,
    quint64 centerFrequency,
    quint64 sampleRate,
    quint64 fftSize,
    quint64 sequence,
    quint64 timestampNanoseconds)
{
    if (m_waterfall || normalizedMagnitudes.size() < 2 ||
        fftSize != static_cast<quint64>(normalizedMagnitudes.size()) ||
        !frameMatchesCurrentSpectrumGeometry(
            centerFrequency, sampleRate, fftSize)) {
        return;
    }

    const bool compatible =
        spectrumHoldsAvailable() &&
        m_holdCenterFrequency == centerFrequency &&
        m_holdSampleRate == sampleRate &&
        m_holdFftSize == fftSize;
    if (!compatible) {
        resetSpectrumHolds();
        m_maximumHoldDbfs.resize(normalizedMagnitudes.size());
        for (qsizetype index = 0; index < normalizedMagnitudes.size(); ++index) {
            const float dbfs = sdr::gui::dbfsForNormalizedSpectrum(
                normalizedMagnitudes[index]);
            m_maximumHoldDbfs[index] = dbfs;
        }
        m_holdCenterFrequency = centerFrequency;
        m_holdSampleRate = sampleRate;
        m_holdFftSize = fftSize;
        m_holdLastSequence = sequence;
        m_holdLastTimestampNanoseconds = timestampNanoseconds;
        m_holdProjectionDirty = true;
        return;
    }

    const bool sequenceIsNewer =
        sequence > m_holdLastSequence;
    const bool timestampIsNewer =
        timestampNanoseconds == 0 ||
        m_holdLastTimestampNanoseconds == 0 ||
        timestampNanoseconds > m_holdLastTimestampNanoseconds;
    if (!sequenceIsNewer || !timestampIsNewer) {
        return;
    }

    for (qsizetype index = 0; index < normalizedMagnitudes.size(); ++index) {
        const float dbfs = sdr::gui::dbfsForNormalizedSpectrum(
            normalizedMagnitudes[index]);
        m_maximumHoldDbfs[index] =
            std::max(m_maximumHoldDbfs[index], dbfs);
    }
    m_holdLastSequence = sequence;
    m_holdLastTimestampNanoseconds = timestampNanoseconds;
    m_holdProjectionDirty = true;
}

void SpectrumWaterfallItem::clearSpectrumFrame()
{
    if (m_waterfall) {
        return;
    }
    resetSpectrumHolds();
    m_latestFrame = {};
    m_noiseScratch.clear();
    m_frameDirty = false;
    if (m_noiseFloorAvailable) {
        m_noiseFloorAvailable = false;
        emit noiseFloorChanged();
    }
    update();
}

void SpectrumWaterfallItem::resetSpectrumHolds()
{
    if (m_waterfall) {
        return;
    }
    m_maximumHoldDbfs.clear();
    m_projectedMaximumHoldDbfs.clear();
    m_holdCenterFrequency = 0;
    m_holdSampleRate = 0;
    m_holdFftSize = 0;
    m_holdLastSequence = 0;
    m_holdLastTimestampNanoseconds = 0;
    m_holdProjectionDirty = false;
    update();
}

void SpectrumWaterfallItem::clearWaterfallFrames()
{
    if (!m_waterfall) {
        return;
    }
    m_latestFrame = {};
    m_waterfallHistory.clear();
    m_viewportWaterfallHistory.clear();
    m_waterfallImage = {};
    m_projectionDirty = false;
    m_pendingWaterfallRows = 0;
    m_projectedRowCache.clear();
    m_projectedViewportRowCache.clear();
    m_retainedWaterfallSequences.clear();
    m_matchingViewportRows.clear();
    invalidateWaterfallViewport();
    m_completedWaterfallViewportGeneration = 0;
    m_renderClockOriginNanoseconds = 0;
    m_initialRenderTimestampNanoseconds = 0;
    m_retainedHistoryDurationSeconds =
        static_cast<double>(m_visibleHistorySeconds);
    m_historyPlan = {};
    m_scrollRasterDirty = false;
    ++m_scrollPhaseResets;
    update();
    reportHistoryMetrics();
}

void SpectrumWaterfallItem::frequencyAxisChanged()
{
    if (m_waterfall) {
        if (m_projectionDirty) {
            ++m_staleReprojectionsDiscarded;
        }
        invalidateWaterfallViewport();
        if (sdr::radio::hasConsistentMetadata(m_latestFrame)) {
            m_waterfallViewport = currentWaterfallViewport(m_latestFrame);
        }
    } else if (spectrumHoldsAvailable()) {
        m_holdProjectionDirty = true;
    }
    update();
}

QSGNode* SpectrumWaterfallItem::updateSpectrumNode(QSGNode* oldNode)
{
    auto* node = static_cast<SpectrumNode*>(oldNode);
    if (!sdr::radio::hasConsistentMetadata(m_latestFrame)) {
        return nullptr;
    }

    if (!node) {
        node = new SpectrumNode;
    }
    node->ensurePaletteTexture(window());

    const QVector<float> frame = displayFrame(m_latestFrame);
    const int frameSize = static_cast<int>(frame.size());
    const float itemWidth = static_cast<float>(width());
    const float itemHeight = static_cast<float>(height());
    QSGGeometry::updateRectGeometry(
        node->background()->geometry(), QRectF(0.0, 0.0, itemWidth, itemHeight));

    auto* fillGeometry = node->fill()->geometry();
    if (fillGeometry->vertexCount() != frameSize * 2) {
        fillGeometry->allocate(frameSize * 2);
    }
    auto* fillVertices = fillGeometry->vertexDataAsTexturedPoint2D();

    auto* traceGeometry = node->trace()->geometry();
    if (traceGeometry->vertexCount() != frameSize) {
        traceGeometry->allocate(frameSize);
    }
    auto* traceVertices = traceGeometry->vertexDataAsPoint2D();

    const float divisor = static_cast<float>(frameSize - 1);
    const float bottomPaletteCoordinate = paletteTextureCoordinate(
        sdr::gui::waterfallPaletteIndex(
            m_spectrumMinimumDbfs,
            m_spectrumMinimumDbfs,
            m_spectrumMaximumDbfs));
    for (int index = 0; index < frameSize; ++index) {
        const float x = static_cast<float>(index) * itemWidth / divisor;
        const float y =
            (1.0F - spectrumDisplayLevel(frame[index])) * itemHeight;
        const float tracePaletteCoordinate = paletteTextureCoordinate(
            sdr::gui::waterfallPaletteIndex(
                sdr::gui::dbfsForNormalizedSpectrum(frame[index]),
                m_spectrumMinimumDbfs,
                m_spectrumMaximumDbfs));
        auto& top = fillVertices[index * 2];
        top.set(x, y, tracePaletteCoordinate, 0.5F);
        auto& bottom = fillVertices[index * 2 + 1];
        bottom.set(x, itemHeight, bottomPaletteCoordinate, 0.5F);
        traceVertices[index].set(x, y);
    }
    node->background()->markDirty(QSGNode::DirtyGeometry);
    node->fill()->markDirty(QSGNode::DirtyGeometry);
    node->trace()->markDirty(QSGNode::DirtyGeometry);
    if (m_holdProjectionDirty) {
        projectSpectrumHolds();
    }
    updateHoldStrokeGeometry(
        *node->maximumHoldUnderStroke(),
        *node->maximumHoldWhiteStroke(),
        m_projectedMaximumHoldDbfs,
        m_maximumHoldEnabled && spectrumHoldsAvailable(),
        itemWidth,
        itemHeight,
        m_spectrumMinimumDbfs,
        m_spectrumMaximumDbfs);
    m_frameDirty = false;
    return node;
}

void SpectrumWaterfallItem::updateFilterIndicatorNode(QSGNode* node)
{
    auto* filterIndicator = static_cast<FilterIndicatorNode*>(node);
    if (!m_applicationModel || !window()) {
        filterIndicator->hide();
        return;
    }
    const float dpr = static_cast<float>(window()->devicePixelRatio());
    const auto gate = sdr::gui::filterGate(
        frequencyAxis(),
        m_applicationModel->filterLowerFrequency(),
        m_applicationModel->listeningFrequency(),
        m_applicationModel->filterUpperFrequency(),
        static_cast<float>(height()), dpr, m_filterWidthAdjustmentActive);
    filterIndicator->update(gate, static_cast<float>(height()), dpr);
}

QSGNode* SpectrumWaterfallItem::updateWaterfallNode(QSGNode* oldNode)
{
    auto* node = static_cast<WaterfallTextureNode*>(oldNode);
    if (!window()) {
        return node;
    }

    ++m_renderedFrames;

    const qreal devicePixelRatio = window()->devicePixelRatio();
    if (m_rasterGeometry.physicalWidth == 0 ||
        m_rasterGeometry.visiblePixelRows == 0) {
        m_rasterGeometry = sdr::gui::waterfallRasterGeometry(
            width(),
            height(),
            devicePixelRatio,
            m_effectiveRowsPerSecond,
            m_visibleHistorySeconds);
        m_projectionDirty = true;
    }
    if (sdr::radio::hasConsistentMetadata(m_latestFrame)) {
        synchronizeWaterfallViewport(m_latestFrame);
    }
    const auto geometry = m_rasterGeometry;
    if (m_waterfallImage.width() !=
            static_cast<int>(geometry.physicalWidth) ||
        m_waterfallImage.height() !=
            static_cast<int>(geometry.visiblePixelRows)) {
        m_projectionDirty = true;
    }
    const std::uint64_t now = steadyTimestampNanoseconds();
    if (m_renderClockOriginNanoseconds == 0 &&
        !m_waterfallHistory.rows().empty()) {
        m_renderClockOriginNanoseconds = now;
        m_initialRenderTimestampNanoseconds =
            m_waterfallHistory.rows().front().timestampNanoseconds;
        ++m_scrollPhaseResets;
    }
    const std::uint64_t renderTimestamp =
        sdr::gui::clampWaterfallRenderTimestamp(
            sdr::gui::waterfallRenderTimestamp(
                m_initialRenderTimestampNanoseconds,
                m_renderClockOriginNanoseconds,
                now),
            m_waterfallHistory.rows());
    bool textureNeedsUpdate = m_projectionDirty ||
                              m_pendingWaterfallRows > 0 ||
                              m_scrollRasterDirty;
    if (!node) {
        node = new WaterfallTextureNode;
        textureNeedsUpdate = true;
    }

    if (textureNeedsUpdate) {
        if (rebuildWaterfallImage(renderTimestamp, geometry)) {
            auto* texture = window()->createTextureFromImage(m_waterfallImage);
            texture->setFiltering(QSGTexture::Nearest);
            node->setTexture(texture);
            if (m_pendingWaterfallRows > 1) {
                m_mergedRenderUpdates +=
                    static_cast<std::uint64_t>(m_pendingWaterfallRows - 1);
            }
            m_pendingWaterfallRows = 0;
            m_projectionDirty = false;
            m_scrollRasterDirty = false;
        }
    }
    const QRectF rasterRect(
        0.0,
        0.0,
        width(),
        height());
    node->setSourceRect(QRectF(
        0.0,
        0.0,
        static_cast<qreal>(geometry.physicalWidth),
        static_cast<qreal>(geometry.visiblePixelRows)));
    node->setRect(rasterRect);
    const std::uint64_t renderFrameTimestamp = steadyTimestampNanoseconds();
    if (m_lastRenderFrameTimestampNanoseconds != 0) {
        m_lastRenderFrameIntervalNanoseconds =
            renderFrameTimestamp -
            std::min(
                renderFrameTimestamp,
                m_lastRenderFrameTimestampNanoseconds);
    }
    m_lastRenderFrameTimestampNanoseconds = renderFrameTimestamp;
    const std::uint64_t clockElapsed =
        renderFrameTimestamp -
        std::min(renderFrameTimestamp, m_renderClockOriginNanoseconds);
    const double scrollPixels = sdr::gui::waterfallFractionalScrollPixels(
        clockElapsed,
        m_visibleHistorySeconds,
        static_cast<double>(geometry.visiblePixelRows));
    reportScrollDiagnostics(static_cast<float>(
        scrollPixels - std::floor(scrollPixels)));
    return node;
}

bool SpectrumWaterfallItem::rebuildWaterfallImage(
    std::uint64_t mappingAnchorTimestampNanoseconds,
    const sdr::gui::WaterfallRasterGeometry& geometry)
{
    QElapsedTimer reprojectionTimer;
    reprojectionTimer.start();
    ++m_waterfallReprojectionCount;
    const int columns = static_cast<int>(geometry.physicalWidth);
    if (columns != m_cachedProjectionColumns) {
        m_projectedRowCache.clear();
        m_projectedViewportRowCache.clear();
        m_cachedProjectionColumns = columns;
    }
    const int pixelRows = static_cast<int>(geometry.visiblePixelRows);
    m_stagingPixelRows = static_cast<int>(geometry.stagingPixelRows);
    const auto viewport = m_waterfallViewport;
    const bool buildReplacement =
        m_projectionDirty ||
        m_completedWaterfallViewportGeneration != viewport.generation ||
        m_waterfallImage.width() != columns ||
        m_waterfallImage.height() != pixelRows;
    QImage replacement;
    QImage* raster = &m_waterfallImage;
    if (buildReplacement) {
        replacement = QImage(columns, pixelRows, QImage::Format_RGB32);
        if (replacement.isNull()) {
            m_lastReprojectionMilliseconds =
                static_cast<double>(reprojectionTimer.nsecsElapsed()) /
                1'000'000.0;
            return false;
        }
        raster = &replacement;
    }
    const QRgb emptyColor = emptyWaterfallColor();
    raster->fill(emptyColor);
    const auto mapping = sdr::gui::mapWaterfallRowsToPixels(
        m_waterfallHistory.rows(),
        mappingAnchorTimestampNanoseconds,
        m_visibleHistorySeconds,
        static_cast<std::size_t>(pixelRows));
    m_retainedWaterfallSequences.clear();
    m_retainedWaterfallSequences.reserve(m_waterfallHistory.size());
    for (const auto& row : m_waterfallHistory.rows()) {
        m_retainedWaterfallSequences.insert(row.sequence);
    }
    std::erase_if(m_projectedRowCache, [this](const auto& entry) {
        return !m_retainedWaterfallSequences.contains(entry.first);
    });
    std::erase_if(
        m_projectedViewportRowCache,
        [this](const auto& entry) {
            return !m_retainedWaterfallSequences.contains(entry.first);
        });
    m_matchingViewportRows.clear();
    m_matchingViewportRows.reserve(m_viewportWaterfallHistory.size());
    for (const auto& row : m_viewportWaterfallHistory.rows()) {
        if (sdr::gui::viewportWaterfallRowMatches(row, viewport)) {
            m_matchingViewportRows.try_emplace(row.sequence, &row);
        }
    }
    auto matchingViewportRow =
        [this](
            std::size_t index)
        -> const sdr::gui::ViewportWaterfallHistoryRow* {
        const auto& compact = m_waterfallHistory.rows()[index];
        const auto found = m_matchingViewportRows.find(compact.sequence);
        if (found == m_matchingViewportRows.end() ||
            found->second->timestampNanoseconds !=
                compact.timestampNanoseconds) {
            return nullptr;
        }
        return found->second;
    };
    auto projectedRow =
        [this, &matchingViewportRow](
            std::size_t index,
            bool useViewportHistory) -> const QVector<float>& {
        const auto& source = m_waterfallHistory.rows()[index];
        auto& cache = useViewportHistory
                          ? m_projectedViewportRowCache
                          : m_projectedRowCache;
        auto [entry, inserted] = cache.try_emplace(source.sequence);
        if (inserted || entry->second.isEmpty()) {
            if (useViewportHistory) {
                entry->second = displayFrame(*matchingViewportRow(index));
            } else {
                entry->second = displayFrame(source);
            }
        }
        return entry->second;
    };
    m_lastHighResolutionRasterRows = 0;
    m_lastCompactRasterRows = 0;
    for (int rowIndex = 0; rowIndex < pixelRows; ++rowIndex) {
        const auto& sample = mapping[static_cast<std::size_t>(rowIndex)];
        if (!sample.hasData) {
            continue;
        }
        bool useViewportHistory = true;
        if (sample.reduce) {
            for (std::size_t source = sample.firstRow;
                 source <= sample.lastRow;
                 ++source) {
                if (!matchingViewportRow(source)) {
                    useViewportHistory = false;
                    break;
                }
            }
        } else if (sample.interpolate) {
            useViewportHistory =
                matchingViewportRow(sample.newerRow) &&
                matchingViewportRow(sample.olderRow);
        } else {
            useViewportHistory =
                matchingViewportRow(sample.firstRow) != nullptr;
        }
        if (useViewportHistory) {
            ++m_lastHighResolutionRasterRows;
        } else {
            ++m_lastCompactRasterRows;
        }
        QVector<float> projected;
        if (sample.reduce) {
            if (m_waterfallAggregation ==
                sdr::gui::WaterfallAggregation::Original) {
                projected =
                    projectedRow(sample.firstRow, useViewportHistory);
                for (std::size_t source = sample.firstRow + 1;
                     source <= sample.lastRow;
                     ++source) {
                    const QVector<float>& additional =
                        projectedRow(source, useViewportHistory);
                    sdr::gui::combineWaterfallFrames(
                        {projected.data(), static_cast<std::size_t>(projected.size())},
                        {additional.data(), static_cast<std::size_t>(additional.size())},
                        m_waterfallAggregation);
                }
            } else {
                const QVector<float>& first =
                    projectedRow(sample.firstRow, useViewportHistory);
                projected.fill(0.0F, first.size());
                double totalWeight = 0.0;
                // Average smooths each timestamp interval in linear power.
                // Nearest-sample time weights keep brightness stable when FFT
                // arrivals jitter; dB conversion happens only after this sum.
                for (std::size_t source = sample.firstRow;
                     source <= sample.lastRow;
                     ++source) {
                    const double weight =
                        sdr::gui::waterfallTemporalWeightNanoseconds(
                            m_waterfallHistory.rows(),
                            source,
                            mappingAnchorTimestampNanoseconds,
                            sample.intervalStartAgeNanoseconds,
                            sample.intervalEndAgeNanoseconds);
                    if (weight <= 0.0) {
                        continue;
                    }
                    const QVector<float>& additional =
                        projectedRow(source, useViewportHistory);
                    for (qsizetype column = 0;
                         column < projected.size();
                         ++column) {
                        projected[column] += static_cast<float>(
                            static_cast<double>(additional[column]) *
                            weight);
                    }
                    totalWeight += weight;
                }
                if (totalWeight > 0.0) {
                    const float reciprocal =
                        static_cast<float>(1.0 / totalWeight);
                    for (float& power : projected) {
                        power *= reciprocal;
                    }
                } else {
                    projected = first;
                }
            }
        } else if (!sample.interpolate) {
            projected =
                projectedRow(sample.firstRow, useViewportHistory);
        } else {
            const QVector<float>& newer =
                projectedRow(sample.newerRow, useViewportHistory);
            const QVector<float>& older =
                projectedRow(sample.olderRow, useViewportHistory);
            projected.resize(newer.size());
            for (qsizetype column = 0; column < newer.size(); ++column) {
                projected[column] = std::lerp(
                    newer[column], older[column], sample.interpolation);
            }
        }
        auto* scanLine = reinterpret_cast<QRgb*>(
            raster->scanLine(rowIndex));
        for (int column = 0; column < projected.size(); ++column) {
            scanLine[column] =
                m_waterfallAggregation ==
                        sdr::gui::WaterfallAggregation::Original
                    ? waterfallColorForNormalizedMagnitude(projected[column])
                    : sdr::gui::slopSpectrumColor(
                          waterfallDbfsForLinearPower(projected[column]),
                          m_waterfallMinimumDbfs,
                          m_waterfallMaximumDbfs);
        }
    }
    if (!replacement.isNull()) {
        m_waterfallImage = std::move(replacement);
    }
    m_completedWaterfallViewportGeneration = viewport.generation;
    m_lastReprojectionMilliseconds =
        static_cast<double>(reprojectionTimer.nsecsElapsed()) / 1'000'000.0;
    return true;
}

void SpectrumWaterfallItem::reportScrollDiagnostics(float verticalPhase)
{
    if (!m_applicationModel ||
        !m_applicationModel->verboseDiagnosticsEnabled()) {
        return;
    }
    if (m_scrollDiagnosticsTimer.isValid() &&
        m_scrollDiagnosticsTimer.elapsed() < 1'000) {
        return;
    }
    qInfo().noquote()
        << QStringLiteral(
               "waterfall scroll: vertical-phase=%1 raster-rebuilds=%2 rebuild=%3 ms phase-resets=%4 render-frame-interval=%5 ms raster=%6x%7+%8x2")
               .arg(verticalPhase, 0, 'f', 3)
               .arg(m_waterfallReprojectionCount)
               .arg(m_lastReprojectionMilliseconds, 0, 'f', 2)
               .arg(m_scrollPhaseResets)
               .arg(
                   static_cast<double>(
                       m_lastRenderFrameIntervalNanoseconds) /
                       1'000'000.0,
                   0,
                   'f',
                   3)
               .arg(m_rasterGeometry.physicalWidth)
               .arg(m_rasterGeometry.visiblePixelRows)
               .arg(m_rasterGeometry.stagingPixelRows);
    if (m_scrollDiagnosticsTimer.isValid()) {
        m_scrollDiagnosticsTimer.restart();
    } else {
        m_scrollDiagnosticsTimer.start();
    }
}

void SpectrumWaterfallItem::rebaseWaterfallRenderAnchor(
    double fractionalPhase,
    std::uint64_t nowNanoseconds)
{
    if (m_waterfallHistory.rows().empty() ||
        m_rasterGeometry.visiblePixelRows == 0) {
        m_renderClockOriginNanoseconds = 0;
        m_initialRenderTimestampNanoseconds = 0;
        return;
    }

    const long double nanosecondsPerPixel =
        static_cast<long double>(m_visibleHistorySeconds) * 1'000'000'000.0L /
        static_cast<long double>(m_rasterGeometry.visiblePixelRows);
    const std::uint64_t phaseLead = static_cast<std::uint64_t>(std::llround(
        std::max(
            0.0L,
            static_cast<long double>(fractionalPhase) *
                nanosecondsPerPixel)));
    const std::uint64_t observedInterval =
        sdr::gui::observedWaterfallRowIntervalNanoseconds(
            m_waterfallHistory.rows());
    const std::uint64_t boundedLead =
        std::min(phaseLead, observedInterval);
    const std::uint64_t newest =
        m_waterfallHistory.rows().front().timestampNanoseconds;
    m_initialRenderTimestampNanoseconds =
        boundedLead > std::numeric_limits<std::uint64_t>::max() - newest
            ? std::numeric_limits<std::uint64_t>::max()
            : newest + boundedLead;
    m_renderClockOriginNanoseconds = nowNanoseconds;
    ++m_scrollPhaseResets;
}

void SpectrumWaterfallItem::updateHistoryConfiguration(std::size_t sourceBins)
{
    if (sourceBins >= 2) {
        m_historySourceBins = sourceBins;
    }
    if (m_historySourceBins < 2) {
        return;
    }
    const qreal devicePixelRatio = window() ? window()->devicePixelRatio() : 1.0;
    const auto geometry = m_rasterGeometry.physicalWidth > 0
                              ? m_rasterGeometry
                              : sdr::gui::waterfallRasterGeometry(
                                    width(),
                                    height(),
                                    devicePixelRatio,
                                    m_effectiveRowsPerSecond,
                                    m_visibleHistorySeconds);
    const std::size_t physicalWidth = geometry.physicalWidth;
    const std::size_t physicalHeight = geometry.visiblePixelRows;
    const auto plan = sdr::gui::selectWaterfallHistoryPlan(
        m_historySourceBins,
        physicalWidth,
        physicalHeight,
        m_effectiveRowsPerSecond,
        m_retainedHistoryDurationSeconds,
        m_waterfallHistory.memoryBudgetBytes());
    if (plan.storedBins < 2 || plan.requiredRows == 0) {
        return;
    }
    const bool changed =
        plan.storedBins != m_historyPlan.storedBins ||
        plan.requiredRows != m_historyPlan.requiredRows ||
        plan.fitsMemoryBudget != m_historyPlan.fitsMemoryBudget ||
        plan.retainedCapacitySeconds != m_historyPlan.retainedCapacitySeconds;
    m_historyPlan = plan;
    m_waterfallHistory.setStoredBinCount(plan.storedBins);
    m_waterfallHistory.setCapacity(std::max({
        std::size_t{2},
        plan.maximumRowsWithinBudget,
        m_waterfallHistory.size(),
    }));
    const std::size_t stagingRows =
        sdr::gui::waterfallStagingPixelRows(
            physicalHeight,
            m_effectiveRowsPerSecond,
            m_retainedHistoryDurationSeconds);
    const double stagingSeconds =
        static_cast<double>(stagingRows) *
        m_retainedHistoryDurationSeconds /
        static_cast<double>(physicalHeight);
    m_waterfallHistory.setRetentionDurationSeconds(
        m_retainedHistoryDurationSeconds + stagingSeconds);
    m_viewportWaterfallHistory.setCapacity(std::max(
        std::size_t{2},
        std::min(
            sdr::gui::viewportWaterfallHistoryMaximumRows,
            plan.requiredRows)));
    if (changed) {
        reportHistoryMetrics();
    }
}

sdr::gui::WaterfallViewportDescriptor
SpectrumWaterfallItem::currentWaterfallViewport(
    const sdr::radio::SpectrumFrame& capture) const noexcept
{
    const auto axis = frequencyAxis();
    const qreal devicePixelRatio =
        window() ? window()->devicePixelRatio() : 1.0;
    const std::size_t physicalWidth =
        m_rasterGeometry.physicalWidth > 0
            ? m_rasterGeometry.physicalWidth
            : sdr::gui::displayColumnCountForWidth(
                  width(), devicePixelRatio);
    return {
        .generation = m_waterfallViewportGeneration,
        .visibleRange = axis.valid()
                            ? axis.visibleRange()
                            : sdr::radio::FrequencyRange{},
        .physicalWidth = physicalWidth,
        .devicePixelRatio = static_cast<double>(devicePixelRatio),
        .captureCenterFrequency = capture.centerFrequency,
        .captureSampleRate = capture.sampleRate,
        .captureSpan = sdr::radio::captureSpan(capture),
        .captureFftSize = capture.fftSize,
        .tuningGeneration = capture.tuningGeneration,
    };
}

void SpectrumWaterfallItem::synchronizeWaterfallViewport(
    const sdr::radio::SpectrumFrame& capture)
{
    auto candidate = currentWaterfallViewport(capture);
    if (m_waterfallViewport.generation != 0 &&
        !sdr::gui::sameWaterfallViewport(
            m_waterfallViewport, candidate)) {
        invalidateWaterfallViewport();
        candidate = currentWaterfallViewport(capture);
    }
    m_waterfallViewport = candidate;
}

void SpectrumWaterfallItem::invalidateWaterfallViewport()
{
    if (m_waterfallViewportGeneration ==
        std::numeric_limits<std::uint64_t>::max()) {
        m_waterfallViewportGeneration = 1;
    } else {
        ++m_waterfallViewportGeneration;
    }
    m_waterfallViewport = {};
    m_projectedRowCache.clear();
    m_projectedViewportRowCache.clear();
    m_matchingViewportRows.clear();
    m_projectionDirty = m_waterfallHistory.size() > 0;
}

QVector<float> SpectrumWaterfallItem::displayFrame(
    const sdr::radio::SpectrumFrame& frame) const
{
    const auto projected = sdr::gui::projectFrameToFrequencyAxis(
        frame,
        frequencyAxis(),
        static_cast<std::size_t>(displayColumnCount()),
        0.0F);
    return QVector<float>(projected.begin(), projected.end());
}

void SpectrumWaterfallItem::projectSpectrumHolds()
{
    if (!spectrumHoldsAvailable()) {
        m_projectedMaximumHoldDbfs.clear();
        m_holdProjectionDirty = false;
        return;
    }
    const int columns = displayColumnCount();
    if (columns < 2) {
        m_projectedMaximumHoldDbfs.clear();
        m_holdProjectionDirty = false;
        return;
    }
    if (m_projectedMaximumHoldDbfs.size() != columns) {
        m_projectedMaximumHoldDbfs.resize(columns);
    }
    sdr::gui::projectMaximumHoldToFrequencyAxis(
        std::span<const float>(
            m_maximumHoldDbfs.data(),
            static_cast<std::size_t>(m_maximumHoldDbfs.size())),
        m_holdCenterFrequency,
        m_holdSampleRate,
        frequencyAxis(),
        m_spectrumMinimumDbfs,
        {m_projectedMaximumHoldDbfs.data(),
         static_cast<std::size_t>(m_projectedMaximumHoldDbfs.size())});
    m_holdProjectionDirty = false;
}

QVector<float> SpectrumWaterfallItem::displayFrame(
    const sdr::gui::WaterfallHistoryRow& frame) const
{
    const auto projected =
        m_waterfallAggregation == sdr::gui::WaterfallAggregation::Original
            ? sdr::gui::projectWaterfallRowToFrequencyAxis(
                  frame,
                  frequencyAxis(),
                  static_cast<std::size_t>(displayColumnCount()),
                  0.0F)
            : sdr::gui::projectAverageWaterfallRowToFrequencyAxis(
                  frame,
                  frequencyAxis(),
                  static_cast<std::size_t>(displayColumnCount()),
                  0.0F);
    return QVector<float>(projected.begin(), projected.end());
}

QVector<float> SpectrumWaterfallItem::displayFrame(
    const sdr::gui::ViewportWaterfallHistoryRow& frame) const
{
    QVector<float> projected(static_cast<qsizetype>(frame.physicalWidth));
    if (m_waterfallAggregation == sdr::gui::WaterfallAggregation::Original) {
        constexpr float scale = 1.0F / 65'535.0F;
        for (std::size_t column = 0;
             column < frame.physicalWidth;
             ++column) {
            projected[static_cast<qsizetype>(column)] =
                static_cast<float>(frame.peakMagnitudes[column]) * scale;
        }
    } else {
        std::copy(
            frame.meanLinearPowers.begin(),
            frame.meanLinearPowers.end(),
            projected.begin());
    }
    return projected;
}

void SpectrumWaterfallItem::reportHistoryMetrics()
{
    emit historyMetricsChanged();
    if (m_applicationModel && m_waterfall) {
        m_applicationModel->reportWaterfallHistoryMetrics(
            historyMemoryUsageBytes(),
            static_cast<quint64>(m_waterfallHistory.size()),
            retainedHistorySeconds(),
            m_visibleHistorySeconds,
            retainedHistoryCapacitySeconds(),
            storedHistoryBins(),
            historyConfigurationFitsMemoryBudget(),
            m_renderedFrames,
            m_mergedRenderUpdates,
            m_lastReprojectionMilliseconds,
            m_staleReprojectionsDiscarded,
            viewportHistoryMemoryUsageBytes(),
            viewportHistoryMemoryBudgetBytes(),
            static_cast<quint64>(m_viewportWaterfallHistory.size()));
    }
}

sdr::radio::FrequencyAxisMapper SpectrumWaterfallItem::frequencyAxis() const noexcept
{
    const sdr::radio::FrequencyPlot plot{
        0.0,
        static_cast<double>(width()),
    };
    if (m_applicationModel) {
        return m_applicationModel->frequencyViewportAxis(plot);
    }
    const auto visibleRange = sdr::radio::visibleCaptureRange(
        m_latestFrame.centerFrequency,
        m_latestFrame.sampleRate,
        {0, std::numeric_limits<std::uint64_t>::max()});
    return {
        visibleRange.value_or(sdr::radio::FrequencyRange{}),
        plot,
    };
}

int SpectrumWaterfallItem::displayColumnCount() const noexcept
{
    if (m_waterfall && m_rasterGeometry.physicalWidth > 0) {
        return static_cast<int>(m_rasterGeometry.physicalWidth);
    }
    const qreal devicePixelRatio = window() ? window()->devicePixelRatio() : 1.0;
    return static_cast<int>(sdr::gui::displayColumnCountForWidth(
        width(), devicePixelRatio));
}

float SpectrumWaterfallItem::spectrumDisplayLevel(float magnitude) const noexcept
{
    const float dbfs = sdr::gui::dbfsForNormalizedSpectrum(magnitude);
    return std::clamp(
        (dbfs - m_spectrumMinimumDbfs) /
            (m_spectrumMaximumDbfs - m_spectrumMinimumDbfs),
        0.0F,
        1.0F);
}

float SpectrumWaterfallItem::yForDbfs(float dbfs) const noexcept
{
    return yForDbfs(dbfs, static_cast<float>(height()));
}

float SpectrumWaterfallItem::waterfallDbfs(float magnitude) const noexcept
{
    return sdr::gui::dbfsForNormalizedSpectrum(magnitude);
}

float SpectrumWaterfallItem::waterfallDbfsForLinearPower(float power) const noexcept
{
    if (!std::isfinite(power) || power <= 0.0F) {
        return m_waterfallMinimumDbfs;
    }
    return sdr::gui::dbfsForNormalizedSpectrum(
        sdr::gui::normalizedMagnitudeForLinearPower(power));
}

void SpectrumWaterfallItem::updateNoiseFloor()
{
    m_noiseScratch.clear();
    m_noiseScratch.reserve(static_cast<qsizetype>(
        m_latestFrame.normalizedMagnitudes.size()));
    for (const float magnitude : m_latestFrame.normalizedMagnitudes) {
        if (std::isfinite(magnitude)) {
            m_noiseScratch.push_back(magnitude);
        }
    }

    const auto estimate = sdr::gui::estimateNoiseFloorDbfs(
        std::span<float>(m_noiseScratch.data(),
                         static_cast<std::size_t>(m_noiseScratch.size())));
    if (!estimate.has_value()) {
        if (m_noiseFloorAvailable) {
            m_noiseFloorAvailable = false;
            emit noiseFloorChanged();
        }
        return;
    }

    constexpr float smoothing = 0.18F;
    const float smoothed = m_noiseFloorAvailable
                               ? sdr::gui::smoothNoiseFloorDbfs(
                                     m_noiseFloorDbfs, *estimate, smoothing)
                               : *estimate;
    const bool changed = !m_noiseFloorAvailable ||
                         std::abs(smoothed - m_noiseFloorDbfs) >= 0.1F;
    m_noiseFloorDbfs = smoothed;
    m_noiseFloorAvailable = true;
    if (changed) {
        emit noiseFloorChanged();
    }
}
