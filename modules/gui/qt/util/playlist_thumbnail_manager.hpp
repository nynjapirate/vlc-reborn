/*****************************************************************************
 * playlist_thumbnail_manager.hpp: per-URI thumbnail manager for the
 * playlist's "Thumbnail List" view.
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * Licensed under GPLv2+ matching the rest of the Qt UI module.
 *
 * Architecture:
 *   - Producer (delegate) calls requestAsync(uri); manager hits the
 *     in-memory cache or pushes the uri onto a LIFO pending stack.
 *   - N worker threads each host one PlaylistThumbnailWorker; the
 *     manager dispatches one uri to whichever worker is idle.
 *   - LIFO ordering means the most-recently-requested rows (typically
 *     what the user just scrolled into view) get worked on first;
 *     older requests for now-offscreen rows naturally bubble down.
 *   - Each task carries the manager's generation counter at dispatch
 *     time. When the user changes the configured width, generation
 *     bumps and any results still in flight from the previous width
 *     are dropped on arrival without poisoning the cache.
 *
 * One global instance is shared across all playlist views via
 * instance() — the cache survives view switches and root changes.
 *****************************************************************************/

#ifndef VLC_QT_PLAYLIST_THUMBNAIL_MANAGER_HPP_
#define VLC_QT_PLAYLIST_THUMBNAIL_MANAGER_HPP_

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class QThread;

class PlaylistThumbnailWorker : public QObject
{
    Q_OBJECT
public:
    PlaylistThumbnailWorker();
    ~PlaylistThumbnailWorker() override;

public slots:
    /* Open the file at `path`, hit the XDG thumbnail cache or decode
     * one frame via libav, scale to `width`, emit thumbnailReady,
     * close. All slot calls run on this worker's thread. The `gen`
     * argument is echoed back unchanged so the manager can detect
     * stale results. */
    void generate(QString uri, QString path, int width, int gen);

signals:
    void thumbnailReady(QString uri, const QImage &img, int gen);
};

class PlaylistThumbnailManager : public QObject
{
    Q_OBJECT
public:
    static PlaylistThumbnailManager *instance();

    QImage cachedFor(const QString &uri) const;
    QImage requestAsync(const QString &uri, int width);
    void clearCache();

signals:
    /* Emitted after each successful generation. The delegate listens
     * and triggers a repaint of the row whose URI matches. */
    void thumbnailReady(const QString &uri);

private slots:
    void onWorkerDone(QString uri, const QImage &img, int gen);

private:
    PlaylistThumbnailManager();
    ~PlaylistThumbnailManager() override;

    void pumpQueue();

    struct WorkerSlot
    {
        QThread *thread;
        PlaylistThumbnailWorker *worker;
        bool busy;
    };

    QHash<QString, QImage> m_cache;     /* key: uri */
    QSet<QString> m_inFlight;
    QList<QString> m_pending;           /* LIFO stack — takeLast */
    QSet<QString> m_pendingSet;
    int m_curWidth = 0;
    int m_generation = 0;

    QVector<WorkerSlot> m_workers;

    static const int N_WORKERS = 4;
    static const int CACHE_MAX = 2000;
};

#endif /* VLC_QT_PLAYLIST_THUMBNAIL_MANAGER_HPP_ */
