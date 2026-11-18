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
#include "playhistoryimport.h"

#include <core/constants.h>
#include <utils/async.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/database/dbtransaction.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QLoggingCategory>
#include <QPointer>
#include <QSaveFile>
#include <QThread>
#include <QtConcurrentRun>

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

QString tr(const char* text)
{
    return QCoreApplication::translate("Fooyin::PlayHistory", text);
}

QString databaseError()
{
    return tr("The listens could not be saved to the database.");
}

Fooyin::PlayHistoryImportResult importNamida(const Fooyin::PlayHistoryDatabase& database, const QString& filepath,
                                             const QString& device, const Fooyin::TrackList& libraryTracks)
{
    using namespace Fooyin;

    PlayHistoryImportResult result;

    const auto history = PlayHistoryImport::readNamida(filepath);
    if(!history) {
        result.error = history.error();
        return result;
    }

    const QString name   = device.trimmed().isEmpty() ? u"Namida"_s : device.trimmed();
    const QString origin = QLatin1String{PlayHistoryImport::NamidaOrigin};
    const PlayHistoryImport::TrackMatcher matcher{libraryTracks};

    DbTransaction transaction{database.db()};
    if(!transaction) {
        result.error = databaseError();
        return result;
    }

    // Only added once it has listens to show for it, so a device given the wrong name doesn't linger
    int source = database.findNamidaSource(name);
    QSet<QString> unmatched;

    const auto& listens = history->listens;
    for(size_t first{0}; first < listens.size();) {
        size_t end{first};
        while(end < listens.size() && listens.at(end).timestamp == listens.at(first).timestamp) {
            ++end;
        }

        // Each backup has everything since Namida was installed, so most of a newer one will have been imported before.
        // A listen is recognised by its time alone, as the file may have moved since, unless there's more than one at
        // that time, as when a listen Namida imported matched several tracks. Then it's the tracks that tell them apart
        QStringList existing = database.hashesAt(origin, listens.at(first).timestamp);
        if(end - first <= static_cast<size_t>(existing.size())) {
            result.duplicates += static_cast<int>(end - first);
            first = end;
            continue;
        }

        QStringList handled;
        for(; first < end; ++first) {
            const auto& listen = listens.at(first);

            const Track* track = matcher.match(listen.path);
            if(!track) {
                ++result.unmatched;
                unmatched.insert(listen.path);
                continue;
            }

            auto entry = PlayHistoryEntry::fromTrack(*track, listen.timestamp);
            // The same track twice at the same time is the same listen
            if(handled.contains(entry.hash) || existing.removeOne(entry.hash)) {
                handled.append(entry.hash);
                ++result.duplicates;
                continue;
            }
            handled.append(entry.hash);

            if(source == 0) {
                source = database.findOrAddNamidaSource(name);
            }

            entry.source = source;
            entry.origin = origin;
            if(source == 0 || !database.insert(entry)) {
                result.error = databaseError();
                return result;
            }
            ++result.added;
        }
    }

    if(source != 0) {
        database.setLastImported(source, QDateTime::currentMSecsSinceEpoch());
    }

    if(!transaction.commit()) {
        result       = {};
        result.error = databaseError();
        return result;
    }

    result.devices         = {name};
    result.invalid         = history->invalid;
    result.unmatchedTracks = {unmatched.cbegin(), unmatched.cend()};
    result.unmatchedTracks.sort();
    return result;
}

