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

#include "statswidgets.h"

#include "statsart.h"

#include <gui/iconloader.h>

#include <QButtonGroup>
#include <QCoreApplication>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSpacerItem>
#include <QTime>
#include <QVBoxLayout>
#include <QWidgetItem>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin::Stats {
namespace {
QString tr(const char* text)
{
    return QCoreApplication::translate("Fooyin::Stats::Widgets", text);
}

QColor withAlpha(QColor colour, int alpha)
{
    colour.setAlpha(alpha);
    return colour;
}

QFont scaled(QFont font, qreal factor, bool bold = false)
{
    if(font.pointSizeF() > 0) {
        font.setPointSizeF(font.pointSizeF() * factor);
    }
    font.setBold(bold);
    return font;
}
} // namespace

QString formatListenTime(qint64 seconds)
{
    const qint64 minutes = seconds / 60;
    const qint64 hours   = minutes / 60;
    const qint64 days    = hours / 24;

    if(days > 0) {
        return tr("%1d %2h").arg(days).arg(hours % 24);
    }
    if(hours > 0) {
        return tr("%1h %2m").arg(hours).arg(minutes % 60);
    }
    return tr("%1m").arg(minutes);
}

QString formatDuration(qint64 ms)
{
    const qint64 seconds = ms / 1000;
    const qint64 hours   = seconds / 3600;
    const qint64 minutes = (seconds / 60) % 60;

    if(hours > 0) {
        return u"%1:%2:%3"_s.arg(hours)
            .arg(minutes, 2, 10, QLatin1Char{'0'})
            .arg(seconds % 60, 2, 10, QLatin1Char{'0'});
    }
    return u"%1:%2"_s.arg(minutes).arg(seconds % 60, 2, 10, QLatin1Char{'0'});
}

QString formatMinuteOfDay(int minute)
{
    if(minute < 0) {
        return {};
    }
    return QLocale().toString(QTime{minute / 60, minute % 60}, QLocale::ShortFormat);
}

QString formatDay(const QDate& day)
{
    return QLocale().toString(day, QLocale::ShortFormat);
}

QString formatRange(const QDate& first, const QDate& last)
{
    const QLocale locale;
    // Only mention the year when it isn't obvious from the range itself
    const QString format
        = first.year() == last.year() && last.year() == QDate::currentDate().year() ? u"d MMM"_s : u"d MMM yyyy"_s;
    return u"%1 – %2"_s.arg(locale.toString(first, format), locale.toString(last, format));
}

StatsCard::StatsCard(const QString& title, const QString& subtitle, QWidget* parent)
    : QFrame{parent}
    , m_subtitle{new QLabel(subtitle, this)}
    , m_header{new QHBoxLayout()}
    , m_body{new QVBoxLayout()}
    , m_spacer{new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding)}
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(12);

    auto* titleLabel = new QLabel(title, this);
    titleLabel->setFont(scaled(font(), 1.15, true));
    titleLabel->setVisible(!title.isEmpty());

    auto* titles = new QVBoxLayout();
    titles->setSpacing(0);
    titles->addWidget(titleLabel);
    titles->addWidget(m_subtitle);

    const auto muted      = Colours::fromPalette(palette()).textMuted;
    QPalette mutedPalette = m_subtitle->palette();
    mutedPalette.setColor(QPalette::WindowText, muted);
    m_subtitle->setPalette(mutedPalette);
    m_subtitle->setFont(scaled(font(), 0.9));
    m_subtitle->setVisible(!subtitle.isEmpty());

    m_header->addLayout(titles, 1);
    layout->addLayout(m_header);
    layout->addLayout(m_body, 1);
    m_body->addItem(m_spacer);
}

void StatsCard::setSubtitle(const QString& subtitle)
{
    m_subtitle->setText(subtitle);
    m_subtitle->setVisible(!subtitle.isEmpty());
}

void StatsCard::addHeaderWidget(QWidget* widget)
{
    m_header->addWidget(widget, 0, Qt::AlignTop);
}

void StatsCard::addWidget(QWidget* widget)
{
    // A card taller than its contents has them at the top, except for charts, which grow to fill it
    const bool grows = widget->sizePolicy().verticalPolicy() == QSizePolicy::Expanding;
    m_body->addWidget(widget, grows ? 1 : 0);
    keepSpacerLast();
}

void StatsCard::addLayout(QLayout* layout)
{
    m_body->addLayout(layout, layout->expandingDirections() & Qt::Vertical ? 1 : 0);
    keepSpacerLast();
}

void StatsCard::keepSpacerLast()
{
    m_body->removeItem(m_spacer);
    m_body->addItem(m_spacer);
}

void StatsCard::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);

    const auto colours = Colours::fromPalette(palette());
    painter.setPen(QPen{colours.grid, 1});
    painter.setBrush(colours.card);
    painter.drawRoundedRect(QRectF{rect()}.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
}

MiniTile::MiniTile(QString label, QString value, QWidget* parent)
    : QWidget{parent}
    , m_label{std::move(label)}
    , m_value{std::move(value)}
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void MiniTile::setDetail(const QString& detail)
{
    m_detail = detail;
    updateGeometry();
    update();
}

