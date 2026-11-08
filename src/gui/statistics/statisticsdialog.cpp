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

#include "statisticsdialog.h"

#include "statisticsservice.h"
#include "statsart.h"
#include "statscharts.h"
#include "statswidgets.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateEdit>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>
#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin {
namespace {
using namespace Stats;
using Category = PlayHistoryStats::Category;

//! Number of categories drawn individually in a donut chart, the rest being combined
constexpr size_t DonutSlices = 8;
//! Size of the cover shown beside each track in a list
constexpr int ListArtSize = 44;

QString tr(const char* text)
{
    return QCoreApplication::translate("Fooyin::StatisticsDialog", text);
}

QString number(qint64 value)
{
    return QLocale().toString(value);
}

QString daysText(int days)
{
    return days == 1 ? tr("%1 day").arg(days) : tr("%1 days").arg(number(days));
}

QString categoryName(Category category)
{
    switch(category) {
        case Category::Genre:
            return tr("Genre");
        case Category::Artist:
            return tr("Artist");
        case Category::AlbumArtist:
            return tr("Album artist");
        case Category::Composer:
            return tr("Composer");
        case Category::Album:
            return tr("Album");
    }
    return {};
}

QString categoryCountText(Category category, int count)
{
    const QString value = number(count);
    switch(category) {
        case Category::Genre:
            return tr("%1 genres").arg(value);
        case Category::Artist:
            return tr("%1 artists").arg(value);
        case Category::AlbumArtist:
            return tr("%1 album artists").arg(value);
        case Category::Composer:
            return tr("%1 composers").arg(value);
        case Category::Album:
            return tr("%1 albums").arg(value);
    }
    return {};
}

RowListWidget::Row trackRow(const PlayHistoryStatsTrack& track, const QString& value, const QString& trailing = {})
{
    QString subtitle = track.artist;
    if(!track.album.isEmpty()) {
        subtitle = subtitle.isEmpty() ? track.album : subtitle + u" — "_s + track.album;
    }

    return {.title    = track.title.isEmpty() ? tr("Unknown title") : track.title,
            .subtitle = subtitle,
            .value    = value,
            .trailing = trailing,
            .hash     = track.hash};
}

QDate dateOf(qint64 timestamp)
{
    return QDateTime::fromMSecsSinceEpoch(timestamp).date();
}

QLabel* captionLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont font  = label->font();
    font.setBold(true);
    label->setFont(font);
    label->setContentsMargins(6, 4, 0, 0);
    return label;
}

//! Tiles in as many columns as fit
TileLayout* tileGrid(const std::vector<MiniTile*>& tiles)
{
    auto* grid = new TileLayout();
    for(auto* tile : tiles) {
        grid->addWidget(tile);
    }
    return grid;
}

//! Cards one above the other, sharing the height of the cell they're in. Missing cards are left out.
QWidget* stackCards(const std::vector<QWidget*>& cards)
{
    std::vector<QWidget*> present;
    std::ranges::copy_if(cards, std::back_inserter(present), [](const QWidget* card) { return card != nullptr; });
    if(present.size() <= 1) {
        return present.empty() ? nullptr : present.front();
    }

    auto* container = new QWidget();
    auto* layout    = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    for(QWidget* card : present) {
        layout->addWidget(card, 1);
    }
    return container;
}

//! A list that starts out short and can be expanded to show the rest.
QWidget* expandableRows(std::vector<RowListWidget::Row> rows, int initial, int step, QWidget* parent,
                        bool showRank = true, StatsArt* art = nullptr)
{
    auto* container = new QWidget(parent);
    auto* layout    = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* list = new RowListWidget(container);
    list->setArt(art, art ? ListArtSize : 0);
    auto* more = new ShowMoreButton(static_cast<int>(rows.size()), initial, step, container);
    auto all   = std::make_shared<std::vector<RowListWidget::Row>>(std::move(rows));

    const auto apply = [list, all, showRank](int shown) {
        const auto count = static_cast<std::ptrdiff_t>(std::min<size_t>(static_cast<size_t>(shown), all->size()));
        list->setRows({all->begin(), all->begin() + count}, showRank);
    };
    apply(more->shown());
    QObject::connect(more, &ShowMoreButton::shownChanged, list, apply);

    layout->addWidget(list);
    layout->addWidget(more);
    return container;
}

