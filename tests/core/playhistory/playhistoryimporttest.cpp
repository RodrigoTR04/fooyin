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

#include "core/playhistory/playhistoryimport.h"
#include "core/database/dbschema.h"
#include "core/database/playhistorydatabase.h"
#include "core/playhistory/zipreader.h"

#include <core/playhistory/playhistory.h>
#include <core/track.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionpool.h>
#include <utils/database/dbconnectionprovider.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

#include <gtest/gtest.h>
#include <zlib.h>

#include <atomic>
#include <memory>

using namespace Qt::StringLiterals;

constexpr auto CurrentSchemaVersion = 27;

namespace Fooyin::Testing {
namespace {
void ensureCoreApplication()
{
    QStandardPaths::setTestModeEnabled(true);

    if(QCoreApplication::instance()) {
        return;
    }

    static int argc{1};
    static char appName[] = "fooyin-playhistoryimport-test";
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

    auto pool = DbConnectionPool::create(params, u"fooyin-playhistoryimport-test-%1"_s.arg(counter.fetch_add(1)));

    const DbConnectionHandler handler{pool};
    EXPECT_TRUE(handler.hasConnection());

    DbSchema schema{DbConnectionProvider{pool}};
    const auto result = schema.upgradeDatabase(CurrentSchemaVersion, u"://dbschema.xml"_s);
    EXPECT_TRUE(result == DbSchema::UpgradeResult::Success || result == DbSchema::UpgradeResult::IsCurrent);

    return pool;
}

void append16(QByteArray& data, quint16 value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    data.append(bytes, 2);
}

void append32(QByteArray& data, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    data.append(bytes, 4);
}

QByteArray deflateRaw(const QByteArray& data)
{
    z_stream stream{};
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);

    QByteArray output;
    output.resize(static_cast<qsizetype>(deflateBound(&stream, static_cast<uLong>(data.size()))));

    stream.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    stream.avail_in  = static_cast<uInt>(data.size());
    stream.next_out  = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    deflate(&stream, Z_FINISH);
    output.resize(static_cast<qsizetype>(stream.total_out));
    deflateEnd(&stream);

    return output;
}

//! A zip archive of @p files, deflated as Namida's backups are.
QByteArray makeZip(const std::vector<std::pair<QString, QByteArray>>& files)
{
    QByteArray archive;
    QByteArray directory;

    for(const auto& [name, data] : files) {
        const QByteArray encodedName = name.toUtf8();
        const QByteArray compressed  = deflateRaw(data);
        const auto crc               = static_cast<quint32>(
            crc32(0, reinterpret_cast<const Bytef*>(data.constData()), static_cast<uInt>(data.size())));
        const auto offset = static_cast<quint32>(archive.size());

        append32(archive, 0x04034b50);
        append16(archive, 20);
        append16(archive, 0x800);
        append16(archive, 8);
        append32(archive, 0);
        append32(archive, crc);
        append32(archive, static_cast<quint32>(compressed.size()));
        append32(archive, static_cast<quint32>(data.size()));
        append16(archive, static_cast<quint16>(encodedName.size()));
        append16(archive, 0);
        archive.append(encodedName);
        archive.append(compressed);

        append32(directory, 0x02014b50);
        append16(directory, 20);
        append16(directory, 20);
        append16(directory, 0x800);
        append16(directory, 8);
        append32(directory, 0);
        append32(directory, crc);
        append32(directory, static_cast<quint32>(compressed.size()));
        append32(directory, static_cast<quint32>(data.size()));
        append16(directory, static_cast<quint16>(encodedName.size()));
        append16(directory, 0);
        append16(directory, 0);
        append16(directory, 0);
        append16(directory, 0);
        append32(directory, 0);
        append32(directory, offset);
        directory.append(encodedName);
    }

    const auto directoryOffset = static_cast<quint32>(archive.size());
    archive.append(directory);

    append32(archive, 0x06054b50);
    append16(archive, 0);
    append16(archive, 0);
    append16(archive, static_cast<quint16>(files.size()));
    append16(archive, static_cast<quint16>(files.size()));
    append32(archive, static_cast<quint32>(directory.size()));
    append32(archive, directoryOffset);
    append16(archive, 0);

    return archive;
}

constexpr auto PhoneMusic = "/storage/emulated/0/Music/Tracks";

QString phonePath(const QString& relative)
{
    return QLatin1String{PhoneMusic} + u'/' + relative;
}

//! A day of Namida history: the listens of @p tracks at @p timestamps, newest first as Namida writes them.
QByteArray namidaDay(const std::vector<std::pair<qint64, QString>>& listens)
{
    QJsonArray array;
    for(auto it = listens.rbegin(); it != listens.rend(); ++it) {
        array.append(QJsonObject{{u"dateAdded"_s, it->first}, {u"track"_s, phonePath(it->second)}});
    }
    return QJsonDocument{array}.toJson();
}

//! A backup as made by Namida: a zip containing a zip of the history, alongside others that have nothing to do with
//! it.
QByteArray namidaBackup(const std::vector<std::pair<qint64, QString>>& listens)
{
    std::vector<std::pair<QString, QByteArray>> days;
    // Split over two days, as Namida does
    const auto half = listens.size() / 2;
    days.emplace_back(u"20537.json"_s, namidaDay({listens.begin(), listens.begin() + static_cast<qsizetype>(half)}));
    days.emplace_back(u"20538.json"_s, namidaDay({listens.begin() + static_cast<qsizetype>(half), listens.end()}));

    const QByteArray history = makeZip(days);
    const QByteArray youtube = makeZip({{u"20537.json"_s, namidaDay({{1, u"youtube.mp3"_s}})}});
    const QByteArray empty   = makeZip({});

    return makeZip({{u"YOUTUBE_FILES.zip"_s, empty},
                    {u"YOUTUBE_TEMPDIR_Youtube Stats.zip"_s, youtube},
                    {u"TEMPDIR_History.zip"_s, history}});
}

//! An export from the fooyin installation "laptop", with @p sources and @p listens as given.
QByteArray fooyinExport(const QJsonArray& sources, const QJsonArray& listens)
{
    return QJsonDocument{QJsonObject{{u"format"_s, u"fooyin-listening-history"_s},
                                     {u"version"_s, 1},
                                     {u"device"_s, QJsonObject{{u"id"_s, u"laptop"_s}, {u"name"_s, u"Laptop"_s}}},
                                     {u"sources"_s, sources},
                                     {u"listens"_s, listens}}}
        .toJson();
}

QJsonObject exportedListen(qint64 timestamp, const QJsonValue& source, const QString& origin)
{
    return QJsonObject{{u"timestamp"_s, timestamp}, {u"source"_s, source},   {u"origin"_s, origin},
                       {u"hash"_s, u"abc"_s},       {u"title"_s, u"Song"_s}, {u"duration"_s, 200'000}};
}

Track makeTrack(const QString& path, const QString& title, const QString& artist, const QString& album)
{
    Track track{path};
    track.setTitle(title);
    track.setArtists({artist});
    track.setAlbum(album);
    track.setAlbumArtists({artist});
    track.setDuration(200'000);
    track.generateHash();
    return track;
}

qint64 at(int day, int hour = 12)
{
    return QDateTime{QDate{2026, 3, day}, QTime{hour, 0}}.toMSecsSinceEpoch();
}
} // namespace

class PlayHistoryImportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureCoreApplication();
        ASSERT_TRUE(m_dir.isValid());

        m_pool = createDbPool(m_dir.filePath(u"fooyin.db"_s));
        m_handler.emplace(m_pool);

        m_library = {
            makeTrack(u"/home/me/Music/Tracks/Kiesza/Hideaway/Hideaway.flac"_s, u"Hideaway"_s, u"Kiesza"_s,
                      u"Hideaway"_s),
            makeTrack(u"/home/me/Music/Tracks/Frizk/Forgotten Arcade/Flashback.mp3"_s, u"Flashback"_s, u"Frizk"_s,
                      u"Forgotten Arcade"_s),
            makeTrack(u"/home/me/Music/Tracks/Modjo/Modjo/Chillin'.mp3"_s, u"Chillin'"_s, u"Modjo"_s, u"Modjo"_s),
        };
    }

    void TearDown() override
    {
        m_handler.reset();
    }

    [[nodiscard]] QString writeFile(const QString& name, const QByteArray& data) const
    {
        const QString path = m_dir.filePath(name);
        QFile file{path};
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(data);
        return path;
    }

    [[nodiscard]] std::vector<PlayHistoryEntry> listens(const DbConnectionPoolPtr& pool) const
    {
        PlayHistoryDatabase database;
        database.initialise(DbConnectionProvider{pool});
        return database.all();
    }

    QTemporaryDir m_dir;
    DbConnectionPoolPtr m_pool;
    std::optional<DbConnectionHandler> m_handler;
    TrackList m_library;
};

//! Another fooyin installation, with a database of its own.
struct Installation
{
    explicit Installation(const QString& name)
        : pool{createDbPool(dir.filePath(u"fooyin.db"_s))}
        , handler{pool}
        , history{std::make_unique<PlayHistory>(pool)}
    {
        database.initialise(DbConnectionProvider{pool});
        database.renameSource(0, name);
    }

