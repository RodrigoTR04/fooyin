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

#include <core/playhistory/playhistorystats.h>

#include <QDateTime>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
// Dates in June avoid daylight saving transitions in the zones the tests are likely to be run in.
// 6 June 2026 is a Saturday.
const QDate Saturday{2026, 6, 6};

qint64 at(const QDate& day, int hour, int minute = 0)
{
    return QDateTime{day, QTime{hour, minute}}.toMSecsSinceEpoch();
}

PlayHistoryEntry makeEntry(const QString& hash, qint64 timestamp)
{
    PlayHistoryEntry entry;
    entry.timestamp = timestamp;
    entry.hash      = hash;
    entry.title     = u"Title "_s + hash;
    entry.artists   = {u"Artist "_s + hash};
    entry.duration  = 180'000;
    return entry;
}

PlayHistoryData makeData(std::vector<PlayHistoryEntry> entries)
{
    PlayHistoryData data;
    data.entries = std::move(entries);
    std::ranges::sort(data.entries, {}, &PlayHistoryEntry::timestamp);

    // Everything is new unless a test says otherwise
    for(const auto& entry : data.entries) {
        if(!data.firstListenByHash.contains(entry.hash)) {
            data.firstListenByHash.insert(entry.hash, entry.timestamp);
        }
        for(const QString& artist : entry.artists) {
            if(!data.firstListenByArtist.contains(artist)) {
                data.firstListenByArtist.insert(artist, entry.timestamp);
            }
        }
    }
    if(!data.entries.empty()) {
        data.oldestTimestamp = data.entries.front().timestamp;
    }
    return data;
}
} // namespace

TEST(PlayHistoryStatsTest, EmptyHistoryHasNothingToShow)
{
    const auto stats = computePlayHistoryStats({}, Saturday, Saturday.addDays(6));

    EXPECT_TRUE(stats.isEmpty());
    EXPECT_EQ(stats.totalDays(), 7);
    EXPECT_EQ(stats.listensPerDay.size(), 7U);
    EXPECT_EQ(stats.daysWithListens, 0);
    EXPECT_EQ(stats.avgFirstMinute, -1);
    EXPECT_FALSE(stats.longestStreak.has_value());
}

TEST(PlayHistoryStatsTest, CountsListensPerDayAndRanksTracks)
{
    const auto data = makeData({makeEntry(u"a"_s, at(Saturday, 10)), makeEntry(u"a"_s, at(Saturday, 11)),
                                makeEntry(u"b"_s, at(Saturday, 12)), makeEntry(u"a"_s, at(Saturday.addDays(2), 9))});

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday.addDays(3));

    EXPECT_EQ(stats.totalListens, 4);
    EXPECT_EQ(stats.daysWithListens, 2);
    EXPECT_EQ(stats.uniqueTracks, 2);
    EXPECT_EQ(stats.listensPerDay, (std::vector<int>{3, 0, 1, 0}));
    EXPECT_EQ(stats.listensOnDay(Saturday), 3);
    EXPECT_EQ(stats.listensOnDay(Saturday.addDays(10)), 0);
    EXPECT_EQ(stats.peakDay, Saturday);
    EXPECT_EQ(stats.peakDayListens, 3);

    ASSERT_EQ(stats.topTracks.size(), 2U);
    EXPECT_EQ(stats.topTracks[0].hash, u"a"_s);
    EXPECT_EQ(stats.topTracks[0].count, 3);
    EXPECT_EQ(stats.topTracks[0].title, u"Title a"_s);
    EXPECT_EQ(stats.topTracks[1].hash, u"b"_s);
}

TEST(PlayHistoryStatsTest, IgnoresListensOutsideTheRange)
{
    const auto data
        = makeData({makeEntry(u"a"_s, at(Saturday.addDays(-1), 23, 59)), makeEntry(u"b"_s, at(Saturday, 0, 0)),
                    makeEntry(u"c"_s, at(Saturday.addDays(1), 0, 0))});

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday);

    EXPECT_EQ(stats.totalListens, 1);
    ASSERT_EQ(stats.topTracks.size(), 1U);
    EXPECT_EQ(stats.topTracks.front().hash, u"b"_s);
}

