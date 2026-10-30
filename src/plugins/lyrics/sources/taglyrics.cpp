/*
 * Fooyin
 * Copyright © 2024, Luke Taylor <luket@pm.me>
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

#include "taglyrics.h"

#include "settings/lyricssettings.h"

#include <core/engine/audioloader.h>
#include <utils/async.h>
#include <utils/settings/settingsmanager.h>

#include <QDateTime>
#include <QFileInfo>

using namespace Qt::StringLiterals;

namespace {
std::vector<Fooyin::Lyrics::LyricData>
lyricsFromTags(const Fooyin::Track& track, const Fooyin::Lyrics::SearchParams& params, const QStringList& searchTags)
{
    std::vector<Fooyin::Lyrics::LyricData> data;

    for(const QString& tag : searchTags) {
        const QStringList lyrics = track.extraTag(tag);

        if(!lyrics.empty()) {
            Fooyin::Lyrics::LyricData lyricData;
            lyricData.tag    = tag;
            lyricData.title  = params.title;
            lyricData.album  = params.album;
            lyricData.artist = params.artist;
            lyricData.data   = lyrics.constFirst();
            data.push_back(lyricData);
        }
    }

    return data;
}

// The stored tags of a track are only as recent as the last time it was indexed, so lyrics embedded
// in the file since then would otherwise be missed and the (much slower) external sources searched instead.
// Tracks with no stored modification time are always read, as there is nothing to tell if they are current.
bool fileChangedSinceIndexed(const Fooyin::Track& track)
{
    if(track.isRemote() || track.isInArchive() || track.hasCue()) {
        return false;
    }

    const QFileInfo info{track.filepath()};
    return info.isFile() && static_cast<uint64_t>(info.lastModified().toMSecsSinceEpoch()) != track.modifiedTime();
}
} // namespace

namespace Fooyin::Lyrics {
QString TagLyrics::name() const
{
    return u"Metadata Tags"_s;
}

bool TagLyrics::isLocal() const
{
    return true;
}

void TagLyrics::setAudioLoader(std::shared_ptr<AudioLoader> audioLoader)
{
    m_audioLoader = std::move(audioLoader);
}

void TagLyrics::search(const SearchParams& params)
{
    cancel();

    const auto searchTags = settings()->fileValue(Settings::SearchTags, Defaults::searchTags()).toStringList();

    const auto storedLyrics = lyricsFromTags(params.track, params, searchTags);
    if(!storedLyrics.empty() || !m_audioLoader || !params.track.isValid()) {
        Q_EMIT searchResult(storedLyrics);
        return;
    }

    m_stopSource                    = std::stop_source{};
    const std::stop_token stopToken = m_stopSource.get_token();

    Utils::asyncExec([audioLoader = m_audioLoader, params, searchTags, stopToken]() -> std::vector<LyricData> {
        if(stopToken.stop_requested() || !fileChangedSinceIndexed(params.track)) {
            return {};
        }

        Track track{params.track};
        if(!audioLoader->readTrackMetadata(track)) {
            return {};
        }

        return lyricsFromTags(track, params, searchTags);
    }).then(this, [this, stopToken](const std::vector<LyricData>& data) {
        if(!stopToken.stop_requested()) {
            Q_EMIT searchResult(data);
        }
    });
}

void TagLyrics::cancel()
{
    m_stopSource.request_stop();
    LyricSource::cancel();
}
} // namespace Fooyin::Lyrics
