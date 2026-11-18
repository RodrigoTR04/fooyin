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

#include "listeninghistorydevicesdialog.h"

#include <core/library/musiclibrary.h>
#include <core/playhistory/playhistory.h>
#include <utils/utils.h>

#include <QApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {
constexpr int SourceIdRole = Qt::UserRole;
constexpr int NameRole     = Qt::UserRole + 1;
constexpr int ListensRole  = Qt::UserRole + 2;
constexpr int KindRole     = Qt::UserRole + 3;

enum Column : uint8_t
{
    Device = 0,
    App,
    Listens,
    FirstListen,
    LastListen,
    LastImported,
};

QString tr(const char* text, int n = -1)
{
    return QCoreApplication::translate("Fooyin::ListeningHistoryDevicesDialog", text, nullptr, n);
}

QString formatDate(qint64 timestamp)
{
    if(timestamp <= 0) {
        return {};
    }
    return QLocale{}.toString(QDateTime::fromMSecsSinceEpoch(timestamp).date(), QLocale::ShortFormat);
}

QString appName(const QString& kind)
{
    if(kind == "namida"_L1) {
        return u"Namida"_s;
    }
    return u"fooyin"_s;
}

//! Where to show the outcome of a transfer, which may outlast the window it was started from.
QWidget* resultParent(const QPointer<QWidget>& parent)
{
    return parent ? parent.data() : Fooyin::Utils::getMainWindow();
}

//! The devices @p item could be merged into: those of the same kind, other than this one.
QList<const QTreeWidgetItem*> mergeTargets(const QTreeWidget* devices, const QTreeWidgetItem* item)
{
    QList<const QTreeWidgetItem*> targets;
    for(int i{0}; i < devices->topLevelItemCount(); ++i) {
        const auto* other = devices->topLevelItem(i);
        if(other != item && other->data(Device, SourceIdRole).toInt() != 0
           && other->data(Device, KindRole) == item->data(Device, KindRole)) {
            targets.append(other);
        }
    }
    return targets;
}

void showImportResult(const Fooyin::PlayHistoryImportResult& result, QWidget* parent)
{
    if(!result.succeeded()) {
        QMessageBox::warning(parent, tr("Import Listening History"), result.error);
        return;
    }

    QStringList lines;
    if(result.devices.isEmpty()) {
        lines.append(tr("Added %n listen(s).", result.added));
    }
    else {
        lines.append(
            tr("Added %n listen(s) from %1.", result.added).arg(QLocale{}.createSeparatedList(result.devices)));
    }
    if(result.duplicates > 0) {
        lines.append(tr("Skipped %n listen(s) that had already been imported.", result.duplicates));
    }
    if(result.invalid > 0) {
        lines.append(
            tr("Skipped %n listen(s) that the file has down with an impossible time or length.", result.invalid));
    }
    if(result.unmatched > 0) {
        lines.append(tr("Skipped %n listen(s) of tracks that aren't in the library. Import the file again once they've "
                        "been added to include them.",
                        result.unmatched));
    }

    QMessageBox box{QMessageBox::Information, tr("Import Listening History"), lines.join("\n\n"_L1), QMessageBox::Ok,
                    parent};
    if(!result.unmatchedTracks.isEmpty()) {
        box.setDetailedText(tr("Tracks not found in the library:") + u"\n\n"_s + result.unmatchedTracks.join(u'\n'));
    }
    box.exec();
}

void runImport(Fooyin::PlayHistory* history, Fooyin::MusicLibrary* library, const QString& filepath,
               const QString& device, QWidget* parent)
{
    QApplication::setOverrideCursor(Qt::BusyCursor);

    // Seen through even if the dialog it was started from is closed meanwhile, so the cursor is always restored
    history->importFile(filepath, device, library->tracks())
        .then(qApp, [guard = QPointer<QWidget>{parent}](const Fooyin::PlayHistoryImportResult& result) {
            QApplication::restoreOverrideCursor();
            showImportResult(result, resultParent(guard));
        });
}
} // namespace