void MiniTile::setDelta(qint64 current, qint64 previous)
{
    if(previous <= 0) {
        m_delta.reset();
    }
    else {
        m_delta = 100.0 * static_cast<double>(current - previous) / static_cast<double>(previous);
    }
    update();
}

QString MiniTile::deltaText() const
{
    if(!m_delta) {
        return {};
    }

    const int percent = static_cast<int>(std::lround(*m_delta));
    if(percent == 0) {
        return u"0%"_s;
    }
    return u"%1 %2%"_s.arg(percent > 0 ? u"▲"_s : u"▼"_s).arg(std::abs(percent));
}

QSize MiniTile::sizeHint() const
{
    const QFontMetrics label{scaled(font(), 0.85)};
    const QFontMetrics value{scaled(font(), 1.15, true)};

    int height = 16 + label.height() + value.height();
    if(!m_detail.isEmpty()) {
        height += label.height();
    }
    return {160, height};
}

QSize MiniTile::minimumSizeHint() const
{
    return {80, sizeHint().height()};
}

void MiniTile::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);

    const auto colours = Colours::fromPalette(palette());
    painter.setPen(Qt::NoPen);
    painter.setBrush(Colours::mix(colours.text, colours.card, 0.06));
    painter.drawRoundedRect(rect(), 8, 8);

    const QFont labelFont = scaled(font(), 0.85);
    const QFont valueFont = scaled(font(), 1.15, true);
    const QFontMetricsF labelMetrics{labelFont};
    const QFontMetricsF valueMetrics{valueFont};

    const QRectF inner = QRectF{rect()}.adjusted(12, 8, -12, -8);
    qreal y            = inner.top();

    painter.setFont(labelFont);
    painter.setPen(colours.textMuted);
    painter.drawText(QRectF{inner.left(), y, inner.width(), labelMetrics.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                     labelMetrics.elidedText(m_label, Qt::ElideRight, inner.width()));
    y += labelMetrics.height();

    const QString delta = deltaText();
    qreal deltaWidth{0};
    if(!delta.isEmpty()) {
        deltaWidth = labelMetrics.horizontalAdvance(delta) + 8;

        QColor deltaColour = colours.textMuted;
        if(*m_delta > 0.5) {
            deltaColour = colours.seriesColour(2);
        }
        else if(*m_delta < -0.5) {
            deltaColour = colours.seriesColour(7);
        }
        // Green and red can vanish on a coloured card, where the arrow alone has to show the direction
        if(std::abs(deltaColour.lightness() - colours.card.lightness()) < 70) {
            deltaColour = colours.text;
        }
        painter.setPen(deltaColour);
        painter.drawText(QRectF{inner.right() - deltaWidth + 8, y, deltaWidth - 8, valueMetrics.height()},
                         Qt::AlignRight | Qt::AlignVCenter, delta);
    }

    painter.setFont(valueFont);
    painter.setPen(colours.text);
    painter.drawText(QRectF{inner.left(), y, inner.width() - deltaWidth, valueMetrics.height()},
                     Qt::AlignLeft | Qt::AlignVCenter,
                     valueMetrics.elidedText(m_value, Qt::ElideRight, inner.width() - deltaWidth));
    y += valueMetrics.height();

    if(!m_detail.isEmpty()) {
        painter.setFont(labelFont);
        painter.setPen(colours.textMuted);
        painter.drawText(QRectF{inner.left(), y, inner.width(), labelMetrics.height()},
                         Qt::AlignLeft | Qt::AlignVCenter,
                         labelMetrics.elidedText(m_detail, Qt::ElideRight, inner.width()));
    }
}

BigStat::BigStat(QString value, QString label, QWidget* parent)
    : QWidget{parent}
    , m_value{std::move(value)}
    , m_label{std::move(label)}
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void BigStat::setDelta(qint64 current, qint64 previous)
{
    if(previous <= 0) {
        m_delta.reset();
    }
    else {
        m_delta = 100.0 * static_cast<double>(current - previous) / static_cast<double>(previous);
    }
    update();
}

QSize BigStat::sizeHint() const
{
    return {140, QFontMetrics{scaled(font(), 2.2, true)}.height() + QFontMetrics{scaled(font(), 1.0)}.height() + 2};
}

QSize BigStat::minimumSizeHint() const
{
    return {60, sizeHint().height()};
}

void BigStat::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter{this};
    const auto colours = Colours::fromPalette(palette());

    const QFont valueFont = scaled(font(), 2.2, true);
    const QFont labelFont = scaled(font(), 1.0);
    const QFontMetricsF valueMetrics{valueFont};
    const QFontMetricsF labelMetrics{labelFont};

    painter.setFont(valueFont);
    painter.setPen(colours.text);
    painter.drawText(QRectF{0, 0, static_cast<qreal>(width()), valueMetrics.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                     valueMetrics.elidedText(m_value, Qt::ElideRight, width()));

    if(m_delta) {
        const int percent = static_cast<int>(std::lround(*m_delta));
        const QString delta
            = percent == 0 ? u"0%"_s : u"%1 %2%"_s.arg(percent > 0 ? u"▲"_s : u"▼"_s).arg(std::abs(percent));
        painter.setFont(labelFont);
        painter.setPen(percent > 0 ? colours.seriesColour(2)
                                   : (percent < 0 ? colours.seriesColour(7) : colours.textMuted));
        painter.drawText(QPointF{valueMetrics.horizontalAdvance(m_value) + 12, valueMetrics.ascent() + 2}, delta);
    }

    painter.setFont(labelFont);
    painter.setPen(colours.textMuted);
    painter.drawText(QRectF{0, valueMetrics.height() + 2, static_cast<qreal>(width()), labelMetrics.height()},
                     Qt::AlignLeft | Qt::AlignVCenter, labelMetrics.elidedText(m_label, Qt::ElideRight, width()));
}

