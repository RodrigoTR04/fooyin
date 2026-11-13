/*
 * Fooyin
 * Copyright © 2026, Luke Taylor <luket@pm.me>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "statsart.h"

#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace Fooyin {
namespace {
//! Scales @p pixmap to fill a square of @p side pixels, cropping what doesn't fit.
QPixmap squareCrop(const QPixmap& pixmap, int side)
{
    if(pixmap.isNull()) {
        return pixmap;
    }

    const QPixmap scaled = pixmap.scaled(side, side, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    return scaled.copy((scaled.width() - side) / 2, (scaled.height() - side) / 2, side, side);
}
} // namespace

StatsArt::StatsArt(CoverRepository* repository, QObject* parent)
    : QObject{parent}
    , m_provider{repository}
{
    // Covers tend to arrive in bursts, so wait a moment before telling views to repaint
    m_changeTimer.setSingleShot(true);
    m_changeTimer.setInterval(60);
    QObject::connect(&m_changeTimer, &QTimer::timeout, this, &StatsArt::coversChanged);
    QObject::connect(&m_provider, &CoverProvider::coverAdded, this, [this]() { m_changeTimer.start(); });
    QObject::connect(&m_provider, &CoverProvider::placeholderChanged, this, &StatsArt::coversChanged);
}

void StatsArt::setTracks(QHash<QString, Track> tracks)
{
    m_tracks = std::move(tracks);
}

QPixmap StatsArt::cover(const QString& hash, int size, qreal devicePixelRatio) const
{
    const int pixels = std::max(1, static_cast<int>(std::ceil(size * devicePixelRatio)));

    QPixmap pixmap;
    if(const auto it = m_tracks.constFind(hash); it != m_tracks.constEnd()) {
        pixmap = m_provider.trackCoverThumbnail(it.value(), QSize{pixels, pixels});
    }
    if(pixmap.isNull()) {
        pixmap = m_provider.placeholderCover();
    }

    QPixmap square = squareCrop(pixmap, pixels);
    square.setDevicePixelRatio(devicePixelRatio);
    return square;
}

QColor StatsArt::dominantColour(const QString& hash) const
{
    const auto it = m_tracks.constFind(hash);
    if(it == m_tracks.constEnd()) {
        return {};
    }

    const QPixmap pixmap = m_provider.trackCoverThumbnail(it.value(), QSize{64, 64});
    if(pixmap.isNull()) {
        return {};
    }

    const QImage image = pixmap.toImage()
                             .scaled(24, 24, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_RGB32);

    // Vivid pixels count for more, so a mostly grey cover with a splash of colour gives that colour
    double red{0};
    double green{0};
    double blue{0};
    double weightSum{0};
    for(int y{0}; y < image.height(); ++y) {
        for(int x{0}; x < image.width(); ++x) {
            const QColor pixel  = image.pixelColor(x, y);
            const double weight = 0.05 + pixel.hsvSaturationF() * std::min(1.0, pixel.valueF() * 1.5);
            red += pixel.redF() * weight;
            green += pixel.greenF() * weight;
            blue += pixel.blueF() * weight;
            weightSum += weight;
        }
    }
    if(weightSum <= 0) {
        return {};
    }

    const QColor average = QColor::fromRgbF(red / weightSum, green / weightSum, blue / weightSum);
    // A cover with hardly any colour (or the grey placeholder) doesn't give a useful tint
    return average.hsvSaturationF() < 0.12 ? QColor{} : average;
}
} // namespace Fooyin

#include "moc_statsart.cpp"
