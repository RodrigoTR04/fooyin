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

#include <QSet>
#include <QString>

namespace Fooyin::Lyrics {
/*!
 * Remembers which tracks have already been searched for on external sources without finding any lyrics, so that
 * automatic searches don't repeat the same failed lookups every time the track is played.
 * Entries are keyed by an opaque string and persisted across sessions.
 */
class LyricsSearchHistory
{
public:
    explicit LyricsSearchHistory(QString filepath);

    [[nodiscard]] bool contains(const QString& key) const;
    void add(const QString& key);
    void remove(const QString& key);

    //! Default location of the history file
    static QString defaultPath();

private:
    void load();
    void rewrite() const;

    QString m_filepath;
    QSet<QString> m_keys;
};
} // namespace Fooyin::Lyrics
