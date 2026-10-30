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
#include <QStringList>

#include <memory>

namespace Fooyin::Lyrics {
/*!
 * Remembers which tracks have already been searched for on external sources without finding any lyrics, so that
 * automatic searches don't repeat the same failed lookups every time the track is played.
 * Entries are keyed by an opaque string and persisted across sessions. The oldest entries are dropped once the
 * history grows beyond a fixed size.
 * Entries are only valid for the context (search settings) they were recorded with; changing it clears the history.
 */
class LyricsSearchHistory
{
public:
    explicit LyricsSearchHistory(QString filepath);

    //! History shared by all finders, so a search anywhere is known everywhere
    static std::shared_ptr<LyricsSearchHistory> shared();

    //! Clears the history if the context differs from the one the entries were recorded with
    void setContext(const QString& context);

    [[nodiscard]] bool contains(const QString& key) const;
    void add(const QString& key);
    void remove(const QString& key);
    void clear();

    //! Default location of the history file
    static QString defaultPath();

private:
    void load();
    void rewrite() const;

    QString m_filepath;
    QString m_context;
    QStringList m_order; // Oldest first
    QSet<QString> m_keys;
};
} // namespace Fooyin::Lyrics