TEST(PlayHistoryStatsTest, RaisesFirstDayToTheOldestListen)
{
    const QDate oldest = Saturday.addDays(3);
    const auto data    = makeData({makeEntry(u"a"_s, at(oldest, 12)), makeEntry(u"a"_s, at(oldest.addDays(1), 12))});

    const auto stats = computePlayHistoryStats(data, Saturday.addDays(-365), Saturday.addDays(10));

    EXPECT_EQ(stats.firstDay, oldest);
    EXPECT_EQ(stats.lastDay, Saturday.addDays(10));
    EXPECT_EQ(stats.totalDays(), 8);
}

TEST(PlayHistoryStatsTest, WorksOutHoursAndWeekdaysFromSundayFirst)
{
    // 6 June 2026 is a Saturday, the last day when weeks start on Sunday
    const auto data
        = makeData({makeEntry(u"a"_s, at(Saturday, 22, 15)), makeEntry(u"b"_s, at(Saturday.addDays(1), 7, 5))});

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday.addDays(1));

    EXPECT_EQ(stats.hours[22], 1);
    EXPECT_EQ(stats.hours[7], 1);
    EXPECT_EQ(stats.weekdays[6], 1);
    EXPECT_EQ(stats.weekdays[0], 1);
    EXPECT_EQ(stats.weekHours[6 * 24 + 22], 1);
    EXPECT_EQ(stats.weekHours[0 * 24 + 7], 1);
    EXPECT_EQ(stats.months[5], 2);
}

TEST(PlayHistoryStatsTest, AveragesTheFirstAndLastListenOfEachDay)
{
    const auto data = makeData({makeEntry(u"a"_s, at(Saturday, 8, 0)), makeEntry(u"a"_s, at(Saturday, 20, 0)),
                                makeEntry(u"a"_s, at(Saturday.addDays(1), 10, 0)),
                                makeEntry(u"a"_s, at(Saturday.addDays(1), 22, 0))});

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday.addDays(1));

    EXPECT_EQ(stats.avgFirstMinute, 9 * 60);
    EXPECT_EQ(stats.avgLastMinute, 21 * 60);
}

TEST(PlayHistoryStatsTest, FindsTheLongestStreakAndTheCurrentOne)
{
    // Listens on days 0-2 and 4-5, with the range ending on day 5
    std::vector<PlayHistoryEntry> entries;
    for(const int day : {0, 1, 2, 4, 5}) {
        entries.push_back(makeEntry(u"a"_s, at(Saturday.addDays(day), 12)));
    }

    const auto stats = computePlayHistoryStats(makeData(std::move(entries)), Saturday, Saturday.addDays(5));

    ASSERT_TRUE(stats.longestStreak.has_value());
    EXPECT_EQ(stats.longestStreak->days, 3);
    EXPECT_EQ(stats.longestStreak->endDay, Saturday.addDays(2));
    EXPECT_EQ(stats.longestStreak->startDay(), Saturday);
    EXPECT_EQ(stats.currentStreak, 2);
}

TEST(PlayHistoryStatsTest, CurrentStreakSurvivesADayWithoutListensYet)
{
    // Nothing yet on the last day, as can be the case early on the day itself
    const auto data = makeData({makeEntry(u"a"_s, at(Saturday, 12)), makeEntry(u"a"_s, at(Saturday.addDays(1), 12))});

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday.addDays(2));

    EXPECT_EQ(stats.currentStreak, 2);
}