Fooyin::PlayHistoryImportResult importFooyin(const Fooyin::PlayHistoryDatabase& database, const QString& filepath)
{
    using namespace Fooyin;

    PlayHistoryImportResult result;

    const auto history = PlayHistoryImport::readFooyin(filepath);
    if(!history) {
        result.error = history.error();
        return result;
    }

    // An installation's own export can be imported too, such as to restore the listens of a device that was removed.
    // Those recorded here are recognised as such and skipped like any other duplicate
    const QString deviceId = database.deviceId();
    if(deviceId.isEmpty()) {
        result.error = databaseError();
        return result;
    }

    const QString fooyinKind  = QLatin1String{PlayHistoryImport::FooyinKind};
    const QString localOrigin = PlayHistoryImport::fooyinOrigin(deviceId);

    DbTransaction transaction{database.db()};
    if(!transaction) {
        result.error = databaseError();
        return result;
    }

    // The listens of each device in the file. The file has been checked to describe every device it has listens of
    QHash<int, PlayHistoryImport::ExportedSource> fileSources;
    for(const auto& source : history->sources) {
        fileSources.insert(source.id, source);
    }
    QHash<int, std::vector<const PlayHistoryEntry*>> listensBySource;
    for(const auto& entry : history->listens) {
        listensBySource[entry.source].push_back(&entry);
    }

    // Which device here each device in the file is: this one, one it was imported as before, or, if any of its listens
    // are already here, whichever device they're from. Namida doesn't identify the device, so each installation that
    // imports a phone gives it an id of its own, and a phone that comes by way of another installation is recognised by
    // the listens it has in common with the one here. That holds however either has been named. A device that isn't
    // here yet is only added once it has listens to show for it
    QHash<int, int> resolved;
    QSet<int> seen;
    for(auto it = listensBySource.cbegin(); it != listensBySource.cend(); ++it) {
        const auto& source = fileSources.value(it.key());

        int id = source.kind == fooyinKind && source.deviceId == deviceId
                   ? 0
                   : database.findSource(source.kind, source.deviceId);
        if(id == 0 && source.deviceId != deviceId) {
            for(const PlayHistoryEntry* entry : it.value()) {
                if(const int existing = database.sourceOf(entry->origin, entry->timestamp); existing > 0) {
                    id = existing;
                    break;
                }
            }
        }

        resolved.insert(it.key(), id);
        if(id != 0) {
            seen.insert(id);
        }
    }

    // Listens that were recorded here and have come back by way of another device are ours again
    std::vector<PlayHistoryEntry> entries = history->listens;
    for(auto& entry : entries) {
        if(entry.origin == localOrigin) {
            entry.origin.clear();
        }
    }
    std::ranges::sort(entries, [](const auto& a, const auto& b) {
        return std::tie(a.timestamp, a.origin) < std::tie(b.timestamp, b.origin);
    });

    QSet<int> contributed;

    for(size_t first{0}; first < entries.size();) {
        size_t end{first};
        while(end < entries.size() && entries.at(end).timestamp == entries.at(first).timestamp
              && entries.at(end).origin == entries.at(first).origin) {
            ++end;
        }

        // As with Namida backups, more than one listen at the same time from the same place is told apart by its track
        QStringList existing = database.hashesAt(entries.at(first).origin, entries.at(first).timestamp);
        if(end - first <= static_cast<size_t>(existing.size())) {
            result.duplicates += static_cast<int>(end - first);
            first = end;
            continue;
        }

        QStringList handled;
        for(; first < end; ++first) {
            PlayHistoryEntry entry = entries.at(first);
            // The same track twice at the same time is the same listen
            if(handled.contains(entry.hash) || existing.removeOne(entry.hash)) {
                handled.append(entry.hash);
                ++result.duplicates;
                continue;
            }
            handled.append(entry.hash);

            const auto& fileSource = fileSources.value(entry.source);
            if(entry.origin.isEmpty()) {
                entry.source = 0;
            }
            else {
                int& id = resolved[entry.source];
                if(id == 0) {
                    id = database.findOrAddSource(fileSource.kind, fileSource.deviceId, fileSource.name);
                    if(id == 0) {
                        result.error = databaseError();
                        return result;
                    }
                    seen.insert(id);
                }
                entry.source = id;
                contributed.insert(id);
            }

            if(!database.insert(entry)) {
                result.error = databaseError();
                return result;
            }
            ++result.added;
        }
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for(const int source : std::as_const(seen)) {
        database.setLastImported(source, now);
    }

    if(!transaction.commit()) {
        result       = {};
        result.error = databaseError();
        return result;
    }

    // By the names they have here, which may not be the ones they have in the file
    for(const auto& source : database.sources()) {
        if(contributed.contains(source.id)) {
            result.devices.append(source.name);
        }
    }
    result.devices.sort();
    result.invalid = history->invalid;
    return result;
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

QFuture<std::vector<PlayHistorySource>> PlayHistory::sources() const
{
    return Utils::asyncExec([pool = m_dbPool]() {
        const DbConnectionHandler dbHandler{pool};

        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});
        return database.sources();
    });
}

QFuture<bool> PlayHistory::renameSource(int id, const QString& name)
{
    return QtConcurrent::run(&m_writePool, [pool = m_dbPool, id, name]() {
        const DbConnectionHandler dbHandler{pool};

        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});
        return database.renameSource(id, name);
    });
}

