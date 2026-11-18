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

#include "playhistorydatabase.h"

#include "core/playhistory/playhistoryimport.h"
#include "settingsdatabase.h"

#include <core/constants.h>
#include <utils/database/dbquery.h>
#include <utils/database/dbtransaction.h>

#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QSysInfo>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace {
QString joinList(const QStringList& list)
{
    return list.join(QLatin1String{Fooyin::Constants::UnitSeparator});
}

QStringList splitList(const QString& text)
{
    return text.isEmpty() ? QStringList{} : text.split(QLatin1String{Fooyin::Constants::UnitSeparator});
}

constexpr auto DeviceIdSetting   = "PlayHistory/DeviceID";
constexpr auto DeviceNameSetting = "PlayHistory/DeviceName";
} // namespace

namespace Fooyin {
bool PlayHistoryDatabase::insert(const PlayHistoryEntry& entry) const
{
    static const QString statement
        = u"INSERT INTO PlayHistory (Timestamp, Hash, Title, Artists, Album, AlbumArtists, Genres, Composers, "
          "Year, Duration, Bitrate, Lossless, CountedAfter, CountedAt, SourceID, Origin) "
          "VALUES (:timestamp, :hash, :title, :artists, :album, :albumArtists, :genres, :composers, "
          ":year, :duration, :bitrate, :lossless, :countedAfter, :countedAt, :source, :origin);"_s;

    DbQuery query{db(), statement};
    query.bindValue(u":timestamp"_s, entry.timestamp);
    query.bindValue(u":hash"_s, entry.hash);
    query.bindValue(u":title"_s, entry.title);
    query.bindValue(u":artists"_s, joinList(entry.artists));
    query.bindValue(u":album"_s, entry.album);
    query.bindValue(u":albumArtists"_s, joinList(entry.albumArtists));
    query.bindValue(u":genres"_s, joinList(entry.genres));
    query.bindValue(u":composers"_s, joinList(entry.composers));
    query.bindValue(u":year"_s, entry.year);
    query.bindValue(u":duration"_s, static_cast<qint64>(entry.duration));
    query.bindValue(u":bitrate"_s, entry.bitrate);
    query.bindValue(u":lossless"_s, entry.lossless);
    query.bindValue(u":countedAfter"_s, entry.countedAfter);
    query.bindValue(u":countedAt"_s, entry.countedAt);
    query.bindValue(u":source"_s, entry.source);
    // A null string would be stored as NULL, which wouldn't be recognised as a listen recorded here
    query.bindValue(u":origin"_s, entry.origin.isEmpty() ? u""_s : entry.origin);

    return query.exec();
}

bool PlayHistoryDatabase::contains(const QString& origin, qint64 timestamp) const
{
    static const QString statement
        = u"SELECT 1 FROM PlayHistory WHERE Timestamp = :timestamp AND IFNULL(Origin, '') = :origin LIMIT 1;"_s;

    DbQuery query{db(), statement};
    query.bindValue(u":timestamp"_s, timestamp);
    query.bindValue(u":origin"_s, origin.isEmpty() ? u""_s : origin);

    return query.exec() && query.next();
}

void PlayHistoryDatabase::moveTrack(const QString& from, const QString& to) const
{
    if(from.isEmpty() || to.isEmpty() || from == to) {
        return;
    }

    // Tracks are identified by hash, which changes with the tags that make it up. Unless another track is still known
    // by the old hash, its listens belong to the track as it is now
    static const QString statement = u"UPDATE PlayHistory SET Hash = :to WHERE Hash = :from AND NOT "
                                     "EXISTS (SELECT 1 FROM Tracks WHERE TrackHash = :remaining);"_s;

    DbQuery query{db(), statement};
    query.bindValue(u":to"_s, to);
    query.bindValue(u":from"_s, from);
    query.bindValue(u":remaining"_s, from);
    query.exec();
}

void PlayHistoryDatabase::refreshMetadata(const PlayHistoryEntry& entry) const
{
    if(entry.hash.isEmpty()) {
        return;
    }

    // Only what comes from the tags, which identify the track. How a listen sounded, such as its bitrate, is a matter
    // of the copy of the track that was played, and the same tags can be on more than one. Only the rows that differ
    // are written, as this is done for every track that is updated
    static const QString statement
        = u"UPDATE PlayHistory SET Title = :title, Artists = :artists, Album = :album, AlbumArtists = :albumArtists, "
          "Genres = :genres, Composers = :composers, Year = :year WHERE Hash = :hash AND (Title IS NOT :title OR "
          "Artists IS NOT :artists OR Album IS NOT :album OR AlbumArtists IS NOT :albumArtists OR "
          "Genres IS NOT :genres OR Composers IS NOT :composers OR Year IS NOT :year);"_s;

    DbQuery query{db(), statement};
    query.bindValue(u":hash"_s, entry.hash);
    query.bindValue(u":title"_s, entry.title);
    query.bindValue(u":artists"_s, joinList(entry.artists));
    query.bindValue(u":album"_s, entry.album);
    query.bindValue(u":albumArtists"_s, joinList(entry.albumArtists));
    query.bindValue(u":genres"_s, joinList(entry.genres));
    query.bindValue(u":composers"_s, joinList(entry.composers));
    query.bindValue(u":year"_s, entry.year);
    query.exec();
}

int PlayHistoryDatabase::sourceOf(const QString& origin, qint64 timestamp) const
{
    static const QString statement = u"SELECT SourceID FROM PlayHistory WHERE Timestamp = :timestamp AND "
                                     "IFNULL(Origin, '') = :origin LIMIT 1;"_s;

    DbQuery query{db(), statement};
    query.bindValue(u":timestamp"_s, timestamp);
    query.bindValue(u":origin"_s, origin.isEmpty() ? u""_s : origin);

    return query.exec() && query.next() ? query.value(0).toInt() : -1;
}

QHash<QString, int> PlayHistoryDatabase::originSources(bool* ok) const
{
    QHash<QString, int> sources;

    // In one pass, as there's no index of origins to look each of them up by
    QSqlQuery query{db()};
    bool succeeded = query.prepare(u"SELECT History.Origin, MIN(History.SourceID) FROM PlayHistory AS History "
                                   "JOIN PlayHistorySources AS Sources ON Sources.SourceID = History.SourceID "
                                   "WHERE Sources.Kind = :kind AND Sources.MergedInto = 0 GROUP BY History.Origin;"_s);
    query.bindValue(u":kind"_s, QLatin1String{PlayHistoryImport::FooyinKind});
    succeeded = succeeded && query.exec();
    if(succeeded) {
        while(query.next()) {
            sources.insert(query.value(0).toString(), query.value(1).toInt());
        }
        succeeded = !query.lastError().isValid();
    }

    if(ok) {
        *ok = succeeded;
    }
    return sources;
}

QStringList PlayHistoryDatabase::hashesAt(const QString& origin, qint64 timestamp) const
{
    static const QString statement
        = u"SELECT Hash FROM PlayHistory WHERE Timestamp = :timestamp AND IFNULL(Origin, '') = :origin;"_s;

    DbQuery query{db(), statement};
    query.bindValue(u":timestamp"_s, timestamp);
    query.bindValue(u":origin"_s, origin.isEmpty() ? u""_s : origin);

    QStringList hashes;
    if(query.exec()) {
        while(query.next()) {
            hashes.append(query.value(0).toString());
        }
    }
    return hashes;
}

PlayHistoryData PlayHistoryDatabase::load(qint64 from, qint64 to) const
{
    PlayHistoryData data;

    {
        static const QString statement
            = u"SELECT Timestamp, Hash, Title, Artists, Album, AlbumArtists, Genres, Composers, "
              "Year, Duration, Bitrate, Lossless, CountedAfter, CountedAt, SourceID FROM PlayHistory "
              "WHERE Timestamp >= :from AND Timestamp < :to ORDER BY Timestamp, EntryID;"_s;

        DbQuery query{db(), statement};
        query.bindValue(u":from"_s, from);
        query.bindValue(u":to"_s, to);

        if(!query.exec()) {
            return data;
        }

        while(query.next()) {
            PlayHistoryEntry entry;
            entry.timestamp    = query.value(0).toLongLong();
            entry.hash         = query.value(1).toString();
            entry.title        = query.value(2).toString();
            entry.artists      = splitList(query.value(3).toString());
            entry.album        = query.value(4).toString();
            entry.albumArtists = splitList(query.value(5).toString());
            entry.genres       = splitList(query.value(6).toString());
            entry.composers    = splitList(query.value(7).toString());
            entry.year         = query.value(8).toInt();
            entry.duration     = query.value(9).toULongLong();
            entry.bitrate      = query.value(10).toInt();
            entry.lossless     = query.value(11).toInt();
            entry.countedAfter = query.value(12).isNull() ? -1 : query.value(12).toLongLong();
            entry.countedAt    = query.value(13).isNull() ? -1 : query.value(13).toLongLong();
            entry.source       = query.value(14).toInt();
            data.entries.emplace_back(std::move(entry));
        }
    }

    {
        DbQuery query{db(), u"SELECT MIN(Timestamp) FROM PlayHistory;"_s};
        if(query.exec() && query.next()) {
            data.oldestTimestamp = query.value(0).toLongLong();
        }
    }

    {
        DbQuery query{db(), u"SELECT Hash, MIN(Timestamp) FROM PlayHistory GROUP BY Hash;"_s};
        if(query.exec()) {
            while(query.next()) {
                data.firstListenByHash.insert(query.value(0).toString(), query.value(1).toLongLong());
            }
        }
    }

    {
        static const QString statement
            = u"SELECT Hash, MAX(Timestamp) FROM PlayHistory WHERE Timestamp < :from GROUP BY Hash;"_s;

        DbQuery query{db(), statement};
        query.bindValue(u":from"_s, from);
        if(query.exec()) {
            while(query.next()) {
                data.lastListenBeforeByHash.insert(query.value(0).toString(), query.value(1).toLongLong());
            }
        }
    }

    {
        DbQuery query{db(), u"SELECT Artists, MIN(Timestamp) FROM PlayHistory GROUP BY Artists;"_s};
        if(query.exec()) {
            while(query.next()) {
                const qint64 timestamp = query.value(1).toLongLong();
                const auto artists     = splitList(query.value(0).toString());
                for(const QString& artist : artists) {
                    auto it = data.firstListenByArtist.find(artist);
                    if(it == data.firstListenByArtist.end()) {
                        data.firstListenByArtist.insert(artist, timestamp);
                    }
                    else if(timestamp < it.value()) {
                        it.value() = timestamp;
                    }
                }
            }
        }
    }

    return data;
}

PlayHistorySummary PlayHistoryDatabase::summary() const
{
    PlayHistorySummary summary;

    // Read together, so that listens recorded or removed meanwhile can't leave the count disagreeing with the years
    DbTransaction transaction{db()};

    {
        DbQuery query{db(), u"SELECT COUNT(*), MIN(Timestamp), MAX(Timestamp) FROM PlayHistory;"_s};
        if(query.exec() && query.next()) {
            summary.totalListens    = query.value(0).toInt();
            summary.oldestTimestamp = query.value(1).toLongLong();
            summary.newestTimestamp = query.value(2).toLongLong();
        }
    }

    if(summary.isEmpty()) {
        return summary;
    }

    {
        DbQuery query{db(), u"SELECT DISTINCT Hash FROM PlayHistory;"_s};
        if(query.exec()) {
            while(query.next()) {
                summary.hashes.insert(query.value(0).toString());
            }
        }
    }

    static const QString statement
        = u"SELECT DISTINCT CAST(strftime('%Y', Timestamp / 1000, 'unixepoch', 'localtime') AS INTEGER) AS Year "
          "FROM PlayHistory ORDER BY Year;"_s;

    DbQuery query{db(), statement};
    if(query.exec()) {
        while(query.next()) {
            summary.years.push_back(query.value(0).toInt());
        }
    }

    transaction.commit();
    return summary;
}

std::vector<PlayHistoryEntry> PlayHistoryDatabase::all(bool* ok) const
{
    std::vector<PlayHistoryEntry> entries;
    if(ok) {
        *ok = false;
    }

    static const QString statement
        = u"SELECT Timestamp, Hash, Title, Artists, Album, AlbumArtists, Genres, Composers, Year, Duration, Bitrate, "
          "Lossless, CountedAfter, CountedAt, SourceID, Origin FROM PlayHistory ORDER BY Timestamp, EntryID;"_s;

    // Read through a plain query, as stepping through the rows can fail partway and that has to be told from the end
    QSqlQuery query{db()};
    if(!query.prepare(statement) || !query.exec()) {
        return entries;
    }

    while(query.next()) {
        PlayHistoryEntry entry;
        entry.timestamp    = query.value(0).toLongLong();
        entry.hash         = query.value(1).toString();
        entry.title        = query.value(2).toString();
        entry.artists      = splitList(query.value(3).toString());
        entry.album        = query.value(4).toString();
        entry.albumArtists = splitList(query.value(5).toString());
        entry.genres       = splitList(query.value(6).toString());
        entry.composers    = splitList(query.value(7).toString());
        entry.year         = query.value(8).toInt();
        entry.duration     = query.value(9).toULongLong();
        entry.bitrate      = query.value(10).toInt();
        entry.lossless     = query.value(11).toInt();
        entry.countedAfter = query.value(12).isNull() ? -1 : query.value(12).toLongLong();
        entry.countedAt    = query.value(13).isNull() ? -1 : query.value(13).toLongLong();
        entry.source       = query.value(14).toInt();
        entry.origin       = query.value(15).toString();
        entries.emplace_back(std::move(entry));
    }

    if(ok) {
        *ok = !query.lastError().isValid();
    }
    return entries;
}

QString PlayHistoryDatabase::deviceId() const
{
    SettingsDatabase settings;
    settings.initialise(dbProvider());

    if(QString id = settings.value(QLatin1String{DeviceIdSetting}); !id.isEmpty()) {
        return id;
    }

    // Created the first time it's needed, which can be on more than one thread at once. Only the first one is kept, and
    // it's read back so every caller has the one that was stored
    {
        DbQuery query{db(), u"INSERT OR IGNORE INTO Settings (Name, Value) VALUES (:name, :value);"_s};
        query.bindValue(u":name"_s, QLatin1String{DeviceIdSetting});
        query.bindValue(u":value"_s, QUuid::createUuid().toString(QUuid::WithoutBraces));
        query.exec();
    }

    return settings.value(QLatin1String{DeviceIdSetting});
}

QString PlayHistoryDatabase::deviceName() const
{
    SettingsDatabase settings;
    settings.initialise(dbProvider());

    QString name = settings.value(QLatin1String{DeviceNameSetting});
    if(name.isEmpty()) {
        name = QSysInfo::machineHostName();
    }

    return name;
}

std::vector<PlayHistorySource> PlayHistoryDatabase::sources(bool* ok) const
{
    std::vector<PlayHistorySource> sources;
    bool succeeded{true};

    PlayHistorySource local;
    local.kind     = QLatin1String{PlayHistoryImport::FooyinKind};
    local.deviceId = deviceId();
    local.name     = deviceName();
    sources.push_back(local);

    {
        QSqlQuery query{db()};
        succeeded = query.prepare(u"SELECT SourceID, Kind, DeviceID, Name, LastImported FROM PlayHistorySources "
                                  "WHERE MergedInto = 0 ORDER BY SourceID;"_s)
                 && query.exec();
        if(succeeded) {
            while(query.next()) {
                PlayHistorySource source;
                source.id           = query.value(0).toInt();
                source.kind         = query.value(1).toString();
                source.deviceId     = query.value(2).toString();
                source.name         = query.value(3).toString();
                source.lastImported = query.value(4).toLongLong();
                sources.push_back(source);
            }
            succeeded = !query.lastError().isValid();
        }
    }

    QSqlQuery query{db()};
    const bool counted = query.prepare(u"SELECT SourceID, COUNT(*), MIN(Timestamp), MAX(Timestamp) FROM PlayHistory "
                                       "GROUP BY SourceID;"_s)
                      && query.exec();
    if(counted) {
        while(query.next()) {
            const int id = query.value(0).toInt();
            auto source  = std::ranges::find(sources, id, &PlayHistorySource::id);
            if(source != sources.end()) {
                source->listens     = query.value(1).toInt();
                source->firstListen = query.value(2).toLongLong();
                source->lastListen  = query.value(3).toLongLong();
            }
        }
    }

    if(ok) {
        *ok = succeeded && counted && !query.lastError().isValid() && !local.deviceId.isEmpty();
    }
    return sources;
}

QHash<int, std::vector<PlayHistorySource>> PlayHistoryDatabase::mergedSources(bool* ok) const
{
    QHash<int, std::vector<PlayHistorySource>> merged;

    QSqlQuery query{db()};
    bool succeeded = query.prepare(u"SELECT SourceID, Kind, DeviceID, Name, MergedInto FROM PlayHistorySources "
                                   "WHERE MergedInto != 0 ORDER BY SourceID;"_s)
                  && query.exec();
    if(succeeded) {
        while(query.next()) {
            PlayHistorySource source;
            source.id       = query.value(0).toInt();
            source.kind     = query.value(1).toString();
            source.deviceId = query.value(2).toString();
            source.name     = query.value(3).toString();
            merged[query.value(4).toInt()].push_back(source);
        }
        succeeded = !query.lastError().isValid();
    }

    if(ok) {
        *ok = succeeded;
    }
    return merged;
}

bool PlayHistoryDatabase::addMergedSource(const QString& kind, const QString& deviceId, const QString& name,
                                          int into) const
{
    if(findSource(kind, deviceId) != 0) {
        return true;
    }

    DbQuery query{db(), u"INSERT INTO PlayHistorySources (Kind, DeviceID, Name, MergedInto) VALUES (:kind, :device, "
                        ":name, :into);"_s};
    query.bindValue(u":kind"_s, kind);
    query.bindValue(u":device"_s, deviceId);
    query.bindValue(u":name"_s, name);
    query.bindValue(u":into"_s, into);
    return query.exec();
}

int PlayHistoryDatabase::findSource(const QString& kind, const QString& deviceId) const
{
    DbQuery query{db(), u"SELECT SourceID, MergedInto FROM PlayHistorySources WHERE Kind = :kind AND "
                        "DeviceID = :device;"_s};
    query.bindValue(u":kind"_s, kind);
    query.bindValue(u":device"_s, deviceId);

    if(!query.exec() || !query.next()) {
        return 0;
    }

    // A device that was merged into another is still recognised by its id, as the one it's now part of
    const int merged = query.value(1).toInt();
    return merged != 0 ? merged : query.value(0).toInt();
}

int PlayHistoryDatabase::findOrAddSource(const QString& kind, const QString& deviceId, const QString& name) const
{
    if(const int id = findSource(kind, deviceId); id != 0) {
        return id;
    }

    // Namida devices are told apart by name when importing a backup, so each has to have its own
    QString uniqueName = name;
    if(kind == QLatin1String{PlayHistoryImport::NamidaKind}) {
        for(int n{2}; findNamidaSource(uniqueName) != 0; ++n) {
            uniqueName = u"%1 (%2)"_s.arg(name).arg(n);
        }
    }

    DbQuery query{db(), u"INSERT INTO PlayHistorySources (Kind, DeviceID, Name) VALUES (:kind, :device, :name);"_s};
    query.bindValue(u":kind"_s, kind);
    query.bindValue(u":device"_s, deviceId);
    query.bindValue(u":name"_s, uniqueName);

    return query.exec() ? query.lastInsertId().toInt() : 0;
}

int PlayHistoryDatabase::findNamidaSource(const QString& name) const
{
    // Namida doesn't identify the device, so it's whichever one the person chose by name. A device that was merged into
    // another is still recognised by its name, as the one it's now part of, unless a device has been given it since
    DbQuery query{db(), u"SELECT SourceID, MergedInto FROM PlayHistorySources WHERE Kind = :kind AND Name = :name "
                        "ORDER BY MergedInto != 0, SourceID LIMIT 1;"_s};
    query.bindValue(u":kind"_s, QLatin1String{PlayHistoryImport::NamidaKind});
    query.bindValue(u":name"_s, name);

    if(!query.exec() || !query.next()) {
        return 0;
    }

    const int merged = query.value(1).toInt();
    return merged != 0 ? merged : query.value(0).toInt();
}

int PlayHistoryDatabase::findOrAddNamidaSource(const QString& name) const
{
    if(const int id = findNamidaSource(name); id != 0) {
        return id;
    }

    return findOrAddSource(QLatin1String{PlayHistoryImport::NamidaKind},
                           QUuid::createUuid().toString(QUuid::WithoutBraces), name);
}

void PlayHistoryDatabase::setLastImported(int id, qint64 timestamp) const
{
    DbQuery query{db(), u"UPDATE PlayHistorySources SET LastImported = :timestamp WHERE SourceID = :id;"_s};
    query.bindValue(u":timestamp"_s, timestamp);
    query.bindValue(u":id"_s, id);
    query.exec();
}

PlayHistoryRenameResult PlayHistoryDatabase::renameSource(int id, const QString& name) const
{
    if(id == 0) {
        SettingsDatabase settings;
        settings.initialise(dbProvider());
        return settings.set(QLatin1String{DeviceNameSetting}, name) ? PlayHistoryRenameResult::Renamed
                                                                    : PlayHistoryRenameResult::Failed;
    }

    // Namida devices are told apart by name, so two can't share one
    DbQuery taken{db(), u"SELECT 1 FROM PlayHistorySources AS Other JOIN PlayHistorySources AS This ON "
                        "This.SourceID = :id WHERE Other.Kind = This.Kind AND Other.Kind = :namida AND "
                        "Other.Name = :name AND Other.SourceID != :other AND Other.MergedInto = 0;"_s};
    taken.bindValue(u":id"_s, id);
    taken.bindValue(u":namida"_s, QLatin1String{PlayHistoryImport::NamidaKind});
    taken.bindValue(u":name"_s, name);
    taken.bindValue(u":other"_s, id);
    if(!taken.exec()) {
        return PlayHistoryRenameResult::Failed;
    }
    if(taken.next()) {
        return PlayHistoryRenameResult::NameTaken;
    }

    DbQuery query{db(), u"UPDATE PlayHistorySources SET Name = :name WHERE SourceID = :id;"_s};
    query.bindValue(u":name"_s, name);
    query.bindValue(u":id"_s, id);
    return query.exec() && query.numRowsAffected() > 0 ? PlayHistoryRenameResult::Renamed
                                                       : PlayHistoryRenameResult::Failed;
}

bool PlayHistoryDatabase::mergeSource(int from, int into) const
{
    if(from == 0 || into == 0 || from == into) {
        return false;
    }

    DbTransaction transaction{db()};
    if(!transaction) {
        return false;
    }

    // Only devices of the same kind, as a listen's origin follows from its device
    DbQuery kinds{db(),
                  u"SELECT COUNT(DISTINCT Kind), COUNT(*) FROM PlayHistorySources WHERE SourceID IN (:from, :into) "
                  "AND MergedInto = 0;"_s};
    kinds.bindValue(u":from"_s, from);
    kinds.bindValue(u":into"_s, into);
    if(!kinds.exec() || !kinds.next() || kinds.value(0).toInt() != 1 || kinds.value(1).toInt() != 2) {
        return false;
    }

    DbQuery listens{db(), u"UPDATE PlayHistory SET SourceID = :into WHERE SourceID = :from;"_s};
    listens.bindValue(u":into"_s, into);
    listens.bindValue(u":from"_s, from);

    // The merged device is kept, so that its listens are still recognised as the other's when imported again
    DbQuery sources{db(), u"UPDATE PlayHistorySources SET MergedInto = :into WHERE SourceID = :from OR "
                          "MergedInto = :merged;"_s};
    sources.bindValue(u":into"_s, into);
    sources.bindValue(u":from"_s, from);
    sources.bindValue(u":merged"_s, from);

    return listens.exec() && sources.exec() && transaction.commit();
}

bool PlayHistoryDatabase::removeSource(int id) const
{
    if(id == 0) {
        // This device isn't stored as a source, so only its listens go
        DbQuery listens{db(), u"DELETE FROM PlayHistory WHERE SourceID = 0;"_s};
        return listens.exec();
    }

    DbTransaction transaction{db()};
    if(!transaction) {
        return false;
    }

    DbQuery listens{db(), u"DELETE FROM PlayHistory WHERE SourceID = :id;"_s};
    listens.bindValue(u":id"_s, id);

    DbQuery source{db(), u"DELETE FROM PlayHistorySources WHERE SourceID = :id OR MergedInto = :merged;"_s};
    source.bindValue(u":id"_s, id);
    source.bindValue(u":merged"_s, id);

    return listens.exec() && source.exec() && transaction.commit();
}
} // namespace Fooyin