TEST(PlayHistoryStatsTest, SplitsSessionsOnLongGaps)
{
    // Three listens 3 minutes apart, then another hour later
    const auto data = makeData({makeEntry(u"a"_s, at(Saturday, 10, 0)), makeEntry(u"b"_s, at(Saturday, 10, 3)),
                                makeEntry(u"c"_s, at(Saturday, 10, 6)), makeEntry(u"d"_s, at(Saturday, 11, 10))});

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday);

    ASSERT_TRUE(stats.longestSession.has_value());
    EXPECT_EQ(stats.longestSession->listens, 3);
    EXPECT_EQ(stats.longestSession->durationMs(), 6 * 60 * 1000);
    EXPECT_EQ(stats.longestSession->startMs, at(Saturday, 10, 0));
}

TEST(PlayHistoryStatsTest, LongTracksPlayedBackToBackStayInOneSession)
{
    // Three 20 minute tracks counted 21 minutes apart, as they would be playing one after the other
    std::vector<PlayHistoryEntry> entries;
    for(int i{0}; i < 3; ++i) {
        auto entry     = makeEntry(QString::number(i), at(Saturday, 10, 0) + i * 21LL * 60 * 1000);
        entry.duration = 20 * 60 * 1000;
        entries.push_back(entry);
    }

    const auto stats = computePlayHistoryStats(makeData(std::move(entries)), Saturday, Saturday);

    ASSERT_TRUE(stats.longestSession.has_value());
    EXPECT_EQ(stats.longestSession->listens, 3);
}

TEST(PlayHistoryStatsTest, ALongTrackAfterAShortOneOnlyAllowsForTheTimeItTakesToCount)
{
    // After a three minute track, a long one is counted about five minutes later if listening carried on. Forty
    // minutes is well beyond that, however long the track, so this is a break
    auto first      = makeEntry(u"a"_s, at(Saturday, 10, 0));
    first.duration  = 3 * 60 * 1000;
    auto second     = makeEntry(u"b"_s, at(Saturday, 10, 40));
    second.duration = 60 * 60 * 1000;

    EXPECT_FALSE(computePlayHistoryStats(makeData({first, second}), Saturday, Saturday).longestSession.has_value());
}

TEST(PlayHistoryStatsTest, UsesTheTimeListensWereCountedAfterWhenItIsKnown)
{
    // A short track followed by a long one, with the playback settings set so listens are counted at the end of a
    // track. The second is counted 40 minutes after the first, as it would be playing straight on. With the default
    // settings that would be a break, but it isn't for the settings they were recorded with
    auto first          = makeEntry(u"a"_s, at(Saturday, 10, 0));
    first.duration      = 3 * 60 * 1000;
    first.countedAfter  = 3 * 60 * 1000;
    auto second         = makeEntry(u"b"_s, at(Saturday, 10, 40));
    second.duration     = 40 * 60 * 1000;
    second.countedAfter = 40 * 60 * 1000;

    const auto stats = computePlayHistoryStats(makeData({first, second}), Saturday, Saturday);

    ASSERT_TRUE(stats.longestSession.has_value());
    EXPECT_EQ(stats.longestSession->listens, 2);

    // Without that, the default is assumed
    first.countedAfter  = -1;
    second.countedAfter = -1;
    EXPECT_FALSE(computePlayHistoryStats(makeData({first, second}), Saturday, Saturday).longestSession.has_value());
}

TEST(PlayHistoryStatsTest, SkippingThroughATrackLeavesLessOfItToPlay)
{
    // A 40 minute track listened to for the four minutes it takes to count, but those were the last four, the rest
    // having been skipped. When the next listen is counted 30 minutes later it can't have been playing straight on
    auto first          = makeEntry(u"a"_s, at(Saturday, 10, 0));
    first.duration      = 40 * 60 * 1000;
    first.countedAfter  = 4 * 60 * 1000;
    first.countedAt     = 40 * 60 * 1000;
    auto second         = makeEntry(u"b"_s, at(Saturday, 10, 30));
    second.duration     = 3 * 60 * 1000;
    second.countedAfter = 90 * 1000;
    second.countedAt    = 90 * 1000;

    EXPECT_FALSE(computePlayHistoryStats(makeData({first, second}), Saturday, Saturday).longestSession.has_value());

    // If it wasn't known where in the track it was counted, it would be taken to have been listened to from the start,
    // and there would seem to be plenty of it left to play
    first.countedAt  = -1;
    const auto stats = computePlayHistoryStats(makeData({first, second}), Saturday, Saturday);
    ASSERT_TRUE(stats.longestSession.has_value());
    EXPECT_EQ(stats.longestSession->listens, 2);
}