    //! Exports the history to @p path, which is returned.
    QString exportTo(const QString& path) const
    {
        EXPECT_EQ(history->exportFile(path).result(), QString{});
        return path;
    }

    QTemporaryDir dir;
    DbConnectionPoolPtr pool;
    DbConnectionHandler handler;
    std::unique_ptr<PlayHistory> history;
    PlayHistoryDatabase database;
};

//! Imports @p path into @p history, expecting it to succeed.
PlayHistoryImportResult importInto(PlayHistory& history, const QString& path, const TrackList& library)
{
    const auto result = history.importFile(path, {}, library).result();
    EXPECT_TRUE(result.succeeded()) << result.error.toStdString();
    return result;
}

TEST_F(PlayHistoryImportTest, ReadsFilesFromAZipInsideAZip)
{
    const QByteArray inner = makeZip({{u"a.txt"_s, "first"}, {u"folder/b.txt"_s, QByteArray(10'000, 'x')}});
    const ZipReader outer{makeZip({{u"inner.zip"_s, inner}})};
    ASSERT_TRUE(outer.isValid());
    ASSERT_EQ(outer.entries().size(), 1U);

    const ZipReader zip{outer.read(outer.entries().front())};
    ASSERT_TRUE(zip.isValid());
    ASSERT_EQ(zip.entries().size(), 2U);
    EXPECT_EQ(zip.entries().at(1).name, u"folder/b.txt"_s);
    EXPECT_EQ(zip.read(zip.entries().at(0)), QByteArray{"first"});
    EXPECT_EQ(zip.read(zip.entries().at(1)), QByteArray(10'000, 'x'));

    EXPECT_FALSE(ZipReader{QByteArray{"not a zip"}}.isValid());
}

TEST_F(PlayHistoryImportTest, RecognisesWhatAFileHolds)
{
    const QString backup = writeFile(u"backup.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}}));
    const QString day    = writeFile(u"20537.json"_s, namidaDay({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}}));
    const QString other  = writeFile(u"other.zip"_s, makeZip({{u"readme.txt"_s, "hello"}}));
    const QString text   = writeFile(u"other.json"_s, R"({"hello": "world"})");

    EXPECT_EQ(PlayHistory::fileFormat(backup), PlayHistoryFormat::Namida);
    EXPECT_EQ(PlayHistory::fileFormat(day), PlayHistoryFormat::Namida);
    EXPECT_EQ(PlayHistory::fileFormat(other), PlayHistoryFormat::Unknown);
    EXPECT_EQ(PlayHistory::fileFormat(text), PlayHistoryFormat::Unknown);
    EXPECT_EQ(PlayHistory::fileFormat(m_dir.filePath(u"missing.json"_s)), PlayHistoryFormat::Unknown);
}

TEST_F(PlayHistoryImportTest, ReadsOnlyTheMusicHistoryOfANamidaBackup)
{
    const QString backup = writeFile(u"backup.zip"_s, namidaBackup({{at(2), u"Frizk/Forgotten Arcade/Flashback.mp3"_s},
                                                                    {at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                                    {at(3), u"Modjo/Modjo/Chillin'.mp3"_s}}));

    const auto listens = PlayHistoryImport::readNamida(backup);
    ASSERT_TRUE(listens.has_value());
    ASSERT_EQ(listens->listens.size(), 3U);
    EXPECT_EQ(listens->listens.at(0).timestamp, at(1));
    EXPECT_EQ(listens->listens.at(0).path, phonePath(u"Kiesza/Hideaway/Hideaway.mp3"_s));
    EXPECT_EQ(listens->listens.at(2).timestamp, at(3));
    EXPECT_EQ(listens->invalid, 0);

    const QString other = writeFile(u"other.zip"_s, makeZip({{u"readme.txt"_s, "hello"}}));
    EXPECT_FALSE(PlayHistoryImport::readNamida(other).has_value());
}

TEST_F(PlayHistoryImportTest, MatchesFilesOnAnotherDeviceToTracksInTheLibrary)
{
    TrackList library = m_library;
    // Another track with the same file name, in a different folder
    library.push_back(
        makeTrack(u"/home/me/Music/Tracks/Someone/Covers/Hideaway.mp3"_s, u"Hideaway"_s, u"Someone"_s, u"Covers"_s));

    const PlayHistoryImport::TrackMatcher matcher{library};

    // A different format, but the same folders
    const Track* track = matcher.match(phonePath(u"Kiesza/Hideaway/Hideaway.mp3"_s));
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->artists(), QStringList{u"Kiesza"_s});

    // Case and separators don't matter
    track = matcher.match(u"C:\\Users\\me\\music\\tracks\\SOMEONE\\covers\\hideaway.MP3"_s);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->artists(), QStringList{u"Someone"_s});

    // Nothing to tell the two apart, so neither rather than possibly the wrong one
    EXPECT_EQ(matcher.match(u"/sdcard/Download/Hideaway.mp3"_s), nullptr);
    EXPECT_EQ(matcher.match(phonePath(u"Nobody/Nothing/Missing.mp3"_s)), nullptr);
    // The only track of that name, but nothing else in common, so it could be another track altogether
    EXPECT_EQ(matcher.match(phonePath(u"Someone Else/Other Album/Flashback.mp3"_s)), nullptr);
}

TEST_F(PlayHistoryImportTest, MatchesNamesWithCharactersThePhoneDoesNotAllow)
{
    TrackList library = m_library;
    library.push_back(makeTrack(u"/home/me/Music/Tracks/Caravan Palace/<I°_°I>/Lay Down.flac"_s, u"Lay Down"_s,
                                u"Caravan Palace"_s, u"<I°_°I>"_s));
    library.push_back(makeTrack(u"/home/me/Music/Tracks/Arctic Monkeys/AM/Do I Wanna Know?.flac"_s,
                                u"Do I Wanna Know?"_s, u"Arctic Monkeys"_s, u"AM"_s));
    library.push_back(makeTrack(u"/home/me/Music/Games/ATLUS/Persona 5 Royal: Original Soundtrack/I believe.flac"_s,
                                u"I believe"_s, u"ATLUS"_s, u"Persona 5 Royal: Original Soundtrack"_s));
    // Written with a separate accent, as some systems store names
    library.push_back(makeTrack(u"/home/me/Music/Tracks/Beyonce\u0301/Lemonade/Formation.flac"_s, u"Formation"_s,
                                u"Beyoncé"_s, u"Lemonade"_s));
    library.push_back(makeTrack(u"/home/me/Music/Tracks/Gorillaz/Strange Timez/Momentary Bliss.flac"_s,
                                u"Momentary Bliss"_s, u"Gorillaz"_s, u"Strange Timez"_s));

    const PlayHistoryImport::TrackMatcher matcher{library};

    const auto titleOf = [&matcher](const QString& path) {
        const Track* track = matcher.match(phonePath(path));
        return track ? track->title() : QString{};
    };
    EXPECT_EQ(titleOf(u"Caravan Palace/_I°_°I_/Lay Down.mp3"_s), u"Lay Down"_s);
    EXPECT_EQ(titleOf(u"Arctic Monkeys/AM/Do I Wanna Know_.mp3"_s), u"Do I Wanna Know?"_s);
    EXPECT_EQ(titleOf(u"ATLUS/Persona 5 Royal_ Original Soundtrack/I believe.mp3"_s), u"I believe"_s);
    EXPECT_EQ(titleOf(u"Beyonc\u00e9/Lemonade/Formation.mp3"_s), u"Formation"_s);
    // A space at the end of a folder's name is left out on some devices but not others
    EXPECT_EQ(titleOf(u"Gorillaz/Strange Timez /Momentary Bliss.mp3"_s), u"Momentary Bliss"_s);
}

TEST_F(PlayHistoryImportTest, ImportingANewerNamidaBackupOnlyAddsWhatsNew)
{
    PlayHistory history{m_pool};
    QSignalSpy changed{&history, &PlayHistory::historyChanged};
    QSignalSpy reimported{&history, &PlayHistory::sourcesChanged};

    std::vector<std::pair<qint64, QString>> phone{{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                  {at(2), u"Frizk/Forgotten Arcade/Flashback.mp3"_s},
                                                  {at(3), u"Kiesza/Hideaway/Hideaway.mp3"_s}};

    auto result = history.importFile(writeFile(u"first.zip"_s, namidaBackup(phone)), u"Phone"_s, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 3);
    EXPECT_EQ(result.duplicates, 0);
    EXPECT_EQ(result.devices, QStringList{u"Phone"_s});

    // Months later, the backup has everything since Namida was installed and some new listens, some of which may
    // be of files that have moved
    phone.emplace_back(at(10), u"Modjo/Modjo/Chillin'.mp3"_s);
    phone.emplace_back(at(11), u"Moved/Kiesza/Hideaway/Hideaway.mp3"_s);
    phone[0].second = u"Moved/Kiesza/Hideaway/Hideaway.mp3"_s;

    result = history.importFile(writeFile(u"second.zip"_s, namidaBackup(phone)), u"Phone"_s, m_library).result();
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.added, 2);
    EXPECT_EQ(result.duplicates, 3);

    // Even if the device is given another name by mistake
    result = history.importFile(writeFile(u"third.zip"_s, namidaBackup(phone)), u"My phone"_s, m_library).result();
    EXPECT_EQ(result.added, 0);
    EXPECT_EQ(result.duplicates, 5);
    // Which doesn't leave a device with nothing from it behind
    EXPECT_EQ(history.sources().result().size(), 2U);

    QCoreApplication::processEvents();
    EXPECT_EQ(changed.count(), 2);
    // Though the time it was last imported has changed
    EXPECT_EQ(reimported.count(), 1);

    const auto entries = listens(m_pool);
    ASSERT_EQ(entries.size(), 5U);
    EXPECT_EQ(entries.at(3).title, u"Chillin'"_s);
    EXPECT_EQ(entries.at(3).hash, m_library.at(2).hash());
    EXPECT_EQ(entries.at(3).origin, u"namida"_s);
    EXPECT_NE(entries.at(3).source, 0);
}

TEST_F(PlayHistoryImportTest, ListensOfTracksNotInTheLibraryAreAddedOnceTheyAre)
{
    PlayHistory history{m_pool};

    const QString backup = writeFile(u"backup.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                                    {at(2), u"New/Album/Song.mp3"_s},
                                                                    {at(3), u"New/Album/Song.mp3"_s}}));

    auto result = history.importFile(backup, u"Phone"_s, m_library).result();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.unmatched, 2);
    EXPECT_EQ(result.unmatchedTracks, QStringList{phonePath(u"New/Album/Song.mp3"_s)});

    TrackList library = m_library;
    library.push_back(makeTrack(u"/home/me/Music/Tracks/New/Album/Song.flac"_s, u"Song"_s, u"New"_s, u"Album"_s));

    result = history.importFile(backup, u"Phone"_s, library).result();
    EXPECT_EQ(result.added, 2);
    EXPECT_EQ(result.duplicates, 1);
    EXPECT_EQ(result.unmatched, 0);
}

TEST_F(PlayHistoryImportTest, KeepsTrackOfWhatEachDeviceContributed)
{
    PlayHistory history{m_pool};

    history.recordPlay(m_library.at(0));
    history.waitForWrites();

    history
        .importFile(writeFile(u"backup.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                             {at(5), u"Frizk/Forgotten Arcade/Flashback.mp3"_s}})),
                    u"Phone"_s, m_library)
        .waitForFinished();

    auto sources = history.sources().result();
    ASSERT_EQ(sources.size(), 2U);

    EXPECT_TRUE(sources.at(0).isLocal());
    EXPECT_EQ(sources.at(0).kind, u"fooyin"_s);
    EXPECT_FALSE(sources.at(0).deviceId.isEmpty());
    EXPECT_EQ(sources.at(0).listens, 1);
    EXPECT_EQ(sources.at(0).lastImported, 0);

    EXPECT_EQ(sources.at(1).name, u"Phone"_s);
    EXPECT_EQ(sources.at(1).kind, u"namida"_s);
    EXPECT_EQ(sources.at(1).listens, 2);
    EXPECT_EQ(sources.at(1).firstListen, at(1));
    EXPECT_EQ(sources.at(1).lastListen, at(5));
    EXPECT_GT(sources.at(1).lastImported, 0);

    EXPECT_EQ(history.renameSource(0, u"Desktop"_s).result(), PlayHistoryRenameResult::Renamed);
    EXPECT_EQ(history.renameSource(sources.at(1).id, u"Pixel"_s).result(), PlayHistoryRenameResult::Renamed);
    EXPECT_EQ(history.renameSource(12345, u"Nothing"_s).result(), PlayHistoryRenameResult::Failed);
    sources = history.sources().result();
    EXPECT_EQ(sources.at(0).name, u"Desktop"_s);
    EXPECT_EQ(sources.at(1).name, u"Pixel"_s);

    // The listens of a device can be taken out again
    EXPECT_TRUE(history.removeSource(sources.at(1).id).result());
    sources = history.sources().result();
    ASSERT_EQ(sources.size(), 1U);
    EXPECT_EQ(listens(m_pool).size(), 1U);
}

TEST_F(PlayHistoryImportTest, ClearingThisDeviceKeepsWhatWasImported)
{
    PlayHistory history{m_pool};

    history.recordPlay(m_library.at(0));
    history.recordPlay(m_library.at(1));
    history.waitForWrites();

    history
        .importFile(writeFile(u"backup.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}})), u"Phone"_s,
                    m_library)
        .waitForFinished();

    ASSERT_EQ(history.renameSource(0, u"Desktop"_s).result(), PlayHistoryRenameResult::Renamed);
    auto sources = history.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    ASSERT_EQ(sources.at(0).listens, 2);
    ASSERT_EQ(sources.at(1).listens, 1);

    // Only the clear's own notification counts, not the import's, which is still waiting to be delivered
    QCoreApplication::processEvents();
    QSignalSpy changed{&history, &PlayHistory::historyChanged};
    EXPECT_TRUE(history.removeSource(0).result());
    QCoreApplication::processEvents();
    EXPECT_EQ(changed.count(), 1);

    // This device stays, with nothing recorded on it
    sources = history.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_TRUE(sources.at(0).isLocal());
    EXPECT_EQ(sources.at(0).name, u"Desktop"_s);
    EXPECT_EQ(sources.at(0).listens, 0);
    EXPECT_EQ(sources.at(1).listens, 1);

    const auto remaining = listens(m_pool);
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(remaining.front().timestamp, at(1));

    // Listens recorded afterwards are still recorded here
    history.recordPlay(m_library.at(2));
    history.waitForWrites();
    EXPECT_EQ(history.sources().result().at(0).listens, 1);
}

TEST_F(PlayHistoryImportTest, MergesTheHistoryOfOtherFooyinInstallationsWithoutCountingAnythingTwice)
{
    // A laptop that has listened to some tracks itself and has had the phone's history imported into it
    QTemporaryDir laptopDir;
    auto laptopPool = createDbPool(laptopDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler laptopHandler{laptopPool};
    PlayHistory laptop{laptopPool};

    PlayHistoryDatabase laptopDb;
    laptopDb.initialise(DbConnectionProvider{laptopPool});
    laptopDb.renameSource(0, u"Laptop"_s);

    auto laptopListen    = PlayHistoryEntry::fromTrack(m_library.at(2), at(20), 30'000, 31'000);
    laptopListen.genres  = {u"House"_s};
    laptopListen.bitrate = 320;
    ASSERT_TRUE(laptopDb.insert(laptopListen));

    const QString phoneBackup
        = writeFile(u"backup.zip"_s,
                    namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}, {at(2), u"Modjo/Modjo/Chillin'.mp3"_s}}));
    laptop.importFile(phoneBackup, u"Phone"_s, m_library).waitForFinished();

    // The desktop has its own listens, and the phone's history imported straight from the phone as well
    PlayHistory desktop{m_pool};
    desktop.recordPlay(m_library.at(0));
    desktop.waitForWrites();
    desktop.importFile(phoneBackup, u"Phone"_s, m_library).waitForFinished();
    ASSERT_EQ(listens(m_pool).size(), 3U);

    const QString laptopExport = m_dir.filePath(u"laptop.json"_s);
    ASSERT_EQ(laptop.exportFile(laptopExport).result(), QString{});
    EXPECT_EQ(PlayHistory::fileFormat(laptopExport), PlayHistoryFormat::Fooyin);

    auto result = desktop.importFile(laptopExport, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.duplicates, 2);
    // Both devices' histories were imported, even though only one had anything new
    EXPECT_EQ(result.devices, (QStringList{u"Laptop"_s, u"Phone"_s}));

    // Everything about the listen is kept, even if the track isn't in this library
    auto entries = listens(m_pool);
    ASSERT_EQ(entries.size(), 4U);
    const auto imported = std::ranges::find(entries, at(20), &PlayHistoryEntry::timestamp);
    ASSERT_NE(imported, entries.end());
    EXPECT_EQ(imported->hash, laptopListen.hash);
    EXPECT_EQ(imported->genres, QStringList{u"House"_s});
    EXPECT_EQ(imported->bitrate, 320);
    EXPECT_EQ(imported->countedAfter, 30'000);
    EXPECT_EQ(imported->countedAt, 31'000);

    // The laptop's copy of the phone's history had nothing new, so the desktop's own copy is all there is of it
    const auto sources = desktop.sources().result();
    EXPECT_EQ(sources.size(), 3U);
    const auto laptopSource = std::ranges::find(sources, u"Laptop"_s, &PlayHistorySource::name);
    ASSERT_NE(laptopSource, sources.end());
    EXPECT_EQ(laptopSource->kind, u"fooyin"_s);
    EXPECT_EQ(laptopSource->deviceId, laptopDb.deviceId());
    EXPECT_EQ(laptopSource->listens, 1);
    EXPECT_EQ(imported->source, laptopSource->id);

    // Importing the same export again changes nothing
    result = desktop.importFile(laptopExport, {}, m_library).result();
    EXPECT_EQ(result.added, 0);
    EXPECT_EQ(result.duplicates, 3);

    // And the desktop's history going back to the laptop only brings the desktop's own listen with it
    const QString desktopExport = m_dir.filePath(u"desktop.json"_s);
    ASSERT_EQ(desktop.exportFile(desktopExport).result(), QString{});
    result = laptop.importFile(desktopExport, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.duplicates, 3);
    EXPECT_EQ(listens(laptopPool).size(), 4U);

    const auto laptopEntries = listens(laptopPool);
    EXPECT_EQ(std::ranges::count(laptopEntries, QString{}, &PlayHistoryEntry::origin), 1);

    // An installation's own export has nothing to add to it
    result = desktop.importFile(desktopExport, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 0);
    EXPECT_EQ(result.duplicates, 4);
}

TEST_F(PlayHistoryImportTest, AnExportStillHoldsTheListensOfMergedInstallations)
{
    // Two installations that turn out to be the same computer, each with a listen of its own
    QTemporaryDir oldDir;
    auto oldPool = createDbPool(oldDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler oldHandler{oldPool};
    PlayHistory oldInstall{oldPool};
    PlayHistoryDatabase oldDb;
    oldDb.initialise(DbConnectionProvider{oldPool});
    oldDb.renameSource(0, u"Old"_s);
    ASSERT_TRUE(oldDb.insert(PlayHistoryEntry::fromTrack(m_library.at(1), at(3))));

    QTemporaryDir newDir;
    auto newPool = createDbPool(newDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler newHandler{newPool};
    PlayHistory newInstall{newPool};
    PlayHistoryDatabase newDb;
    newDb.initialise(DbConnectionProvider{newPool});
    newDb.renameSource(0, u"New"_s);
    ASSERT_TRUE(newDb.insert(PlayHistoryEntry::fromTrack(m_library.at(2), at(4))));

    const QString oldExport = m_dir.filePath(u"old.json"_s);
    const QString newExport = m_dir.filePath(u"new.json"_s);
    ASSERT_EQ(oldInstall.exportFile(oldExport).result(), QString{});
    ASSERT_EQ(newInstall.exportFile(newExport).result(), QString{});

    // The desktop imports both and merges them
    PlayHistory desktop{m_pool};
    desktop.recordPlay(m_library.at(0));
    desktop.waitForWrites();
    ASSERT_TRUE(desktop.importFile(oldExport, {}, m_library).result().succeeded());
    ASSERT_TRUE(desktop.importFile(newExport, {}, m_library).result().succeeded());

    auto sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 3U);
    const int oldId = std::ranges::find(sources, u"Old"_s, &PlayHistorySource::name)->id;
    const int newId = std::ranges::find(sources, u"New"_s, &PlayHistorySource::name)->id;
    ASSERT_TRUE(desktop.mergeSource(oldId, newId).result());

    // Its export can restore what's cleared from it
    const QString desktopExport = m_dir.filePath(u"desktop.json"_s);
    ASSERT_EQ(desktop.exportFile(desktopExport).result(), QString{});
    ASSERT_TRUE(desktop.removeSource(0).result());
    ASSERT_EQ(listens(m_pool).size(), 2U);

    auto result = desktop.importFile(desktopExport, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.duplicates, 2);

    sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(0).listens, 1);
    EXPECT_EQ(sources.at(1).id, newId);
    EXPECT_EQ(sources.at(1).listens, 2);

    // Another installation that imports the desktop's export learns that the two are the same, so the older
    // installation's own history goes to the same device when it's imported there later on
    QTemporaryDir laptopDir;
    auto laptopPool = createDbPool(laptopDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler laptopHandler{laptopPool};
    PlayHistory laptop{laptopPool};

    result = laptop.importFile(desktopExport, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 3);

    ASSERT_TRUE(oldDb.insert(PlayHistoryEntry::fromTrack(m_library.at(0), at(8))));
    ASSERT_EQ(oldInstall.exportFile(oldExport).result(), QString{});
    result = laptop.importFile(oldExport, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.duplicates, 1);
    EXPECT_EQ(result.devices, QStringList{u"New"_s});

    const auto laptopSources = laptop.sources().result();
    ASSERT_EQ(laptopSources.size(), 3U);
    const auto merged = std::ranges::find(laptopSources, u"New"_s, &PlayHistorySource::name);
    ASSERT_NE(merged, laptopSources.end());
    EXPECT_EQ(merged->listens, 3);
}

class MergedInstallationsTest : public PlayHistoryImportTest
{
protected:
    void SetUp() override
    {
        PlayHistoryImportTest::SetUp();

        m_old = std::make_unique<Installation>(u"Old"_s);
        m_new = std::make_unique<Installation>(u"New"_s);
        ASSERT_TRUE(m_old->database.insert(PlayHistoryEntry::fromTrack(m_library.at(1), at(3))));
        ASSERT_TRUE(m_new->database.insert(PlayHistoryEntry::fromTrack(m_library.at(2), at(4))));

        // The desktop has both installations' history, and knows they're the same computer
        m_desktop = std::make_unique<Installation>(u"Desktop"_s);
        importInto(*m_desktop->history, oldExport(), m_library);
        importInto(*m_desktop->history, newExport(), m_library);
        const auto sources = m_desktop->history->sources().result();
        ASSERT_EQ(sources.size(), 3U);
        ASSERT_TRUE(m_desktop->history
                        ->mergeSource(std::ranges::find(sources, u"Old"_s, &PlayHistorySource::name)->id,
                                      std::ranges::find(sources, u"New"_s, &PlayHistorySource::name)->id)
                        .result());
    }

    [[nodiscard]] QString oldExport() const
    {
        return m_old->exportTo(m_dir.filePath(u"old.json"_s));
    }
    [[nodiscard]] QString newExport() const
    {
        return m_new->exportTo(m_dir.filePath(u"new.json"_s));
    }
    [[nodiscard]] QString desktopExport() const
    {
        return m_desktop->exportTo(m_dir.filePath(u"desktop.json"_s));
    }

    //! Checks that @p installation's export can be read and imported again, without anything being new to it.
    void expectRestorable(const Installation& installation, int listens)
    {
        const QString path = installation.exportTo(m_dir.filePath(u"check.json"_s));
        const auto read    = PlayHistoryImport::readFooyin(path);
        ASSERT_TRUE(read.has_value()) << read.error().toStdString();
        EXPECT_EQ(read->listens.size(), static_cast<size_t>(listens));

        // Device ids are only ever used once
        QSet<int> ids;
        for(const auto& source : read->sources) {
            EXPECT_FALSE(ids.contains(source.id));
            ids.insert(source.id);
        }

        const auto again = importInto(*installation.history, path, m_library);
        EXPECT_EQ(again.added, 0);
        EXPECT_EQ(again.duplicates, listens);

        Installation fresh{u"Fresh"_s};
        EXPECT_EQ(importInto(*fresh.history, path, m_library).added, listens);
    }

    std::unique_ptr<Installation> m_old;
    std::unique_ptr<Installation> m_new;
    std::unique_ptr<Installation> m_desktop;
};

TEST_F(MergedInstallationsTest, AnInstallationKnownOnlyByAnotherNameGetsTheRestOfItsListens)
{
    // The laptop has only had the older installation's history so far
    Installation laptop{u"Laptop"_s};
    importInto(*laptop.history, oldExport(), m_library);

    const auto result = importInto(*laptop.history, desktopExport(), m_library);
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.duplicates, 1);

    // The newer installation's listen joins the older one's, as the desktop says they're the same
    auto sources = laptop.history->sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).name, u"Old"_s);
    EXPECT_EQ(sources.at(1).listens, 2);

    expectRestorable(laptop, 2);

    // And the newer installation's own history goes to it too
    ASSERT_TRUE(m_new->database.insert(PlayHistoryEntry::fromTrack(m_library.at(0), at(9))));
    EXPECT_EQ(importInto(*laptop.history, newExport(), m_library).added, 1);
    sources = laptop.history->sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).listens, 3);
}

TEST_F(MergedInstallationsTest, InstallationsKeptApartHereStayApart)
{
    // The laptop has had both installations' history, separately
    Installation laptop{u"Laptop"_s};
    importInto(*laptop.history, oldExport(), m_library);
    importInto(*laptop.history, newExport(), m_library);

    // The older installation has listened to something since, which reaches the laptop by way of the desktop
    ASSERT_TRUE(m_old->database.insert(PlayHistoryEntry::fromTrack(m_library.at(0), at(8))));
    importInto(*m_desktop->history, oldExport(), m_library);

    const auto result = importInto(*laptop.history, desktopExport(), m_library);
    EXPECT_EQ(result.added, 1);

    // Each installation's listens stay with its own device here
    const auto sources = laptop.history->sources().result();
    ASSERT_EQ(sources.size(), 3U);
    EXPECT_EQ(std::ranges::find(sources, u"Old"_s, &PlayHistorySource::name)->listens, 2);
    EXPECT_EQ(std::ranges::find(sources, u"New"_s, &PlayHistorySource::name)->listens, 1);

    expectRestorable(laptop, 3);
}

TEST_F(MergedInstallationsTest, AnInstallationGivenItsOwnHistoryBackKeepsTheRestApart)
{
    // The newer installation is given the desktop's history, in which the older one is part of it
    const auto result = importInto(*m_new->history, desktopExport(), m_library);
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.duplicates, 1);

