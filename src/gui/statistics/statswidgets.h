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

#include "statscharts.h"

#include <QColor>
#include <QDate>
#include <QFrame>
#include <QLayout>
#include <QPushButton>
#include <QString>
#include <QStringList>

#include <array>
#include <optional>
#include <vector>

namespace Fooyin {
class StatsArt;
}

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QVBoxLayout;

namespace Fooyin::Stats {
//! Listening time as "3d 4h", "12h 34m" or "45m".
QString formatListenTime(qint64 seconds);
//! A span as "1:23:45" or "4:05".
QString formatDuration(qint64 ms);
//! A time of day given as a minute of the day, e.g. 1,125 as 18:45 in the locale's time format.
QString formatMinuteOfDay(int minute);
QString formatDay(const QDate& day);
//! A date range as "3 Nov – 9 Nov".
QString formatRange(const QDate& first, const QDate& last);

//! A titled panel holding a chart or list.
class StatsCard : public QFrame
{
    Q_OBJECT

public:
    explicit StatsCard(const QString& title, const QString& subtitle = {}, QWidget* parent = nullptr);

    void setSubtitle(const QString& subtitle);
    //! Adds a widget, such as a toggle, to the right of the title.
    void addHeaderWidget(QWidget* widget);
    void addWidget(QWidget* widget);
    void addLayout(QLayout* layout);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* m_subtitle;
    QHBoxLayout* m_header;
    QVBoxLayout* m_body;
};

//! A labelled figure, optionally with how much it changed compared with a previous period.
class MiniTile : public QWidget
{
    Q_OBJECT

public:
    MiniTile(QString label, QString value, QWidget* parent = nullptr);

    void setDetail(const QString& detail);
    //! Shows the percentage change from @p previous to @p current. Nothing is shown when there's no previous value.
    void setDelta(qint64 current, qint64 previous);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] QString deltaText() const;

    QString m_label;
    QString m_value;
    QString m_detail;
    std::optional<double> m_delta;
};

//! A large figure with a caption, used on the year in review cards.
class BigStat : public QWidget
{
    Q_OBJECT

public:
    BigStat(QString value, QString label, QWidget* parent = nullptr);

    //! Shows the percentage change from @p previous to @p current next to the figure.
    void setDelta(qint64 current, qint64 previous);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_value;
    QString m_label;
    std::optional<double> m_delta;
};

//! A row of mutually exclusive toggle buttons.
class ChipBar : public QWidget
{
    Q_OBJECT

public:
    explicit ChipBar(QWidget* parent = nullptr);

    void setItems(const QStringList& items);
    //! Selects an item without emitting indexChanged(). -1 clears the selection.
    void setCurrentIndex(int index);
    [[nodiscard]] int currentIndex() const;

Q_SIGNALS:
    void indexChanged(int index);

private:
    QHBoxLayout* m_layout;
    QButtonGroup* m_group;
};

//! Reveals more rows of a list in steps, and collapses it again.
class ShowMoreButton : public QPushButton
{
    Q_OBJECT

public:
    ShowMoreButton(int total, int initial, int step, QWidget* parent = nullptr);

    [[nodiscard]] int shown() const;

Q_SIGNALS:
    void shownChanged(int shown);

private:
    void updateText();

    int m_total;
    int m_initial;
    int m_step;
    int m_shown;
};

//! Rows of a ranked list, each with a title and subtitle on the left and a value on the right.
class RowListWidget : public ChartWidget
{
    Q_OBJECT

public:
    struct Row
    {
        QString title;
        QString subtitle;
        //! The main figure, e.g. the number of listens.
        QString value;
        //! A secondary note under the value, e.g. a date.
        QString trailing;
        //! The track the row is for, used to show its artwork.
        QString hash;
    };

    explicit RowListWidget(QWidget* parent = nullptr);

