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

#include "core/database/dbschema.h"
#include "core/database/playhistorydatabase.h"
#include "core/database/trackdatabase.h"

#include <core/playhistory/playhistory.h>
#include <core/track.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionpool.h>
#include <utils/database/dbconnectionprovider.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include <atomic>

using namespace Qt::StringLiterals;

constexpr auto CurrentSchemaVersion = 26;

namespace Fooyin::Testing {
namespace {
void ensureCoreApplication()
{
    QStandardPaths::setTestModeEnabled(true);

    if(QCoreApplication::instance()) {
        return;
    }

    static int argc{1};
    static char appName[] = "fooyin-playhistory-test";
    static char* argv[]   = {appName, nullptr};
    static QCoreApplication app{argc, argv};
}

DbConnectionPoolPtr createDbPool(const QString& dbPath)
{
    static std::atomic_int counter{0};

    DbConnection::DbParams params;
    params.type           = u"QSQLITE"_s;
    params.connectOptions = u"QSQLITE_OPEN_URI"_s;
    params.filePath       = dbPath;

    auto pool = DbConnectionPool::create(params, u"fooyin-playhistory-test-%1"_s.arg(counter.fetch_add(1)));

    const DbConnectionHandler handler{pool};
    EXPECT_TRUE(handler.hasConnection());

    DbSchema schema{DbConnectionProvider{pool}};
    const auto result = schema.upgradeDatabase(CurrentSchemaVersion, u"://dbschema.xml"_s);
    EXPECT_TRUE(result == DbSchema::UpgradeResult::Success || result == DbSchema::UpgradeResult::IsCurrent);

    return pool;
}

qint64 at(int year, int month, int day, int hour = 12)
{
    return QDateTime{QDate{year, month, day}, QTime{hour, 0}}.toMSecsSinceEpoch();
}

PlayHistoryEntry makeEntry(const QString& hash, qint64 timestamp)
{
    PlayHistoryEntry entry;
    entry.timestamp = timestamp;
    entry.hash      = hash;
    entry.title     = u"Title "_s + hash;
    entry.artists   = {u"Artist"_s};
    entry.duration  = 200'000;
    return entry;
}
} // namespace

class PlayHistoryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureCoreApplication();
        ASSERT_TRUE(m_dir.isValid());

        m_dbPath = m_dir.filePath(u"fooyin.db"_s);
        m_pool   = createDbPool(m_dbPath);
        m_handler.emplace(m_pool);
    }

    void TearDown() override
    {
        m_handler.reset();
    }

    [[nodiscard]] PlayHistoryDatabase database() const
    {
        PlayHistoryDatabase db;
        db.initialise(DbConnectionProvider{m_pool});
        return db;
    }

    QTemporaryDir m_dir;
    QString m_dbPath;
    DbConnectionPoolPtr m_pool;
    std::optional<DbConnectionHandler> m_handler;
};