    // Its own listen is still its own, and the older installation's is a device of its own, as this one can't be
    // merged into another
    const auto sources = m_new->history->sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(0).listens, 1);
    EXPECT_EQ(sources.at(1).name, u"Old"_s);
    EXPECT_EQ(sources.at(1).kind, u"fooyin"_s);
    EXPECT_EQ(sources.at(1).deviceId, m_old->database.deviceId());
    EXPECT_EQ(sources.at(1).listens, 1);

    expectRestorable(*m_new, 2);
}

TEST_F(MergedInstallationsTest, OnlyExportsWithMergedInstallationsNeedTheNewerFormat)
{
    const auto version = [](const QString& path) {
        QFile file{path};
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return QJsonDocument::fromJson(file.readAll()).object().value("version"_L1).toInt();
    };

    EXPECT_EQ(version(oldExport()), 1);
    EXPECT_EQ(version(desktopExport()), 2);
}

TEST_F(PlayHistoryImportTest, ExportsListensGivenToADeviceWithoutItBeingRemembered)
{
    // Imports from before installations were remembered as part of a device could leave a device with another
    // installation's listens and nothing to say so
    Installation laptop{u"Laptop"_s};
    const int device = laptop.database.findOrAddSource(u"fooyin"_s, u"new-install"_s, u"New"_s);
    ASSERT_NE(device, 0);

    auto listen   = PlayHistoryEntry::fromTrack(m_library.at(0), at(2));
    listen.source = device;
    listen.origin = PlayHistoryImport::fooyinOrigin(u"old-install"_s);
    ASSERT_TRUE(laptop.database.insert(listen));

    const auto read = PlayHistoryImport::readFooyin(laptop.exportTo(m_dir.filePath(u"laptop.json"_s)));
    ASSERT_TRUE(read.has_value()) << read.error().toStdString();
    ASSERT_EQ(read->sources.size(), 2U);
    ASSERT_EQ(read->sources.at(1).aliases.size(), 1U);
    EXPECT_EQ(read->sources.at(1).aliases.front().deviceId, u"old-install"_s);
}