    void setRows(std::vector<Row> rows, bool showRank = true);
    void setRowHeight(int height);
    //! Shows the artwork of each row's track at @p size (0 to hide it), taken from @p art.
    void setArt(StatsArt* art, int size);
    //! Scales the text, e.g. for the larger rows of the year in review.
    void setTextScale(qreal scale);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    [[nodiscard]] int hitTest(const QPoint& pos) const override;
    [[nodiscard]] QString tipFor(int item, const QPoint& pos) const override;

    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] int effectiveRowHeight() const;

    std::vector<Row> m_rows;
    StatsArt* m_art;
    qreal m_textScale;
    bool m_showRank;
    int m_rowHeight;
    int m_artSize;
};

//! A month of the year in review: the cover of its most played track with the track beneath.
class MonthCell : public QWidget
{
    Q_OBJECT

public:
    //! @p hash is empty for a month without listens.
    MonthCell(QString month, QString title, QString detail, QString hash, StatsArt* art, QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] int textHeight() const;

    QString m_month;
    QString m_title;
    QString m_detail;
    QString m_hash;
    StatsArt* m_art;
};

//! A track's cover drawn as a large rounded square, as wide as there is room for up to a maximum.
class HeroCover : public QWidget
{
    Q_OBJECT

public:
    HeroCover(QString hash, StatsArt* art, int maxSide, QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_hash;
    StatsArt* m_art;
    int m_maxSide;
};

//! A rounded label with a name and a count, such as an artist and the number of times they were listened to.
class PillLabel : public QWidget
{
    Q_OBJECT

public:
    PillLabel(QString name, QString count, QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_name;
    QString m_count;
};

//! Lays widgets out in rows, wrapping to a new row when one is full.
class FlowLayout : public QLayout
{
    Q_OBJECT

public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 8);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSize() const override;

private:
    int layoutItems(const QRect& rect, bool apply) const;

    QList<QLayoutItem*> m_items;
    int m_spacing;
};

//! Lays widgets out in columns, each going under the column with the least in it. Narrow windows get a single column.
class MasonryLayout : public QLayout
{
    Q_OBJECT

public:
    explicit MasonryLayout(QWidget* parent = nullptr, int spacing = 12, int minColumnWidth = 430, int maxColumns = 2);
    ~MasonryLayout() override;

    //! Adds a widget that takes the full width, below everything added before it.
    void addSpanningWidget(QWidget* widget);

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSize() const override;

private:
    struct Entry
    {
        QLayoutItem* item{nullptr};
        bool spanning{false};
    };

    int layoutItems(const QRect& rect, bool apply) const;

    std::vector<Entry> m_entries;
    int m_spacing;
    int m_minColumnWidth;
    int m_maxColumns;
    bool m_nextSpanning;
};

//! Lays widgets out in a grid of equally wide tiles, with as many columns as fit the width.
class TileLayout : public QLayout
{
    Q_OBJECT

public:
    explicit TileLayout(int minTileWidth = 170, int spacing = 8);
    ~TileLayout() override;

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSize() const override;

private:
    int layoutItems(const QRect& rect, bool apply) const;

    QList<QLayoutItem*> m_items;
    int m_minTileWidth;
    int m_spacing;
};

//! The dark card each page of the year in review is shown on, tinted by a colour from the year's top cover.
class StoryCard : public QWidget
{
    Q_OBJECT

public:
    //! Height reserved at the bottom of a page for the badge.
    static constexpr int BadgeSpace = 92;

    explicit StoryCard(QWidget* parent = nullptr);

    //! Sets the colour the card and the charts on it are drawn in. Widgets on the card use the palette set here.
    void setTint(const QColor& tint);
    //! Sets the year and the listens in each month shown in the badge.
    void setBadge(int year, const std::array<int, 12>& months);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void paintBadge(QPainter& painter) const;

    QColor m_background;
    int m_year;
    std::array<int, 12> m_months;
};

//! Shows how many pages there are and which is current, and lets one be picked.
class PageDots : public QWidget
{
    Q_OBJECT

public:
    explicit PageDots(QWidget* parent = nullptr);

    void setCount(int count);
    void setCurrent(int current);

    [[nodiscard]] QSize sizeHint() const override;

Q_SIGNALS:
    void pageClicked(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRectF dotRect(int index) const;

    int m_count;
    int m_current;
};

//! A message shown when there's nothing to chart.
QWidget* createEmptyHint(const QString& text, QWidget* parent = nullptr);
} // namespace Fooyin::Stats