TEST(PlayHistoryStatsTest, AGapLongerThanTheTrackStillEndsASession)
{
    // The same tracks, but an hour apart, which is a break whatever their length
    std::vector<PlayHistoryEntry> entries;
    for(int i{0}; i < 2; ++i) {
        auto entry     = makeEntry(QString::number(i), at(Saturday, 10, 0) + i * 60LL * 60 * 1000);
        entry.duration = 20 * 60 * 1000;
        entries.push_back(entry);
    }

    EXPECT_FALSE(computePlayHistoryStats(makeData(std::move(entries)), Saturday, Saturday).longestSession.has_value());
}

TEST(PlayHistoryStatsTest, SingleListensAreNotSessions)
{
    const auto data = makeData({makeEntry(u"a"_s, at(Saturday, 10, 0)), makeEntry(u"b"_s, at(Saturday, 14, 0))});

    EXPECT_FALSE(computePlayHistoryStats(data, Saturday, Saturday).longestSession.has_value());
}

TEST(PlayHistoryStatsTest, CountsRepeatedListensOfTheSameTrack)
{
    const auto data = makeData({makeEntry(u"a"_s, at(Saturday, 10)), makeEntry(u"a"_s, at(Saturday, 11)),
                                makeEntry(u"a"_s, at(Saturday, 12)), makeEntry(u"b"_s, at(Saturday, 13)),
                                makeEntry(u"a"_s, at(Saturday, 14))});

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday);

    EXPECT_EQ(stats.repeatListens, 2);
    EXPECT_EQ(stats.oneTimeTracks, 1);
}

TEST(PlayHistoryStatsTest, CountsTracksInHeavyRotation)
{
    std::vector<PlayHistoryEntry> entries;
    for(int i{0}; i < PlayHistoryStats::HeavyRotationListens; ++i) {
        entries.push_back(makeEntry(u"a"_s, at(Saturday, 8, i)));
    }
    entries.push_back(makeEntry(u"b"_s, at(Saturday, 12)));

    const auto stats = computePlayHistoryStats(makeData(std::move(entries)), Saturday, Saturday);

    EXPECT_EQ(stats.heavyRotationTracks, 1);
}

TEST(PlayHistoryStatsTest, FindsTheMostPlayedTrackOfADay)
{
    std::vector<PlayHistoryEntry> entries;
    for(int i{0}; i < 3; ++i) {
        entries.push_back(makeEntry(u"a"_s, at(Saturday.addDays(1), 8 + i)));
    }
    entries.push_back(makeEntry(u"a"_s, at(Saturday, 8)));
    entries.push_back(makeEntry(u"b"_s, at(Saturday, 9)));
    entries.push_back(makeEntry(u"b"_s, at(Saturday.addDays(3), 9)));

    const auto stats = computePlayHistoryStats(makeData(std::move(entries)), Saturday, Saturday.addDays(3));

    // Tracks played once in a day aren't an obsession
    ASSERT_EQ(stats.obsessions.size(), 1U);
    EXPECT_EQ(stats.obsessions.front().hash, u"a"_s);
    EXPECT_EQ(stats.obsessions.front().count, 3);
    EXPECT_EQ(QDateTime::fromMSecsSinceEpoch(stats.obsessions.front().timestamp).date(), Saturday.addDays(1));
}