TEST_F(PlayHistoryImportTest, AnInstallationHeldByTwoDevicesIsExportedAsPartOfOne)
{
    // The same could leave an installation's listens with more than one device
    Installation laptop{u"Laptop"_s};
    const int old   = laptop.database.findOrAddSource(u"fooyin"_s, u"old-install"_s, u"Old"_s);
    const int other = laptop.database.findOrAddSource(u"fooyin"_s, u"new-install"_s, u"New"_s);
    ASSERT_NE(old, 0);
    ASSERT_NE(other, 0);

    const auto insert = [&](qint64 timestamp, int source, const QString& installation) {
        auto listen   = PlayHistoryEntry::fromTrack(m_library.at(0), timestamp);
        listen.source = source;
        listen.origin = PlayHistoryImport::fooyinOrigin(installation);
        ASSERT_TRUE(laptop.database.insert(listen));
    };
    insert(at(1), old, u"old-install"_s);
    insert(at(2), other, u"old-install"_s);
    insert(at(3), other, u"stray-install"_s);
    insert(at(4), old, u"stray-install"_s);

    const QString path = laptop.exportTo(m_dir.filePath(u"laptop.json"_s));
    const auto read    = PlayHistoryImport::readFooyin(path);
    ASSERT_TRUE(read.has_value()) << read.error().toStdString();
    ASSERT_EQ(read->listens.size(), 4U);

    // The installation's own device has all of its listens, and the rest go with whichever had them first
    EXPECT_EQ(read->listens.at(0).source, old);
    EXPECT_EQ(read->listens.at(1).source, old);
    EXPECT_EQ(read->listens.at(2).source, other);
    EXPECT_EQ(read->listens.at(3).source, other);
    const auto otherSource = std::ranges::find(read->sources, other, &PlayHistoryImport::ExportedSource::id);
    ASSERT_NE(otherSource, read->sources.end());
    ASSERT_EQ(otherSource->aliases.size(), 1U);
    EXPECT_EQ(otherSource->aliases.front().deviceId, u"stray-install"_s);

    Installation fresh{u"Fresh"_s};
    EXPECT_EQ(importInto(*fresh.history, path, m_library).added, 4);
}