ChipBar::ChipBar(QWidget* parent)
    : QWidget{parent}
    , m_layout{new QHBoxLayout(this)}
    , m_group{new QButtonGroup(this)}
{
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(6);
    m_group->setExclusive(true);

    QObject::connect(m_group, &QButtonGroup::idClicked, this, &ChipBar::indexChanged);
}

void ChipBar::setItems(const QStringList& items)
{
    const auto buttons = m_group->buttons();
    for(auto* button : buttons) {
        m_group->removeButton(button);
        delete button;
    }
    while(m_layout->count() > 0) {
        delete m_layout->takeAt(0);
    }

    for(int i{0}; i < items.size(); ++i) {
        auto* button = new QPushButton(items.at(i), this);
        button->setCheckable(true);
        button->setFlat(false);
        m_group->addButton(button, i);
        m_layout->addWidget(button);
    }
    m_layout->addStretch();
}

void ChipBar::setCurrentIndex(int index)
{
    // Exclusive groups can't be cleared by unchecking, so lift the restriction briefly
    m_group->setExclusive(false);
    const auto buttons = m_group->buttons();
    for(auto* button : buttons) {
        button->setChecked(m_group->id(button) == index);
    }
    m_group->setExclusive(true);
}

int ChipBar::currentIndex() const
{
    return m_group->checkedId();
}

ShowMoreButton::ShowMoreButton(int total, int initial, int step, QWidget* parent)
    : QPushButton{parent}
    , m_total{total}
    , m_initial{initial}
    , m_step{step}
    , m_shown{std::min(total, initial)}
{
    setFlat(true);
    setVisible(total > initial);
    updateText();

    QObject::connect(this, &QPushButton::clicked, this, [this]() {
        // Once everything is shown the same button collapses the list again
        m_shown = m_shown >= m_total ? m_initial : std::min(m_total, m_shown + m_step);
        updateText();
        Q_EMIT shownChanged(m_shown);
    });
}

int ShowMoreButton::shown() const
{
    return m_shown;
}

void ShowMoreButton::updateText()
{
    setText(m_shown >= m_total ? tr("Show less") : tr("Show more"));
}

RowListWidget::RowListWidget(QWidget* parent)
    : ChartWidget{parent}
    , m_art{nullptr}
    , m_textScale{1.0}
    , m_showRank{true}
    , m_rowHeight{0}
    , m_artSize{0}
{ }

void RowListWidget::setRows(std::vector<Row> rows, bool showRank)
{
    m_rows     = std::move(rows);
    m_showRank = showRank;
    setHovered(-1);
    updateGeometry();
    update();
}

void RowListWidget::setRowHeight(int height)
{
    m_rowHeight = height;
    updateGeometry();
}

void RowListWidget::setArt(StatsArt* art, int size)
{
    if(m_art) {
        m_art->disconnect(this);
    }

    m_art     = art;
    m_artSize = size;
    if(m_art) {
        QObject::connect(m_art, &StatsArt::coversChanged, this, qOverload<>(&QWidget::update));
    }
    updateGeometry();
    update();
}

void RowListWidget::setTextScale(qreal scale)
{
    m_textScale = scale;
    updateGeometry();
    update();
}

int RowListWidget::effectiveRowHeight() const
{
    if(m_rowHeight > 0) {
        return m_rowHeight;
    }
    if(m_art && m_artSize > 0) {
        return m_artSize + 14;
    }
    return QFontMetrics{scaled(font(), m_textScale)}.height() + QFontMetrics{scaled(smallFont(), m_textScale)}.height()
         + 16;
}

QSize RowListWidget::sizeHint() const
{
    return {300, effectiveRowHeight() * static_cast<int>(m_rows.size())};
}

QSize RowListWidget::minimumSizeHint() const
{
    return {160, sizeHint().height()};
}

int RowListWidget::hitTest(const QPoint& pos) const
{
    if(m_rows.empty() || pos.y() < 0) {
        return -1;
    }

    const int row = pos.y() / effectiveRowHeight();
    return row < static_cast<int>(m_rows.size()) ? row : -1;
}

QString RowListWidget::tipFor(int item, const QPoint& /*pos*/) const
{
    const auto& row = m_rows.at(static_cast<size_t>(item));
    return row.subtitle.isEmpty() ? row.title : row.title + u"\n"_s + row.subtitle;
}