TEST(PlayHistoryStatsTest, FindsTheTrackListenedToOnTheMostConsecutiveDays)
{
    std::vector<PlayHistoryEntry> entries;
    for(const int day : {0, 1, 2, 3}) {
        entries.push_back(makeEntry(u"a"_s, at(Saturday.addDays(day), 9)));
    }
    for(const int day : {0, 2, 4}) {
        entries.push_back(makeEntry(u"b"_s, at(Saturday.addDays(day), 10)));
    }

    const auto stats = computePlayHistoryStats(makeData(std::move(entries)), Saturday, Saturday.addDays(4));

    ASSERT_TRUE(stats.longestTrackStreak.has_value());
    EXPECT_EQ(stats.longestTrackStreak->hash, u"a"_s);
    EXPECT_EQ(stats.longestTrackStreak->days, 4);
    EXPECT_EQ(QDateTime::fromMSecsSinceEpoch(stats.longestTrackStreak->timestamp).date(), Saturday.addDays(3));
}

TEST(PlayHistoryStatsTest, SeparatesDiscoveriesFromOldFavourites)
{
    const QDate today = Saturday.addDays(30);

    auto data = makeData({makeEntry(u"new"_s, at(today, 10)), makeEntry(u"new"_s, at(today, 11)),
                          makeEntry(u"old"_s, at(today, 12)), makeEntry(u"old"_s, at(today, 13)),
                          makeEntry(u"older"_s, at(today, 14)), makeEntry(u"older"_s, at(today, 15))});
    data.firstListenByHash[u"old"_s]            = at(Saturday.addDays(-100), 10);
    data.firstListenByHash[u"older"_s]          = at(Saturday.addDays(-400), 10);
    data.firstListenByArtist[u"Artist old"_s]   = at(Saturday.addDays(-100), 10);
    data.firstListenByArtist[u"Artist older"_s] = at(Saturday.addDays(-400), 10);
    data.oldestTimestamp                        = at(Saturday.addDays(-400), 10);

    const auto stats = computePlayHistoryStats(data, today, today);

    EXPECT_EQ(stats.newTracks, 1);
    ASSERT_EQ(stats.newTracksTop.size(), 1U);
    EXPECT_EQ(stats.newTracksTop.front().hash, u"new"_s);
    EXPECT_EQ(stats.newTracksTop.front().timestamp, at(today, 10));

    EXPECT_EQ(stats.newArtists, 1);
    ASSERT_EQ(stats.newArtistsTop.size(), 1U);
    EXPECT_EQ(stats.newArtistsTop.front().key, u"Artist new"_s);
    EXPECT_EQ(stats.newArtistsTop.front().count, 2);

    ASSERT_TRUE(stats.oldestFavourite.has_value());
    EXPECT_EQ(stats.oldestFavourite->hash, u"older"_s);
    EXPECT_EQ(stats.oldestFavourite->count, 2);
}

TEST(PlayHistoryStatsTest, FindsTracksListenedToAgainAfterALongBreak)
{
    const QDate today = Saturday.addDays(400);

    auto data = makeData({makeEntry(u"back"_s, at(today, 10)), makeEntry(u"back"_s, at(today, 11)),
                          makeEntry(u"regular"_s, at(today, 12))});
    data.lastListenBeforeByHash[u"back"_s]    = at(today.addDays(-(PlayHistoryStats::RediscoveryGapDays + 20)), 10);
    data.lastListenBeforeByHash[u"regular"_s] = at(today.addDays(-3), 10);

    const auto stats = computePlayHistoryStats(data, today, today);

    ASSERT_EQ(stats.rediscoveries.size(), 1U);
    EXPECT_EQ(stats.rediscoveries.front().hash, u"back"_s);
    EXPECT_EQ(stats.rediscoveries.front().count, 2);
    EXPECT_GE(stats.rediscoveries.front().days, PlayHistoryStats::RediscoveryGapDays);
}

