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

#include "playhistoryimport.h"

#include "zipreader.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cmath>
#include <limits>
#include <optional>

using namespace Qt::StringLiterals;

namespace {
constexpr auto FormatName   = "fooyin-listening-history";
constexpr int FormatVersion = 1;
//! How much of a file is read to tell what kind of history it holds.
constexpr qint64 SniffSize = 16 * 1024;
//! How deep to look for the history inside a backup. Namida's backup is a zip of zips.
constexpr int MaxArchiveDepth = 2;

QString tr(const char* text)
{
    return QCoreApplication::translate("Fooyin::PlayHistory", text);
}

QString fileName(const QString& path)
{
    const auto slash = path.lastIndexOf(u'/');
    return slash < 0 ? path : path.mid(slash + 1);
}

//! Namida keeps a file of listens for each day, named after the number of days since the epoch.
bool isNamidaDayFile(const QString& entryName)
{
    static const QRegularExpression dayFile{u"^\\d+\\.json$"_s};
    return dayFile.match(fileName(entryName)).hasMatch();
}

//! The history archive in a Namida backup, as opposed to those for its YouTube history, playlists and so on.
bool isNamidaHistoryArchive(const QString& entryName)
{
    const QString name = fileName(entryName);
    return name.endsWith(".zip"_L1, Qt::CaseInsensitive) && name.contains("History"_L1, Qt::CaseInsensitive)
        && !name.startsWith("YOUTUBE"_L1, Qt::CaseInsensitive);
}

bool isNamidaListen(const QJsonValue& value)
{
    const QJsonObject object = value.toObject();
    return object.value("dateAdded"_L1).isDouble() && object.value("track"_L1).isString();
}

//! A whole number of milliseconds held by @p value, or -1 if it isn't one that fits.
qint64 toTimestamp(const QJsonValue& value)
{
    // Doubles hold integers exactly up to 2^53, far beyond any real time, and converting one out of range is undefined
    constexpr double MaxExact = 9007199254740992.0;

    const double number = value.toDouble(-1);
    if(number < 0 || number > MaxExact || std::trunc(number) != number) {
        return -1;
    }
    return static_cast<qint64>(number);
}

//! Reads a day of Namida history into @p history. Returns false if it isn't one.
bool readNamidaDay(const QByteArray& data, Fooyin::PlayHistoryImport::NamidaHistory& history)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if(error.error != QJsonParseError::NoError || !document.isArray()) {
        return false;
    }

    const QJsonArray array = document.array();
    for(const auto& value : array) {
        const qint64 timestamp = isNamidaListen(value) ? toTimestamp(value.toObject().value("dateAdded"_L1)) : -1;
        if(!Fooyin::PlayHistoryImport::isPlausibleTimestamp(timestamp)) {
            ++history.invalid;
            continue;
        }
        history.listens.push_back({.timestamp = timestamp, .path = value.toObject().value("track"_L1).toString()});
    }

    return true;
}

bool archiveHasNamidaHistory(const Fooyin::ZipReader& zip)
{
    return std::ranges::any_of(zip.entries(), [](const auto& entry) {
        return isNamidaDayFile(entry.name) || isNamidaHistoryArchive(entry.name);
    });
}

//! Reads the history in @p zip into @p history. Returns the name of a part that couldn't be read, if any, as importing
//! the rest would quietly leave out the listens in it.
std::optional<QString> readNamidaArchive(const Fooyin::ZipReader& zip, int depth,
                                         Fooyin::PlayHistoryImport::NamidaHistory& history)
{
    for(const auto& entry : zip.entries()) {
        if(isNamidaDayFile(entry.name)) {
            if(!readNamidaDay(zip.read(entry), history)) {
                return entry.name;
            }
        }
        else if(depth < MaxArchiveDepth && isNamidaHistoryArchive(entry.name)) {
            const Fooyin::ZipReader nested{zip.read(entry)};
            if(!nested.isValid()) {
                return entry.name;
            }
            if(auto damaged = readNamidaArchive(nested, depth + 1, history)) {
                return entry.name + u'/' + *damaged;
            }
        }
    }

    return {};
}

//! The longest a track can be, in milliseconds. Anything longer is a mistake, and would swamp listening time.
constexpr qint64 MaxDuration = 7LL * 24 * 60 * 60 * 1000;

//! The id of a device in an export, which has to be a whole number.
std::optional<int> toId(const QJsonValue& value)
{
    const double number = value.toDouble(-1);
    if(!value.isDouble() || number < 0 || number > std::numeric_limits<int>::max() || std::trunc(number) != number) {
        return {};
    }
    return static_cast<int>(number);
}

