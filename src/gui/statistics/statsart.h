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

#pragma once

#include <core/track.h>
#include <gui/coverprovider.h>

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QTimer>

namespace Fooyin {
class CoverRepository;

/*!
 * Artwork for the tracks in the statistics, which only know a track by its hash.
 *
 * Covers load in the background. coversChanged() is emitted once more have arrived so views can repaint.
 */
class StatsArt : public QObject
{
    Q_OBJECT

public:
    StatsArt(CoverRepository* repository, QObject* parent = nullptr);

    //! The tracks that artwork can be found for, by hash.
    void setTracks(QHash<QString, Track> tracks);

    //! A square cover for the track with @p hash, or the placeholder when it has none (or isn't in the library).
    [[nodiscard]] QPixmap cover(const QString& hash, int size, qreal devicePixelRatio = 1.0) const;

    /*!
     * The dominant colour of the track's cover, favouring vivid areas.
     * Invalid when the track has no cover (yet).
     */
    [[nodiscard]] QColor dominantColour(const QString& hash) const;

Q_SIGNALS:
    void coversChanged();

private:
    CoverProvider m_provider;
    QHash<QString, Track> m_tracks;
    QTimer m_changeTimer;
};
} // namespace Fooyin
