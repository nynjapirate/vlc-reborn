/*****************************************************************************
 * views.hpp : Icon view for the Playlist
 ****************************************************************************
 * Copyright © 2010 the VideoLAN team
 * $Id$
 *
 * Authors:         Jean-Baptiste Kempf <jb@videolan.org>
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
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef VLC_QT_VIEWS_HPP_
#define VLC_QT_VIEWS_HPP_

#include <QStyledItemDelegate>
#include <QListView>
#include <QTreeView>
#include <QAbstractItemView>
#include "util/pictureflow.hpp"

class QPainter;

class AbstractPlViewItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    AbstractPlViewItemDelegate( QWidget * parent = 0 ) : QStyledItemDelegate(parent) {}
    void paintBackground( QPainter *, const QStyleOptionViewItem &, const QModelIndex & ) const;
};

class PlIconViewItemDelegate : public AbstractPlViewItemDelegate
{
    Q_OBJECT

public:
    PlIconViewItemDelegate(QWidget *parent = 0) : AbstractPlViewItemDelegate( parent ) {}
    void paint ( QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index ) const Q_DECL_OVERRIDE;
    QSize sizeHint ( const QStyleOptionViewItem & option = QStyleOptionViewItem(),
                     const QModelIndex & index = QModelIndex() ) const Q_DECL_OVERRIDE;
};

class PlListViewItemDelegate : public AbstractPlViewItemDelegate
{
    Q_OBJECT

public:
    PlListViewItemDelegate(QWidget *parent = 0) : AbstractPlViewItemDelegate(parent) {}

    void paint ( QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index ) const Q_DECL_OVERRIDE;
    QSize sizeHint ( const QStyleOptionViewItem & option, const QModelIndex & index ) const Q_DECL_OVERRIDE;
};

class PlTreeViewItemDelegate : public AbstractPlViewItemDelegate
{
    Q_OBJECT

public:
    PlTreeViewItemDelegate(QWidget *parent = 0) : AbstractPlViewItemDelegate(parent) {}

    void paint ( QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index ) const Q_DECL_OVERRIDE;
};

class CellPixmapDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    CellPixmapDelegate(QWidget *parent = 0) : QStyledItemDelegate(parent) {}
    void paint ( QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index ) const Q_DECL_OVERRIDE;
};

/* Used only by the THUMBNAIL_VIEW (a QTreeView identical to the
 * Detailed List except the title column is augmented with a per-row
 * video thumbnail). The delegate paints a thumbnail to the left of the
 * title text when one is cached for the row's URI; otherwise it paints
 * a placeholder and triggers an async generation via
 * PlaylistThumbnailManager. Row height is clamped at the thumbnail
 * height; the title text wraps inside the available text band and any
 * overflow is clipped — this keeps rows uniform-tall regardless of
 * title length. */
class PlThumbViewItemDelegate : public PlTreeViewItemDelegate
{
    Q_OBJECT

public:
    PlThumbViewItemDelegate( QAbstractItemView *view, intf_thread_t *intf );
    void paint ( QPainter *, const QStyleOptionViewItem &, const QModelIndex & ) const Q_DECL_OVERRIDE;
    QSize sizeHint( const QStyleOptionViewItem &, const QModelIndex & ) const Q_DECL_OVERRIDE;

private slots:
    void onThumbnailReady( const QString &uri );

private:
    int currentWidth() const;          /* var_InheritInteger qt-pl-thumb-width, clamped */

    QAbstractItemView *m_view;         /* for invalidating row geometry on ready */
    intf_thread_t *m_intf;
};

/* Wrap-with-cap text delegate for non-title columns in the Thumbnail
 * List view. Wraps text inside the column's current width (so a
 * long path-style Location string flows onto multiple lines instead
 * of forcing the column wide), but clips the row height at the
 * thumbnail height so a very long string doesn't expand the whole
 * row beyond the thumbnail. */
class PlThumbCappedTextDelegate : public PlTreeViewItemDelegate
{
    Q_OBJECT

public:
    PlThumbCappedTextDelegate( QWidget *parent, intf_thread_t *intf );
    void paint ( QPainter *, const QStyleOptionViewItem &, const QModelIndex & ) const Q_DECL_OVERRIDE;
    QSize sizeHint( const QStyleOptionViewItem &, const QModelIndex & ) const Q_DECL_OVERRIDE;

private:
    int rowCapHeight() const;
    intf_thread_t *m_intf;
};

class PlIconView : public QListView
{
    Q_OBJECT

public:
    PlIconView( QAbstractItemModel *model, QWidget *parent = 0 );
protected:
    void startDrag ( Qt::DropActions supportedActions ) Q_DECL_OVERRIDE;
    void dragMoveEvent ( QDragMoveEvent * event ) Q_DECL_OVERRIDE;
    bool viewportEvent ( QEvent * ) Q_DECL_OVERRIDE;
};

class PlListView : public QListView
{
    Q_OBJECT

public:
    PlListView( QAbstractItemModel *model, QWidget *parent = 0 );
protected:
    void startDrag ( Qt::DropActions supportedActions ) Q_DECL_OVERRIDE;
    void dragMoveEvent ( QDragMoveEvent * event ) Q_DECL_OVERRIDE;
    void keyPressEvent( QKeyEvent *event ) Q_DECL_OVERRIDE;
    bool viewportEvent ( QEvent * ) Q_DECL_OVERRIDE;
};

class PlTreeView : public QTreeView
{
    Q_OBJECT

public:
    PlTreeView( QAbstractItemModel *, QWidget *parent = 0 );
protected:
    void startDrag ( Qt::DropActions supportedActions ) Q_DECL_OVERRIDE;
    void dragMoveEvent ( QDragMoveEvent * event ) Q_DECL_OVERRIDE;
    void keyPressEvent( QKeyEvent *event ) Q_DECL_OVERRIDE;
    void setModel( QAbstractItemModel * ) Q_DECL_OVERRIDE;
};

class PicFlowView : public QAbstractItemView
{
    Q_OBJECT
public:
    PicFlowView( QAbstractItemModel *model, QWidget *parent = 0 );

    QRect visualRect(const QModelIndex&) const Q_DECL_OVERRIDE;
    void scrollTo(const QModelIndex&, QAbstractItemView::ScrollHint) Q_DECL_OVERRIDE;
    QModelIndex indexAt(const QPoint&) const Q_DECL_OVERRIDE;
    void setModel(QAbstractItemModel *model) Q_DECL_OVERRIDE;

protected:
    int horizontalOffset() const Q_DECL_OVERRIDE;
    int verticalOffset() const Q_DECL_OVERRIDE;
    QModelIndex moveCursor(QAbstractItemView::CursorAction, Qt::KeyboardModifiers) Q_DECL_OVERRIDE;
    bool isIndexHidden(const QModelIndex&) const Q_DECL_OVERRIDE;
    QRegion visualRegionForSelection(const QItemSelection&) const Q_DECL_OVERRIDE;
    void setSelection(const QRect&, QFlags<QItemSelectionModel::SelectionFlag>) Q_DECL_OVERRIDE;
    bool viewportEvent ( QEvent * ) Q_DECL_OVERRIDE;

private:
    PictureFlow *picFlow;

public slots:
    void dataChanged( const QModelIndex &, const QModelIndex &);
private slots:
    void playItem( int );
};

#endif