//! How far into a listen it was counted, or -1 if that isn't known or is impossible.
qint64 toTiming(const QJsonValue& value)
{
    const qint64 timing = value.toInteger(-1);
    return timing < 0 || timing > MaxDuration ? -1 : timing;
}

QByteArray readStart(const QString& filepath)
{
    QFile file{filepath};
    if(!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.read(4);
}

QJsonArray toJson(const QStringList& list)
{
    return QJsonArray::fromStringList(list);
}

QStringList toStringList(const QJsonValue& value)
{
    QStringList list;
    const QJsonArray array = value.toArray();
    for(const auto& item : array) {
        list.append(item.toString());
    }
    return list;
}

QStringList pathParts(const QString& path)
{
    QString normalised = path;
    normalised.replace(u'\\', u'/');
    return normalised.toCaseFolded().split(u'/', Qt::SkipEmptyParts);
}

//! Whether @p folder (case folded) is one that many albums have, such as for each disc.
bool isGenericFolder(const QString& folder)
{
    static const QRegularExpression generic{
        u"^((cd|dis[ck]|vol(ume)?|part|side)[\\s._-]*[0-9a-z]{1,2}|bonus( tracks?)?|extras?|singles?|unknown album|"
        "music|tracks|songs)$"_s};
    return generic.match(folder).hasMatch();
}

QString baseName(const QString& filename)
{
    const auto dot = filename.lastIndexOf(u'.');
    return dot <= 0 ? filename : filename.left(dot);
}

QString suffix(const QString& filename)
{
    const auto dot = filename.lastIndexOf(u'.');
    return dot <= 0 ? QString{} : filename.mid(dot + 1);
}
} // namespace

namespace Fooyin::PlayHistoryImport {
QString fooyinOrigin(const QString& deviceId)
{
    return QLatin1String{FooyinKind} + u':' + deviceId;
}

PlayHistoryFormat detectFormat(const QString& filepath)
{
    if(ZipReader::isZip(readStart(filepath))) {
        const ZipReader zip{filepath};
        return zip.isValid() && archiveHasNamidaHistory(zip) ? PlayHistoryFormat::Namida : PlayHistoryFormat::Unknown;
    }

    QFile file{filepath};
    if(!file.open(QIODevice::ReadOnly)) {
        return PlayHistoryFormat::Unknown;
    }

    // An export of a long history runs to tens of megabytes, which would take a while to parse just to tell what it is.
    // How it starts is enough: the file is read properly when it's imported. The keys of an export are written in
    // alphabetical order, so its format comes after only the device and the time it was exported
    const QString start = QString::fromUtf8(file.read(SniffSize));

    static const QRegularExpression fooyin{uR"(^\s*\{.*"format"\s*:\s*")"_s + QLatin1String{FormatName} + u"\""_s,
                                           QRegularExpression::DotMatchesEverythingOption};
    static const QRegularExpression namida{uR"(^\s*\[\s*\{.*"dateAdded"\s*:)"_s,
                                           QRegularExpression::DotMatchesEverythingOption};

    if(fooyin.match(start).hasMatch()) {
        return PlayHistoryFormat::Fooyin;
    }
    if(namida.match(start).hasMatch()) {
        return PlayHistoryFormat::Namida;
    }

    return PlayHistoryFormat::Unknown;
}

bool isPlausibleTimestamp(qint64 timestamp)
{
    // From the start of 2000 to a year from now, as the clock of the device that recorded a listen may be ahead of
    // this one's
    constexpr qint64 Earliest = 946'684'800'000;
    constexpr qint64 Year     = 366LL * 86'400'000;
    return timestamp >= Earliest && timestamp <= QDateTime::currentMSecsSinceEpoch() + Year;
}

std::expected<NamidaHistory, QString> readNamida(const QString& filepath)
{
    NamidaHistory history;

    if(ZipReader::isZip(readStart(filepath))) {
        const ZipReader zip{filepath};
        if(!zip.isValid()) {
            return std::unexpected{tr("The archive could not be read.")};
        }
        if(const auto damaged = readNamidaArchive(zip, 1, history)) {
            return std::unexpected{tr("The backup is damaged: %1 could not be read.").arg(*damaged)};
        }
    }
    else {
        QFile file{filepath};
        if(!file.open(QIODevice::ReadOnly)) {
            return std::unexpected{tr("The file could not be opened: %1").arg(file.errorString())};
        }
        if(!readNamidaDay(file.readAll(), history)) {
            return std::unexpected{tr("The file is not a Namida history file.")};
        }
    }

    if(history.listens.empty()) {
        return std::unexpected{tr("No Namida listening history was found in the file.")};
    }

    std::ranges::sort(history.listens, {}, &NamidaListen::timestamp);
    return history;
}

