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

#include <core/playhistory/playhistory.h>
#include <core/playhistory/playhistorystats.h>

#include <QDate>
#include <QDialog>

#include <memory>
#include <optional>

class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace Fooyin {
class StatisticsService;

namespace Stats {
class ChipBar;
} // namespace Stats

/*!
 * Charts of listening habits over a chosen period, such as the most played tracks, when music is listened to
 * and how the period compares to the one before it.
 */
class ChartsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChartsDialog(StatisticsService* service, QWidget* parent = nullptr);

Q_SIGNALS:
    void yearInReviewRequested(int year);

private:
    enum class Period : uint8_t
    {
        Week = 0,
        Month,
        ThreeMonths,
        SixMonths,
        Year,
        AllTime,
        Custom,
    };

    [[nodiscard]] std::pair<QDate, QDate> resolveRange() const;
    void selectPeriod(Period period);
    void chooseCustomRange();
    void setCustomRange(const QDate& first, const QDate& last);

    void loadSummary();
    void applySummary(const PlayHistorySummary& summary);
    //! Brings what's shown up to date after listens have been recorded, without disturbing it.
    void reloadSummary();
    void refresh();
    void showStats(const PlayHistoryStats& current, const std::optional<PlayHistoryStats>& previous);
    void showMessage(const QString& message);
    void setContent(QWidget* content);
    void exportImage();

    QWidget* makeOverview(const PlayHistoryStats& stats, const std::optional<PlayHistoryStats>& previous);
    QWidget* makeMonths(const PlayHistoryStats& stats);
    QWidget* makeListens(const PlayHistoryStats& stats);
    QWidget* makeDays(const PlayHistoryStats& stats);
    QWidget* makeListeningClock(const PlayHistoryStats& stats);
    QWidget* makeHabits(const PlayHistoryStats& stats);
    QWidget* makeObsession(const PlayHistoryStats& stats);
    QWidget* makeDiscoveries(const PlayHistoryStats& stats);
    QWidget* makeTopTracks(const PlayHistoryStats& stats);
    QWidget* makeMostPlayed(const PlayHistoryStats& stats);
    QWidget* makeTopArtists(const PlayHistoryStats& stats);
    QWidget* makeRediscoveries(const PlayHistoryStats& stats);
    QWidget* makeCompletedAlbums(const PlayHistoryStats& stats);
    QWidget* makeCategories(const PlayHistoryStats& stats);
    QWidget* makeAudioQuality(const PlayHistoryStats& stats);
    QWidget* makeListenTime(const PlayHistoryStats& stats);
    QWidget* makeDecades(const PlayHistoryStats& stats);

    StatisticsService* m_service;

    Stats::ChipBar* m_periods;
    QPushButton* m_customButton;
    QPushButton* m_yearButton;
    QPushButton* m_exportButton;
    QScrollArea* m_scrollArea;
    QTimer* m_reloadTimer;

    PlayHistorySummary m_summary;
    std::shared_ptr<const PlayHistoryStats> m_stats;
    Period m_period;
    QDate m_customFirst;
    QDate m_customLast;
    int m_requestId;
    bool m_keepScroll;

    bool m_cumulative;
    PlayHistoryStats::Category m_category;
};
} // namespace Fooyin