TEST(PlayHistoryStatsTest, RanksCategoriesAndSplitsOutTracksWithSeveralValues)
{
    auto first      = makeEntry(u"a"_s, at(Saturday, 10));
    first.genres    = {u"Rock"_s, u"Indie"_s};
    first.album     = u"Album One"_s;
    first.composers = {u"Composer"_s};

    auto second   = makeEntry(u"a"_s, at(Saturday, 11));
    second.genres = {u"Rock"_s};
    second.album  = u"Album One"_s;

    auto third    = makeEntry(u"b"_s, at(Saturday, 12));
    third.genres  = {u"Jazz"_s};
    third.artists = {u"Artist a"_s, u"Guest"_s};
    third.album   = u"Album Two"_s;

    const auto stats = computePlayHistoryStats(makeData({first, second, third}), Saturday, Saturday);
    using Category   = PlayHistoryStats::Category;

    const auto& genres = stats.category(Category::Genre);
    EXPECT_EQ(genres.unique, 3);
    EXPECT_EQ(stats.uniqueGenres, 3);
    ASSERT_GE(genres.top.size(), 3U);
    EXPECT_EQ(genres.top[0].key, u"Rock"_s);
    EXPECT_EQ(genres.top[0].count, 2);

    const auto& artists = stats.category(Category::Artist);
    EXPECT_EQ(stats.uniqueArtists, 2);
    EXPECT_EQ(artists.top.front().key, u"Artist a"_s);
    EXPECT_EQ(artists.top.front().count, 3);

    EXPECT_EQ(stats.uniqueAlbums, 2);
    EXPECT_EQ(stats.category(Category::Album).top.front().key, u"Album One"_s);
    EXPECT_EQ(stats.category(Category::Composer).unique, 1);
    EXPECT_EQ(stats.category(Category::AlbumArtist).unique, 0);
}

TEST(PlayHistoryStatsTest, KeepsAlbumsWithTheSameTitleApart)
{
    const auto greatestHits = [](const QString& hash, const QString& artist, int minute) {
        auto entry         = makeEntry(hash, at(Saturday, 10, minute));
        entry.album        = u"Greatest Hits"_s;
        entry.albumArtists = {artist};
        return entry;
    };

    const auto data = makeData({greatestHits(u"a1"_s, u"Band A"_s, 0), greatestHits(u"a2"_s, u"Band A"_s, 1),
                                greatestHits(u"b1"_s, u"Band B"_s, 2)});

    const auto stats  = computePlayHistoryStats(data, Saturday, Saturday);
    const auto& album = stats.category(PlayHistoryStats::Category::Album);

    EXPECT_EQ(stats.uniqueAlbums, 2);
    ASSERT_EQ(album.top.size(), 2U);
    // The album artists are only shown because the titles are the same
    EXPECT_EQ(album.top[0].key, u"Greatest Hits — Band A"_s);
    EXPECT_EQ(album.top[0].count, 2);
    EXPECT_EQ(album.top[1].key, u"Greatest Hits — Band B"_s);
    EXPECT_EQ(album.top[1].count, 1);
}

TEST(PlayHistoryStatsTest, ShowsAlbumsWithADistinctTitleByTitleAlone)
{
    auto entry         = makeEntry(u"a"_s, at(Saturday, 10));
    entry.album        = u"Night Circuits"_s;
    entry.albumArtists = {u"Aurora Lane"_s};

    const auto stats = computePlayHistoryStats(makeData({entry}), Saturday, Saturday);

    ASSERT_EQ(stats.category(PlayHistoryStats::Category::Album).top.size(), 1U);
    EXPECT_EQ(stats.category(PlayHistoryStats::Category::Album).top.front().key, u"Night Circuits"_s);
}

TEST(PlayHistoryStatsTest, SplitsReleaseDecadesAndAudioQuality)
{
    auto flac     = makeEntry(u"a"_s, at(Saturday, 10));
    flac.year     = 1994;
    flac.bitrate  = 900;
    flac.lossless = 1;

    auto mp3     = makeEntry(u"b"_s, at(Saturday, 11));
    mp3.year     = 2003;
    mp3.bitrate  = 128;
    mp3.lossless = 0;

    auto lowRate     = makeEntry(u"c"_s, at(Saturday, 12));
    lowRate.year     = 1999;
    lowRate.bitrate  = 96;
    lowRate.lossless = 0;

    const auto stats = computePlayHistoryStats(makeData({flac, mp3, lowRate}), Saturday, Saturday);

    ASSERT_EQ(stats.decades.size(), 2U);
    EXPECT_EQ(stats.decades[0].key, u"1990"_s);
    EXPECT_EQ(stats.decades[0].count, 2);
    EXPECT_EQ(stats.decades[1].key, u"2000"_s);
    EXPECT_EQ(stats.decades[1].count, 1);

    EXPECT_EQ(stats.losslessListens, 1);
    EXPECT_EQ(stats.lossyListens, 2);
    EXPECT_EQ(stats.bitrateBuckets, (std::array<int, 4>{1, 1, 0, 1}));
}

