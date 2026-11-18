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
#include <utils/database/dbmodule.h>

namespace Fooyin {
class FYCORE_EXPORT PlayHistoryDatabase : public DbModule
{
public:
    bool insert(const PlayHistoryEntry& entry) const;
    //! Whether there's a listen from @p origin at @p timestamp, i.e. it has already been recorded or imported.
    [[nodiscard]] bool contains(const QString& origin, qint64 timestamp) const;
    //! The device the listen from @p origin at @p timestamp came from, or -1 if there isn't one.
    [[nodiscard]] int sourceOf(const QString& origin, qint64 timestamp) const;
    //! The tracks of the listens from @p origin at @p timestamp.
    [[nodiscard]] QStringList hashesAt(const QString& origin, qint64 timestamp) const;

    //! Moves the listens of the track with hash @p from to @p to, unless a track in the library still has @p from.
    void moveTrack(const QString& from, const QString& to) const;
    //! Brings the details of the track kept with its listens, such as its album, up to date with @p entry.
    void refreshMetadata(const PlayHistoryEntry& entry) const;

    //! Listens in [from, to), oldest first.
    [[nodiscard]] PlayHistoryData load(qint64 from, qint64 to) const;
    [[nodiscard]] PlayHistorySummary summary() const;
    //! Every listen, including the device it came from and where it was recorded, oldest first.
    //! Sets @p ok to whether they could all be read.
    [[nodiscard]] std::vector<PlayHistoryEntry> all(bool* ok = nullptr) const;

    //! Identifies this installation to the others its history is imported into. Created the first time it's needed.
    //! Empty if it couldn't be stored.
    [[nodiscard]] QString deviceId() const;
    [[nodiscard]] QString deviceName() const;

    //! The devices listens have been recorded on, starting with this one.
    //! Sets @p ok to whether they could all be read.
    [[nodiscard]] std::vector<PlayHistorySource> sources(bool* ok = nullptr) const;
    //! The id of the device of @p kind identified by @p deviceId, or 0 if there isn't one.
    [[nodiscard]] int findSource(const QString& kind, const QString& deviceId) const;
    //! The id of the device of @p kind identified by @p deviceId, which is added as @p name if it's new. 0 on error.
    [[nodiscard]] int findOrAddSource(const QString& kind, const QString& deviceId, const QString& name) const;
    //! The id of the Namida device called @p name, or of the one it was merged into, or 0 if there isn't one.
    [[nodiscard]] int findNamidaSource(const QString& name) const;
    //! The id of the Namida device called @p name, which is added if it's new. 0 on error.
    [[nodiscard]] int findOrAddNamidaSource(const QString& name) const;
    void setLastImported(int id, qint64 timestamp) const;
    //! Fails if @p name is already that of another Namida device, as they're told apart by name.
    PlayHistoryRenameResult renameSource(int id, const QString& name) const;
    //! Moves the listens of the device with @p from to the one with @p into, which has to be of the same kind, and
    //! remembers that it's the same device for later imports.
    bool mergeSource(int from, int into) const;
    bool removeSource(int id) const;
};
} // namespace Fooyin