namespace Fooyin {
namespace ListeningHistoryTransfer {
void importHistory(PlayHistory* history, MusicLibrary* library, QWidget* parent)
{
    const QString filepath = QFileDialog::getOpenFileName(
        parent, tr("Import Listening History"), QDir::homePath(),
        tr("Listening history (*.json *.zip);;fooyin listening history (*.json);;Namida backup (*.zip *.json)"));
    if(filepath.isEmpty()) {
        return;
    }

    switch(PlayHistory::fileFormat(filepath)) {
        case PlayHistoryFormat::Unknown:
            QMessageBox::warning(parent, tr("Import Listening History"),
                                 tr("The file is not a listening history exported by fooyin or a Namida backup."));
            return;
        case PlayHistoryFormat::Fooyin:
            runImport(history, library, filepath, {}, parent);
            return;
        case PlayHistoryFormat::Namida:
            break;
    }

    // Namida doesn't say which device it's on, so ask, offering the ones that have been imported before
    QPointer<QWidget> guard{parent};
    history->sources().then(parent, [history, library, filepath, guard](const std::vector<PlayHistorySource>& sources) {
        QStringList devices;
        for(const auto& source : sources) {
            if(source.kind == "namida"_L1) {
                devices.append(source.name);
            }
        }
        if(devices.isEmpty()) {
            devices.append(tr("Phone"));
        }

        bool ok{false};
        const QString device = QInputDialog::getItem(
            guard, tr("Import Namida History"),
            tr("Which device is this backup from?\n\nListens that have already been imported from an earlier backup "
               "are skipped, so only new listens are added."),
            devices, 0, true, &ok);

        if(ok && guard) {
            runImport(history, library, filepath, device.trimmed(), guard);
        }
    });
}

void exportHistory(PlayHistory* history, QWidget* parent)
{
    const QString suggested
        = QDir::home().filePath(u"fooyin-listening-history-%1.json"_s.arg(QDate::currentDate().toString(Qt::ISODate)));

    const QString filepath = QFileDialog::getSaveFileName(parent, tr("Export Listening History"), suggested,
                                                          tr("fooyin listening history (*.json)"));
    if(filepath.isEmpty()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::BusyCursor);

    history->exportFile(filepath).then(qApp, [guard = QPointer<QWidget>{parent}, filepath](const QString& error) {
        QApplication::restoreOverrideCursor();
        QWidget* window = resultParent(guard);
        if(error.isEmpty()) {
            QMessageBox::information(
                window, tr("Export Listening History"),
                tr("The listening history was exported to %1.\n\nImport it into fooyin on another device to add these "
                   "listens to its history.")
                    .arg(QDir::toNativeSeparators(filepath)));
        }
        else {
            QMessageBox::warning(window, tr("Export Listening History"), error);
        }
    });
}
} // namespace ListeningHistoryTransfer

ListeningHistoryDevicesDialog::ListeningHistoryDevicesDialog(PlayHistory* history, MusicLibrary* library,
                                                             QWidget* parent)
    : QDialog{parent}
    , m_history{history}
    , m_library{library}
    , m_devices{new QTreeWidget(this)}
    , m_rename{new QPushButton(tr("&Rename…"), this)}
    , m_merge{new QPushButton(tr("Mer&ge into…"), this)}
    , m_remove{new QPushButton(tr("Re&move…"), this)}
{
    setWindowTitle(tr("Listening History Devices"));

    auto* description = new QLabel(
        tr("The devices whose listens make up the listening history. Import the history of another device to add its "
           "listens. Each listen is only ever added once, so a newer export can be imported without counting anything "
           "twice."),
        this);
    description->setWordWrap(true);

    m_devices->setRootIsDecorated(false);
    m_devices->setUniformRowHeights(true);
    m_devices->setAllColumnsShowFocus(true);
    m_devices->setHeaderLabels(
        {tr("Device"), tr("App"), tr("Listens"), tr("First listen"), tr("Last listen"), tr("Last imported")});
    m_devices->header()->setStretchLastSection(false);
    m_devices->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_devices->header()->setSectionResizeMode(Device, QHeaderView::Stretch);

    auto* importButton = new QPushButton(tr("&Import…"), this);
    auto* exportButton = new QPushButton(tr("&Export…"), this);

    auto* buttons = new QVBoxLayout();
    buttons->addWidget(importButton);
    buttons->addWidget(exportButton);
    buttons->addSpacing(12);
    buttons->addWidget(m_rename);
    buttons->addWidget(m_merge);
    buttons->addWidget(m_remove);
    buttons->addStretch();

    auto* content = new QHBoxLayout();
    content->addWidget(m_devices, 1);
    content->addLayout(buttons);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(description);
    layout->addLayout(content, 1);
    layout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(importButton, &QPushButton::clicked, this,
                     [this]() { ListeningHistoryTransfer::importHistory(m_history, m_library, this); });
    QObject::connect(exportButton, &QPushButton::clicked, this,
                     [this]() { ListeningHistoryTransfer::exportHistory(m_history, this); });
    QObject::connect(m_rename, &QPushButton::clicked, this, &ListeningHistoryDevicesDialog::renameSelected);
    QObject::connect(m_merge, &QPushButton::clicked, this, &ListeningHistoryDevicesDialog::mergeSelected);
    QObject::connect(m_remove, &QPushButton::clicked, this, &ListeningHistoryDevicesDialog::removeSelected);
    QObject::connect(m_devices, &QTreeWidget::itemSelectionChanged, this,
                     &ListeningHistoryDevicesDialog::updateButtons);
    QObject::connect(m_devices, &QTreeWidget::itemDoubleClicked, this, &ListeningHistoryDevicesDialog::renameSelected);
    QObject::connect(m_history, &PlayHistory::historyChanged, this, &ListeningHistoryDevicesDialog::reload);
    QObject::connect(m_history, &PlayHistory::playRecorded, this, &ListeningHistoryDevicesDialog::reload);

    updateButtons();
    reload();
}

QSize ListeningHistoryDevicesDialog::sizeHint() const
{
    return {720, 360};
}

void ListeningHistoryDevicesDialog::reload()
{
    m_history->sources().then(this, [this](const std::vector<PlayHistorySource>& sources) {
        const QVariant selected = m_devices->currentItem() ? m_devices->currentItem()->data(Device, SourceIdRole) : 0;

        m_devices->clear();

        for(const auto& source : sources) {
            const QString name = source.isLocal() ? tr("%1 (this device)").arg(source.name) : source.name;

            auto* item = new QTreeWidgetItem(m_devices);
            item->setText(Device, name);
            item->setText(App, appName(source.kind));
            item->setText(Listens, QLocale{}.toString(source.listens));
            item->setText(FirstListen, formatDate(source.firstListen));
            item->setText(LastListen, formatDate(source.lastListen));
            item->setText(LastImported, source.isLocal() ? QString{} : formatDate(source.lastImported));
            item->setTextAlignment(Listens, Qt::AlignRight | Qt::AlignVCenter);
            item->setData(Device, SourceIdRole, source.id);
            item->setData(Device, NameRole, source.name);
            item->setData(Device, ListensRole, source.listens);
            item->setData(Device, KindRole, source.kind);

            if(source.id == selected.toInt()) {
                m_devices->setCurrentItem(item);
            }
        }

        updateButtons();
    });
}

void ListeningHistoryDevicesDialog::updateButtons()
{
    const auto* item = m_devices->currentItem();
    m_rename->setEnabled(item != nullptr);
    const bool imported = item != nullptr && item->data(Device, SourceIdRole).toInt() != 0;
    m_remove->setEnabled(imported);
    m_merge->setEnabled(imported && !mergeTargets(m_devices, item).isEmpty());
}

void ListeningHistoryDevicesDialog::renameSelected()
{
    const auto* item = m_devices->currentItem();
    if(!item) {
        return;
    }

    const int id = item->data(Device, SourceIdRole).toInt();

    bool ok{false};
    const QString name = QInputDialog::getText(this, tr("Rename Device"), tr("Name:"), QLineEdit::Normal,
                                               item->data(Device, NameRole).toString(), &ok)
                             .trimmed();
    if(!ok || name.isEmpty()) {
        return;
    }

    m_history->renameSource(id, name).then(this, [this, name](bool renamed) {
        if(!renamed) {
            QMessageBox::warning(this, tr("Rename Device"),
                                 tr("Another Namida device is already called %1. If they're the same phone, merge one "
                                    "into the other instead.")
                                     .arg(name));
        }
        reload();
    });
}

void ListeningHistoryDevicesDialog::mergeSelected()
{
    const auto* item = m_devices->currentItem();
    if(!item || item->data(Device, SourceIdRole).toInt() == 0) {
        return;
    }

    const auto targets = mergeTargets(m_devices, item);
    if(targets.isEmpty()) {
        return;
    }

    QStringList names;
    for(const auto* target : targets) {
        names.append(target->data(Device, NameRole).toString());
    }

    const QString name = item->data(Device, NameRole).toString();

    bool ok{false};
    const QString chosen = QInputDialog::getItem(
        this, tr("Merge Device"),
        tr("If %1 is the same device as another, its listens can be merged into that one's. History imported from %1 "
           "later on will go to it too.\n\nMerge into:")
            .arg(name),
        names, 0, false, &ok);
    if(!ok) {
        return;
    }

    const int from = item->data(Device, SourceIdRole).toInt();
    const int into = targets.at(names.indexOf(chosen))->data(Device, SourceIdRole).toInt();

    m_history->mergeSource(from, into).then(this, [this](bool) { reload(); });
}

void ListeningHistoryDevicesDialog::removeSelected()
{
    const auto* item = m_devices->currentItem();
    if(!item || item->data(Device, SourceIdRole).toInt() == 0) {
        return;
    }

    const int id       = item->data(Device, SourceIdRole).toInt();
    const int listens  = item->data(Device, ListensRole).toInt();
    const QString name = item->data(Device, NameRole).toString();

    const auto answer = QMessageBox::question(
        this, tr("Remove Device"),
        tr("Remove %1 and the %n listen(s) imported from it? They can be imported again later.", nullptr, listens)
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if(answer == QMessageBox::Yes) {
        m_history->removeSource(id).then(this, [this](bool) { reload(); });
    }
}
} // namespace Fooyin

#include "moc_listeninghistorydevicesdialog.cpp"
