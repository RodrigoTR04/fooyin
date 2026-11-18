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

#include <QByteArray>
#include <QString>

#include <memory>
#include <vector>

class QIODevice;

namespace Fooyin {
/*!
 * Reads the files stored in a zip archive, which is how other players back up their history.
 *
 * Only what those backups use is supported: files that are stored or deflated, in archives without ZIP64 extensions.
 */
class FYCORE_EXPORT ZipReader
{
public:
    struct Entry
    {
        QString name;
        quint16 method{0};
        quint32 crc{0};
        quint32 compressedSize{0};
        quint32 size{0};
        quint32 localHeaderOffset{0};
    };

    //! Reads the archive at @p filepath.
    explicit ZipReader(const QString& filepath);
    //! Reads an archive held in memory, such as one stored inside another archive.
    explicit ZipReader(const QByteArray& data);
    ~ZipReader();

    ZipReader(const ZipReader&)            = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    //! Whether the archive could be opened and its list of files read.
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] const std::vector<Entry>& entries() const;

    //! The contents of @p entry, or an empty array if it can't be read.
    [[nodiscard]] QByteArray read(const Entry& entry) const;

    //! Whether @p data starts the way a zip archive does.
    [[nodiscard]] static bool isZip(const QByteArray& data);

private:
    bool readCentralDirectory();

    std::unique_ptr<QIODevice> m_device;
    std::vector<Entry> m_entries;
    bool m_valid{false};
};
} // namespace Fooyin