TEST_F(PlayHistoryImportTest, ListensKeptWithTheWrongKindOfDeviceCanStillBeRestored)
{
    Installation laptop{u"Laptop"_s};
    const int phone  = laptop.database.findOrAddNamidaSource(u"Phone"_s);
    const int device = laptop.database.findOrAddSource(u"fooyin"_s, u"new-install"_s, u"New"_s);
    ASSERT_NE(phone, 0);
    ASSERT_NE(device, 0);

    const auto insert = [&](qint64 timestamp, int source, const QString& origin) {
        auto listen   = PlayHistoryEntry::fromTrack(m_library.at(0), timestamp);
        listen.source = source;
        listen.origin = origin;
        ASSERT_TRUE(laptop.database.insert(listen));
    };
    // A phone or this device with another installation's listens, and a device with this one's own
    insert(at(1), phone, PlayHistoryImport::fooyinOrigin(u"old-install"_s));
    insert(at(2), 0, PlayHistoryImport::fooyinOrigin(u"other-install"_s));
    insert(at(3), device, PlayHistoryImport::fooyinOrigin(laptop.database.deviceId()));
    // And a device that's gone, under the id the first of those would otherwise be given in the file
    insert(at(5), device + 1, PlayHistoryImport::fooyinOrigin(u"gone-install"_s));

    const QString path = laptop.exportTo(m_dir.filePath(u"laptop.json"_s));
    const auto read    = PlayHistoryImport::readFooyin(path);
    ASSERT_TRUE(read.has_value()) << read.error().toStdString();
    ASSERT_EQ(read->listens.size(), 4U);
    EXPECT_EQ(read->listens.at(2).source, 0);

    // None of which is new to it
    const auto again = importInto(*laptop.history, path, m_library);
    EXPECT_EQ(again.added, 0);
    EXPECT_EQ(again.duplicates, 4);

    Installation fresh{u"Fresh"_s};
    EXPECT_EQ(importInto(*fresh.history, path, m_library).added, 4);
    // The other installations' listens go to devices of their own, as the phone and this device can't hold them
    const auto sources = fresh.history->sources().result();
    ASSERT_EQ(sources.size(), 5U);
    EXPECT_NE(fresh.database.findSource(u"fooyin"_s, u"old-install"_s), 0);
    EXPECT_NE(fresh.database.findSource(u"fooyin"_s, u"other-install"_s), 0);
    EXPECT_NE(fresh.database.findSource(u"fooyin"_s, u"gone-install"_s),
              fresh.database.findSource(u"fooyin"_s, u"old-install"_s));

    // A Namida listen kept with a fooyin device can't be told where it came from
    insert(at(4), device, QString::fromLatin1(PlayHistoryImport::NamidaOrigin));
    EXPECT_NE(laptop.history->exportFile(m_dir.filePath(u"broken.json"_s)).result(), QString{});
}

TEST_F(PlayHistoryImportTest, APhoneIsNotTakenForAnInstallationWhoseListenItHas)
{
    Installation desktop{u"Desktop"_s};
    Installation old{u"Old"_s};
    const int phone = desktop.database.findOrAddNamidaSource(u"Phone"_s);
    ASSERT_NE(phone, 0);

    auto listen   = PlayHistoryEntry::fromTrack(m_library.at(0), at(2));
    listen.source = phone;
    listen.origin = PlayHistoryImport::fooyinOrigin(old.database.deviceId());
    ASSERT_TRUE(desktop.database.insert(listen));

    ASSERT_TRUE(old.database.insert(PlayHistoryEntry::fromTrack(m_library.at(1), at(5))));
    EXPECT_EQ(importInto(*desktop.history, old.exportTo(m_dir.filePath(u"old.json"_s)), m_library).added, 1);

    const int device = desktop.database.findSource(u"fooyin"_s, old.database.deviceId());
    EXPECT_NE(device, 0);
    EXPECT_NE(device, phone);
}

