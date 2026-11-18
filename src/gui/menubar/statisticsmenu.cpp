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

#include "statisticsmenu.h"

#include "statistics/chartsdialog.h"
#include "statistics/listeninghistorydevicesdialog.h"
#include "statistics/statisticsservice.h"
#include "statistics/yearinreviewdialog.h"

#include <core/application.h>
#include <core/library/musiclibrary.h>
#include <core/playhistory/playhistory.h>
#include <gui/guiconstants.h>
#include <utils/actions/actioncontainer.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/utils.h>

#include <QAction>
#include <QMainWindow>

namespace Fooyin {
StatisticsMenu::StatisticsMenu(Application* core, ActionManager* actionManager, CoverRepository* covers,
                               QObject* parent)
    : QObject{parent}
    , m_history{core->playHistory()}
    , m_library{core->library()}
    , m_service{new StatisticsService(m_history, m_library, covers, this)}
{
    auto* statisticsMenu = actionManager->actionContainer(Constants::Menus::Statistics);

    const QStringList category{tr("Statistics")};

    auto* charts = new QAction(tr("&Listening statistics"), this);
    charts->setStatusTip(tr("Open charts of your listening history"));
    auto* chartsCmd = actionManager->registerAction(charts, Constants::Actions::ListeningCharts);
    chartsCmd->setCategories(category);
    statisticsMenu->addAction(chartsCmd);
    QObject::connect(charts, &QAction::triggered, this, &StatisticsMenu::showCharts);

    auto* yearInReview = new QAction(tr("&Year in review"), this);
    yearInReview->setStatusTip(tr("Open a summary of your listening over the year"));
    auto* yearCmd = actionManager->registerAction(yearInReview, Constants::Actions::YearInReview);
    yearCmd->setCategories(category);
    statisticsMenu->addAction(yearCmd);
    QObject::connect(yearInReview, &QAction::triggered, this, [this]() { showYearInReview(); });

    statisticsMenu->addSeparator();

    auto* importHistory = new QAction(tr("&Import listening history…"), this);
    importHistory->setStatusTip(tr("Add the listens of another device, exported from fooyin or backed up by Namida"));
    auto* importCmd = actionManager->registerAction(importHistory, Constants::Actions::ImportListeningHistory);
    importCmd->setCategories(category);
    statisticsMenu->addAction(importCmd);
    QObject::connect(importHistory, &QAction::triggered, this, [this]() {
        ListeningHistoryTransfer::importHistory(m_history, m_library, Utils::getMainWindow());
    });

    auto* exportHistory = new QAction(tr("&Export listening history…"), this);
    exportHistory->setStatusTip(tr("Save the listening history to import it on another device"));
    auto* exportCmd = actionManager->registerAction(exportHistory, Constants::Actions::ExportListeningHistory);
    exportCmd->setCategories(category);
    statisticsMenu->addAction(exportCmd);
    QObject::connect(exportHistory, &QAction::triggered, this,
                     [this]() { ListeningHistoryTransfer::exportHistory(m_history, Utils::getMainWindow()); });

    auto* devices = new QAction(tr("Listening history &devices"), this);
    devices->setStatusTip(tr("Show which devices the listening history is made up of"));
    auto* devicesCmd = actionManager->registerAction(devices, Constants::Actions::ListeningHistoryDevices);
    devicesCmd->setCategories(category);
    statisticsMenu->addAction(devicesCmd);
    QObject::connect(devices, &QAction::triggered, this, &StatisticsMenu::showDevices);
}

void StatisticsMenu::showCharts()
{
    if(m_chartsDialog) {
        m_chartsDialog->show();
        m_chartsDialog->raise();
        m_chartsDialog->activateWindow();
        return;
    }

    m_chartsDialog = new ChartsDialog(m_service, Utils::getMainWindow());
    m_chartsDialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(m_chartsDialog, &ChartsDialog::yearInReviewRequested, this,
                     [this](int year) { showYearInReview(year); });
    m_chartsDialog->show();
}

void StatisticsMenu::showDevices()
{
    if(!m_devicesDialog) {
        m_devicesDialog = new ListeningHistoryDevicesDialog(m_history, m_library, Utils::getMainWindow());
        m_devicesDialog->setAttribute(Qt::WA_DeleteOnClose);
    }

    m_devicesDialog->show();
    m_devicesDialog->raise();
    m_devicesDialog->activateWindow();
}

void StatisticsMenu::showYearInReview(int year)
{
    if(!m_yearDialog) {
        m_yearDialog = new YearInReviewDialog(m_service, Utils::getMainWindow());
        m_yearDialog->setAttribute(Qt::WA_DeleteOnClose);
    }

    m_yearDialog->showYear(year);
    m_yearDialog->show();
    m_yearDialog->raise();
    m_yearDialog->activateWindow();
}
} // namespace Fooyin

#include "moc_statisticsmenu.cpp"
