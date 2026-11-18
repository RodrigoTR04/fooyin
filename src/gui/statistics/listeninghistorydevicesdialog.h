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

#include <QDialog>

class QPushButton;
class QTreeWidget;

namespace Fooyin {
class MusicLibrary;
class PlayHistory;

namespace ListeningHistoryTransfer {
//! Asks for a file of listens exported from another device and adds them to @p history.
void importHistory(PlayHistory* history, MusicLibrary* library, QWidget* parent);
//! Asks where to save @p history so it can be imported on another device.
void exportHistory(PlayHistory* history, QWidget* parent);
} // namespace ListeningHistoryTransfer

/*!
 * Lists the devices whose listens are in the history, so it's clear what each has contributed, and lets imported
 * listens be removed again.
 */
class ListeningHistoryDevicesDialog : public QDialog
{
    Q_OBJECT

public:
    ListeningHistoryDevicesDialog(PlayHistory* history, MusicLibrary* library, QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;

private:
    void reload();
    void updateButtons();
    void renameSelected();
    void mergeSelected();
    void removeSelected();

    PlayHistory* m_history;
    MusicLibrary* m_library;
    QTreeWidget* m_devices;
    QPushButton* m_rename;
    QPushButton* m_merge;
    QPushButton* m_remove;
};
} // namespace Fooyin