TEST_F(PlayHistoryTest, RecordsTheMetadataOfAPlayedTrack)
{
    PlayHistory history{m_pool};
    QSignalSpy recorded{&history, &PlayHistory::playRecorded};

    Track track{u"/music/band/album/01 song.flac"_s};
    track.setTitle(u"Song"_s);
    track.setArtists({u"Band"_s, u"Guest"_s});
    track.setAlbum(u"Album"_s);
    track.setAlbumArtists({u"Band"_s});
    track.setGenres({u"Rock"_s, u"Indie"_s});
    track.setComposers({u"Writer"_s});
    track.setYear(1999);
    track.setDuration(215'000);
    track.setBitrate(900);
    track.setCodec(u"FLAC"_s);
    track.generateHash();

    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    history.recordPlay(track, 123'456, 130'000);
    history.waitForWrites();
    QCoreApplication::processEvents();
    const qint64 after = QDateTime::currentMSecsSinceEpoch();

    EXPECT_EQ(recorded.count(), 1);

    const auto data = history.load(0, after + 1000).result();
    ASSERT_EQ(data.entries.size(), 1U);

    const auto& entry = data.entries.front();
    EXPECT_GE(entry.timestamp, before);
    EXPECT_LE(entry.timestamp, after);
    EXPECT_EQ(entry.hash, track.hash());
    EXPECT_EQ(entry.title, u"Song"_s);
    EXPECT_EQ(entry.artists, (QStringList{u"Band"_s, u"Guest"_s}));
    EXPECT_EQ(entry.album, u"Album"_s);
    EXPECT_EQ(entry.albumArtists, QStringList{u"Band"_s});
    EXPECT_EQ(entry.genres, (QStringList{u"Rock"_s, u"Indie"_s}));
    EXPECT_EQ(entry.composers, QStringList{u"Writer"_s});
    EXPECT_EQ(entry.year, 1999);
    EXPECT_EQ(entry.duration, 215'000U);
    EXPECT_EQ(entry.bitrate, 900);
    EXPECT_EQ(entry.lossless, 1);
    EXPECT_EQ(entry.countedAfter, 123'456);
    EXPECT_EQ(entry.countedAt, 130'000);
}

TEST_F(PlayHistoryTest, ListensRecordedWithoutATimeToBeCountedHaveNone)
{
    PlayHistory history{m_pool};

    Track track{u"/music/song.mp3"_s};
    track.setTitle(u"Song"_s);
    track.generateHash();
    history.recordPlay(track);
    history.waitForWrites();

    const auto data = history.load(0, QDateTime::currentMSecsSinceEpoch() + 1000).result();
    ASSERT_EQ(data.entries.size(), 1U);
    EXPECT_EQ(data.entries.front().countedAfter, -1);
    EXPECT_EQ(data.entries.front().countedAt, -1);
}

TEST_F(PlayHistoryTest, RecordingAListenDoesNotWaitForABusyDatabaseAndIsNotLostToIt)
{
    PlayHistory history{m_pool};
    QSignalSpy recorded{&history, &PlayHistory::playRecorded};

    // Something else is partway through writing, as a library scan can be, so the database is locked
    const auto connectionName = u"playhistory-lock-holder"_s;
    {
        auto holder = QSqlDatabase::addDatabase(u"QSQLITE"_s, connectionName);
        holder.setDatabaseName(m_dbPath);
        ASSERT_TRUE(holder.open());
        ASSERT_TRUE(holder.transaction());
        QSqlQuery lock{holder};
        ASSERT_TRUE(lock.exec(u"INSERT INTO Libraries (Name, Path) VALUES ('Busy', '/busy');"_s));

        Track track{u"/music/song.mp3"_s};
        track.setTitle(u"Song"_s);
        track.generateHash();

        QElapsedTimer timer;
        timer.start();
        history.recordPlay(track);
        // The caller carries on straight away rather than waiting for the lock
        EXPECT_LT(timer.elapsed(), 200);

        QThread::msleep(400);
        ASSERT_TRUE(holder.commit());
        holder.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    // Once the lock was let go the listen was stored
    history.waitForWrites();
    QCoreApplication::processEvents();
    EXPECT_EQ(recorded.count(), 1);
    EXPECT_EQ(history.load(0, QDateTime::currentMSecsSinceEpoch() + 1000).result().entries.size(), 1U);
}

TEST_F(PlayHistoryTest, WorksOutWhetherTheCodecIsLossless)
{
    PlayHistory history{m_pool};

    const auto record = [&history](const QString& name, const QString& codec) {
        Track track{u"/music/"_s + name};
        track.setTitle(name);
        track.setCodec(codec);
        track.generateHash();
        history.recordPlay(track);
    };
    record(u"a.flac"_s, u"FLAC"_s);
    record(u"b.mp3"_s, u"MP3"_s);
    record(u"c.xyz"_s, {});
    history.waitForWrites();

    auto data = history.load(0, QDateTime::currentMSecsSinceEpoch() + 1000).result();
    ASSERT_EQ(data.entries.size(), 3U);

    std::ranges::sort(data.entries, {}, &PlayHistoryEntry::title);
    EXPECT_EQ(data.entries[0].lossless, 1);
    EXPECT_EQ(data.entries[1].lossless, 0);
    EXPECT_EQ(data.entries[2].lossless, -1);
}

TEST_F(PlayHistoryTest, LoadsOnlyTheListensInTheRangeOldestFirst)
{
    const auto db = database();
    ASSERT_TRUE(db.insert(makeEntry(u"late"_s, at(2025, 3, 3))));
    ASSERT_TRUE(db.insert(makeEntry(u"early"_s, at(2025, 3, 1))));
    ASSERT_TRUE(db.insert(makeEntry(u"before"_s, at(2025, 2, 1))));
    ASSERT_TRUE(db.insert(makeEntry(u"after"_s, at(2025, 4, 1))));

    // The end of the range is exclusive
    const auto data = db.load(at(2025, 3, 1), at(2025, 4, 1));

    ASSERT_EQ(data.entries.size(), 2U);
    EXPECT_EQ(data.entries[0].hash, u"early"_s);
    EXPECT_EQ(data.entries[1].hash, u"late"_s);
}

TEST_F(PlayHistoryTest, LoadsWhatWasHeardOutsideTheRangeToPutListensInContext)
{
    const auto db = database();

    auto early      = makeEntry(u"a"_s, at(2024, 1, 10));
    early.artists   = {u"First Artist"_s};
    auto older      = makeEntry(u"a"_s, at(2024, 6, 10));
    older.artists   = {u"First Artist"_s};
    auto inRange    = makeEntry(u"a"_s, at(2025, 1, 10));
    inRange.artists = {u"First Artist"_s, u"Second Artist"_s};
    auto other      = makeEntry(u"b"_s, at(2025, 1, 11));
    other.artists   = {u"Second Artist"_s};

    for(const auto& entry : {early, older, inRange, other}) {
        ASSERT_TRUE(db.insert(entry));
    }

    const auto data = db.load(at(2025, 1, 1), at(2025, 2, 1));

    EXPECT_EQ(data.entries.size(), 2U);
    EXPECT_EQ(data.oldestTimestamp, early.timestamp);
    EXPECT_EQ(data.firstListenByHash.value(u"a"_s), early.timestamp);
    EXPECT_EQ(data.firstListenByHash.value(u"b"_s), other.timestamp);
    EXPECT_EQ(data.lastListenBeforeByHash.value(u"a"_s), older.timestamp);
    EXPECT_FALSE(data.lastListenBeforeByHash.contains(u"b"_s));
    EXPECT_EQ(data.firstListenByArtist.value(u"First Artist"_s), early.timestamp);
    EXPECT_EQ(data.firstListenByArtist.value(u"Second Artist"_s), inRange.timestamp);
}

TEST_F(PlayHistoryTest, SummarisesTheWholeHistory)
{
    const auto db = database();

    EXPECT_TRUE(db.summary().isEmpty());

    ASSERT_TRUE(db.insert(makeEntry(u"a"_s, at(2025, 7, 1))));
    ASSERT_TRUE(db.insert(makeEntry(u"b"_s, at(2023, 5, 1))));
    ASSERT_TRUE(db.insert(makeEntry(u"c"_s, at(2025, 12, 31, 20))));

    const auto summary = db.summary();

    EXPECT_FALSE(summary.isEmpty());
    EXPECT_EQ(summary.totalListens, 3);
    EXPECT_EQ(summary.oldestTimestamp, at(2023, 5, 1));
    EXPECT_EQ(summary.newestTimestamp, at(2025, 12, 31, 20));
    // Years with nothing in them are left out
    EXPECT_EQ(summary.years, (std::vector<int>{2023, 2025}));
    EXPECT_EQ(summary.hashes, (QSet<QString>{u"a"_s, u"b"_s, u"c"_s}));
}

TEST_F(PlayHistoryTest, FollowsATrackWhenRetaggingChangesItsHash)
{
    const auto db = database();
    TrackDatabase trackDb;
    trackDb.initialise(DbConnectionProvider{m_pool});

    Track track{u"/music/song.flac"_s};
    track.setTitle(u"Old title"_s);
    track.generateHash();
    const QString oldHash = track.hash();

    TrackList tracks{track};
    ASSERT_TRUE(trackDb.storeTracks(tracks));
    ASSERT_TRUE(db.insert(makeEntry(oldHash, at(2025, 3, 1))));
    ASSERT_TRUE(db.insert(makeEntry(oldHash, at(2025, 3, 2))));

    tracks.front().setTitle(u"New title"_s);
    ASSERT_NE(tracks.front().hash(), oldHash);
    ASSERT_TRUE(trackDb.updateTrack(tracks.front()));

    // Plays from before the retag are still the same track
    const auto data = db.load(0, at(2026, 1, 1));
    ASSERT_EQ(data.entries.size(), 2U);
    EXPECT_EQ(data.entries[0].hash, tracks.front().hash());
    EXPECT_EQ(data.entries[1].hash, tracks.front().hash());
    EXPECT_EQ(data.entries[0].title, u"New title"_s);
}

TEST_F(PlayHistoryTest, ChangingGenresKeepsListensWithTheTrackAndUpdatesTheirGenres)
{
    const auto db = database();
    TrackDatabase trackDb;
    trackDb.initialise(DbConnectionProvider{m_pool});

    Track track{u"/music/song.flac"_s};
    track.setTitle(u"Song"_s);
    track.setGenres({u"Rock"_s});
    track.generateHash();
    const QString hash = track.hash();

    TrackList tracks{track};
    ASSERT_TRUE(trackDb.storeTracks(tracks));
    ASSERT_TRUE(db.insert(PlayHistoryEntry::fromTrack(tracks.front(), at(2025, 3, 1))));

    tracks.front().setGenres({u"Jazz"_s, u"Blues"_s});
    ASSERT_EQ(tracks.front().hash(), hash);
    ASSERT_TRUE(trackDb.updateTrack(tracks.front()));

    // Still the same track, now counted under its new genres
    const auto data = db.load(0, at(2026, 1, 1));
    ASSERT_EQ(data.entries.size(), 1U);
    EXPECT_EQ(data.entries.front().hash, hash);
    EXPECT_EQ(data.entries.front().genres, (QStringList{u"Jazz"_s, u"Blues"_s}));
}

TEST_F(PlayHistoryTest, UpdatingATrackLeavesHowEachListenSoundedAlone)
{
    const auto db = database();
    TrackDatabase trackDb;
    trackDb.initialise(DbConnectionProvider{m_pool});

    // A track that was listened to as a FLAC, and which has a lossy copy with the same tags
    Track track{u"/music/song.mp3"_s};
    track.setTitle(u"Song"_s);
    track.setCodec(u"MP3"_s);
    track.setBitrate(128);
    track.setDuration(180'000);
    track.generateHash();

    TrackList tracks{track};
    ASSERT_TRUE(trackDb.storeTracks(tracks));

    auto flacListen     = makeEntry(tracks.front().hash(), at(2025, 3, 1));
    flacListen.bitrate  = 900;
    flacListen.lossless = 1;
    flacListen.duration = 200'000;
    ASSERT_TRUE(db.insert(flacListen));

    // Updating the copy refreshes the tags, but a listen of the other copy didn't sound like this one
    tracks.front().setGenres({u"Jazz"_s});
    ASSERT_TRUE(trackDb.updateTrack(tracks.front()));

    const auto data = db.load(0, at(2026, 1, 1));
    ASSERT_EQ(data.entries.size(), 1U);
    EXPECT_EQ(data.entries.front().genres, QStringList{u"Jazz"_s});
    EXPECT_EQ(data.entries.front().bitrate, 900);
    EXPECT_EQ(data.entries.front().lossless, 1);
    EXPECT_EQ(data.entries.front().duration, 200'000U);
}

TEST_F(PlayHistoryTest, KeepsTheAlbumOfListensUpToDateWhenOnlyTheAlbumArtistChanges)
{
    const auto db = database();
    TrackDatabase trackDb;
    trackDb.initialise(DbConnectionProvider{m_pool});

    // The album artist isn't part of a track's hash, so correcting it leaves the hash as it was
    Track track{u"/music/song.flac"_s};
    track.setTitle(u"Song"_s);
    track.setAlbum(u"Album"_s);
    track.setAlbumArtists({u"Old Artist"_s});
    track.generateHash();
    const QString hash = track.hash();

    TrackList tracks{track};
    ASSERT_TRUE(trackDb.storeTracks(tracks));
    ASSERT_TRUE(db.insert(PlayHistoryEntry::fromTrack(tracks.front(), at(2025, 3, 1))));

    tracks.front().setAlbumArtists({u"New Artist"_s});
    ASSERT_EQ(tracks.front().hash(), hash);
    ASSERT_TRUE(trackDb.updateTrack(tracks.front()));

    // So the listen belongs to the same album as the track does now
    const auto data = db.load(0, at(2026, 1, 1));
    ASSERT_EQ(data.entries.size(), 1U);
    EXPECT_EQ(data.entries.front().albumArtists, QStringList{u"New Artist"_s});
    EXPECT_EQ(data.entries.front().albumKey(), PlayHistoryEntry::albumKey(u"Album"_s, {u"New Artist"_s}));
}

TEST_F(PlayHistoryTest, LeavesHistoryAloneWhileAnotherTrackStillHasTheOldHash)
{
    const auto db = database();
    TrackDatabase trackDb;
    trackDb.initialise(DbConnectionProvider{m_pool});

    // Two copies of the same track, so they share a hash
    Track first{u"/music/a/song.flac"_s};
    first.setTitle(u"Song"_s);
    first.generateHash();
    Track second{u"/music/b/song.flac"_s};
    second.setTitle(u"Song"_s);
    second.generateHash();
    ASSERT_EQ(first.hash(), second.hash());
    const QString sharedHash = first.hash();

    TrackList tracks{first, second};
    ASSERT_TRUE(trackDb.storeTracks(tracks));
    ASSERT_TRUE(db.insert(makeEntry(sharedHash, at(2025, 3, 1))));

    tracks.front().setTitle(u"Renamed"_s);
    ASSERT_TRUE(trackDb.updateTrack(tracks.front()));

    // Which of the copies the listen was of isn't known, so it stays with the one that still has the old hash
    const auto data = db.load(0, at(2026, 1, 1));
    ASSERT_EQ(data.entries.size(), 1U);
    EXPECT_EQ(data.entries.front().hash, sharedHash);
}

TEST_F(PlayHistoryTest, KeepsValuesWithoutMetadata)
{
    const auto db = database();

    PlayHistoryEntry bare;
    bare.timestamp = at(2025, 1, 1);
    bare.hash      = u"bare"_s;
    ASSERT_TRUE(db.insert(bare));

    const auto data = db.load(0, at(2026, 1, 1));
    ASSERT_EQ(data.entries.size(), 1U);
    EXPECT_TRUE(data.entries.front().artists.isEmpty());
    EXPECT_TRUE(data.entries.front().genres.isEmpty());
    EXPECT_EQ(data.entries.front().lossless, -1);
}
} // namespace Fooyin::Testing
