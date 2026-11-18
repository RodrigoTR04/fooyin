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
#include <QSet>
#include <QThread>
#include <QtConcurrentRun>

#include <array>
#include <optional>

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

    // Namida doesn't identify the device, so it's recognised by the listens it has in common with one here, however
    // either is named, such as a phone that first came by way of another installation. Failing that, it's the device
    // with the name given, which is only added once it has listens to show for it, so a mistyped name doesn't linger
    int source{0};
    for(const auto& listen : history->listens) {
        if(const int existing = database.sourceOf(origin, listen.timestamp); existing > 0) {
            source = existing;
            break;
        }
    }
    if(source == 0) {
        source = database.findNamidaSource(name);
    }

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

    // By the name it has here, which may not be the one given
    if(source != 0) {
        for(const auto& known : database.sources()) {
            if(known.id == source) {
                result.devices = {known.name};
            }
        }
    }
    result.invalid         = history->invalid;
    result.unmatchedTracks = {unmatched.cbegin(), unmatched.cend()};
    result.unmatchedTracks.sort();
    return result;
}

//! A device here that listens in an imported file go to.
struct ImportDestination
{
    //! 0 until the device is here.
    int id{0};
    //! What the device is added as if it isn't here yet.
    Fooyin::PlayHistoryImport::ExportedSource device;
    //! Installations that are part of the device, to be remembered as such once it's here.
    std::vector<Fooyin::PlayHistoryImport::ExportedAlias> aliases;
    //! Whether any of the listens in the file go to it.
    bool used{false};
};

