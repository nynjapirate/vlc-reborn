/*****************************************************************************
 * thumbnail_provider.hpp: hover-thumbnail provider, libav-direct backend.
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * Licensed under GPLv2+ matching the rest of the Qt UI module.
 *
 * Architecture:
 *
 *   ThumbnailProvider  (UI thread)             LibavWorker  (worker thread)
 *   ─────────────────────────────              ────────────────────────────
 *     scheduling layer:                          libav state:
 *       latest-wins pending slot                   AVFormatContext (one open file)
 *       direction-aware prefetch                   AVCodecContext  (video stream)
 *       60-bucket warmup queue                     SwsContext      (pix_fmt → RGB)
 *       per-bucket cache                           AVPacket / AVFrame scratch
 *       generation counter                       slots:
 *       inputChanged debouncer                     openFile, closeFile,
 *                                                  seekAndDecode
 *
 *     ───── queued signals/slots ─────────►
 *
 *   Producer (ThumbnailProvider) coalesces hover bursts in m_pendingMs
 *   and only emits one seek-and-decode at a time. Worker runs single-
 *   consumer on its own event loop. Generation counter on every signal
 *   lets the producer drop frames decoded from a file the user has
 *   already moved past.
 *
 *   Why not subprocess: the previous mpv-subprocess backend round-tripped
 *   JSON-RPC + JPEG file I/O per request (~30-80 ms in practice, plus a
 *   ~50-200 ms loadfile gap on every file change). Direct libav calls
 *   stay in-process: one avformat_open_input per file, then seek+decode
 *   per request lands ~5-15 ms with no IPC race window.
 *****************************************************************************/

#ifndef VLC_QT_THUMBNAIL_PROVIDER_HPP_
#define VLC_QT_THUMBNAIL_PROVIDER_HPP_

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "qt.hpp"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QQueue>
#include <QString>

class QThread;
class QTimer;

/* Forward-declare libav opaque types so this header doesn't drag the
 * libav C headers into every includer. The .cpp includes them for real. */
struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;

class LibavWorker : public QObject
{
    Q_OBJECT
public:
    explicit LibavWorker(intf_thread_t *p_intf);
    ~LibavWorker() override;

public slots:
    /* All slots run on the worker thread (Qt's queued connections
     * marshal args across the thread boundary). The producer never
     * touches LibavWorker's data members directly. */
    void openFile(QString path, int generation);
    void closeFile();
    void seekAndDecode(qint64 timeMs, int width, int generation);

signals:
    void frameReady(qint64 timeMs, const QImage &img, int generation);
    void openFailed(int generation);

private:
    void closeFileImpl();

    intf_thread_t *m_intf;
    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    int m_streamIdx = -1;
    /* AVRational stored as raw num/den so we don't have to expose
     * <libavutil/rational.h> in this header. */
    int m_tbNum = 1;
    int m_tbDen = 1;
    AVPacket *m_pkt = nullptr;
    AVFrame *m_frame = nullptr;
    SwsContext *m_sws = nullptr;
    int m_swsW = 0, m_swsH = 0, m_swsSrcW = 0, m_swsSrcH = 0;
    /* AVPixelFormat is an enum; -1 (AV_PIX_FMT_NONE) means "unset".
     * Stored as int to avoid the enum decl in this header. */
    int m_swsSrcFmt = -1;
    int m_genOpen = -1;
};

class ThumbnailProvider : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailProvider(intf_thread_t *p_intf, QObject *parent = nullptr);
    ~ThumbnailProvider() override;

    /* Ask for a thumbnail at timeMs (already bucketed by the caller).
     * Fires thumbnailReady asynchronously when available. Cache hits
     * fire synchronously via queued connection. Re-requests for the
     * same bucket while one is in flight are coalesced. */
    void requestThumbnail(qint64 timeMs);

    /* Generation bumps each time the playing media changes. Callers can
     * compare a captured generation against this to drop late results
     * after a media change. */
    int generation() const { return m_generation; }

public slots:
    /* Hook this to MainInputManager::inputChanged. Pulls the current
     * input's URI, bumps generation, drops cache + in-flight request. */
    void onInputChanged();

    /* Drop all cached thumbnails (e.g. when size/quality settings
     * change via the context menu — the cached images are at the old
     * size). */
    void clearCache();

    /* Called by SeekSlider when the input's duration becomes known.
     * Schedules N evenly-spaced thumbnails for background generation
     * so most hover positions are already cached when the user starts
     * scrubbing. Idempotent — safe to call repeatedly with the same
     * length. */
    void seedWarmup(qint64 lengthMs);

signals:
    void thumbnailReady(qint64 timeMs, const QImage &img);

    /* Outbound to LibavWorker (queued by Qt across threads). */
    void wantOpen(QString path, int generation);
    void wantClose();
    void wantSeekAndDecode(qint64 timeMs, int width, int generation);

private slots:
    void onInputChangeDebounced();
    void onFrameReady(qint64 timeMs, const QImage &img, int generation);
    void onOpenFailed(int generation);

private:
    void startNextRequest();
    void trimCache();
    void prefetchAhead();      /* speculative: fill the next bucket in the
                                * cursor's direction during idle time */

    intf_thread_t *m_intf;
    QString m_currentPath;          /* local filesystem path of playing media; empty if not local */
    int m_generation = 0;

    qint64 m_inFlightTimeMs = -1;   /* -1 when worker is idle */
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
    QHash<qint64, QImage> m_cache;  /* timeMs (bucketed) → decoded thumbnail */
    bool m_workerOpen = false;      /* worker has an open AVFormatContext for m_currentPath */

    /* inputChanged debouncer. VLC's MainInputManager fires inputChanged
     * repeatedly during input setup + auto-advance, with different
     * paths each time. We collapse rapid bursts into one settled-state
     * event. */
    QTimer *m_inputDebouncer = nullptr;
    QString m_debouncedNewPath;

    /* Worker thread. m_thread is parented to this; m_worker has no
     * QObject parent (Qt requires that for moveToThread) and is
     * deleted via deleteLater on m_thread::finished. */
    QThread *m_thread = nullptr;
    LibavWorker *m_worker = nullptr;

    static const int CACHE_MAX = 200;
    static const int PREFETCH_MAX = 4;   /* up to N speculative buckets ahead
                                          * during one idle stretch */
    static const int WARMUP_BUCKETS = 60; /* evenly-spaced thumbnails generated
                                           * in background when a file loads */
};

#endif /* VLC_QT_THUMBNAIL_PROVIDER_HPP_ */