TEST_F(PlayHistoryImportTest, AnInstallationsListensGoWhereTheRestOfThemAreWhateverTheirTime)
{
    // The desktop was given some of the old installation's listens by a device that didn't remember being part of it
    Installation desktop{u"Desktop"_s};
    Installation old{u"Old"_s};
    const int device = desktop.database.findOrAddSource(u"fooyin"_s, u"new-install"_s, u"New"_s);
    ASSERT_NE(device, 0);

    const auto give = [&](qint64 timestamp, const QString& installation) {
        auto listen   = PlayHistoryEntry::fromTrack(m_library.at(0), timestamp);
        listen.source = device;
        listen.origin = PlayHistoryImport::fooyinOrigin(installation);
        ASSERT_TRUE(desktop.database.insert(listen));
    };
    give(at(2), old.database.deviceId());
    give(at(3), u"other-install"_s);

    // The old installation's own export has none of the listens the desktop was given
    ASSERT_TRUE(old.database.insert(PlayHistoryEntry::fromTrack(m_library.at(1), at(5))));
    EXPECT_EQ(importInto(*desktop.history, old.exportTo(m_dir.filePath(u"old.json"_s)), m_library).added, 1);
    EXPECT_EQ(desktop.database.findSource(u"fooyin"_s, old.database.deviceId()), device);

    // Nor does an export that has the desktop as part of the other installation, made by a device that merged them
    const QJsonObject root{
        {"format"_L1, "fooyin-listening-history"_L1},
        {"version"_L1, 2},
        {"device"_L1, QJsonObject{{"id"_L1, "laptop"_L1}, {"name"_L1, "Laptop"_L1}}},
        {"sources"_L1, QJsonArray{QJsonObject{{"id"_L1, 1},
                                              {"kind"_L1, "fooyin"_L1},
                                              {"device"_L1, desktop.database.deviceId()},
                                              {"name"_L1, "Desktop"_L1},
                                              {"aliases"_L1, QJsonArray{QJsonObject{{"device"_L1, "other-install"_L1},
                                                                                    {"name"_L1, "Other"_L1}}}}}}},
        {"listens"_L1, QJsonArray{QJsonObject{{"timestamp"_L1, at(6)},
                                              {"source"_L1, 1},
                                              {"origin"_L1, "fooyin:other-install"_L1},
                                              {"hash"_L1, m_library.at(2).hash()},
                                              {"duration"_L1, 180000}}}}};
    EXPECT_EQ(importInto(*desktop.history, writeFile(u"laptop.json"_s, QJsonDocument{root}.toJson()), m_library).added,
              1);
    EXPECT_EQ(desktop.database.findSource(u"fooyin"_s, u"other-install"_s), device);

    const auto sources = desktop.history->sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).listens, 4);
}

TEST_F(PlayHistoryImportTest, RejectsExportsThatMixUpMergedInstallations)
{
    const auto exportWith = [this](const QJsonArray& sources, const QJsonArray& listens) {
        const QJsonObject root{{"format"_L1, "fooyin-listening-history"_L1},
                               {"version"_L1, 1},
                               {"device"_L1, QJsonObject{{"id"_L1, "desktop"_L1}, {"name"_L1, "Desktop"_L1}}},
                               {"sources"_L1, sources},
                               {"listens"_L1, listens}};
        return writeFile(u"export.json"_s, QJsonDocument{root}.toJson());
    };
    const auto listen = [this](int source, const QString& origin) {
        return QJsonObject{{"timestamp"_L1, at(1)},
                           {"source"_L1, source},
                           {"origin"_L1, origin},
                           {"hash"_L1, "abc"_L1},
                           {"duration"_L1, 180000}};
    };
    const QJsonObject laptop{{"id"_L1, 1},
                             {"kind"_L1, "fooyin"_L1},
                             {"device"_L1, "laptop"_L1},
                             {"name"_L1, "Laptop"_L1},
                             {"aliases"_L1, QJsonArray{QJsonObject{{"device"_L1, "old"_L1}, {"name"_L1, "Old"_L1}}}}};

    // A listen from a merged installation belongs to the device it was merged into
    auto history = PlayHistoryImport::readFooyin(exportWith({laptop}, {listen(1, u"fooyin:old"_s)}));
    ASSERT_TRUE(history.has_value()) << history.error().toStdString();
    ASSERT_EQ(history->sources.size(), 2U);
    ASSERT_EQ(history->sources.at(1).aliases.size(), 1U);
    EXPECT_EQ(history->sources.at(1).aliases.front().deviceId, u"old"_s);

    // But not to any other
    EXPECT_FALSE(PlayHistoryImport::readFooyin(exportWith({laptop}, {listen(0, u"fooyin:old"_s)})).has_value());

    // Nor can an installation be part of two devices, or a device and part of another
    QJsonObject other{laptop};
    other["id"_L1]     = 2;
    other["device"_L1] = "other"_L1;
    EXPECT_FALSE(PlayHistoryImport::readFooyin(exportWith({laptop, other}, {})).has_value());

    QJsonObject old{{"id"_L1, 2}, {"kind"_L1, "fooyin"_L1}, {"device"_L1, "old"_L1}, {"name"_L1, "Old"_L1}};
    EXPECT_FALSE(PlayHistoryImport::readFooyin(exportWith({laptop, old}, {})).has_value());

    QJsonObject itself{laptop};
    itself["aliases"_L1] = QJsonArray{QJsonObject{{"device"_L1, "desktop"_L1}}};
    EXPECT_FALSE(PlayHistoryImport::readFooyin(exportWith({itself}, {})).has_value());

    // Namida doesn't identify the device, so it has nothing to merge this way
    QJsonObject phone{laptop};
    phone["kind"_L1] = "namida"_L1;
    EXPECT_FALSE(PlayHistoryImport::readFooyin(exportWith({phone}, {})).has_value());
}

TEST_F(PlayHistoryImportTest, APhoneForwardedByAnotherInstallationIsTheSamePhone)
{
    // The desktop imported the phone's backup a while ago
    PlayHistory desktop{m_pool};
    desktop
        .importFile(writeFile(u"old.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                          {at(2), u"Modjo/Modjo/Chillin'.mp3"_s}})),
                    u"Phone"_s, m_library)
        .waitForFinished();

    // Since then the laptop has imported a newer one
    QTemporaryDir laptopDir;
    auto laptopPool = createDbPool(laptopDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler laptopHandler{laptopPool};
    PlayHistory laptop{laptopPool};
    laptop
        .importFile(writeFile(u"new.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                          {at(2), u"Modjo/Modjo/Chillin'.mp3"_s},
                                                          {at(9), u"Frizk/Forgotten Arcade/Flashback.mp3"_s}})),
                    u"Phone"_s, m_library)
        .waitForFinished();

    const QString laptopExport = m_dir.filePath(u"laptop.json"_s);
    ASSERT_EQ(laptop.exportFile(laptopExport).result(), QString{});

    const auto result = desktop.importFile(laptopExport, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.devices, QStringList{u"Phone"_s});

    // The new listen joins the rest of the phone's, so the phone can still be removed as a whole
    const auto sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).name, u"Phone"_s);
    EXPECT_EQ(sources.at(1).listens, 3);
}

TEST_F(PlayHistoryImportTest, RestoresARemovedDeviceFromTheInstallationsOwnExport)
{
    PlayHistory history{m_pool};
    history.recordPlay(m_library.at(0));
    history.waitForWrites();
    history
        .importFile(writeFile(u"backup.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                             {at(2), u"Modjo/Modjo/Chillin'.mp3"_s}})),
                    u"Phone"_s, m_library)
        .waitForFinished();

    const QString exported = m_dir.filePath(u"export.json"_s);
    ASSERT_EQ(history.exportFile(exported).result(), QString{});

    ASSERT_TRUE(history.removeSource(history.sources().result().at(1).id).result());
    ASSERT_EQ(listens(m_pool).size(), 1U);

    const auto result = history.importFile(exported, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 2);
    EXPECT_EQ(result.duplicates, 1);
    EXPECT_EQ(result.devices, QStringList{u"Phone"_s});

    const auto entries = listens(m_pool);
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(std::ranges::count(entries, QString{}, &PlayHistoryEntry::origin), 1);
}

TEST_F(PlayHistoryImportTest, RejectsExportsWithListensFromAnUndescribedDevice)
{
    PlayHistory history{m_pool};

    const QJsonObject listen{{u"timestamp"_s, at(1)}, {u"source"_s, 7},        {u"origin"_s, u"namida"_s},
                             {u"hash"_s, u"abc"_s},   {u"title"_s, u"Song"_s}, {u"duration"_s, 1000}};
    const QJsonObject root{{u"format"_s, u"fooyin-listening-history"_s},
                           {u"version"_s, 1},
                           {u"device"_s, QJsonObject{{u"id"_s, u"other"_s}, {u"name"_s, u"Laptop"_s}}},
                           {u"sources"_s, QJsonArray{}},
                           {u"listens"_s, QJsonArray{listen}}};

    const auto result
        = history.importFile(writeFile(u"bad.json"_s, QJsonDocument{root}.toJson()), {}, m_library).result();
    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(listens(m_pool).empty());
}

TEST_F(PlayHistoryImportTest, SkipsListensWithImpossibleValues)
{
    PlayHistory history{m_pool};

    QJsonArray day{
        QJsonObject{{u"dateAdded"_s, at(1)}, {u"track"_s, phonePath(u"Kiesza/Hideaway/Hideaway.mp3"_s)}},
        QJsonObject{{u"dateAdded"_s, 1e300}, {u"track"_s, phonePath(u"Modjo/Modjo/Chillin'.mp3"_s)}},
        QJsonObject{{u"dateAdded"_s, -5}, {u"track"_s, phonePath(u"Modjo/Modjo/Chillin'.mp3"_s)}},
        QJsonObject{{u"dateAdded"_s, 1774441174948.5}, {u"track"_s, phonePath(u"Modjo/Modjo/Chillin'.mp3"_s)}}};

    const auto result
        = history.importFile(writeFile(u"20537.json"_s, QJsonDocument{day}.toJson()), u"Phone"_s, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.invalid, 3);
}

TEST_F(PlayHistoryImportTest, RejectsABackupWithADamagedDay)
{
    PlayHistory history{m_pool};

    const QByteArray historyZip = makeZip({{u"20537.json"_s, namidaDay({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}})},
                                           {u"20538.json"_s, "[{\"dateAdded\": 17"}});
    const QString backup        = writeFile(u"backup.zip"_s, makeZip({{u"TEMPDIR_History.zip"_s, historyZip}}));

    const auto result = history.importFile(backup, u"Phone"_s, m_library).result();
    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(result.error.contains(u"20538.json"_s)) << result.error.toStdString();
    EXPECT_TRUE(listens(m_pool).empty());
}

