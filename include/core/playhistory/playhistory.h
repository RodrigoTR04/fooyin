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

#include "fycore_export.h"

#include <core/track.h>
#include <utils/database/dbconnectionpool.h>

#include <QFuture>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <memory>
#include <vector>

namespace Fooyin {

/*!
 * A single recorded listen.
 *
 * Track metadata is copied into the entry at the time of the listen, so history remains meaningful
 * if the track is later retagged, moved or removed from the library.
 */
struct FYCORE_EXPORT PlayHistoryEntry
{
    //! Milliseconds since the Unix epoch at which the listen was counted.
    qint64 timestamp{0};
    QString hash;
    QString title;
    QStringList artists;
    QString album;
    QStringList albumArtists;
    QStringList genres;
    QStringList composers;
    int year{0};
    //! Track duration in milliseconds.
    uint64_t duration{0};
    //! Bitrate in kbps, or 0 when unknown.
    int bitrate{0};
    //! 1 for lossless codecs, 0 for lossy codecs, -1 when unknown.
    int lossless{-1};
    //! How long, in milliseconds, the track had been listened to when the listen was counted, which depends on the
    //! playback settings. -1 when unknown.
    qint64 countedAfter{-1};
    //! The position in the track, in milliseconds, when the listen was counted. This differs from countedAfter when
    //! part of the track was skipped. -1 when unknown.
    qint64 countedAt{-1};
    //! The device the listen was imported from (see PlayHistorySource), or 0 if it was recorded here.
    int source{0};
    //! Where the listen was recorded, which together with the timestamp identifies it on every device it's imported
    //! to. Empty for listens recorded here.
    QString origin;

    //! Identifies the album this entry belongs to, or an empty string when it has no album.
    [[nodiscard]] QString albumKey() const;

    //! Builds the key identifying an album from its title and album artists.
    [[nodiscard]] static QString albumKey(const QString& album, const QStringList& albumArtists);

    static PlayHistoryEntry fromTrack(const Track& track, qint64 timestamp, qint64 countedAfter = -1,
                                      qint64 countedAt = -1);
};

/*!
 * Listens within a time range, along with the lifetime aggregates needed to put them in context
 * (e.g. whether a track was first heard in the range).
 */
struct FYCORE_EXPORT PlayHistoryData
{
    //! Listens in the requested range, oldest first.
    std::vector<PlayHistoryEntry> entries;
    //! Timestamp of the earliest listen of any entry in the history.
    qint64 oldestTimestamp{0};
    //! First ever listen of each track hash.
    QHash<QString, qint64> firstListenByHash;
    //! Most recent listen of each track hash before the start of the requested range.
    QHash<QString, qint64> lastListenBeforeByHash;
    //! First ever listen of each artist.
    QHash<QString, qint64> firstListenByArtist;
};

//! Overview of everything in the history.
struct FYCORE_EXPORT PlayHistorySummary
{
    int totalListens{0};
    qint64 oldestTimestamp{0};
    qint64 newestTimestamp{0};
    //! Calendar years (local time) containing at least one listen, ascending.
    std::vector<int> years;
    //! Every track (by hash) that has been listened to at least once.
    QSet<QString> hashes;

    [[nodiscard]] bool isEmpty() const
    {
        return totalListens == 0;
    }
};

//! A device that listens in the history were recorded on.
struct FYCORE_EXPORT PlayHistorySource
{
    //! 0 for this device.
    int id{0};
    //! The app the listens were recorded with: "fooyin" or "namida".
    QString kind;
    QString deviceId;
    QString name;
    int listens{0};
    qint64 firstListen{0};
    qint64 lastListen{0};
    //! When history was last imported from this device, or 0 if it never was.
    qint64 lastImported{0};