TEST(PlayHistoryStatsTest, UnknownAudioQualityIsLeftOut)
{
    const auto stats = computePlayHistoryStats(makeData({makeEntry(u"a"_s, at(Saturday, 10))}), Saturday, Saturday);

    EXPECT_EQ(stats.losslessListens + stats.lossyListens, 0);
    EXPECT_EQ(stats.bitrateBuckets, (std::array<int, 4>{}));
}

TEST(PlayHistoryStatsTest, FindsAlbumsWithEveryTrackListenedTo)
{
    const auto albumEntry = [](const QString& hash, const QString& album, int minute) {
        auto entry         = makeEntry(hash, at(Saturday, 10, minute));
        entry.album        = album;
        entry.albumArtists = {u"Band"_s};
        return entry;
    };

    const auto data = makeData({albumEntry(u"a1"_s, u"Whole"_s, 0), albumEntry(u"a2"_s, u"Whole"_s, 1),
                                albumEntry(u"a1"_s, u"Whole"_s, 2), albumEntry(u"b1"_s, u"Partial"_s, 3),
                                albumEntry(u"c1"_s, u"Single"_s, 4)});

    PlayHistoryStatsOptions options;
    options.albumTracks.insert(PlayHistoryEntry::albumKey(u"Whole"_s, {u"Band"_s}), {u"a1"_s, u"a2"_s});
    options.albumTracks.insert(PlayHistoryEntry::albumKey(u"Partial"_s, {u"Band"_s}), {u"b1"_s, u"b2"_s, u"b3"_s});
    options.albumTracks.insert(PlayHistoryEntry::albumKey(u"Single"_s, {u"Band"_s}), {u"c1"_s});

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday, options);

    // Albums of a single track don't count
    ASSERT_EQ(stats.completedAlbums.size(), 1U);
    EXPECT_EQ(stats.completedAlbums.front().name, u"Whole"_s);
    EXPECT_EQ(stats.completedAlbums.front().tracks, 2);
    EXPECT_EQ(stats.completedAlbums.front().listens, 3);
}

TEST(PlayHistoryStatsTest, AnAlbumIsOnlyCompleteWhenItsCurrentTracksWereAllListenedTo)
{
    const auto albumEntry = [](const QString& hash, int minute) {
        auto entry         = makeEntry(hash, at(Saturday, 10, minute));
        entry.album        = u"Album"_s;
        entry.albumArtists = {u"Band"_s};
        return entry;
    };

    // Tracks A and B were listened to. B has since been removed and an unplayed C added, so the album still has
    // two tracks, but they aren't the two that were listened to
    const auto data = makeData({albumEntry(u"A"_s, 0), albumEntry(u"B"_s, 1)});

    PlayHistoryStatsOptions options;
    options.albumTracks.insert(PlayHistoryEntry::albumKey(u"Album"_s, {u"Band"_s}), {u"A"_s, u"C"_s});

    EXPECT_TRUE(computePlayHistoryStats(data, Saturday, Saturday, options).completedAlbums.empty());

    options.albumTracks[PlayHistoryEntry::albumKey(u"Album"_s, {u"Band"_s})] = {u"A"_s, u"B"_s};
    EXPECT_EQ(computePlayHistoryStats(data, Saturday, Saturday, options).completedAlbums.size(), 1U);
}