QString minuteRange(const PlayHistoryStatsSession& session)
{
    const QLocale locale;
    return u"%1 → %2"_s.arg(
        locale.toString(QDateTime::fromMSecsSinceEpoch(session.startMs).time(), QLocale::ShortFormat),
        locale.toString(QDateTime::fromMSecsSinceEpoch(session.endMs).time(), QLocale::ShortFormat));
}
} // namespace

StatisticsDialog::StatisticsDialog(StatisticsService* service, QWidget* parent)
    : QDialog{parent}
    , m_service{service}
    , m_periods{new ChipBar(this)}
    , m_customButton{new QPushButton(tr("Custom range…"), this)}
    , m_yearButton{new QPushButton(this)}
    , m_exportButton{new QPushButton(tr("Export image…"), this)}
    , m_scrollArea{new QScrollArea(this)}
    , m_reloadTimer{new QTimer(this)}
    , m_period{Period::AllTime}
    , m_requestId{0}
    , m_keepScroll{false}
    , m_cumulative{true}
    , m_category{Category::Genre}
{
    setWindowTitle(tr("Statistics"));
    resize(1200, 880);

    auto* layout = new QVBoxLayout(this);

    auto* title     = new QLabel(tr("Listening statistics"), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.4);
    title->setFont(titleFont);

    m_yearButton->setVisible(false);
    m_exportButton->setEnabled(false);

    auto* header = new QHBoxLayout();
    header->addWidget(title, 1);
    header->addWidget(m_yearButton);
    header->addWidget(m_exportButton);
    layout->addLayout(header);

    m_customButton->setCheckable(true);
    m_periods->setItems({tr("7 days"), tr("30 days"), tr("3 months"), tr("6 months"), tr("1 year"), tr("All time")});
    m_periods->setCurrentIndex(static_cast<int>(Period::AllTime));

    auto* periods = new QHBoxLayout();
    periods->addWidget(m_customButton);
    periods->addWidget(m_periods, 1);
    layout->addLayout(periods);

    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_scrollArea, 1);

    QObject::connect(m_periods, &ChipBar::indexChanged, this,
                     [this](int index) { selectPeriod(static_cast<Period>(index)); });
    QObject::connect(m_customButton, &QPushButton::clicked, this, &StatisticsDialog::chooseCustomRange);
    QObject::connect(m_yearButton, &QPushButton::clicked, this, [this]() {
        if(!m_summary.years.empty()) {
            Q_EMIT yearInReviewRequested(m_summary.years.back());
        }
    });
    QObject::connect(m_exportButton, &QPushButton::clicked, this, &StatisticsDialog::exportImage);

    // Listens counted while the dialog is open show up shortly after, rather than all at once in a burst
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(1500);
    QObject::connect(m_service, &StatisticsService::historyChanged, m_reloadTimer, qOverload<>(&QTimer::start));
    QObject::connect(m_reloadTimer, &QTimer::timeout, this, &StatisticsDialog::reloadSummary);

    loadSummary();
}

std::pair<QDate, QDate> StatisticsDialog::resolveRange() const
{
    const QDate today = QDate::currentDate();

    switch(m_period) {
        case Period::Week:
            return {today.addDays(-6), today};
        case Period::Month:
            return {today.addDays(-29), today};
        case Period::ThreeMonths:
            return {today.addDays(-89), today};
        case Period::SixMonths:
            return {today.addDays(-181), today};
        case Period::Year:
            return {today.addDays(-364), today};
        case Period::AllTime: {
            const QDate oldest = m_summary.isEmpty() ? today : dateOf(m_summary.oldestTimestamp);
            return {oldest, today};
        }
        case Period::Custom:
            return {m_customFirst, m_customLast};
    }
    return {today, today};
}

void StatisticsDialog::selectPeriod(Period period)
{
    m_period = period;
    m_periods->setCurrentIndex(period == Period::Custom ? -1 : static_cast<int>(period));
    m_customButton->setChecked(period == Period::Custom);
    if(period != Period::Custom) {
        m_customButton->setText(tr("Custom range…"));
    }
    refresh();
}

