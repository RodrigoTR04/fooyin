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

#include "statisticsservice.h"

#include "statsart.h"

#include <core/library/musiclibrary.h>

#include <QDateTime>
#include <QFutureWatcher>
#include <QSet>

namespace Fooyin {
StatisticsService::StatisticsService(PlayHistory* history, MusicLibrary* library, CoverRepository* covers,
                                     QObject* parent)
    : QObject{parent}
    , m_history{history}
    , m_library{library}
    , m_art{new StatsArt(covers, this)}
{
    QObject::connect(m_history, &PlayHistory::playRecorded, this, &StatisticsService::historyChanged);
}

void StatisticsService::refreshLibraryInfo()
{
    Stats::LibraryInfo info;

    const TrackList tracks = m_library->tracks();
    QSet<QString> seen;
    seen.reserve(static_cast<qsizetype>(tracks.size()));
    QHash<QString, Track> byHash;
    byHash.reserve(static_cast<qsizetype>(tracks.size()));

    for(const Track& track : tracks) {
        const QString hash = track.hash().isEmpty() ? track.uniqueFilepath() : track.hash();
        // The same track can be in more than one library
        if(seen.contains(hash)) {
            continue;
        }
        seen.insert(hash);
        byHash.insert(hash, track);

        ++info.trackCount;
        info.durationMs += static_cast<qint64>(track.duration());
        info.tracks.insert(hash);
        if(track.playCount() == 0) {
            info.unplayed.insert(hash);
        }

        if(const QString key = PlayHistoryEntry::albumKey(track.album(), track.albumArtists()); !key.isEmpty()) {
            info.albumTracks[key].insert(hash);
        }
    }

    m_libraryInfo = std::move(info);
    m_art->setTracks(std::move(byHash));
}

StatsArt* StatisticsService::art() const
{
    return m_art;
}

const Stats::LibraryInfo& StatisticsService::libraryInfo() const
{
    return m_libraryInfo;
}

QFuture<PlayHistorySummary> StatisticsService::summary() const
{
    return m_history->summary();
}

QFuture<PlayHistoryStats> StatisticsService::compute(const QDate& firstDay, const QDate& lastDay, bool monthlyTop) const
{
    const qint64 from = firstDay.startOfDay().toMSecsSinceEpoch();
    const qint64 to   = lastDay.addDays(1).startOfDay().toMSecsSinceEpoch();

    PlayHistoryStatsOptions options;
    options.monthlyTop    = monthlyTop;
    options.albumTracks   = m_libraryInfo.albumTracks;
    options.libraryTracks = m_libraryInfo.tracks;

    return m_history->load(from, to).then(QtFuture::Launch::Async,
                                          [firstDay, lastDay, options](const PlayHistoryData& data) {
                                              return computePlayHistoryStats(data, firstDay, lastDay, options);
                                          });
}
} // namespace Fooyin
