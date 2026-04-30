/*****************************************************************************
 * thumbnail_provider.hpp: hover-thumbnail provider with two backends.
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * Licensed under GPLv2+ matching the rest of the Qt UI module.
 *
 * Two backends are supported, picked at runtime:
 *
 *   1. mpv (preferred, ~5–20 ms per thumbnail):
 *      A single persistent `mpv --idle=yes` subprocess is spawned per file
 *      load. We talk to it over a Unix socket via JSON-RPC. Each request
 *      is just `seek` + `screenshot-to-file` — no process spawn, no file
 *      open, no decoder init per request. This matches Haruna's pattern
 *      (see project memory `project_3x_backport_state.md`).
 *
 *   2. ffmpegthumbnailer (fallback, ~130 ms per thumbnail):
 *      Spawn-per-request. Slower because each request pays the full
 *      process+open+decode cost. Used when /usr/bin/mpv isn't installed.
 *
 * The user-facing scheduling layer (latest-wins pending slot, direction-
 * aware prefetch, evenly-spaced warmup queue) is identical for both
 * backends — only the actual generation differs.
 *****************************************************************************/

#ifndef VLC_QT_THUMBNAIL_PROVIDER_HPP_
#define VLC_QT_THUMBNAIL_PROVIDER_HPP_

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "qt.hpp"

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>

class QProcess;
class QLocalSocket;
class QTimer;

class ThumbnailProvider : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailProvider(intf_thread_t *p_intf, QObject *parent = nullptr);
    ~ThumbnailProvider() override;

    /* Ask for a thumbnail at timeMs (already bucketed by the caller).
     * Fires thumbnailReady asynchronously when available. Cache hits
     * fire synchronously via queued connection. Re-requests for the same
     * bucket while one is in flight are coalesced. */
    void requestThumbnail(qint64 timeMs);

    /* Generation bumps each time the playing media changes. Callers can
     * compare a captured generation against this to drop late results
     * after a media change. */
    int generation() const { return m_generation; }

public slots:
    /* Hook this to MainInputManager::inputChanged. Pulls the current
     * input's URI, bumps generation, drops cache + in-flight request. */
    void onInputChanged();

    /* Drop all cached thumbnails (e.g. when size/quality settings change
     * via the context menu — the cached images are at the old size). */
    void clearCache();

    /* Called by SeekSlider when the input's duration becomes known.
     * Schedules N evenly-spaced thumbnails for background generation
     * so most hover positions are already cached when the user starts
     * scrubbing. Idempotent — safe to call repeatedly with the same
     * length. */
    void seedWarmup(qint64 lengthMs);

signals:
    void thumbnailReady(qint64 timeMs, const QImage &img);

private slots:
    void onProcessFinished(int exitCode);
    void onInputChangeDebounced();
    /* mpv backend slots */
    void onMpvSocketConnected();
    void onMpvSocketReadyRead();
    void onMpvSocketDisconnected();
    void onMpvProcessFinished(int exitCode);
    void onMpvConnectRetry();

private:
    enum Backend { BACKEND_NONE, BACKEND_MPV, BACKEND_FFMPEGTHUMBNAILER };

    void startNextRequest();
    void killInFlight();
    void trimCache();
    void prefetchAhead();    /* speculative: fill the next bucket in the
                              * cursor's direction during idle time */
    void launchWarmupWorker(); /* parallel ffmpegthumbnailer worker */
    void killWarmupWorkers();
    QStringList buildArgs(qint64 timeMs) const;  /* ffmpegthumbnailer args */

    /* mpv backend lifecycle */
    Backend detectBackend() const;
    void mpvStart(const QString &filePath);
    void mpvStop();
    void mpvLoadFile(const QString &filePath);    /* hot-swap file in
                                                   * existing mpv via IPC */
    void mpvSendCommand(const QByteArray &jsonCmd);
    void mpvProcessLine(const QByteArray &line);
    void mpvIssueRequest(qint64 timeMs);
    void mpvHandleScreenshotResult(qint64 timeMs, bool success);

    intf_thread_t *m_intf;
    QString m_currentPath;          /* local filesystem path of playing media; empty if not local */
    int m_generation = 0;

    QProcess *m_proc = nullptr;
    qint64 m_inFlightTimeMs = -1;   /* -1 when idle */
    qint64 m_pendingMs = -1;        /* one-slot LIFO: only the most recent
                                     * hover request is queued. New requests
                                     * always overwrite the slot, so when
                                     * the in-flight finishes we serve the
                                     * user's CURRENT hover, not a stale one. */
    bool m_inFlightIsPrefetch = false;
    qint64 m_lastUserMs = -1;       /* most-recent user-driven hover */
    qint64 m_lastDeltaMs = 0;       /* signed step between last two user hovers */
    int m_prefetchCount = 0;        /* how many speculative buckets we've
                                     * fetched without a user move in between */
    qint64 m_warmupLengthMs = -1;   /* duration we've already seeded for; -1 = none yet */
    QQueue<qint64> m_warmupQueue;   /* evenly-spaced positions to fill at idle */
    QList<QProcess*> m_warmupProcs; /* parallel warmup workers (ffmpegthumbnailer
                                     * backend only — mpv is intrinsically serial) */
    QHash<qint64, QImage> m_cache;  /* timeMs (bucketed) → JPEG-decoded image */

    /* Backend selection. Set once in the constructor by detectBackend(). */
    Backend m_backend = BACKEND_NONE;

    /* inputChanged debouncer. VLC's MainInputManager fires inputChanged
     * repeatedly during input setup + auto-advance, with different paths
     * each time. We collapse rapid bursts into one settled-state event. */
    QTimer *m_inputDebouncer = nullptr;
    QString m_debouncedNewPath;

    /* mpv backend state */
    QProcess *m_mpv = nullptr;
    QLocalSocket *m_mpvSocket = nullptr;
    QTimer *m_mpvConnectTimer = nullptr;
    int m_mpvConnectAttempts = 0;
    bool m_mpvReady = false;        /* socket connected + first file loaded */
    bool m_mpvFileLoaded = false;
    QByteArray m_mpvRecvBuf;        /* accumulated socket bytes; commands are
                                     * newline-delimited JSON */
    QString m_mpvRunDir;            /* /run/user/<uid> or /tmp */
    int m_mpvSpawnSerial = 0;       /* increments each mpvStart so paths are unique */
    QString m_mpvSocketPath;        /* current spawn's socket */
    QString m_mpvScreenshotPath;    /* base path for screenshots; per-request paths
                                     * append the request-id to avoid races */
    int m_mpvNextRequestId = 1;
    int m_mpvScreenshotReqId = -1;  /* request_id of the screenshot we're
                                     * waiting on; -1 = idle */
    QString m_mpvScreenshotInFlightPath; /* the unique-per-request output
                                          * path the in-flight screenshot
                                          * is being written to */

    static const int CACHE_MAX = 200;
    static const int THUMB_WIDTH = 240;  /* ffmpeg scales to this; height is auto */
    static const int PREFETCH_MAX = 4;   /* up to N speculative buckets ahead
                                          * during one idle stretch */
    static const int WARMUP_BUCKETS = 60; /* evenly-spaced thumbnails generated
                                           * in background when a file loads */
    static const int WARMUP_PARALLEL = 2; /* concurrent ffmpegthumbnailer
                                           * processes during warmup. */
};

#endif /* VLC_QT_THUMBNAIL_PROVIDER_HPP_ */