//! Remembers the installations that are part of @p destination, so that their own history goes to it when imported
//! later on. Returns false on error.
bool rememberAliases(const Fooyin::PlayHistoryDatabase& database, ImportDestination& destination)
{
    for(const auto& alias : destination.aliases) {
        if(!database.addMergedSource(QLatin1String{Fooyin::PlayHistoryImport::FooyinKind}, alias.deviceId, alias.name,
                                     destination.id)) {
            return false;
        }
    }
    destination.aliases.clear();
    return true;
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
    QSet<int> fooyinSources;
    for(const auto& source : history->sources) {
        fileSources.insert(source.id, source);
        if(source.kind == fooyinKind) {
            fooyinSources.insert(source.id);
        }
    }

    QSet<QString> fooyinOrigins;
    QHash<int, std::vector<const PlayHistoryEntry*>> namidaListens;
    for(const auto& entry : history->listens) {
        if(fooyinSources.contains(entry.source)) {
            fooyinOrigins.insert(entry.origin);
        }
        else {
            namidaListens[entry.source].push_back(&entry);
        }
    }

    // Where the listens in the file go. Those of a fooyin installation go to the device here that already has them,
    // which is the one it was imported as or merged into before, or whichever has any of its listens. That holds even
    // if the file groups installations differently, so an installation's listens are never split between two devices
    // here. Failing that, they go with the installations the file has as the same device. Namida doesn't identify the
    // device, so each installation that imports a phone gives it an id of its own, and a phone that comes by way of
    // another installation is recognised by the listens it has in common with the one here, however either has been
    // named. A device that isn't here yet is only added once it has listens to show for it
    std::vector<ImportDestination> destinations;
    QHash<QString, size_t> byOrigin;
    QHash<int, size_t> byNamidaDevice;
    QHash<int, size_t> byId;

    const auto existingDestination = [&](int id) {
        if(const auto it = byId.constFind(id); it != byId.cend()) {
            return it.value();
        }
        destinations.push_back({.id = id, .device = {}, .aliases = {}, .used = false});
        byId.insert(id, destinations.size() - 1);
        return destinations.size() - 1;
    };
    const auto route = [&](const QString& installation, size_t destination) {
        const QString origin = PlayHistoryImport::fooyinOrigin(installation);
        byOrigin.insert(origin, destination);
        destinations[destination].used = destinations[destination].used || fooyinOrigins.contains(origin);
    };

    // The device here with the listens of an installation, if any. One that has them without knowing the installation
    // is told it's part of it, so that its listens go to it whenever they're imported. Returns -1 on error
    bool originsRead{false};
    const auto originSources = database.originSources(&originsRead);
    if(!originsRead) {
        result.error = databaseError();
        return result;
    }
    const auto ownerOf = [&](const PlayHistoryImport::ExportedAlias& installation) {
        if(const int id = database.findSource(fooyinKind, installation.deviceId); id != 0) {
            return id;
        }
        const int owner = originSources.value(PlayHistoryImport::fooyinOrigin(installation.deviceId));
        if(owner <= 0) {
            return 0;
        }
        return database.addMergedSource(fooyinKind, installation.deviceId, installation.name, owner) ? owner : -1;
    };

    for(const auto& source : std::as_const(fileSources)) {
        if(source.kind != fooyinKind) {
            if(!namidaListens.contains(source.id)) {
                continue;
            }
            int id = database.findSource(source.kind, source.deviceId);
            if(id == 0) {
                for(const PlayHistoryEntry* entry : namidaListens.value(source.id)) {
                    if(const int existing = database.sourceOf(entry->origin, entry->timestamp); existing > 0) {
                        id = existing;
                        break;
                    }
                }
            }
            destinations.push_back({.id = id, .device = source, .aliases = {}, .used = true});
            byNamidaDevice.insert(source.id, destinations.size() - 1);
            continue;
        }

        // Every installation that's part of this device, other than this one, whose listens are ours again
        std::vector<PlayHistoryImport::ExportedAlias> installations;
        if(source.deviceId != deviceId) {
            installations.push_back({.deviceId = source.deviceId, .name = source.name});
        }
        for(const auto& alias : source.aliases) {
            if(alias.deviceId != deviceId) {
                installations.push_back(alias);
            }
        }

        // An installation that was merged into this one elsewhere is a device of its own here, as this one can't be
        // merged into another
        if(source.deviceId == deviceId) {
            for(const auto& installation : installations) {
                const int owner = ownerOf(installation);
                if(owner < 0) {
                    result.error = databaseError();
                    return result;
                }
                if(owner != 0) {
                    route(installation.deviceId, existingDestination(owner));
                    continue;
                }
                destinations.push_back({.id      = 0,
                                        .device  = {.id       = 0,
                                                    .kind     = fooyinKind,
                                                    .deviceId = installation.deviceId,
                                                    .name     = installation.name,
                                                    .aliases  = {}},
                                        .aliases = {},
                                        .used    = false});
                route(installation.deviceId, destinations.size() - 1);
            }
            continue;
        }

        std::vector<int> owners;
        for(const auto& installation : installations) {
            owners.push_back(ownerOf(installation));
            if(owners.back() < 0) {
                result.error = databaseError();
                return result;
            }
        }

        // The device itself if it's here, or else whichever of its installations is
        const auto known = std::ranges::find_if(owners, [](int owner) { return owner != 0; });
        destinations.push_back(
            {.id = known != owners.end() ? *known : 0, .device = source, .aliases = {}, .used = false});
        const size_t group = destinations.size() - 1;

        for(size_t i{0}; i < installations.size(); ++i) {
            const auto& installation = installations.at(i);
            if(owners.at(i) != 0) {
                route(installation.deviceId,
                      owners.at(i) == destinations.at(group).id ? group : existingDestination(owners.at(i)));
                continue;
            }

            route(installation.deviceId, group);
            // The device is added as itself if it's new, but the rest are remembered as part of it
            if(installation.deviceId != source.deviceId || destinations.at(group).id != 0) {
                destinations[group].aliases.push_back(installation);
            }
        }
    }

    QSet<int> seen;
    for(auto& destination : destinations) {
        if(destination.id == 0) {
            continue;
        }
        if(!rememberAliases(database, destination)) {
            result.error = databaseError();
            return result;
        }
        if(destination.used) {
            seen.insert(destination.id);
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

    for(size_t first{0}; first < entries.size();) {
        size_t end{first};
        while(end < entries.size() && entries.at(end).timestamp == entries.at(first).timestamp
              && entries.at(end).origin == entries.at(first).origin) {
            ++end;
        }

        // As with Namida backups, more than one listen at the same time from the same place is told apart by its track.
        // Listens recorded here may also have been kept with this installation's origin rather than none
        QStringList existing = database.hashesAt(entries.at(first).origin, entries.at(first).timestamp);
        if(entries.at(first).origin.isEmpty()) {
            existing.append(database.hashesAt(localOrigin, entries.at(first).timestamp));
        }
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

            if(entry.origin.isEmpty()) {
                entry.source = 0;
            }
            else {
                // Namida listens all have the same origin, so they go by device
                std::optional<size_t> index;
                if(fooyinSources.contains(entry.source)) {
                    if(const auto it = byOrigin.constFind(entry.origin); it != byOrigin.cend()) {
                        index = it.value();
                    }
                }
                else if(const auto it = byNamidaDevice.constFind(entry.source); it != byNamidaDevice.cend()) {
                    index = it.value();
                }
                if(!index) {
                    result.error = databaseError();
                    return result;
                }

                ImportDestination& destination = destinations[*index];
                if(destination.id == 0) {
                    destination.id = database.findOrAddSource(destination.device.kind, destination.device.deviceId,
                                                              destination.device.name);
                    if(destination.id == 0 || !rememberAliases(database, destination)) {
                        result.error = databaseError();
                        return result;
                    }
                    seen.insert(destination.id);
                }
                entry.source = destination.id;
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

    // Every device whose history was imported, even if it had nothing new, as with the time it was last imported. By
    // the names they have here, which may not be the ones they have in the file
    for(const auto& source : database.sources()) {
        if(seen.contains(source.id)) {
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

QFuture<PlayHistoryRenameResult> PlayHistory::renameSource(int id, const QString& name)
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
            else if(result.succeeded() && self) {
                QMetaObject::invokeMethod(self, &PlayHistory::sourcesChanged, Qt::QueuedConnection);
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
        bool mergedRead{false};
        bool listensRead{false};

        const QString fooyinKind = QLatin1String{PlayHistoryImport::FooyinKind};
        const auto sources       = database.sources(&sourcesRead);
        const auto merged        = database.mergedSources(&mergedRead);
        for(const auto& source : sources) {
            history.sources.push_back({.id       = source.id,
                                       .kind     = source.kind,
                                       .deviceId = source.deviceId,
                                       .name     = source.name,
                                       .aliases  = {}});
        }

        // Where each listen was recorded goes with it, so that wherever it ends up it's only ever counted once
        const QString localOrigin = PlayHistoryImport::fooyinOrigin(history.deviceId);
        history.listens           = database.all(&listensRead);
        for(auto& entry : history.listens) {
            if(entry.origin.isEmpty()) {
                entry.origin = localOrigin;
            }
        }

        // Installations merged into a device keep their own origin, so the device has to say which they are for its
        // listens to be accepted as its own. Each installation can only be part of one device in the file, which is
        // the device that is that installation, or else the one it was merged into, or else the first fooyin device
        // with any of its listens, such as by an import from before installations were remembered that way. Any of its
        // listens held by another device go with the rest of them, and those held by no fooyin device here go to a
        // device of its own in the file
        QHash<int, size_t> sourceIndex;
        QSet<int> namidaSources;
        QHash<QString, int> owners;
        int nextId{1};
        for(size_t i{0}; i < history.sources.size(); ++i) {
            const auto& source = history.sources.at(i);
            nextId             = std::max(nextId, source.id + 1);
            if(source.kind == fooyinKind) {
                sourceIndex.insert(source.id, i);
                owners.insert(source.deviceId, source.id);
            }
            else {
                namidaSources.insert(source.id);
            }
        }
        const auto addInstallation = [&](int id, const QString& installation, const QString& name) {
            if(owners.contains(installation)) {
                return;
            }
            owners.insert(installation, id);
            history.sources[sourceIndex.value(id)].aliases.push_back({.deviceId = installation, .name = name});
        };
        for(const auto& [id, aliases] : merged.asKeyValueRange()) {
            if(id != 0 && sourceIndex.contains(id)) {
                for(const auto& alias : aliases) {
                    addInstallation(id, alias.deviceId, alias.name);
                }
            }
        }

        // A device of its own mustn't take the id of one that's gone but still has listens, or they'd be taken as its
        for(const auto& entry : history.listens) {
            nextId = std::max(nextId, entry.source + 1);
        }

        const QString fooyinPrefix = PlayHistoryImport::fooyinOrigin({});
        for(auto& entry : history.listens) {
            if(!entry.origin.startsWith(fooyinPrefix)) {
                // Only a Namida device can have Namida's listens, and there's no telling which one a stray one is from
                if(entry.origin != QLatin1String{PlayHistoryImport::NamidaOrigin}
                   || !namidaSources.contains(entry.source)) {
                    return tr("The listening history has listens that don't say where they were recorded.");
                }
                continue;
            }

            const QString installation = entry.origin.mid(fooyinPrefix.size());
            if(!owners.contains(installation)) {
                if(entry.source != 0 && sourceIndex.contains(entry.source)) {
                    addInstallation(entry.source, installation, {});
                }
                else {
                    history.sources.push_back({.id       = nextId,
                                               .kind     = fooyinKind,
                                               .deviceId = installation,
                                               .name     = tr("Unknown device"),
                                               .aliases  = {}});
                    sourceIndex.insert(nextId, history.sources.size() - 1);
                    owners.insert(installation, nextId++);
                }
            }
            entry.source = owners.value(installation);
        }

        if(!sourcesRead || !mergedRead || !listensRead || !transaction.commit()) {
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