void RowListWidget::paintEvent(QPaintEvent* /*event*/)
{
    if(m_rows.empty()) {
        return;
    }

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const QFont titleFont = scaled(font(), m_textScale);
    const QFont smaller   = scaled(smallFont(), m_textScale);
    const QFontMetricsF titleMetrics{titleFont};
    const QFontMetricsF smallMetrics{smaller};
    const qreal rowHeight = effectiveRowHeight();

    QFont valueFont = titleFont;
    valueFont.setBold(true);
    const QFontMetricsF valueMetrics{valueFont};

    qreal rightWidth{0};
    for(const auto& row : m_rows) {
        rightWidth = std::max(
            {rightWidth, valueMetrics.horizontalAdvance(row.value), smallMetrics.horizontalAdvance(row.trailing)});
    }

    const bool showArt    = m_art && m_artSize > 0;
    const qreal rankWidth = m_showRank ? titleMetrics.horizontalAdvance(QString::number(m_rows.size())) + 18 : 0;
    const qreal artLeft   = 8 + rankWidth;
    const qreal textLeft  = artLeft + (showArt ? m_artSize + 14 : 0);
    const qreal textWidth = std::max(0.0, width() - textLeft - rightWidth - 20);

    for(size_t i{0}; i < m_rows.size(); ++i) {
        const auto& row = m_rows[i];
        const QRectF bounds{0, static_cast<qreal>(i) * rowHeight, static_cast<qreal>(width()), rowHeight};

        if(static_cast<int>(i) == hovered()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Colours::mix(colours().text, colours().card, 0.07));
            painter.drawRoundedRect(bounds, 8, 8);
        }

        if(m_showRank) {
            painter.setFont(titleFont);
            painter.setPen(colours().textMuted);
            painter.drawText(QRectF{8, bounds.top(), rankWidth - 8, bounds.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                             QString::number(i + 1));
        }

        if(showArt) {
            const QRectF cover{artLeft, bounds.top() + (bounds.height() - m_artSize) / 2, static_cast<qreal>(m_artSize),
                               static_cast<qreal>(m_artSize)};
            QPainterPath clip;
            clip.addRoundedRect(cover, std::min(12.0, m_artSize * 0.16), std::min(12.0, m_artSize * 0.16));
            painter.save();
            painter.setClipPath(clip);
            painter.drawPixmap(cover.toRect(), m_art->cover(row.hash, m_artSize, devicePixelRatioF()));
            painter.restore();
        }

        const bool hasSubtitle  = !row.subtitle.isEmpty();
        const qreal blockHeight = titleMetrics.height() + (hasSubtitle ? smallMetrics.height() : 0);
        const qreal top         = bounds.top() + (bounds.height() - blockHeight) / 2;

        painter.setFont(titleFont);
        painter.setPen(colours().text);
        painter.drawText(QRectF{textLeft, top, textWidth, titleMetrics.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                         titleMetrics.elidedText(row.title, Qt::ElideRight, textWidth));

        if(hasSubtitle) {
            painter.setFont(smaller);
            painter.setPen(colours().textMuted);
            painter.drawText(QRectF{textLeft, top + titleMetrics.height(), textWidth, smallMetrics.height()},
                             Qt::AlignLeft | Qt::AlignVCenter,
                             smallMetrics.elidedText(row.subtitle, Qt::ElideRight, textWidth));
        }

        const bool hasTrailing = !row.trailing.isEmpty();
        const qreal rightBlock = valueMetrics.height() + (hasTrailing ? smallMetrics.height() : 0);
        const qreal rightTop   = bounds.top() + (bounds.height() - rightBlock) / 2;
        const qreal rightLeft  = width() - rightWidth - 10;

        painter.setFont(valueFont);
        painter.setPen(colours().text);
        painter.drawText(QRectF{rightLeft, rightTop, rightWidth, valueMetrics.height()},
                         Qt::AlignRight | Qt::AlignVCenter, row.value);

        if(hasTrailing) {
            painter.setFont(smaller);
            painter.setPen(colours().textMuted);
            painter.drawText(QRectF{rightLeft, rightTop + valueMetrics.height(), rightWidth, smallMetrics.height()},
                             Qt::AlignRight | Qt::AlignVCenter, row.trailing);
        }
    }
}

HeroCover::HeroCover(QString hash, StatsArt* art, int maxSide, QWidget* parent)
    : QWidget{parent}
    , m_hash{std::move(hash)}
    , m_art{art}
    , m_maxSide{maxSide}
{
    QSizePolicy policy{QSizePolicy::Expanding, QSizePolicy::Minimum};
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    if(m_art) {
        QObject::connect(m_art, &StatsArt::coversChanged, this, qOverload<>(&QWidget::update));
    }
}

QSize HeroCover::sizeHint() const
{
    return {m_maxSide, m_maxSide};
}

bool HeroCover::hasHeightForWidth() const
{
    return true;
}

int HeroCover::heightForWidth(int width) const
{
    return std::min(width, m_maxSide);
}

void HeroCover::paintEvent(QPaintEvent* /*event*/)
{
    if(!m_art) {
        return;
    }

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const int side = std::min({width(), height(), m_maxSide});
    const QRectF cover{(width() - side) / 2.0, 0, static_cast<qreal>(side), static_cast<qreal>(side)};

    QPainterPath clip;
    clip.addRoundedRect(cover, side * 0.09, side * 0.09);
    painter.setClipPath(clip);
    painter.drawPixmap(cover.toRect(), m_art->cover(m_hash, side, devicePixelRatioF()));
}

MonthCell::MonthCell(QString month, QString title, QString detail, QString hash, StatsArt* art, QWidget* parent)
    : QWidget{parent}
    , m_month{std::move(month)}
    , m_title{std::move(title)}
    , m_detail{std::move(detail)}
    , m_hash{std::move(hash)}
    , m_art{art}
{
    QSizePolicy policy{QSizePolicy::Expanding, QSizePolicy::Minimum};
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    // Twelve of these make a tall card, so don't let them grow with a wide one
    setMaximumWidth(122);

    if(m_art) {
        QObject::connect(m_art, &StatsArt::coversChanged, this, qOverload<>(&QWidget::update));
    }
}

int MonthCell::textHeight() const
{
    return QFontMetrics{scaled(font(), 1.0, true)}.height() + QFontMetrics{scaled(font(), 0.9)}.height() + 8;
}

QSize MonthCell::sizeHint() const
{
    return {120, heightForWidth(120)};
}

bool MonthCell::hasHeightForWidth() const
{
    return true;
}

int MonthCell::heightForWidth(int width) const
{
    // The cell is drawn no wider than its maximum, whatever width the layout offers
    return std::min(width, maximumWidth()) + textHeight();
}

void MonthCell::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const auto colours = Colours::fromPalette(palette());
    const int side     = std::min(width(), maximumWidth());
    const QRectF cover{0, 0, static_cast<qreal>(side), static_cast<qreal>(side)};
    const qreal radius = std::min(18.0, side * 0.14);

    QPainterPath clip;
    clip.addRoundedRect(cover, radius, radius);
    if(m_hash.isEmpty() || !m_art) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(colours.text, 14));
        painter.drawPath(clip);
    }
    else {
        painter.save();
        painter.setClipPath(clip);
        painter.drawPixmap(cover.toRect(), m_art->cover(m_hash, side, devicePixelRatioF()));
        painter.restore();
    }

    // The month on a badge in the corner of the cover
    const QFont badgeFont = scaled(font(), 0.85, true);
    const QFontMetricsF badgeMetrics{badgeFont};
    const qreal badgeWidth  = badgeMetrics.horizontalAdvance(m_month) + 16;
    const qreal badgeHeight = badgeMetrics.height() + 6;
    const QRectF badge{8, 8, badgeWidth, badgeHeight};
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{0, 0, 0, 175});
    painter.drawRoundedRect(badge, badgeHeight / 2, badgeHeight / 2);
    painter.setFont(badgeFont);
    painter.setPen(QColor{255, 255, 255, 235});
    painter.drawText(badge, Qt::AlignCenter, m_month);

    // The track beneath, or just the month when there's nothing for it
    const QFont titleFont  = scaled(font(), 1.0, true);
    const QFont detailFont = scaled(font(), 0.9);
    const QFontMetricsF titleMetrics{titleFont};
    const QFontMetricsF detailMetrics{detailFont};

    painter.setFont(titleFont);
    painter.setPen(colours.text);
    painter.drawText(QRectF{2, side + 6.0, side - 4.0, titleMetrics.height()}, Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(m_hash.isEmpty() ? m_month : m_title, Qt::ElideRight, side - 4));

    if(!m_detail.isEmpty()) {
        painter.setFont(detailFont);
        painter.setPen(colours.textMuted);
        painter.drawText(QRectF{2, side + 6.0 + titleMetrics.height(), side - 4.0, detailMetrics.height()},
                         Qt::AlignLeft | Qt::AlignVCenter,
                         detailMetrics.elidedText(m_detail, Qt::ElideRight, side - 4));
    }
}