std::expected<FooyinExport, QString> readFooyin(const QString& filepath)
{
    QFile file{filepath};
    if(!file.open(QIODevice::ReadOnly)) {
        return std::unexpected{tr("The file could not be opened: %1").arg(file.errorString())};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root       = document.object();

    if(parseError.error != QJsonParseError::NoError
       || root.value("format"_L1).toString() != QLatin1String{FormatName}) {
        return std::unexpected{tr("The file is not a fooyin listening history export.")};
    }
    if(root.value("version"_L1).toInt() > FormatVersion) {
        return std::unexpected{tr("The file was exported by a newer version of fooyin.")};
    }

    FooyinExport history;

    const QJsonObject device = root.value("device"_L1).toObject();
    history.deviceId         = device.value("id"_L1).toString();
    history.deviceName       = device.value("name"_L1).toString();
    history.exported         = root.value("exported"_L1).toInteger();

    if(history.deviceId.isEmpty()) {
        return std::unexpected{tr("The file does not say which device it was exported from.")};
    }

    const QString damaged = tr("The file is damaged: the devices its listens are from are not described properly.");

    // The device the file came from is always 0, even if it isn't listed. A file that says otherwise can't be trusted
    // with who recorded what, which once imported couldn't be put right by importing a better file, as it would then
    // be a duplicate
    QHash<int, ExportedSource> sources;
    sources.insert(
        0, {.id = 0, .kind = QLatin1String{FooyinKind}, .deviceId = history.deviceId, .name = history.deviceName});

    bool exporterListed{false};
    const QJsonArray sourceArray = root.value("sources"_L1).toArray();
    for(const auto& value : sourceArray) {
        const QJsonObject object = value.toObject();
        const auto id            = toId(object.value("id"_L1));

        ExportedSource source;
        source.id       = id.value_or(-1);
        source.kind     = object.value("kind"_L1).toString();
        source.deviceId = object.value("device"_L1).toString();
        source.name     = object.value("name"_L1).toString();

        const bool known = source.kind == QLatin1String{FooyinKind} || source.kind == QLatin1String{NamidaKind};
        if(!id || !known || source.deviceId.isEmpty()) {
            return std::unexpected{damaged};
        }
        if(source.id == 0) {
            if(source.kind != QLatin1String{FooyinKind} || source.deviceId != history.deviceId || exporterListed) {
                return std::unexpected{damaged};
            }
            exporterListed  = true;
            sources[0].name = source.name;
            continue;
        }
        if(sources.contains(source.id)) {
            return std::unexpected{damaged};
        }
        sources.insert(source.id, source);
    }

    for(const auto& source : std::as_const(sources)) {
        history.sources.push_back(source);
    }
    std::ranges::sort(history.sources, {}, &ExportedSource::id);

    const QJsonArray listens = root.value("listens"_L1).toArray();
    history.listens.reserve(static_cast<size_t>(listens.size()));

    for(const auto& value : listens) {
        const QJsonObject object = value.toObject();

        PlayHistoryEntry entry;
        entry.timestamp = object.value("timestamp"_L1).toInteger();
        entry.origin    = object.value("origin"_L1).toString();
        if(object.contains("origin"_L1) && !object.value("origin"_L1).isString()) {
            return std::unexpected{damaged};
        }

        // Where a listen was recorded follows from its device, and the two have to agree
        const auto source = object.contains("source"_L1) ? toId(object.value("source"_L1)) : std::optional<int>{0};
        if(!source || !sources.contains(*source)) {
            return std::unexpected{damaged};
        }
        entry.source = *source;

        const ExportedSource listenSource = sources.value(entry.source);
        const QString deviceOrigin        = listenSource.kind == QLatin1String{NamidaKind}
                                              ? QString{QLatin1String{NamidaOrigin}}
                                              : fooyinOrigin(listenSource.deviceId);
        if(entry.origin.isEmpty()) {
            entry.origin = deviceOrigin;
        }
        else if(entry.origin != deviceOrigin) {
            return std::unexpected{damaged};
        }

        entry.hash         = object.value("hash"_L1).toString();
        entry.title        = object.value("title"_L1).toString();
        entry.artists      = toStringList(object.value("artists"_L1));
        entry.album        = object.value("album"_L1).toString();
        entry.albumArtists = toStringList(object.value("albumArtists"_L1));
        entry.genres       = toStringList(object.value("genres"_L1));
        entry.composers    = toStringList(object.value("composers"_L1));
        entry.year         = object.value("year"_L1).toInt();
        entry.bitrate      = object.value("bitrate"_L1).toInt();
        entry.lossless     = object.value("lossless"_L1).toInt(-1);
        entry.countedAfter = toTiming(object.value("countedAfter"_L1));
        entry.countedAt    = toTiming(object.value("countedAt"_L1));

        const qint64 duration = object.value("duration"_L1).toInteger(-1);
        if(!isPlausibleTimestamp(entry.timestamp) || duration < 0 || duration > MaxDuration || entry.hash.isEmpty()) {
            ++history.invalid;
            continue;
        }
        entry.duration = static_cast<uint64_t>(duration);
        history.listens.push_back(std::move(entry));
    }

    return history;
}

QByteArray writeFooyin(const FooyinExport& history)
{
    QJsonObject root;
    root["format"_L1]   = QLatin1String{FormatName};
    root["version"_L1]  = FormatVersion;
    root["exported"_L1] = history.exported;
    root["device"_L1]   = QJsonObject{{"id"_L1, history.deviceId}, {"name"_L1, history.deviceName}};

    QJsonArray sources;
    for(const auto& source : history.sources) {
        sources.append(QJsonObject{
            {"id"_L1, source.id}, {"kind"_L1, source.kind}, {"device"_L1, source.deviceId}, {"name"_L1, source.name}});
    }
    root["sources"_L1] = sources;

    QJsonArray listens;
    for(const auto& entry : history.listens) {
        listens.append(QJsonObject{{"timestamp"_L1, entry.timestamp},
                                   {"source"_L1, entry.source},
                                   {"origin"_L1, entry.origin},
                                   {"hash"_L1, entry.hash},
                                   {"title"_L1, entry.title},
                                   {"artists"_L1, toJson(entry.artists)},
                                   {"album"_L1, entry.album},
                                   {"albumArtists"_L1, toJson(entry.albumArtists)},
                                   {"genres"_L1, toJson(entry.genres)},
                                   {"composers"_L1, toJson(entry.composers)},
                                   {"year"_L1, entry.year},
                                   {"duration"_L1, static_cast<qint64>(entry.duration)},
                                   {"bitrate"_L1, entry.bitrate},
                                   {"lossless"_L1, entry.lossless},
                                   {"countedAfter"_L1, entry.countedAfter},
                                   {"countedAt"_L1, entry.countedAt}});
    }
    root["listens"_L1] = listens;

    return QJsonDocument{root}.toJson(QJsonDocument::Compact);
}

TrackMatcher::TrackMatcher(const TrackList& tracks)
    : m_tracks{tracks}
{
    for(int i{0}; std::cmp_less(i, m_tracks.size()); ++i) {
        const QStringList parts = pathParts(m_tracks.at(i).prettyFilepath());
        if(!parts.isEmpty()) {
            m_byName[baseName(parts.back())].push_back(i);
        }
    }
}

const Track* TrackMatcher::match(const QString& path) const
{
    const QStringList parts = pathParts(path);
    if(parts.isEmpty()) {
        return nullptr;
    }

    const auto candidates = m_byName.constFind(baseName(parts.back()));
    if(candidates == m_byName.cend()) {
        return nullptr;
    }

    const QString extension = suffix(parts.back());

    const Track* best{nullptr};
    int bestScore{-1};
    bool ambiguous{false};

    for(const int index : candidates.value()) {
        const Track& track           = m_tracks.at(index);
        const QStringList trackParts = pathParts(track.prettyFilepath());

        // Each folder in common counts for more than the format being the same. Folders such as "CD1" only count along
        // with one that says more, as many albums have them
        int folders{0};
        bool telling{false};
        while(folders + 1 < parts.size() && folders + 1 < trackParts.size()
              && parts.at(parts.size() - folders - 2) == trackParts.at(trackParts.size() - folders - 2)) {
            telling = telling || !isGenericFolder(parts.at(parts.size() - folders - 2));
            ++folders;
        }
        if(!telling) {
            folders = 0;
        }
        const int score = (folders * 2) + (suffix(trackParts.back()) == extension ? 1 : 0);

        if(score > bestScore) {
            best      = &track;
            bestScore = score;
            ambiguous = false;
        }
        else if(score == bestScore && best->hash() != track.hash()) {
            ambiguous = true;
        }
    }

    // It takes a folder in common, such as the album, to be sure. A file name alone, such as "01 - Intro", could be a
    // different track, and a listen that was put down to the wrong one couldn't be put right by importing again
    if(ambiguous || bestScore < 2) {
        return nullptr;
    }

    return best;
}
} // namespace Fooyin::PlayHistoryImport