    [[nodiscard]] bool isLocal() const
    {
        return id == 0;
    }
};

enum class PlayHistoryFormat : uint8_t
{
    Unknown,
    //! A listening history exported by fooyin.
    Fooyin,
    //! A backup made by Namida, or the history files in one.
    Namida,
};

enum class PlayHistoryRenameResult : uint8_t
{
    Renamed,
    //! Another Namida device already has the name. They're told apart by name, so two can't share one.
    NameTaken,
    Failed,
};

struct FYCORE_EXPORT PlayHistoryImportResult
{
    //! Why nothing was imported, or empty if the import succeeded.
    QString error;
    //! The devices that listens were imported from.
    QStringList devices;
    //! Listens added to the history.
    int added{0};
    //! Listens skipped because they had already been imported.
    int duplicates{0};
    //! Listens skipped because the track couldn't be found in the library. They're imported if the file is imported
    //! again once the track is in the library.
    int unmatched{0};
    //! The files that unmatched listens were of.
    QStringList unmatchedTracks;
    //! Listens skipped because the file had them down with an impossible time or length.
    int invalid{0};

    [[nodiscard]] bool succeeded() const
    {
        return error.isEmpty();
    }
};

/*!
 * Records each counted listen so that statistics can be derived from them.
 *
 * Fooyin's per-track play count only keeps totals and the first/last play times, which can't answer
 * questions such as when or how often a track was played. Every time a listen is counted the track's
 * metadata and the time are stored here instead.
 */
class FYCORE_EXPORT PlayHistory : public QObject
{
    Q_OBJECT

public:
    explicit PlayHistory(DbConnectionPoolPtr dbPool, QObject* parent = nullptr);
    ~PlayHistory() override;

    /*!
     * Records a listen of @p track at the current time, which was counted once it had been listened to for
     * @p countedAfter milliseconds and had got to @p countedAt milliseconds in (if known).
     *
     * The listen is stored in the background, retrying if the database is busy, as it can be while the library is
     * being scanned. playRecorded() is emitted once it has been stored.
     */
    void recordPlay(const Track& track, qint64 countedAfter = -1, qint64 countedAt = -1);
    //! Waits for listens being recorded to be stored. This is done when the history is deleted so none are lost.
    void waitForWrites();

    //! Loads the listens in [@p from, @p to) (milliseconds since the epoch) without blocking the caller.
    [[nodiscard]] QFuture<PlayHistoryData> load(qint64 from, qint64 to) const;
    [[nodiscard]] QFuture<PlayHistorySummary> summary() const;

    //! The devices listens have been recorded on, starting with this one.
    [[nodiscard]] QFuture<std::vector<PlayHistorySource>> sources() const;
    //! Renames the device with @p id, which is this one if @p id is 0.
    QFuture<PlayHistoryRenameResult> renameSource(int id, const QString& name);
    //! Removes the device with @p id along with every listen imported from it. If @p id is 0, this device is kept but
    //! every listen recorded on it is removed.
    QFuture<bool> removeSource(int id);
    /*!
     * Merges the device with @p from into the one with @p into, which has to be of the same kind, for when they turn
     * out to be the same device. History imported from either later on goes to @p into.
     */
    QFuture<bool> mergeSource(int from, int into);

    //! What kind of history the file at @p filepath holds.
    [[nodiscard]] static PlayHistoryFormat fileFormat(const QString& filepath);

    /*!
     * Adds the listens in the file at @p filepath to the history, skipping any that were imported before so that
     * importing a newer export from the same device only adds what's new.
     *
     * Listens exported by fooyin say which device they were recorded on. Namida's don't, so they're attributed to
     * the device named @p namidaDevice, and matched to the tracks in @p libraryTracks by their path on the phone.
     */
    QFuture<PlayHistoryImportResult> importFile(const QString& filepath, const QString& namidaDevice,
                                                const TrackList& libraryTracks);
    //! Writes the whole history, including what was imported from other devices, to @p filepath. Returns an error
    //! message, or an empty string if it was written.
    [[nodiscard]] QFuture<QString> exportFile(const QString& filepath) const;

Q_SIGNALS:
    void playRecorded();
    //! Emitted when listens have been imported or removed.
    void historyChanged();
    //! Emitted when an import had no new listens, though what's known about the devices may still have changed.
    void sourcesChanged();

private:
    DbConnectionPoolPtr m_dbPool;
    //! Listens are written one at a time, off the thread of the interface.
    QThreadPool m_writePool;
};
} // namespace Fooyin