TEST_F(PlayHistoryImportTest, DoesNotReturnTheContentsOfADamagedFileInAnArchive)
{
    QByteArray archive = makeZip({{u"a.txt"_s, QByteArray(1000, 'a')}});
    // Flip a byte of the compressed data, just after the 30 byte header and the 5 byte name
    archive[40] = static_cast<char>(archive.at(40) ^ 0x55);

    const ZipReader zip{archive};
    ASSERT_TRUE(zip.isValid());
    EXPECT_TRUE(zip.read(zip.entries().front()).isEmpty());
}

TEST_F(PlayHistoryImportTest, RejectsFilesThatAreNotListeningHistory)
{
    PlayHistory history{m_pool};

    const auto result = history.importFile(writeFile(u"other.json"_s, R"({"hello": "world"})"), {}, m_library).result();
    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(listens(m_pool).empty());
}
TEST_F(PlayHistoryImportTest, ListensFromBeforeTheUpgradeWereRecordedHere)
{
    QTemporaryDir dir;

    DbConnection::DbParams params;
    params.type     = u"QSQLITE"_s;
    params.filePath = dir.filePath(u"fooyin.db"_s);
    auto pool       = DbConnectionPool::create(params, u"fooyin-playhistoryimport-upgrade"_s);
    const DbConnectionHandler handler{pool};

    DbSchema schema{DbConnectionProvider{pool}};
    ASSERT_EQ(schema.upgradeDatabase(26, u"://dbschema.xml"_s), DbSchema::UpgradeResult::Success);

    {
        QSqlQuery query{DbConnectionProvider{pool}.db()};
        ASSERT_TRUE(
            query.exec(u"INSERT INTO PlayHistory (Timestamp, Hash, Title) VALUES (%1, 'abc', 'Song');"_s.arg(at(1))));
    }

    ASSERT_EQ(schema.upgradeDatabase(CurrentSchemaVersion, u"://dbschema.xml"_s), DbSchema::UpgradeResult::Success);

    PlayHistoryDatabase database;
    database.initialise(DbConnectionProvider{pool});

    const auto entries = database.all();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().source, 0);
    EXPECT_TRUE(entries.front().origin.isEmpty());
    EXPECT_TRUE(database.contains({}, at(1)));

    const auto sources = database.sources();
    ASSERT_EQ(sources.size(), 1U);
    EXPECT_EQ(sources.front().listens, 1);
}
TEST_F(PlayHistoryImportTest, RecognisesAForwardedPhoneByItsListensWhateverItsCalled)
{
    PlayHistory desktop{m_pool};
    desktop
        .importFile(writeFile(u"old.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                          {at(2), u"Modjo/Modjo/Chillin'.mp3"_s}})),
                    u"Pixel"_s, m_library)
        .waitForFinished();

    // The laptop calls the same phone something else
    QTemporaryDir laptopDir;
    auto laptopPool = createDbPool(laptopDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler laptopHandler{laptopPool};
    PlayHistory laptop{laptopPool};
    laptop
        .importFile(writeFile(u"new.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                          {at(2), u"Modjo/Modjo/Chillin'.mp3"_s},
                                                          {at(9), u"Frizk/Forgotten Arcade/Flashback.mp3"_s}})),
                    u"Phone"_s, m_library)
        .waitForFinished();

    const QString laptopExport = m_dir.filePath(u"laptop.json"_s);
    ASSERT_EQ(laptop.exportFile(laptopExport).result(), QString{});

    const auto result = desktop.importFile(laptopExport, {}, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);

    const auto sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).name, u"Pixel"_s);
    EXPECT_EQ(sources.at(1).listens, 3);
}

TEST_F(PlayHistoryImportTest, KeepsDifferentPhonesWithTheSameNameApart)
{
    PlayHistory desktop{m_pool};
    desktop
        .importFile(writeFile(u"mine.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                           {at(2), u"Modjo/Modjo/Chillin'.mp3"_s}})),
                    u"Phone"_s, m_library)
        .waitForFinished();

    // Another phone, which only the laptop has imported, under the same name
    QTemporaryDir laptopDir;
    auto laptopPool = createDbPool(laptopDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler laptopHandler{laptopPool};
    PlayHistory laptop{laptopPool};
    laptop
        .importFile(writeFile(u"theirs.zip"_s, namidaBackup({{at(5), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                             {at(6), u"Frizk/Forgotten Arcade/Flashback.mp3"_s}})),
                    u"Phone"_s, m_library)
        .waitForFinished();

    const QString laptopExport = m_dir.filePath(u"laptop.json"_s);
    ASSERT_EQ(laptop.exportFile(laptopExport).result(), QString{});
    ASSERT_EQ(desktop.importFile(laptopExport, {}, m_library).result().added, 2);

    // Each can be removed without taking the other's listens with it, and they can be told apart when importing
    const auto sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 3U);
    EXPECT_EQ(sources.at(1).name, u"Phone"_s);
    EXPECT_EQ(sources.at(1).listens, 2);
    EXPECT_EQ(sources.at(2).name, u"Phone (2)"_s);
    EXPECT_EQ(sources.at(2).listens, 2);
    EXPECT_EQ(desktop.renameSource(sources.at(2).id, u"Phone"_s).result(), PlayHistoryRenameResult::NameTaken);
    EXPECT_EQ(desktop.renameSource(sources.at(2).id, u"Their phone"_s).result(), PlayHistoryRenameResult::Renamed);
}

TEST_F(PlayHistoryImportTest, RejectsExportsThatDescribeTheirDevicesWrongly)
{
    PlayHistory history{m_pool};
    const QJsonObject phone{{u"id"_s, 1}, {u"kind"_s, u"namida"_s}, {u"device"_s, u"p"_s}, {u"name"_s, u"Phone"_s}};

    const std::vector<std::pair<const char*, QByteArray>> files{
        {"a device id that isn't a number", fooyinExport({phone}, {exportedListen(at(1), u"1"_s, u"namida"_s)})},
        {"a listen whose origin isn't its device's",
         fooyinExport({phone}, {exportedListen(at(1), 1, u"fooyin:laptop"_s)})},
        {"another device in the place of the one the file came from",
         fooyinExport({QJsonObject{{u"id"_s, 0}, {u"kind"_s, u"namida"_s}, {u"device"_s, u"p"_s}}},
                      {exportedListen(at(1), 0, u"namida"_s)})},
        {"two devices with the same id", fooyinExport({phone, phone}, {exportedListen(at(1), 1, u"namida"_s)})},
    };

    for(const auto& [what, data] : files) {
        const auto result = history.importFile(writeFile(u"bad.json"_s, data), {}, m_library).result();
        EXPECT_FALSE(result.succeeded()) << what;
    }
    EXPECT_TRUE(listens(m_pool).empty());

    // The same, described properly
    const auto result
        = history
              .importFile(writeFile(u"good.json"_s, fooyinExport({phone}, {exportedListen(at(1), 1, u"namida"_s)})), {},
                          m_library)
              .result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
}

TEST_F(PlayHistoryImportTest, LeavesOutImpossibleLengthsAndTimings)
{
    PlayHistory history{m_pool};

    QJsonObject tooLong          = exportedListen(at(1), 0, u"fooyin:laptop"_s);
    tooLong[u"duration"_s]       = 1e15;
    QJsonObject badTiming        = exportedListen(at(2), 0, u"fooyin:laptop"_s);
    badTiming[u"countedAfter"_s] = 9.2e18;
    badTiming[u"countedAt"_s]    = -40;

    const auto result
        = history.importFile(writeFile(u"export.json"_s, fooyinExport({}, {tooLong, badTiming})), {}, m_library)
              .result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.invalid, 1);

    const auto entries = listens(m_pool);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().countedAfter, -1);
    EXPECT_EQ(entries.front().countedAt, -1);
}

TEST_F(PlayHistoryImportTest, RecognisesADayOfNamidaHistoryWhateverItStartsWith)
{
    QJsonArray day{QJsonObject{{u"dateAdded"_s, QJsonValue{}}, {u"track"_s, phonePath(u"a.mp3"_s)}},
                   QJsonObject{{u"dateAdded"_s, at(1)}, {u"track"_s, phonePath(u"Kiesza/Hideaway/Hideaway.mp3"_s)}}};
    EXPECT_EQ(PlayHistory::fileFormat(writeFile(u"20537.json"_s, QJsonDocument{day}.toJson())),
              PlayHistoryFormat::Namida);
}