PillLabel::PillLabel(QString name, QString count, QWidget* parent)
    : QWidget{parent}
    , m_name{std::move(name)}
    , m_count{std::move(count)}
{
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
}

QSize PillLabel::sizeHint() const
{
    const QFontMetrics name{scaled(font(), 1.0, true)};
    const QFontMetrics count{scaled(font(), 0.9)};
    return {name.horizontalAdvance(m_name) + count.horizontalAdvance(m_count) + 44, name.height() + 16};
}

void PillLabel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);

    const auto colours = Colours::fromPalette(palette());
    painter.setPen(Qt::NoPen);
    painter.setBrush(withAlpha(colours.text, 20));
    painter.drawRoundedRect(rect(), height() / 2.0, height() / 2.0);

    const QFont nameFont  = scaled(font(), 1.0, true);
    const QFont countFont = scaled(font(), 0.9);
    const QFontMetricsF nameMetrics{nameFont};
    const QFontMetricsF countMetrics{countFont};

    painter.setFont(nameFont);
    painter.setPen(colours.text);
    painter.drawText(QRectF{16, 0, nameMetrics.horizontalAdvance(m_name) + 2, static_cast<qreal>(height())},
                     Qt::AlignVCenter | Qt::AlignLeft, m_name);

    painter.setFont(countFont);
    painter.setPen(colours.textMuted);
    painter.drawText(QRectF{16 + nameMetrics.horizontalAdvance(m_name) + 10, 0,
                            countMetrics.horizontalAdvance(m_count) + 2, static_cast<qreal>(height())},
                     Qt::AlignVCenter | Qt::AlignLeft, m_count);
}

