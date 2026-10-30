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

#include "lyricssearchhistory.h"

#include <utils/fypaths.h>

#include <QDir>
#include <QFile>
#include <QSaveFile>

using namespace Qt::StringLiterals;

namespace Fooyin::Lyrics {
LyricsSearchHistory::LyricsSearchHistory(QString filepath)
    : m_filepath{std::move(filepath)}
{
    load();
}

QString LyricsSearchHistory::defaultPath()
{
    return QDir::cleanPath(Utils::sharePath().append("/lyrics-unavailable"_L1));
}

bool LyricsSearchHistory::contains(const QString& key) const
{
    return m_keys.contains(key);
}

void LyricsSearchHistory::add(const QString& key)
{
    if(key.isEmpty() || m_keys.contains(key)) {
        return;
    }

    m_keys.insert(key);

    QFile file{m_filepath};
    if(file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(key.toUtf8() + '\n');
    }
}

void LyricsSearchHistory::remove(const QString& key)
{
    if(m_keys.remove(key)) {
        rewrite();
    }
}

void LyricsSearchHistory::load()
{
    QFile file{m_filepath};
    if(!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    while(!file.atEnd()) {
        const QString key = QString::fromUtf8(file.readLine()).trimmed();
        if(!key.isEmpty()) {
            m_keys.insert(key);
        }
    }
}

void LyricsSearchHistory::rewrite() const
{
    QSaveFile file{m_filepath};
    if(!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }

    for(const QString& key : m_keys) {
        file.write(key.toUtf8() + '\n');
    }
    file.commit();
}
} // namespace Fooyin::Lyrics
