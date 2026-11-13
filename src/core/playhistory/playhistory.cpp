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

#include <core/playhistory/playhistory.h>

#include "database/playhistorydatabase.h"

#include <core/constants.h>
#include <utils/async.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionprovider.h>

#include <QDateTime>
#include <QLoggingCategory>
#include <QPointer>
#include <QThread>

#include <array>

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(PLAY_HISTORY, "fy.playhistory")

namespace {
int losslessState(const QString& codec)
{
    if(codec.isEmpty()) {
        return -1;
    }

    static constexpr std::array LosslessCodecs{"flac", "alac", "apple lossless", "wavpack", "ape", "monkey", "tta",
                                               "tak",  "wav",  "aiff",           "pcm",     "dsd", "mlp",    "truehd"};

    const QString lower = codec.toLower();
    for(const char* name : LosslessCodecs) {
        if(lower.contains(QLatin1String{name})) {
            return 1;
        }
    }

    return 0;
}
} // namespace

namespace Fooyin {
QString PlayHistoryEntry::albumKey() const
{
    return albumKey(album, albumArtists);
}

QString PlayHistoryEntry::albumKey(const QString& album, const QStringList& albumArtists)
{
    if(album.isEmpty()) {
        return {};
    }

    return album + QLatin1String{Constants::UnitSeparator} + albumArtists.join(QLatin1String{Constants::UnitSeparator});
}

PlayHistoryEntry PlayHistoryEntry::fromTrack(const Track& track, qint64 timestamp, qint64 countedAfter,
                                             qint64 countedAt)
{
    PlayHistoryEntry entry;
    entry.timestamp    = timestamp;
    entry.countedAfter = countedAfter;
    entry.countedAt    = countedAt;
    entry.hash         = track.hash().isEmpty() ? track.uniqueFilepath() : track.hash();
    entry.title        = track.title();
    entry.artists      = track.artists();
    entry.album        = track.album();
    entry.albumArtists = track.albumArtists();
    entry.genres       = track.genres();
    entry.composers    = track.composers();
    entry.year         = track.year();
    entry.duration     = track.duration();
    entry.bitrate      = track.bitrate();
    entry.lossless     = losslessState(track.codec());
    return entry;
}

PlayHistory::PlayHistory(DbConnectionPoolPtr dbPool, QObject* parent)
    : QObject{parent}
    , m_dbPool{std::move(dbPool)}
{
    // One writer, so listens are stored in the order they happened and never compete with each other
    m_writePool.setMaxThreadCount(1);
}

PlayHistory::~PlayHistory()
{
    waitForWrites();
}

void PlayHistory::recordPlay(const Track& track, qint64 countedAfter, qint64 countedAt)
{
    const auto entry = PlayHistoryEntry::fromTrack(track, QDateTime::currentMSecsSinceEpoch(), countedAfter, countedAt);

    // A listen is only counted once, so it has to be kept until it's been stored. The database can be busy for a while,
    // such as when a scan is writing a batch of tracks, and writing here would hold up the interface meanwhile
    m_writePool.start([pool = m_dbPool, entry, self = QPointer<PlayHistory>(this)]() {
        const DbConnectionHandler dbHandler{pool};

        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});

        constexpr std::array retryDelaysMs{0, 250, 500, 1000, 2000, 4000};
        for(const int delay : retryDelaysMs) {
            if(delay > 0) {
                QThread::msleep(static_cast<unsigned long>(delay));
            }
            if(database.insert(entry)) {
                if(self) {
                    QMetaObject::invokeMethod(self, &PlayHistory::playRecorded, Qt::QueuedConnection);
                }
                return;
            }
        }

        qCWarning(PLAY_HISTORY) << "Could not record the listen of" << entry.title << "as the database stayed busy";
    });
}

void PlayHistory::waitForWrites()
{
    m_writePool.waitForDone();
}

QFuture<PlayHistoryData> PlayHistory::load(qint64 from, qint64 to) const
{
    return Utils::asyncExec([pool = m_dbPool, from, to]() {
        const DbConnectionHandler dbHandler{pool};

        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});
        return database.load(from, to);
    });
}

QFuture<PlayHistorySummary> PlayHistory::summary() const
{
    return Utils::asyncExec([pool = m_dbPool]() {
        const DbConnectionHandler dbHandler{pool};

        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});
        return database.summary();
    });
}
} // namespace Fooyin

#include "core/playhistory/moc_playhistory.cpp"