FlowLayout::FlowLayout(QWidget* parent, int spacing)
    : QLayout{parent}
    , m_spacing{spacing}
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while(QLayoutItem* item = takeAt(0)) {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem* item)
{
    m_items.append(item);
}

int FlowLayout::count() const
{
    return static_cast<int>(m_items.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const
{
    return m_items.value(index);
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return layoutItems(QRect{0, 0, width, 0}, false);
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    layoutItems(rect, true);
}

QSize FlowLayout::sizeHint() const
{
    return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for(const QLayoutItem* item : m_items) {
        size = size.expandedTo(item->minimumSize());
    }

    const QMargins margins = contentsMargins();
    return size + QSize{margins.left() + margins.right(), margins.top() + margins.bottom()};
}

int FlowLayout::layoutItems(const QRect& rect, bool apply) const
{
    const QMargins margins = contentsMargins();
    const QRect inner      = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());

    int x{inner.x()};
    int y{inner.y()};
    int rowHeight{0};
    for(QLayoutItem* item : m_items) {
        const QSize size = item->sizeHint();
        if(x + size.width() > inner.right() + 1 && rowHeight > 0) {
            x = inner.x();
            y += rowHeight + m_spacing;
            rowHeight = 0;
        }
        if(apply) {
            item->setGeometry(QRect{QPoint{x, y}, size});
        }
        x += size.width() + m_spacing;
        rowHeight = std::max(rowHeight, size.height());
    }
    return y + rowHeight - rect.y() + margins.bottom();
}

RowGridLayout::RowGridLayout(QWidget* parent, int spacing, int minColumnWidth)
    : QLayout{parent}
    , m_spacing{spacing}
    , m_minColumnWidth{minColumnWidth}
{ }

RowGridLayout::~RowGridLayout()
{
    while(QLayoutItem* item = takeAt(0)) {
        delete item;
    }
}

void RowGridLayout::addRow(QWidget* first, QWidget* second)
{
    if(!first) {
        std::swap(first, second);
    }
    if(!first) {
        return;
    }

    addChildWidget(first);
    Row row{.first = new QWidgetItem(first), .second = nullptr};
    if(second) {
        addChildWidget(second);
        row.second = new QWidgetItem(second);
    }
    m_rows.push_back(row);
    invalidate();
}

void RowGridLayout::addItem(QLayoutItem* item)
{
    m_rows.push_back({.first = item, .second = nullptr});
}

int RowGridLayout::count() const
{
    int count{0};
    for(const Row& row : m_rows) {
        count += (row.first ? 1 : 0) + (row.second ? 1 : 0);
    }
    return count;
}

QLayoutItem* RowGridLayout::itemAt(int index) const
{
    int position{0};
    for(const Row& row : m_rows) {
        for(QLayoutItem* item : {row.first, row.second}) {
            if(item && position++ == index) {
                return item;
            }
        }
    }
    return nullptr;
}

QLayoutItem* RowGridLayout::takeAt(int index)
{
    int position{0};
    for(auto it = m_rows.begin(); it != m_rows.end(); ++it) {
        for(QLayoutItem** slot : {&it->first, &it->second}) {
            if(*slot && position++ == index) {
                QLayoutItem* item = *slot;
                *slot             = nullptr;
                if(!it->first && !it->second) {
                    m_rows.erase(it);
                }
                invalidate();
                return item;
            }
        }
    }
    return nullptr;
}

Qt::Orientations RowGridLayout::expandingDirections() const
{
    return Qt::Horizontal;
}

bool RowGridLayout::hasHeightForWidth() const
{
    return true;
}

int RowGridLayout::heightForWidth(int width) const
{
    return layoutItems(QRect{0, 0, width, 0}, false);
}

void RowGridLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    layoutItems(rect, true);
}

QSize RowGridLayout::sizeHint() const
{
    return {m_minColumnWidth, heightForWidth(m_minColumnWidth)};
}

QSize RowGridLayout::minimumSize() const
{
    QSize size;
    for(const Row& row : m_rows) {
        for(const QLayoutItem* item : {row.first, row.second}) {
            if(item) {
                size = size.expandedTo(item->minimumSize());
            }
        }
    }
    const QMargins margins = contentsMargins();
    return size + QSize{margins.left() + margins.right(), margins.top() + margins.bottom()};
}