void StatisticsDialog::chooseCustomRange()
{
    const QDate today  = QDate::currentDate();
    const QDate oldest = m_summary.isEmpty() ? today : dateOf(m_summary.oldestTimestamp);

    QDialog dialog{this};
    dialog.setWindowTitle(tr("Choose a date range"));

    auto* from = new QDateEdit(m_customFirst.isValid() ? m_customFirst : today.addDays(-29), &dialog);
    auto* to   = new QDateEdit(m_customLast.isValid() ? m_customLast : today, &dialog);
    for(auto* edit : {from, to}) {
        edit->setCalendarPopup(true);
        edit->setDateRange(oldest, today);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* form = new QFormLayout(&dialog);
    form->addRow(tr("From"), from);
    form->addRow(tr("To"), to);
    form->addRow(buttons);

    if(dialog.exec() == QDialog::Accepted) {
        setCustomRange(std::min(from->date(), to->date()), std::max(from->date(), to->date()));
    }
    else {
        m_customButton->setChecked(m_period == Period::Custom);
    }
}

void StatisticsDialog::setCustomRange(const QDate& first, const QDate& last)
{
    m_customFirst = first;
    m_customLast  = last;
    selectPeriod(Period::Custom);
    m_customButton->setText(formatRange(first, last));
}

void StatisticsDialog::loadSummary()
{
    m_service->refreshLibraryInfo();
    showMessage(tr("Loading…"));

    m_service->summary().then(this, [this](const PlayHistorySummary& summary) { applySummary(summary); });
}

void StatisticsDialog::reloadSummary()
{
    // The library may have changed too, and what's shown about it has to agree with the listens
    m_service->refreshLibraryInfo();
    m_keepScroll = true;
    m_service->summary().then(this, [this](const PlayHistorySummary& summary) { applySummary(summary); });
}

void StatisticsDialog::applySummary(const PlayHistorySummary& summary)
{
    m_summary = summary;
    m_yearButton->setVisible(!summary.years.empty());
    if(!summary.years.empty()) {
        m_yearButton->setText(tr("Your %1").arg(summary.years.back()));
    }
    refresh();
}

void StatisticsDialog::refresh()
{
    const int id = ++m_requestId;

    if(m_summary.isEmpty()) {
        m_stats.reset();
        m_exportButton->setEnabled(false);
        showMessage(tr("There's no listening history yet.\n\nFooyin now records every track you listen to, so "
                       "statistics will appear here as you play music."));
        return;
    }

    const auto [first, last] = resolveRange();

    m_service->compute(first, last).then(this, [this, id, first, last](const PlayHistoryStats& current) {
        if(id != m_requestId) {
            return;
        }

        if(m_period == Period::AllTime) {
            showStats(current, std::nullopt);
            return;
        }

        // Compare against the period of the same length directly before it
        const qint64 length = first.daysTo(last) + 1;
        m_service->compute(first.addDays(-length), first.addDays(-1))
            .then(this, [this, id, current](const PlayHistoryStats& previous) {
                if(id == m_requestId) {
                    showStats(current, previous);
                }
            });
    });
}

void StatisticsDialog::showMessage(const QString& message)
{
    setContent(createEmptyHint(message));
}

void StatisticsDialog::setContent(QWidget* content)
{
    const int scrolled = m_scrollArea->verticalScrollBar()->value();

    // The scroll area deletes the previous content
    m_scrollArea->setWidget(content);

    if(std::exchange(m_keepScroll, false)) {
        // The new content doesn't have its size until it's been laid out
        QTimer::singleShot(0, m_scrollArea,
                           [this, scrolled]() { m_scrollArea->verticalScrollBar()->setValue(scrolled); });
    }
}

void StatisticsDialog::showStats(const PlayHistoryStats& current, const std::optional<PlayHistoryStats>& previous)
{
    m_stats                       = std::make_shared<const PlayHistoryStats>(current);
    const PlayHistoryStats& stats = *m_stats;

    if(stats.isEmpty()) {
        m_exportButton->setEnabled(false);
        showMessage(tr("No listens in this period."));
        return;
    }

    // Rows of two cards of the same size when there's room, or one card across the width. The most useful come first
    auto* content = new QWidget();
    auto* layout  = new RowGridLayout(content, 12, 440);
    layout->setContentsMargins(0, 4, 8, 12);

    layout->addRow(makeOverview(stats, previous));
    layout->addRow(makeMonths(stats), makeListens(stats));
    layout->addRow(makeTopTracks(stats), makeTopArtists(stats));
    layout->addRow(makeDays(stats));
    layout->addRow(makeListeningClock(stats), stackCards({makeHabits(stats), makeMostPlayed(stats)}));
    layout->addRow(makeDiscoveries(stats), stackCards({makeObsession(stats), makeRediscoveries(stats)}));
    layout->addRow(makeCategories(stats), makeAudioQuality(stats));
    layout->addRow(makeCompletedAlbums(stats), stackCards({makeDecades(stats), makeListenTime(stats)}));

    setContent(content);
    m_exportButton->setEnabled(true);
}

void StatisticsDialog::exportImage()
{
    QWidget* content = m_scrollArea->widget();
    if(!content) {
        return;
    }

    const QString defaultPath
        = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + u"/fooyin-statistics.png"_s;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export image"), defaultPath, tr("PNG images (*.png)"));
    if(path.isEmpty()) {
        return;
    }

    // Charts grow in when first shown, so settle them before taking the picture
    const auto charts = content->findChildren<ChartWidget*>();
    for(auto* chart : charts) {
        chart->skipReveal();
    }

    content->grab().save(path, "PNG");
}

QWidget* StatisticsDialog::makeOverview(const PlayHistoryStats& stats, const std::optional<PlayHistoryStats>& previous)
{
    auto* card = new StatsCard({});

    const bool hasPrevious = previous && !previous->isEmpty();

    auto* listens = new MiniTile(tr("Total listens"), number(stats.totalListens));
    auto* time    = new MiniTile(tr("Total listen time"), formatListenTime(stats.listenSeconds));
    auto* tracks  = new MiniTile(tr("Tracks"), number(stats.uniqueTracks));
    auto* artists = new MiniTile(tr("Artists"), number(stats.uniqueArtists));
    auto* days   = new MiniTile(tr("Days"), u"%1 / %2"_s.arg(number(stats.daysWithListens), number(stats.totalDays())));
    auto* streak = new MiniTile(tr("Streak"), daysText(stats.currentStreak));

    if(hasPrevious) {
        listens->setDelta(stats.totalListens, previous->totalListens);
        time->setDelta(stats.listenSeconds, previous->listenSeconds);
        tracks->setDelta(stats.uniqueTracks, previous->uniqueTracks);
        artists->setDelta(stats.uniqueArtists, previous->uniqueArtists);
        days->setDelta(stats.daysWithListens, previous->daysWithListens);
    }

    card->addLayout(tileGrid({listens, time, tracks, artists, days, streak}));

    if(hasPrevious) {
        auto* caption
            = new QLabel(tr("Compared with %1").arg(formatRange(previous->firstDay, previous->lastDay)), card);
        QPalette palette = caption->palette();
        palette.setColor(QPalette::WindowText, Colours::fromPalette(palette).textMuted);
        caption->setPalette(palette);
        card->addWidget(caption);
    }

    return card;
}

QWidget* StatisticsDialog::makeMonths(const PlayHistoryStats& stats)
{
    if(stats.totalDays() <= 31) {
        return nullptr;
    }

    const QLocale locale;
    std::vector<ChartData> points;
    for(int month{0}; month < 12; ++month) {
        points.push_back({locale.monthName(month + 1, QLocale::ShortFormat), stats.months[static_cast<size_t>(month)]});
    }

    auto* card
        = new StatsCard(tr("Months"), stats.totalDays() > 366 ? tr("All time") : QString::number(stats.lastDay.year()));
    auto* chart = new VerticalBarsChart(card);
    chart->setPlotHeight(150);
    chart->setClickable(true);
    chart->setData(std::move(points));
    card->addWidget(chart);

    // Selecting a month narrows the statistics down to it
    QObject::connect(chart, &VerticalBarsChart::barClicked, this,
                     [this, lastDay = stats.lastDay, firstDay = stats.firstDay](int month) {
                         int year = lastDay.year();
                         if(month + 1 > lastDay.month()) {
                             --year;
                         }
                         const QDate start{year, month + 1, 1};
                         const QDate end = start.addMonths(1).addDays(-1);
                         if(end < firstDay) {
                             return;
                         }
                         setCustomRange(start, std::min(end, QDate::currentDate()));
                     });

    return card;
}

QWidget* StatisticsDialog::makeListens(const PlayHistoryStats& stats)
{
    auto* card = new StatsCard(tr("Listens"), formatRange(stats.firstDay, stats.lastDay));

    auto* chart = new ListensLineChart(card);
    chart->setData(stats.listensPerDay, stats.firstDay, m_cumulative);

    auto* toggle = new ChipBar(card);
    toggle->setItems({tr("Total"), tr("Daily")});
    toggle->setCurrentIndex(m_cumulative ? 0 : 1);
    QObject::connect(toggle, &ChipBar::indexChanged, chart, [this, chart](int index) {
        m_cumulative = index == 0;
        chart->setData(m_stats->listensPerDay, m_stats->firstDay, m_cumulative);
    });

    card->addHeaderWidget(toggle);
    card->addWidget(chart);
    return card;
}

QWidget* StatisticsDialog::makeDays(const PlayHistoryStats& stats)
{
    QString subtitle;
    if(stats.longestStreak) {
        subtitle
            = tr("Longest streak: %1 • Busiest day: %2 (%3)")
                  .arg(daysText(stats.longestStreak->days), formatDay(stats.peakDay), number(stats.peakDayListens));
    }

    auto* card  = new StatsCard(tr("Days"), subtitle);
    auto* chart = new CalendarHeatmapChart(card);
    chart->setMaxCellSize(22);
    chart->setData(stats.listensPerDay, stats.firstDay);
    // Long periods are shown as whole calendar years, the days outside the period being faded
    if(stats.totalDays() >= 180) {
        chart->setDisplayRange(QDate{stats.firstDay.year(), 1, 1}, QDate{stats.lastDay.year(), 12, 31});
    }
    card->addWidget(chart);
    return card;
}

QWidget* StatisticsDialog::makeListeningClock(const PlayHistoryStats& stats)
{
    const QLocale locale;
    // Sunday first, matching the data
    QStringList weekdayNames;
    std::vector<ChartData> weekdays;
    for(int i{0}; i < 7; ++i) {
        const QString name = locale.dayName(i == 0 ? 7 : i, QLocale::ShortFormat);
        weekdayNames.push_back(name);
        weekdays.push_back({name, stats.weekdays[static_cast<size_t>(i)]});
    }

    auto* card = new StatsCard(tr("Listening clock"));

    auto* clock = new ListeningClockChart(card);
    clock->setClockSize(250);
    clock->setData(stats.hours);
    clock->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* bars = new VerticalBarsChart(card);
    bars->setPlotHeight(150);
    bars->setData(weekdays);

    auto* top = new QHBoxLayout();
    top->setSpacing(16);
    top->addWidget(clock);
    top->addWidget(bars, 1, Qt::AlignVCenter);
    card->addLayout(top);

    QStringList hourNames;
    for(int hour{0}; hour < 24; ++hour) {
        hourNames.push_back(u"%1"_s.arg(hour, 2, 10, QLatin1Char{'0'}));
    }

    auto* grid = new GridHeatmapChart(card);
    grid->setData({stats.weekHours.begin(), stats.weekHours.end()}, 7, 24);
    grid->setLabels(weekdayNames, hourNames, 3);
    grid->setTipFormatter([weekdayNames](int row, int column, int value) {
        return u"%1 %2:00\n%3"_s.arg(weekdayNames.at(row)).arg(column, 2, 10, QLatin1Char{'0'}).arg(listensText(value));
    });
    card->addWidget(grid);

    return card;
}

QWidget* StatisticsDialog::makeHabits(const PlayHistoryStats& stats)
{
    const int repeatPercent = stats.totalListens <= 1
                                ? 0
                                : static_cast<int>(std::lround(stats.repeatListens * 100.0 / (stats.totalListens - 1)));
    const auto& library     = m_service->libraryInfo();

    auto* card = new StatsCard(tr("Habits"));

    std::vector<MiniTile*> tiles;
    tiles.push_back(new MiniTile(tr("Repeat rate"), u"%1%"_s.arg(repeatPercent)));
    tiles.push_back(new MiniTile(tr("Played once"), tr("%1 tracks").arg(number(stats.oneTimeTracks))));
    tiles.push_back(new MiniTile(tr("Played %1+ times").arg(PlayHistoryStats::HeavyRotationListens),
                                 tr("%1 tracks").arg(number(stats.heavyRotationTracks))));
    if(library.trackCount > 0) {
        // Tracks that were listened to and then removed from the library aren't part of the reach
        const int reach = static_cast<int>(std::lround(stats.libraryTracksHeard * 100.0 / library.trackCount));
        tiles.push_back(new MiniTile(tr("Library reach"), u"%1%"_s.arg(reach)));
        // Not played according to its play count, nor listened to since the history began
        qint64 neverPlayed{0};
        for(const QString& hash : library.unplayed) {
            if(!m_summary.hashes.contains(hash)) {
                ++neverPlayed;
            }
        }
        tiles.push_back(new MiniTile(tr("Never played"), tr("%1 tracks").arg(number(neverPlayed))));
    }
    if(stats.avgFirstMinute >= 0) {
        tiles.push_back(new MiniTile(tr("First listen"), formatMinuteOfDay(stats.avgFirstMinute)));
    }
    if(stats.avgLastMinute >= 0) {
        tiles.push_back(new MiniTile(tr("Last listen"), formatMinuteOfDay(stats.avgLastMinute)));
    }
    card->addLayout(tileGrid(tiles));

    if(const auto& streak = stats.longestTrackStreak) {
        const QDate end = dateOf(streak->timestamp);
        card->addWidget(
            captionLabel(tr("Streak • %1 • %2 → %3")
                             .arg(daysText(streak->days), formatDay(end.addDays(1 - streak->days)), formatDay(end)),
                         card));

        auto* list = new RowListWidget(card);
        list->setArt(m_service->art(), ListArtSize);
        list->setRows({trackRow(*streak, number(streak->count))}, false);
        card->addWidget(list);
    }

    return card;
}

QWidget* StatisticsDialog::makeObsession(const PlayHistoryStats& stats)
{
    if(stats.obsessions.empty() && !stats.longestSession) {
        return nullptr;
    }

    auto* card = new StatsCard(tr("Obsession"), stats.obsessions.empty() ? QString{} : tr("Most played • day"));

    if(!stats.obsessions.empty()) {
        std::vector<RowListWidget::Row> rows;
        for(const auto& obsession : stats.obsessions) {
            rows.push_back(trackRow(obsession, number(obsession.count), formatDay(dateOf(obsession.timestamp))));
        }
        card->addWidget(expandableRows(std::move(rows), 7, 7, card, false, m_service->art()));
    }

    if(const auto& session = stats.longestSession) {
        card->addWidget(captionLabel(tr("Longest session • %1").arg(formatDay(dateOf(session->startMs))), card));
        card->addLayout(tileGrid({new MiniTile(minuteRange(*session), formatDuration(session->durationMs())),
                                  new MiniTile(tr("Listens"), number(session->listens))}));
    }

    return card;
}

QWidget* StatisticsDialog::makeDiscoveries(const PlayHistoryStats& stats)
{
    auto* card
        = new StatsCard(tr("Discoveries"), tr("First listen • %1").arg(formatRange(stats.firstDay, stats.lastDay)));

    card->addLayout(tileGrid(
        {new MiniTile(tr("Tracks"), number(stats.newTracks)), new MiniTile(tr("Artists"), number(stats.newArtists))}));

    if(!stats.newTracksTop.empty()) {
        std::vector<RowListWidget::Row> rows;
        for(const auto& track : stats.newTracksTop) {
            rows.push_back(trackRow(track, number(track.count), formatDay(dateOf(track.timestamp))));
        }
        card->addWidget(expandableRows(std::move(rows), 5, 5, card, false, m_service->art()));
    }

    if(!stats.newArtistsTop.empty()) {
        card->addWidget(captionLabel(tr("Top artists • First listen"), card));

        // The first few, as with the year in review
        std::vector<MiniTile*> tiles;
        for(size_t i{0}; i < std::min<size_t>(6, stats.newArtistsTop.size()); ++i) {
            tiles.push_back(new MiniTile(listensText(stats.newArtistsTop[i].count), stats.newArtistsTop[i].key));
        }
        card->addLayout(tileGrid(tiles));
    }

    if(const auto& favourite = stats.oldestFavourite) {
        card->addWidget(captionLabel(tr("Oldest favourite • %1").arg(formatDay(dateOf(favourite->timestamp))), card));

        auto* list = new RowListWidget(card);
        list->setArt(m_service->art(), ListArtSize);
        list->setRows({trackRow(*favourite, number(favourite->count))}, false);
        card->addWidget(list);
    }

    return card;
}

QWidget* StatisticsDialog::makeTopTracks(const PlayHistoryStats& stats)
{
    auto* card = new StatsCard(tr("Top tracks"));

    std::vector<RowListWidget::Row> rows;
    for(const auto& track : stats.topTracks) {
        rows.push_back(trackRow(track, number(track.count)));
    }
    card->addWidget(expandableRows(std::move(rows), 10, 10, card, true, m_service->art()));

    return card;
}

QWidget* StatisticsDialog::makeMostPlayed(const PlayHistoryStats& stats)
{
    std::vector<RowListWidget::Row> rows;
    for(const Category category : {Category::Artist, Category::Album, Category::Genre}) {
        const auto& top = stats.category(category).top;
        if(!top.empty()) {
            rows.push_back({.title    = top.front().key,
                            .subtitle = categoryName(category),
                            .value    = number(top.front().count),
                            .trailing = {},
                            .hash     = {}});
        }
    }
    if(rows.empty()) {
        return nullptr;
    }

    auto* card = new StatsCard(tr("Most played"));
    auto* list = new RowListWidget(card);
    list->setRows(std::move(rows), false);
    card->addWidget(list);
    return card;
}

QWidget* StatisticsDialog::makeTopArtists(const PlayHistoryStats& stats)
{
    if(stats.category(Category::Artist).top.empty()) {
        return nullptr;
    }

    const auto& artists = stats.category(Category::Artist).top;

    auto* card = new StatsCard(tr("Top artists"));

    auto* container = new QWidget(card);
    auto* box       = new QVBoxLayout(container);
    box->setContentsMargins(0, 0, 0, 0);

    auto* chart = new RankBarsChart(container);
    chart->setPillStyle(true);
    chart->setRowHeight(ListArtSize + 14);
    auto* more = new ShowMoreButton(static_cast<int>(artists.size()), 10, 10, container);

    const auto apply = [chart, &artists](int shown) {
        std::vector<ChartData> points;
        for(size_t i{0}; i < std::min<size_t>(static_cast<size_t>(shown), artists.size()); ++i) {
            points.push_back({artists[i].key, artists[i].count});
        }
        chart->setData(std::move(points));
    };
    apply(more->shown());
    // The statistics outlive the card as they're only replaced along with it
    QObject::connect(more, &ShowMoreButton::shownChanged, chart, apply);

    box->addWidget(chart);
    box->addWidget(more);
    card->addWidget(container);
    return card;
}

QWidget* StatisticsDialog::makeRediscoveries(const PlayHistoryStats& stats)
{
    if(stats.rediscoveries.empty()) {
        return nullptr;
    }

    auto* card = new StatsCard(tr("Rediscoveries"),
                               tr("Last listen over %1 ago").arg(daysText(PlayHistoryStats::RediscoveryGapDays)));

    std::vector<RowListWidget::Row> rows;
    for(const auto& track : stats.rediscoveries) {
        rows.push_back(trackRow(track, number(track.count), daysText(track.days)));
    }

    auto* list = new RowListWidget(card);
    list->setArt(m_service->art(), ListArtSize);
    list->setRows(std::move(rows), false);
    card->addWidget(list);
    return card;
}

QWidget* StatisticsDialog::makeCompletedAlbums(const PlayHistoryStats& stats)
{
    if(stats.completedAlbums.empty()) {
        return nullptr;
    }

    auto* card = new StatsCard(tr("Completed albums"),
                               categoryCountText(Category::Album, static_cast<int>(stats.completedAlbums.size())));

    std::vector<RowListWidget::Row> rows;
    for(const auto& album : stats.completedAlbums) {
        rows.push_back({.title    = album.name,
                        .subtitle = tr("%1 tracks").arg(album.tracks),
                        .value    = number(album.listens),
                        .trailing = {},
                        .hash     = {}});
    }
    card->addWidget(expandableRows(std::move(rows), 10, 10, card, false));
    return card;
}

QWidget* StatisticsDialog::makeCategories(const PlayHistoryStats& /*stats*/)
{
    auto* card = new StatsCard(tr("Listens by"));

    auto* selector = new QComboBox(card);
    for(const Category category :
        {Category::Genre, Category::Artist, Category::AlbumArtist, Category::Composer, Category::Album}) {
        selector->addItem(categoryName(category), static_cast<int>(category));
        if(category == m_category) {
            selector->setCurrentIndex(selector->count() - 1);
        }
    }

    auto* donut = new DonutChart(card);
    donut->setCentreLabel(tr("Listens"));

    const auto update = [this, card, donut](Category category) {
        const auto& rank = m_stats->category(category);

        std::vector<ChartData> points;
        int other = rank.otherListens;
        for(size_t i{0}; i < rank.top.size(); ++i) {
            if(i < DonutSlices) {
                points.push_back({rank.top[i].key, rank.top[i].count});
            }
            else {
                other += rank.top[i].count;
            }
        }

        card->setSubtitle(categoryCountText(category, rank.unique));
        donut->setVisible(!points.empty());
        donut->setData(std::move(points), other);
    };
    update(m_category);

    QObject::connect(selector, &QComboBox::currentIndexChanged, donut, [this, selector, update]() {
        m_category = static_cast<Category>(selector->currentData().toInt());
        update(m_category);
    });

    card->addHeaderWidget(selector);
    card->addWidget(donut);
    return card;
}

QWidget* StatisticsDialog::makeAudioQuality(const PlayHistoryStats& stats)
{
    if(stats.losslessListens + stats.lossyListens == 0
       && std::ranges::none_of(stats.bitrateBuckets, [](int count) { return count > 0; })) {
        return nullptr;
    }

    const int total = stats.losslessListens + stats.lossyListens;

    QString subtitle;
    if(total > 0) {
        subtitle = tr("Lossless: %1%").arg(static_cast<int>(std::lround(stats.losslessListens * 100.0 / total)));
    }
    auto* card = new StatsCard(tr("Audio"), subtitle);

    const QStringList names{tr("< 128 kbps"), tr("128 – 255 kbps"), tr("256 – 319 kbps"), tr("320+ kbps")};
    std::vector<ChartData> points;
    for(size_t i{0}; i < stats.bitrateBuckets.size(); ++i) {
        if(stats.bitrateBuckets[i] > 0) {
            points.push_back({names.at(static_cast<qsizetype>(i)), stats.bitrateBuckets[i]});
        }
    }

    if(!points.empty()) {
        auto* donut = new DonutChart(card);
        donut->setCentreLabel(tr("Bitrate"));
        donut->setData(std::move(points));
        card->addWidget(donut);
    }
    if(total > 0) {
        card->addLayout(tileGrid({new MiniTile(tr("Lossless"), number(stats.losslessListens)),
                                  new MiniTile(tr("Lossy"), number(stats.lossyListens))}));
    }

    return card;
}

QWidget* StatisticsDialog::makeListenTime(const PlayHistoryStats& stats)
{
    const qint64 listenMs  = stats.listenSeconds * 1000;
    const qint64 libraryMs = m_service->libraryInfo().durationMs;
    const double fraction  = libraryMs > 0 ? static_cast<double>(listenMs) / static_cast<double>(libraryMs) : 0.0;

    auto* card = new StatsCard(tr("Total listen time"),
                               tr("Total track duration: %1").arg(formatListenTime(libraryMs / 1000)));

    auto* gauge = new GaugeChart(card);
    gauge->setValue(fraction, formatListenTime(listenMs / 1000),
                    QLocale().toString(fraction * 100.0, 'f', fraction >= 10 ? 0 : 1) + u"%"_s);

    auto* centred = new QHBoxLayout();
    centred->addStretch();
    centred->addWidget(gauge);
    centred->addStretch();
    card->addLayout(centred);

    return card;
}

QWidget* StatisticsDialog::makeDecades(const PlayHistoryStats& stats)
{
    if(stats.decades.empty()) {
        return nullptr;
    }

    std::vector<ChartData> points;
    for(const auto& decade : stats.decades) {
        points.push_back({decade.key + u"s"_s, decade.count});
    }

    auto* card  = new StatsCard(tr("Release decades"));
    auto* chart = new VerticalBarsChart(card);
    chart->setPlotHeight(110);
    chart->setData(std::move(points));
    card->addWidget(chart);
    return card;
}
} // namespace Fooyin

#include "moc_statisticsdialog.cpp"