TEST_F(PlayHistoryImportTest, RejectsAnArchiveThatDoesNotListAllItsFiles)
{
    QByteArray archive = makeZip({{u"20537.json"_s, "[]"}, {u"20538.json"_s, "[]"}});
    // Make the end of the archive say there's one fewer file than its directory lists
    const auto eocd    = archive.size() - 22;
    archive[eocd + 8]  = 1;
    archive[eocd + 10] = 1;

    EXPECT_FALSE(ZipReader{archive}.isValid());
}
TEST_F(PlayHistoryImportTest, TellsListensAtTheSameTimeApartByTheirTracks)
{
    PlayHistory history{m_pool};

    // As when Namida matches a listen it imported to more than one track
    const QString backup = writeFile(u"backup.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s},
                                                                    {at(1), u"Modjo/Modjo/Chillin'.mp3"_s},
                                                                    {at(2), u"Modjo/Modjo/Chillin'.mp3"_s},
                                                                    {at(2), u"Modjo/Modjo/Chillin'.mp3"_s}}));

    auto result = history.importFile(backup, u"Phone"_s, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    // The same track twice at the same time is the same listen
    EXPECT_EQ(result.added, 3);
    EXPECT_EQ(result.duplicates, 1);

    result = history.importFile(backup, u"Phone"_s, m_library).result();
    EXPECT_EQ(result.added, 0);
    EXPECT_EQ(result.duplicates, 4);

    // Passed on by another installation, they're still told apart
    QTemporaryDir otherDir;
    auto otherPool = createDbPool(otherDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler otherHandler{otherPool};
    PlayHistory other{otherPool};

    const QString exported = m_dir.filePath(u"export.json"_s);
    ASSERT_EQ(history.exportFile(exported).result(), QString{});
    result = other.importFile(exported, {}, m_library).result();
    EXPECT_EQ(result.added, 3);
    result = other.importFile(exported, {}, m_library).result();
    EXPECT_EQ(result.added, 0);
}

TEST_F(PlayHistoryImportTest, MergesDevicesThatTurnOutToBeTheSame)
{
    PlayHistory desktop{m_pool};
    desktop
        .importFile(writeFile(u"a.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}})), u"Phone"_s,
                    m_library)
        .waitForFinished();

    // The same phone by way of a laptop, with no listens in common to show it's the same
    const QJsonObject phone{
        {u"id"_s, 1}, {u"kind"_s, u"namida"_s}, {u"device"_s, u"laptops-phone"_s}, {u"name"_s, u"Phone"_s}};
    desktop
        .importFile(writeFile(u"laptop.json"_s, fooyinExport({phone}, {exportedListen(at(2), 1, u"namida"_s)})), {},
                    m_library)
        .waitForFinished();

    auto sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 3U);
    ASSERT_EQ(sources.at(2).deviceId, u"laptops-phone"_s);

    // Only devices of the same kind, and not this one
    EXPECT_FALSE(desktop.mergeSource(sources.at(2).id, 0).result());
    EXPECT_TRUE(desktop.mergeSource(sources.at(2).id, sources.at(1).id).result());

    sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).listens, 2);

    // Later imports of the laptop's copy of the phone go to the merged device
    const auto result
        = desktop
              .importFile(writeFile(u"laptop2.json"_s, fooyinExport({phone}, {exportedListen(at(3), 1, u"namida"_s)})),
                          {}, m_library)
              .result();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.devices, QStringList{u"Phone"_s});

    sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).listens, 3);

    // Removing the device takes everything merged into it too
    EXPECT_TRUE(desktop.removeSource(sources.at(1).id).result());
    EXPECT_TRUE(listens(m_pool).empty());
}

TEST_F(PlayHistoryImportTest, AFolderEveryAlbumMightHaveIsNotEnoughToMatch)
{
    TrackList library{
        makeTrack(u"/music/Artist B/Album B/CD1/01 - Intro.mp3"_s, u"Intro"_s, u"Artist B"_s, u"Album B"_s)};
    const PlayHistoryImport::TrackMatcher matcher{library};

    EXPECT_EQ(matcher.match(phonePath(u"Artist A/Album A/CD1/01 - Intro.mp3"_s)), nullptr);
    EXPECT_NE(matcher.match(phonePath(u"Artist B/Album B/CD1/01 - Intro.mp3"_s)), nullptr);
    EXPECT_NE(matcher.match(phonePath(u"Album B/CD1/01 - Intro.flac"_s)), nullptr);
}
TEST_F(PlayHistoryImportTest, RecognisesAPhoneByItsListensWhenImportedUnderAnotherName)
{
    PlayHistory history{m_pool};

    std::vector<std::pair<qint64, QString>> phone{{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}};
    history.importFile(writeFile(u"first.zip"_s, namidaBackup(phone)), u"Phone"_s, m_library).waitForFinished();

    // A newer backup of the same phone, given another name
    phone.emplace_back(at(2), u"Modjo/Modjo/Chillin'.mp3"_s);
    const auto result
        = history.importFile(writeFile(u"second.zip"_s, namidaBackup(phone)), u"Pixel"_s, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.devices, QStringList{u"Phone"_s});

    const auto sources = history.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).name, u"Phone"_s);
    EXPECT_EQ(sources.at(1).listens, 2);
}

TEST_F(PlayHistoryImportTest, RecognisesAPhoneThatFirstCameByWayOfAnotherInstallation)
{
    // The laptop imported the phone's backup and its history came to the desktop from there
    QTemporaryDir laptopDir;
    auto laptopPool = createDbPool(laptopDir.filePath(u"fooyin.db"_s));
    const DbConnectionHandler laptopHandler{laptopPool};
    PlayHistory laptop{laptopPool};

    std::vector<std::pair<qint64, QString>> phone{{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}};
    laptop.importFile(writeFile(u"first.zip"_s, namidaBackup(phone)), u"Phone"_s, m_library).waitForFinished();

    const QString laptopExport = m_dir.filePath(u"laptop.json"_s);
    ASSERT_EQ(laptop.exportFile(laptopExport).result(), QString{});

    PlayHistory desktop{m_pool};
    desktop.importFile(laptopExport, {}, m_library).waitForFinished();

    // Then a newer backup straight from the phone, under another name
    phone.emplace_back(at(2), u"Modjo/Modjo/Chillin'.mp3"_s);
    const auto result
        = desktop.importFile(writeFile(u"second.zip"_s, namidaBackup(phone)), u"Pixel"_s, m_library).result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.added, 1);
    EXPECT_EQ(result.devices, QStringList{u"Phone"_s});

    const auto sources = desktop.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).listens, 2);
}

TEST_F(PlayHistoryImportTest, ImportingUnderTheNameOfAMergedDeviceGoesToTheOneItWasMergedInto)
{
    PlayHistory history{m_pool};

    history
        .importFile(writeFile(u"a.zip"_s, namidaBackup({{at(1), u"Kiesza/Hideaway/Hideaway.mp3"_s}})), u"Phone"_s,
                    m_library)
        .waitForFinished();
    history
        .importFile(writeFile(u"b.zip"_s, namidaBackup({{at(5), u"Modjo/Modjo/Chillin'.mp3"_s}})), u"Pixel"_s,
                    m_library)
        .waitForFinished();

    auto sources = history.sources().result();
    ASSERT_EQ(sources.size(), 3U);
    ASSERT_TRUE(history.mergeSource(sources.at(2).id, sources.at(1).id).result());

    // A backup with nothing in common with what's here, under the old name
    const auto result
        = history
              .importFile(writeFile(u"c.zip"_s, namidaBackup({{at(9), u"Frizk/Forgotten Arcade/Flashback.mp3"_s}})),
                          u"Pixel"_s, m_library)
              .result();
    ASSERT_TRUE(result.succeeded()) << result.error.toStdString();
    EXPECT_EQ(result.devices, QStringList{u"Phone"_s});

    sources = history.sources().result();
    ASSERT_EQ(sources.size(), 2U);
    EXPECT_EQ(sources.at(1).listens, 3);

    // And the name isn't given to a new device either
    PlayHistoryDatabase database;
    database.initialise(DbConnectionProvider{m_pool});
    EXPECT_EQ(database.findOrAddNamidaSource(u"Pixel"_s), sources.at(1).id);
}

TEST_F(PlayHistoryImportTest, TellsALargeExportFromHowItStarts)
{
    QJsonArray listens;
    for(int i{0}; i < 1000; ++i) {
        listens.append(exportedListen(at(1) + i, 0, u"fooyin:laptop"_s));
    }
    const QByteArray data = fooyinExport({}, listens);
    ASSERT_GT(data.size(), 64 * 1024);

    EXPECT_EQ(PlayHistory::fileFormat(writeFile(u"large.json"_s, data)), PlayHistoryFormat::Fooyin);
    // Only the start is looked at, so a file that merely starts like one is found out when it's imported
    const QString truncated = writeFile(u"truncated.json"_s, data.left(20 * 1024));
    EXPECT_EQ(PlayHistory::fileFormat(truncated), PlayHistoryFormat::Fooyin);

    PlayHistory history{m_pool};
    EXPECT_FALSE(history.importFile(truncated, {}, m_library).result().succeeded());
    EXPECT_EQ(history.importFile(writeFile(u"large2.json"_s, data), {}, m_library).result().added, 1000);
}
} // namespace Fooyin::Testing