int RowGridLayout::layoutItems(const QRect& rect, bool apply) const
{
    const QMargins margins = contentsMargins();
    const QRect inner      = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());

    // Two columns once each has room for its minimum width
    const bool twoColumns = inner.width() >= m_minColumnWidth * 2 + m_spacing;
    const int columnWidth = twoColumns ? (inner.width() - m_spacing) / 2 : inner.width();

    const auto heightFor = [](QLayoutItem* item, int width) {
        return item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height();
    };

    int y{inner.y()};
    const auto place = [&](QLayoutItem* item, int x, int width, int height) {
        if(apply) {
            item->setGeometry(QRect{x, y, width, height});
        }
    };

    for(const Row& row : m_rows) {
        if(!row.first) {
            continue;
        }

        if(!row.second) {
            const int height = heightFor(row.first, inner.width());
            place(row.first, inner.x(), inner.width(), height);
            y += height + m_spacing;
        }
        else if(twoColumns) {
            // Both cards are as tall as the taller, so the rows line up
            const int height = std::max(heightFor(row.first, columnWidth), heightFor(row.second, columnWidth));
            place(row.first, inner.x(), columnWidth, height);
            place(row.second, inner.x() + columnWidth + m_spacing, columnWidth, height);
            y += height + m_spacing;
        }
        else {
            for(QLayoutItem* item : {row.first, row.second}) {
                const int height = heightFor(item, inner.width());
                place(item, inner.x(), inner.width(), height);
                y += height + m_spacing;
            }
        }
    }

    return std::max(0, y - m_spacing - rect.y()) + margins.bottom();
}

TileLayout::TileLayout(int minTileWidth, int spacing)
    : m_minTileWidth{minTileWidth}
    , m_spacing{spacing}
{
    setContentsMargins(0, 0, 0, 0);
}

TileLayout::~TileLayout()
{
    while(QLayoutItem* item = takeAt(0)) {
        delete item;
    }
}

void TileLayout::addItem(QLayoutItem* item)
{
    m_items.append(item);
}

int TileLayout::count() const
{
    return static_cast<int>(m_items.size());
}

QLayoutItem* TileLayout::itemAt(int index) const
{
    return m_items.value(index);
}

QLayoutItem* TileLayout::takeAt(int index)
{
    return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
}

Qt::Orientations TileLayout::expandingDirections() const
{
    return Qt::Horizontal;
}

bool TileLayout::hasHeightForWidth() const
{
    return true;
}

int TileLayout::heightForWidth(int width) const
{
    return layoutItems(QRect{0, 0, width, 0}, false);
}

void TileLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    layoutItems(rect, true);
}

QSize TileLayout::sizeHint() const
{
    return {m_minTileWidth, heightForWidth(m_minTileWidth)};
}

QSize TileLayout::minimumSize() const
{
    QSize size;
    for(const QLayoutItem* item : m_items) {
        size = size.expandedTo(item->minimumSize());
    }
    const QMargins margins = contentsMargins();
    return size + QSize{margins.left() + margins.right(), margins.top() + margins.bottom()};
}

int TileLayout::layoutItems(const QRect& rect, bool apply) const
{
    const QMargins margins = contentsMargins();
    const QRect inner      = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    if(m_items.isEmpty()) {
        return margins.top() + margins.bottom();
    }

    const int columns
        = std::clamp((inner.width() + m_spacing) / (m_minTileWidth + m_spacing), 1, static_cast<int>(m_items.size()));
    const int tileWidth = (inner.width() - m_spacing * (columns - 1)) / columns;

    int y{inner.y()};
    for(int start{0}; start < m_items.size(); start += columns) {
        int rowHeight{0};
        for(int i{start}; i < std::min<int>(start + columns, static_cast<int>(m_items.size())); ++i) {
            QLayoutItem* item = m_items[i];
            const int height  = item->hasHeightForWidth() ? item->heightForWidth(tileWidth) : item->sizeHint().height();
            if(apply) {
                item->setGeometry(QRect{inner.x() + (i - start) * (tileWidth + m_spacing), y, tileWidth, height});
            }
            rowHeight = std::max(rowHeight, height);
        }
        y += rowHeight + m_spacing;
    }
    return y - m_spacing - rect.y() + margins.bottom();
}

StoryCard::StoryCard(QWidget* parent)
    : QWidget{parent}
    , m_year{0}
    , m_months{}
{
    setTint(QColor::fromHsv(210, 140, 220));
}

void StoryCard::setTint(const QColor& tint)
{
    // A near black card with the faintest hint of the tint, the tint itself being kept for the charts
    const int hue        = tint.hsvHue() >= 0 ? tint.hsvHue() : 210;
    const int saturation = std::clamp(tint.hsvSaturation(), 90, 215);
    m_background         = QColor::fromHsv(hue, saturation / 2, 26);
    const QColor accent  = QColor::fromHsv(hue, std::min(saturation, 150), 245);
    const QColor text{236, 236, 240};

    QPalette cardPalette = palette();
    cardPalette.setColor(QPalette::Window, m_background);
    cardPalette.setColor(QPalette::Base, m_background);
    cardPalette.setColor(QPalette::WindowText, text);
    cardPalette.setColor(QPalette::Text, text);
    cardPalette.setColor(QPalette::ButtonText, text);
    cardPalette.setColor(QPalette::Highlight, accent);
    setPalette(cardPalette);
    update();
}

void StoryCard::setBadge(int year, const std::array<int, 12>& months)
{
    m_year   = year;
    m_months = months;
    update();
}

void StoryCard::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath shape;
    shape.addRoundedRect(QRectF{rect()}, 28, 28);
    painter.setClipPath(shape);
    painter.fillPath(shape, m_background);

    // Two large faint circles behind the content
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{255, 255, 255, 9});
    painter.drawEllipse(QPointF{width() * 0.95, height() * 0.07}, width() * 0.5, width() * 0.5);
    painter.drawEllipse(QPointF{width() * 0.0, height() * 0.97}, width() * 0.36, width() * 0.36);

    paintBadge(painter);
}

