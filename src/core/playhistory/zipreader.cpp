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

#include "zipreader.h"

#include <QBuffer>
#include <QFile>
#include <QtEndian>

#include <zlib.h>

namespace {
constexpr quint32 EndOfCentralDirectorySignature = 0x06054b50;
constexpr quint32 CentralDirectorySignature      = 0x02014b50;
constexpr quint32 LocalHeaderSignature           = 0x04034b50;

constexpr qint64 EndOfCentralDirectorySize = 22;
constexpr qint64 CentralDirectoryEntrySize = 46;
constexpr qint64 LocalHeaderSize           = 30;
constexpr qint64 MaxCommentSize            = 0xFFFF;

constexpr quint16 MethodStored   = 0;
constexpr quint16 MethodDeflated = 8;

//! Nothing in a history backup comes close to this, so a larger file is not what we're looking for.
constexpr quint32 MaxEntrySize = 256U * 1024U * 1024U;

quint16 read16(const char* data)
{
    return qFromLittleEndian<quint16>(data);
}

quint32 read32(const char* data)
{
    return qFromLittleEndian<quint32>(data);
}

QByteArray inflateRaw(const QByteArray& compressed, quint32 size)
{
    QByteArray output;
    output.resize(static_cast<qsizetype>(size));

    z_stream stream{};
    if(inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return {};
    }

    stream.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.constData()));
    stream.avail_in  = static_cast<uInt>(compressed.size());
    stream.next_out  = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    const int result = inflate(&stream, Z_FINISH);
    const auto total = stream.total_out;
    inflateEnd(&stream);

    if(result != Z_STREAM_END || total != size) {
        return {};
    }

    return output;
}
} // namespace

namespace Fooyin {
ZipReader::ZipReader(const QString& filepath)
    : m_device{std::make_unique<QFile>(filepath)}
{
    m_valid = m_device->open(QIODevice::ReadOnly) && readCentralDirectory();
}

ZipReader::ZipReader(const QByteArray& data)
    : m_device{std::make_unique<QBuffer>()}
{
    auto* buffer = static_cast<QBuffer*>(m_device.get());
    buffer->setData(data);
    m_valid = buffer->open(QIODevice::ReadOnly) && readCentralDirectory();
}

ZipReader::~ZipReader() = default;

bool ZipReader::isValid() const
{
    return m_valid;
}

const std::vector<ZipReader::Entry>& ZipReader::entries() const
{
    return m_entries;
}

QByteArray ZipReader::read(const Entry& entry) const
{
    if(!m_valid || entry.size > MaxEntrySize || entry.compressedSize > MaxEntrySize
       || (entry.method == MethodStored && entry.compressedSize != entry.size)
       || !m_device->seek(entry.localHeaderOffset)) {
        return {};
    }

    const QByteArray header = m_device->read(LocalHeaderSize);
    if(header.size() != LocalHeaderSize || read32(header.constData()) != LocalHeaderSignature) {
        return {};
    }

    const qint64 dataOffset
        = entry.localHeaderOffset + LocalHeaderSize + read16(header.constData() + 26) + read16(header.constData() + 28);
    // Sizes are checked against the archive before anything is read, so a damaged one can't ask for more than it has
    if(dataOffset + entry.compressedSize > m_device->size() || !m_device->seek(dataOffset)) {
        return {};
    }

    const QByteArray data = m_device->read(entry.compressedSize);
    if(data.size() != static_cast<qsizetype>(entry.compressedSize)) {
        return {};
    }

    QByteArray contents;
    if(entry.method == MethodStored) {
        contents = data;
    }
    else if(entry.method == MethodDeflated) {
        contents = inflateRaw(data, entry.size);
    }

    const auto crc = static_cast<quint32>(
        crc32(0, reinterpret_cast<const Bytef*>(contents.constData()), static_cast<uInt>(contents.size())));
    if(contents.size() != static_cast<qsizetype>(entry.size) || crc != entry.crc) {
        return {};
    }

    return contents;
}

bool ZipReader::isZip(const QByteArray& data)
{
    return data.size() >= 4 && read32(data.constData()) == LocalHeaderSignature;
}

bool ZipReader::readCentralDirectory()
{
    const qint64 fileSize = m_device->size();
    if(fileSize < EndOfCentralDirectorySize) {
        return false;
    }

    // The end of central directory record sits at the very end, unless the archive has a comment after it
    const qint64 tailSize = std::min(fileSize, EndOfCentralDirectorySize + MaxCommentSize);
    if(!m_device->seek(fileSize - tailSize)) {
        return false;
    }
    const QByteArray tail = m_device->read(tailSize);

    qsizetype eocd = -1;
    for(qsizetype i = tail.size() - EndOfCentralDirectorySize; i >= 0; --i) {
        if(read32(tail.constData() + i) == EndOfCentralDirectorySignature) {
            eocd = i;
            break;
        }
    }
    if(eocd < 0) {
        return false;
    }

    const char* record            = tail.constData() + eocd;
    const quint16 count           = read16(record + 10);
    const quint32 directorySize   = read32(record + 12);
    const quint32 directoryOffset = read32(record + 16);
    const bool needsZip64         = count == 0xFFFF || directoryOffset == 0xFFFFFFFF;
    if(needsZip64 || static_cast<qint64>(directoryOffset) + directorySize > fileSize
       || !m_device->seek(directoryOffset)) {
        return false;
    }

    const QByteArray directory = m_device->read(directorySize);
    qsizetype pos              = 0;

    for(quint16 i{0}; i < count; ++i) {
        if(pos + CentralDirectoryEntrySize > directory.size()) {
            return false;
        }

        const char* header = directory.constData() + pos;
        if(read32(header) != CentralDirectorySignature) {
            return false;
        }

        const quint16 flags         = read16(header + 8);
        const quint16 nameLength    = read16(header + 28);
        const quint16 extraLength   = read16(header + 30);
        const quint16 commentLength = read16(header + 32);

        if(pos + CentralDirectoryEntrySize + nameLength > directory.size()) {
            return false;
        }

        const QByteArray rawName = directory.mid(pos + CentralDirectoryEntrySize, nameLength);

        Entry entry;
        // Bit 11 marks names stored as UTF-8. Others are in the code page of the system that made the archive, which
        // can't be known, but the names that matter here are plain ASCII either way
        entry.name              = (flags & 0x800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        entry.method            = read16(header + 10);
        entry.crc               = read32(header + 16);
        entry.compressedSize    = read32(header + 20);
        entry.size              = read32(header + 24);
        entry.localHeaderOffset = read32(header + 42);

        if(!entry.name.endsWith(u'/')) {
            m_entries.push_back(entry);
        }

        pos += CentralDirectoryEntrySize + nameLength + extraLength + commentLength;
    }

    // The files listed have to account for the whole directory, or some would be left out without anything to show for
    // it
    return directory.size() == static_cast<qsizetype>(directorySize) && pos == directory.size();
}
} // namespace Fooyin