TEST(PlayHistoryStatsTest, CountsOnlyListenedToTracksStillInTheLibrary)
{
    const auto data = makeData({makeEntry(u"kept"_s, at(Saturday, 10)), makeEntry(u"removed"_s, at(Saturday, 11)),
                                makeEntry(u"kept"_s, at(Saturday, 12))});

    PlayHistoryStatsOptions options;
    options.libraryTracks = {u"kept"_s, u"never"_s};

    const auto stats = computePlayHistoryStats(data, Saturday, Saturday, options);

    EXPECT_EQ(stats.uniqueTracks, 2);
    EXPECT_EQ(stats.libraryTracksHeard, 1);
}

TEST(PlayHistoryStatsTest, LibraryTracksHeardIsNotWorkedOutWithoutALibrary)
{
    const auto stats = computePlayHistoryStats(makeData({makeEntry(u"a"_s, at(Saturday, 10))}), Saturday, Saturday);

    EXPECT_EQ(stats.libraryTracksHeard, 0);
}

TEST(PlayHistoryStatsTest, FindsTheTopTrackOfEachMonthWhenAsked)
{
    const QDate june{2026, 6, 10};
    const QDate july{2026, 7, 10};

    const auto data = makeData({makeEntry(u"a"_s, at(june, 10)), makeEntry(u"b"_s, at(june, 11)),
                                makeEntry(u"b"_s, at(june, 12)), makeEntry(u"c"_s, at(july, 10))});

    PlayHistoryStatsOptions options;
    options.monthlyTop = true;

    const auto stats = computePlayHistoryStats(data, QDate{2026, 1, 1}, QDate{2026, 12, 31}, options);

    ASSERT_TRUE(stats.monthlyTop[5].has_value());
    EXPECT_EQ(stats.monthlyTop[5]->hash, u"b"_s);
    EXPECT_EQ(stats.monthlyTop[5]->count, 2);
    ASSERT_TRUE(stats.monthlyTop[6].has_value());
    EXPECT_EQ(stats.monthlyTop[6]->hash, u"c"_s);
    EXPECT_FALSE(stats.monthlyTop[0].has_value());

    // Not worked out unless requested
    const auto without = computePlayHistoryStats(data, QDate{2026, 1, 1}, QDate{2026, 12, 31});
    EXPECT_FALSE(without.monthlyTop[5].has_value());
}

TEST(PlayHistoryStatsTest, EstimatesListenTimeFromTrackDurations)
{
    auto first      = makeEntry(u"a"_s, at(Saturday, 10));
    first.duration  = 200'000;
    auto second     = makeEntry(u"b"_s, at(Saturday, 11));
    second.duration = 100'500;

    const auto stats = computePlayHistoryStats(makeData({first, second}), Saturday, Saturday);

    EXPECT_EQ(stats.listenSeconds, 300);
}

TEST(PlayHistoryStatsTest, LimitsTheTrackRankings)
{
    std::vector<PlayHistoryEntry> entries;
    for(int i{0}; i < 10; ++i) {
        entries.push_back(makeEntry(QString::number(i), at(Saturday, 8, i)));
    }

    PlayHistoryStatsOptions options;
    options.topCount = 3;

    const auto stats = computePlayHistoryStats(makeData(std::move(entries)), Saturday, Saturday, options);

    EXPECT_EQ(stats.topTracks.size(), 3U);
    EXPECT_EQ(stats.uniqueTracks, 10);
}

TEST(PlayHistoryEntryTest, AlbumKeysTellAlbumsWithTheSameNameApart)
{
    const QString first  = PlayHistoryEntry::albumKey(u"Greatest Hits"_s, {u"Band A"_s});
    const QString second = PlayHistoryEntry::albumKey(u"Greatest Hits"_s, {u"Band B"_s});

    EXPECT_NE(first, second);
    EXPECT_EQ(first, PlayHistoryEntry::albumKey(u"Greatest Hits"_s, {u"Band A"_s}));
    EXPECT_TRUE(PlayHistoryEntry::albumKey({}, {u"Band A"_s}).isEmpty());
}
} // namespace Fooyin::Testing