void StoryCard::paintBadge(QPainter& painter) const
{
    if(m_year <= 0) {
        return;
    }

    constexpr qreal badgeWidth  = 186;
    constexpr qreal badgeHeight = 50;
    const QRectF badge{width() - badgeWidth - 22.0, height() - badgeHeight - 22.0, badgeWidth, badgeHeight};

    // A ticket: the icon, a perforated line with notches, the year and a sparkline of the year
    constexpr qreal divider = 58;
    QPainterPath ticket;
    ticket.addRoundedRect(badge, 14, 14);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{255, 255, 255, 20});
    painter.drawPath(ticket);

    painter.setBrush(m_background);
    painter.drawEllipse(QPointF{badge.left() + divider, badge.top()}, 7, 7);
    painter.drawEllipse(QPointF{badge.left() + divider, badge.bottom()}, 7, 7);

    QPen dashes{QColor{255, 255, 255, 60}, 1.5};
    dashes.setStyle(Qt::CustomDashLine);
    dashes.setDashPattern({2.0, 3.0});
    painter.setPen(dashes);
    painter.drawLine(QPointF{badge.left() + divider, badge.top() + 10},
                     QPointF{badge.left() + divider, badge.bottom() - 10});

    const QPixmap icon = Gui::applicationIcon().pixmap(QSize{28, 28}, devicePixelRatioF());
    painter.drawPixmap(QPointF{badge.left() + (divider - 28) / 2, badge.center().y() - 14}, icon);

    QFont brand = font();
    brand.setPointSizeF(brand.pointSizeF() * 0.7);
    brand.setBold(true);
    brand.setLetterSpacing(QFont::AbsoluteSpacing, 1.4);
    QFont year = font();
    year.setBold(true);
    year.setPointSizeF(year.pointSizeF() * 1.35);

    const qreal textLeft = badge.left() + divider + 12;
    painter.setFont(brand);
    painter.setPen(QColor{255, 255, 255, 140});
    painter.drawText(QPointF{textLeft, badge.top() + 20}, u"FOOYIN"_s);
    painter.setFont(year);
    painter.setPen(QColor{255, 255, 255, 235});
    painter.drawText(QPointF{textLeft, badge.bottom() - 9}, QString::number(m_year));

    const int maxMonth       = std::max(1, *std::ranges::max_element(m_months));
    constexpr qreal barWidth = 3;
    constexpr qreal barGap   = 2;
    const qreal sparkLeft    = badge.right() - 14 - 12 * barWidth - 11 * barGap;
    painter.setPen(Qt::NoPen);
    for(size_t i{0}; i < m_months.size(); ++i) {
        const qreal height = std::max(3.0, 24.0 * m_months[i] / maxMonth);
        painter.setBrush(QColor{255, 255, 255, m_months[i] == maxMonth ? 190 : 85});
        painter.drawRoundedRect(QRectF{sparkLeft + static_cast<qreal>(i) * (barWidth + barGap),
                                       badge.center().y() + 12 - height, barWidth, height},
                                1.5, 1.5);
    }
}

PageDots::PageDots(QWidget* parent)
    : QWidget{parent}
    , m_count{0}
    , m_current{0}
{
    setCursor(Qt::PointingHandCursor);
}

void PageDots::setCount(int count)
{
    m_count = count;
    updateGeometry();
    update();
}

void PageDots::setCurrent(int current)
{
    m_current = current;
    update();
}

QSize PageDots::sizeHint() const
{
    return {200, 18};
}

QRectF PageDots::dotRect(int index) const
{
    constexpr qreal dot     = 6;
    constexpr qreal current = 18;
    constexpr qreal gap     = 6;

    const qreal total = current + (m_count - 1) * dot + (m_count - 1) * gap;
    qreal x           = (width() - total) / 2;
    for(int i{0}; i < index; ++i) {
        x += (i == m_current ? current : dot) + gap;
    }
    return QRectF{x, (height() - dot) / 2, index == m_current ? current : dot, dot};
}

void PageDots::paintEvent(QPaintEvent* /*event*/)
{
    if(m_count < 2) {
        return;
    }

    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);

    const auto colours = Colours::fromPalette(palette());
    for(int i{0}; i < m_count; ++i) {
        QColor colour = i == m_current ? colours.accent : colours.text;
        if(i != m_current) {
            colour.setAlpha(70);
        }
        painter.setBrush(colour);
        painter.drawRoundedRect(dotRect(i), 3, 3);
    }
}

void PageDots::mousePressEvent(QMouseEvent* event)
{
    for(int i{0}; i < m_count; ++i) {
        // Pad the dots so they're easier to hit
        if(dotRect(i).adjusted(-3, -8, 3, 8).contains(event->position())) {
            Q_EMIT pageClicked(i);
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

QWidget* createEmptyHint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setMargin(32);

    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, Colours::fromPalette(label->palette()).textMuted);
    label->setPalette(palette);
    return label;
}
} // namespace Fooyin::Stats
