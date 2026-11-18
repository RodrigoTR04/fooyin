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

#include <core/playhistory/playhistory.h>
#include <core/track.h>

#include <QHash>
#include <QString>

#include <expected>
#include <vector>

namespace Fooyin::PlayHistoryImport {
//! Origin of listens recorded by Namida. Namida doesn't identify the device, but a person can't have two listens
//! counted at the same millisecond, so the time alone is enough to recognise a listen that was already imported.
inline constexpr auto NamidaOrigin = "namida";
inline constexpr auto NamidaKind   = "namida";
inline constexpr auto FooyinKind   = "fooyin";

//! Origin of the listens recorded by the fooyin installation identified by @p deviceId.
FYCORE_EXPORT QString fooyinOrigin(const QString& deviceId);

//! A listen as Namida records it: when, and the path of the file on the phone.
struct NamidaListen
{
    qint64 timestamp{0};
    QString path;
};

//! A device whose listens are in an export, identified within the file by @c id.
struct ExportedSource
{
    int id{0};
    QString kind;
    QString deviceId;
    QString name;
};

//! The listening history of a fooyin installation, as exported to a file.
struct FooyinExport
{
    QString deviceId;
    QString deviceName;
    qint64 exported{0};
    std::vector<ExportedSource> sources;
    //! Listens, with PlayHistoryEntry::source referring to an ExportedSource::id.
    std::vector<PlayHistoryEntry> listens;
    //! Listens left out because their time or length was impossible.
    int invalid{0};
};

//! The listens in a Namida backup.
struct NamidaHistory
{
    //! Oldest first.
    std::vector<NamidaListen> listens;
    //! Listens left out because their time was impossible.
    int invalid{0};
};

//! Whether @p timestamp (milliseconds since the epoch) could be the time of a listen.
FYCORE_EXPORT bool isPlausibleTimestamp(qint64 timestamp);

//! Works out what kind of history the file at @p filepath holds without reading all of it.
FYCORE_EXPORT PlayHistoryFormat detectFormat(const QString& filepath);

/*!
 * Reads the listens in a Namida backup (the zip made by Namida's backup option, or the history archive inside it)
 * or in one of the daily history files it's made up of.
 */
FYCORE_EXPORT std::expected<NamidaHistory, QString> readNamida(const QString& filepath);

FYCORE_EXPORT std::expected<FooyinExport, QString> readFooyin(const QString& filepath);
FYCORE_EXPORT QByteArray writeFooyin(const FooyinExport& history);

/*!
 * Finds the track in the library that a file on another device is a copy of.
 *
 * Paths on another device (e.g. /storage/emulated/0/Music/Artist/Album/Song.mp3) won't be the same as in the library,
 * so tracks are matched by file name and then by as many of the folders above it as are the same, at least one of
 * which has to be. A copy in another format (Song.flac) is matched too, but one in the same format is preferred. If
 * more than one track fits equally well, none is chosen, rather than counting the listen towards the wrong one.
 */
class FYCORE_EXPORT TrackMatcher
{
public:
    explicit TrackMatcher(const TrackList& tracks);

    //! The track @p path is a copy of, or nullptr if there isn't one or it's unclear which.
    [[nodiscard]] const Track* match(const QString& path) const;

private:
    TrackList m_tracks;
    //! Indexes into m_tracks, by the case folded file name without its extension.
    QHash<QString, std::vector<int>> m_byName;
};
} // namespace Fooyin::PlayHistoryImport