QFuture<bool> PlayHistory::removeSource(int id)
{
    return QtConcurrent::run(&m_writePool, [pool = m_dbPool, id, self = QPointer<PlayHistory>(this)]() {
        const DbConnectionHandler dbHandler{pool};

        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});

        const bool removed = database.removeSource(id);
        if(removed && self) {
            QMetaObject::invokeMethod(self, &PlayHistory::historyChanged, Qt::QueuedConnection);
        }
        return removed;
    });
}

QFuture<bool> PlayHistory::mergeSource(int from, int into)
{
    return QtConcurrent::run(&m_writePool, [pool = m_dbPool, from, into, self = QPointer<PlayHistory>(this)]() {
        const DbConnectionHandler dbHandler{pool};

        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});

        const bool merged = database.mergeSource(from, into);
        if(merged && self) {
            QMetaObject::invokeMethod(self, &PlayHistory::historyChanged, Qt::QueuedConnection);
        }
        return merged;
    });
}

PlayHistoryFormat PlayHistory::fileFormat(const QString& filepath)
{
    return PlayHistoryImport::detectFormat(filepath);
}

QFuture<PlayHistoryImportResult> PlayHistory::importFile(const QString& filepath, const QString& namidaDevice,
                                                         const TrackList& libraryTracks)
{
    // Written by the same thread as listens, so an import doesn't compete with recording them
    return QtConcurrent::run(
        &m_writePool, [pool = m_dbPool, filepath, namidaDevice, libraryTracks, self = QPointer<PlayHistory>(this)]() {
            const DbConnectionHandler dbHandler{pool};

            PlayHistoryDatabase database;
            database.initialise(DbConnectionProvider{pool});

            PlayHistoryImportResult result;
            switch(PlayHistoryImport::detectFormat(filepath)) {
                case PlayHistoryFormat::Fooyin:
                    result = importFooyin(database, filepath);
                    break;
                case PlayHistoryFormat::Namida:
                    result = importNamida(database, filepath, namidaDevice, libraryTracks);
                    break;
                case PlayHistoryFormat::Unknown:
                    result.error = tr("The file is not a listening history exported by fooyin or a Namida backup.");
                    break;
            }

            if(result.added > 0 && self) {
                QMetaObject::invokeMethod(self, &PlayHistory::historyChanged, Qt::QueuedConnection);
            }
            if(!result.succeeded()) {
                qCWarning(PLAY_HISTORY) << "Could not import" << filepath << ":" << result.error;
            }

            return result;
        });
}

QFuture<QString> PlayHistory::exportFile(const QString& filepath) const
{
    return Utils::asyncExec([pool = m_dbPool, filepath]() {
        const DbConnectionHandler dbHandler{pool};

        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});

        PlayHistoryImport::FooyinExport history;
        history.deviceId = database.deviceId();
        if(history.deviceId.isEmpty()) {
            return databaseError();
        }

        // The devices and their listens are read together, so a device added by an import meanwhile can't be missing
        // from the list while its listens are there
        DbTransaction transaction{database.db()};
        if(!transaction) {
            return databaseError();
        }
        history.deviceName = database.deviceName();
        history.exported   = QDateTime::currentMSecsSinceEpoch();

        // An export that's missing anything mustn't take the place of one that isn't
        bool sourcesRead{false};
        bool listensRead{false};

        for(const auto& source : database.sources(&sourcesRead)) {
            history.sources.push_back(
                {.id = source.id, .kind = source.kind, .deviceId = source.deviceId, .name = source.name});
        }

        // Where each listen was recorded goes with it, so that wherever it ends up it's only ever counted once
        const QString localOrigin = PlayHistoryImport::fooyinOrigin(history.deviceId);
        history.listens           = database.all(&listensRead);
        for(auto& entry : history.listens) {
            if(entry.origin.isEmpty()) {
                entry.origin = localOrigin;
            }
        }

        if(!sourcesRead || !listensRead || !transaction.commit()) {
            return tr("The listening history could not be read from the database.");
        }

        QSaveFile file{filepath};
        if(!file.open(QIODevice::WriteOnly) || file.write(PlayHistoryImport::writeFooyin(history)) < 0
           || !file.commit()) {
            return tr("The file could not be written: %1").arg(file.errorString());
        }

        return QString{};
    });
}
} // namespace Fooyin

#include "core/playhistory/moc_playhistory.cpp"
