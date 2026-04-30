/*****************************************************************************
 * Copyright © 2011 VideoLAN
 * $Id$
 *
 * Authors: Ludovic Fauvet <etix@l0cal.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef TIMETOOLTIP_H
#define TIMETOOLTIP_H

#include "qt.hpp"

#include <QImage>
#include <QWidget>
#include <QPainterPath>

class TimeTooltip : public QWidget
{
    Q_OBJECT
public:
    explicit TimeTooltip( QWidget *parent = 0 );
    /* Atomic state update. Target, time, text, and thumbnail are all
     * applied together in a single adjustPosition() pass — there is no
     * intermediate state where, e.g., the widget has resized for a new
     * thumbnail but the target position hasn't yet been re-derived.
     * That intermediate state is what produced the "phantom thumbnail
     * lower-right of cursor" artifact on fast scrubs. */
    void setTip( const QPoint& pos, const QString& time, const QString& text,
                 const QImage& thumbnail );
    /* Convenience overloads — both funnel through the 4-arg setTip with
     * the current cached thumbnail/target/time/text, so every caller
     * lands in the same single state-mutation path. */
    void setTip( const QPoint& pos, const QString& time, const QString& text );
    void setThumbnail( const QImage& img );
    void clearThumbnail();
    virtual void show();

protected:
    void paintEvent( QPaintEvent * ) Q_DECL_OVERRIDE;

private:
    void applyState( const QPoint& target, const QString& time,
                     const QString& text, const QImage& thumbnail );
    void adjustPosition();
    void buildPath();
    QPoint mTarget;
    QString mTime;
    QString mText;
    QString mDisplayedText;
    QFont mFont;
    QRect mBox;            /* text-area box (excludes thumbnail) */
    QRect mThumbBox;       /* thumbnail box (zero-size when no thumbnail) */
    QImage mThumbnail;
    QPainterPath mPainterPath;
    int mTipX;
};

#endif // TIMETOOLTIP_H
