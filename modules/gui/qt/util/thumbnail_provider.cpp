/*****************************************************************************
 * thumbnail_provider.cpp: libav-direct hover-thumbnail provider.
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * Licensed under GPLv2+ matching the rest of the Qt UI module.
 *****************************************************************************/

/* QThread MUST be included before any VLC header. vlc_threads.h defines
 * `msleep` as a function-like macro that mangles QThread's static
 * msleep(unsigned long) member declaration when the Qt header is parsed
 * after the VLC one. By pulling Qt in first, QThread is fully parsed
 * before the macro gets defined. */
#include <QThread>
#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>

#include "thumbnail_provider.hpp"
#include "input_manager.hpp"

#include <vlc_input.h>
#include <vlc_url.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

/*****************************************************************************
 * LibavWorker — runs on a dedicated QThread. All libav state lives here.
 *
 * The producer (ThumbnailProvider) only ever talks to it via queued
 * signals/slots, so cross-thread safety reduces to "Qt's queued connection
 * marshals the args"; the worker's own data members are only ever touched
 * on the worker thread.
 *****************************************************************************/

LibavWorker::LibavWorker(intf_thread_t *p_intf)
    : m_intf(p_intf)
{
    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
}

LibavWorker::~LibavWorker()
{
    closeFileImpl();
    if (m_pkt)   av_packet_free(&m_pkt);
    if (m_frame) av_frame_free(&m_frame);
}

void LibavWorker::closeFileImpl()
{
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    m_swsW = m_swsH = m_swsSrcW = m_swsSrcH = 0;
    m_swsSrcFmt = AV_PIX_FMT_NONE;
    if (m_codec) avcodec_free_context(&m_codec);
    if (m_fmt)   avformat_close_input(&m_fmt);
    m_streamIdx = -1;
    m_tbNum = 1;
    m_tbDen = 1;
    m_genOpen = -1;
}

void LibavWorker::openFile(QString path, int generation)
{
    closeFileImpl();

    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(),
                            nullptr, nullptr) < 0)
    {
        msg_Dbg(m_intf, "[hover-thumb] avformat_open_input failed: %s",
                path.toUtf8().constData());
        emit openFailed(generation);
        return;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0)
    {
        avformat_close_input(&fmt);
        msg_Dbg(m_intf, "[hover-thumb] avformat_find_stream_info failed");
        emit openFailed(generation);
        return;
    }

    int videoIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
    {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoIdx < 0)
    {
        avformat_close_input(&fmt);
        msg_Dbg(m_intf, "[hover-thumb] no video stream");
        emit openFailed(generation);
        return;
    }

    AVStream *st = fmt->streams[videoIdx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
    {
        avformat_close_input(&fmt);
        msg_Dbg(m_intf, "[hover-thumb] no decoder for codec_id=%d",
                static_cast<int>(st->codecpar->codec_id));
        emit openFailed(generation);
        return;
    }

    AVCodecContext *cctx = avcodec_alloc_context3(codec);
    if (!cctx)
    {
        avformat_close_input(&fmt);
        emit openFailed(generation);
        return;
    }
    if (avcodec_parameters_to_context(cctx, st->codecpar) < 0)
    {
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt);
        emit openFailed(generation);
        return;
    }

    /* Decode-only: no display, no hwaccel, no audio. Single thread keeps
     * memory low; thumbnails don't need parallelism. AVDISCARD_NONREF
     * skips B-frame decoding when possible — small accuracy hit at
     * non-keyframe seek targets, big speed win. */
    cctx->thread_count = 1;
    cctx->skip_loop_filter = AVDISCARD_NONREF;

    if (avcodec_open2(cctx, codec, nullptr) < 0)
    {
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt);
        msg_Dbg(m_intf, "[hover-thumb] avcodec_open2 failed");
        emit openFailed(generation);
        return;
    }

    m_fmt = fmt;
    m_codec = cctx;
    m_streamIdx = videoIdx;
    m_tbNum = st->time_base.num;
    m_tbDen = st->time_base.den;
    m_genOpen = generation;
}

void LibavWorker::closeFile()
{
    closeFileImpl();
}

void LibavWorker::seekAndDecode(qint64 timeMs, int width, int generation)
{
    /* Drop stale work: the file we have open belongs to a previous
     * generation (the user moved on before the queued openFile
     * arrived). The producer side will requeue this when the new
     * openFile lands. */
    if (m_genOpen != generation || !m_fmt || !m_codec)
    {
        emit frameReady(timeMs, QImage(), generation);
        return;
    }

    /* Convert ms → stream-timebase pts. */
    AVRational tb;
    tb.num = m_tbNum;
    tb.den = m_tbDen;
    AVRational msBase;
    msBase.num = 1;
    msBase.den = 1000;
    const int64_t targetPts = av_rescale_q(timeMs, msBase, tb);

    /* Seek to the nearest keyframe at-or-before target, then decode
     * forward until we land on/after target. SEEK_BACKWARD is the
     * libav idiom for "first keyframe ≤ target". */
    if (av_seek_frame(m_fmt, m_streamIdx, targetPts,
                      AVSEEK_FLAG_BACKWARD) < 0)
    {
        emit frameReady(timeMs, QImage(), generation);
        return;
    }
    avcodec_flush_buffers(m_codec);

    bool got = false;
    bool eof = false;
    int loops = 0;
    while (!got && loops++ < 400)
    {
        if (!eof)
        {
            int rc = av_read_frame(m_fmt, m_pkt);
            if (rc < 0)
            {
                eof = true;
                avcodec_send_packet(m_codec, nullptr); /* flush */
            }
            else if (m_pkt->stream_index != m_streamIdx)
            {
                av_packet_unref(m_pkt);
                continue;
            }
            else
            {
                avcodec_send_packet(m_codec, m_pkt);
                av_packet_unref(m_pkt);
            }
        }

        while (true)
        {
            int rc = avcodec_receive_frame(m_codec, m_frame);
            if (rc == AVERROR(EAGAIN)) break;
            if (rc < 0) { eof = true; goto decode_done; }

            int64_t pts = m_frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = m_frame->pts;
            if (pts == AV_NOPTS_VALUE || pts >= targetPts)
            {
                got = true;
                break;
            }
            av_frame_unref(m_frame);
        }
    }
decode_done:
    if (!got)
    {
        av_frame_unref(m_frame);
        emit frameReady(timeMs, QImage(), generation);
        return;
    }

    const int srcW = m_codec->width;
    const int srcH = m_codec->height;
    if (srcW <= 0 || srcH <= 0)
    {
        av_frame_unref(m_frame);
        emit frameReady(timeMs, QImage(), generation);
        return;
    }

    int outW = qBound(80, width, 480);
    int outH = static_cast<int>(static_cast<qint64>(outW) * srcH / srcW);
    if (outH < 1) outH = 1;

    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(m_frame->format);
    if (srcFmt == AV_PIX_FMT_NONE)
        srcFmt = m_codec->pix_fmt;

    if (!m_sws || m_swsW != outW || m_swsH != outH
        || m_swsSrcW != srcW || m_swsSrcH != srcH
        || m_swsSrcFmt != static_cast<int>(srcFmt))
    {
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
        m_sws = sws_getContext(srcW, srcH, srcFmt,
                               outW, outH, AV_PIX_FMT_RGB24,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        m_swsW = outW;     m_swsH = outH;
        m_swsSrcW = srcW;  m_swsSrcH = srcH;
        m_swsSrcFmt = static_cast<int>(srcFmt);
    }
    if (!m_sws)
    {
        av_frame_unref(m_frame);
        emit frameReady(timeMs, QImage(), generation);
        return;
    }

    QImage img(outW, outH, QImage::Format_RGB888);
    uint8_t *dst[1] = { img.bits() };
    int dstStride[1] = { static_cast<int>(img.bytesPerLine()) };
    sws_scale(m_sws, m_frame->data, m_frame->linesize, 0,
              srcH, dst, dstStride);

    av_frame_unref(m_frame);
    emit frameReady(timeMs, img, generation);
}

/*****************************************************************************
 * ThumbnailProvider — UI-thread side. Scheduling layer + worker glue.
 *****************************************************************************/

ThumbnailProvider::ThumbnailProvider(intf_thread_t *p_intf, QObject *parent)
    : QObject(parent)
    , m_intf(p_intf)
{
    /* Worker thread. Worker QObject has no parent — Qt requires that
     * before moveToThread. The QThread is parented here so it shares
     * our lifetime; we explicitly quit+wait in the dtor. */
    m_thread = new QThread(this);
    m_worker = new LibavWorker(m_intf);
    m_worker->moveToThread(m_thread);

    /* Worker is destroyed on the worker thread when the thread exits,
     * so its libav state is freed in the same thread it was created. */
    connect(m_thread, &QThread::finished,
            m_worker, &QObject::deleteLater);

    /* Producer → worker. Default queued connection across thread
     * boundaries means each emit becomes a single event in the
     * worker's queue; the worker processes them serially. */
    connect(this, SIGNAL(wantOpen(QString,int)),
            m_worker, SLOT(openFile(QString,int)));
    connect(this, SIGNAL(wantClose()),
            m_worker, SLOT(closeFile()));
    connect(this, SIGNAL(wantSeekAndDecode(qint64,int,int)),
            m_worker, SLOT(seekAndDecode(qint64,int,int)));

    /* Worker → producer. */
    connect(m_worker, SIGNAL(frameReady(qint64,QImage,int)),
            this, SLOT(onFrameReady(qint64,QImage,int)));
    connect(m_worker, SIGNAL(openFailed(int)),
            this, SLOT(onOpenFailed(int)));

    m_thread->start();
    msg_Dbg(m_intf, "[hover-thumb] backend=libav-direct, worker thread started");

    /* Pick up the currently loaded media if any. */
    onInputChanged();
}

ThumbnailProvider::~ThumbnailProvider()
{
    if (m_thread)
    {
        m_thread->quit();
        m_thread->wait();
    }
    /* m_worker is deleted via deleteLater on thread::finished. */
}

void ThumbnailProvider::onInputChanged()
{
    /* MainInputManager fires inputChanged many times during file setup
     * and again on every auto-advance. We debounce: don't actually
     * issue an openFile to the worker until the input has been stable
     * for 800 ms. Each fresh inputChanged restarts the timer. */
    QString newPath;
    input_thread_t *p_input = MainInputManager::getInstance(m_intf)->getInput();
    if (p_input)
    {
        input_item_t *item = input_GetItem(p_input);
        if (item)
        {
            char *uri = input_item_GetURI(item);
            if (uri)
            {
                char *path = vlc_uri2path(uri);
                if (path)
                {
                    newPath = QString::fromUtf8(path);
                    free(path);
                }
                free(uri);
            }
        }
    }

    /* Same path AND worker still has it open: no work to do. Common case. */
    if (!newPath.isEmpty() && newPath == m_currentPath && m_workerOpen)
    {
        if (m_inputDebouncer) m_inputDebouncer->stop();
        return;
    }

    m_debouncedNewPath = newPath;
    if (!m_inputDebouncer)
    {
        m_inputDebouncer = new QTimer(this);
        m_inputDebouncer->setSingleShot(true);
        connect(m_inputDebouncer, SIGNAL(timeout()),
                this, SLOT(onInputChangeDebounced()));
    }
    m_inputDebouncer->start(800);
}

void ThumbnailProvider::onInputChangeDebounced()
{
    QString newPath = m_debouncedNewPath;

    /* Re-check dedupe — m_currentPath may have caught up via a
     * transient path bouncing through. */
    if (!newPath.isEmpty() && newPath == m_currentPath && m_workerOpen)
        return;

    /* Per-file reset. */
    m_pendingMs = -1;
    m_inFlightTimeMs = -1;
    m_inFlightIsPrefetch = false;
    m_lastUserMs = -1;
    m_lastDeltaMs = 0;
    m_prefetchCount = 0;
    m_warmupLengthMs = -1;
    m_warmupQueue.clear();
    m_cache.clear();
    m_currentPath = newPath;
    m_generation++;
    /* The worker still has the previous file open at this point. The
     * queued wantOpen below will replace it; the producer treats the
     * worker as "not open" until the next request actually fires. We
     * optimistically flip m_workerOpen=true after emitting wantOpen
     * — if the worker reports openFailed, onOpenFailed will flip it
     * back. The window where we issue a seekAndDecode for a not-yet-
     * actually-open file is fine: that signal is queued AFTER the
     * openFile signal, so the worker processes them in order. */
    if (!m_currentPath.isEmpty())
    {
        emit wantOpen(m_currentPath, m_generation);
        m_workerOpen = true;
    }
    else
    {
        emit wantClose();
        m_workerOpen = false;
    }
}

void ThumbnailProvider::requestThumbnail(qint64 timeMs)
{
    if (m_currentPath.isEmpty() || timeMs < 0)
        return;

    /* User-toggleable. Tools → Preferences → Show All → Interface →
     * Main interfaces → Qt → "Show preview thumbnails when hovering
     * the seek bar". Default on; off-switch is honored on each
     * request. */
    if (!var_InheritBool(m_intf, "qt-hover-thumbnails"))
        return;

    /* Cache hit — fire immediately (queued so callers don't see a
     * synchronous emit during their own slot). */
    auto it = m_cache.constFind(timeMs);
    if (it != m_cache.constEnd())
    {
        QImage img = it.value();
        QMetaObject::invokeMethod(this, [this, timeMs, img]() {
            emit thumbnailReady(timeMs, img);
        }, Qt::QueuedConnection);
        return;
    }

    /* Track direction + step from the user's previous hover so we can
     * prefetch speculatively in the cursor's apparent direction during
     * idle time. Reset the prefetch-count budget on every user move. */
    if (m_lastUserMs >= 0 && timeMs != m_lastUserMs)
        m_lastDeltaMs = timeMs - m_lastUserMs;
    m_lastUserMs = timeMs;
    m_prefetchCount = 0;

    /* Already being generated — coalesce. */
    if (m_inFlightTimeMs == timeMs)
        return;

    /* Latest-wins: overwrite whatever was pending. If the user moved
     * past several buckets quickly, only the most recent one matters. */
    m_pendingMs = timeMs;
    if (m_inFlightTimeMs < 0)
        startNextRequest();
}

void ThumbnailProvider::startNextRequest()
{
    if (m_pendingMs < 0 || m_currentPath.isEmpty() || !m_workerOpen)
        return;

    m_inFlightTimeMs = m_pendingMs;
    m_pendingMs = -1;
    if (m_inFlightTimeMs == m_lastUserMs)
        m_inFlightIsPrefetch = false;

    int width = static_cast<int>(var_InheritInteger(m_intf, "qt-hover-thumb-width"));
    if (width < 80) width = 80;
    if (width > 480) width = 480;

    emit wantSeekAndDecode(m_inFlightTimeMs, width, m_generation);
}

void ThumbnailProvider::onFrameReady(qint64 timeMs, const QImage &img, int generation)
{
    /* Drop late frames from a previous file. The worker may still be
     * mid-decode when the user changes media; by the time its
     * frameReady arrives our generation has bumped. */
    if (generation != m_generation)
    {
        if (m_inFlightTimeMs == timeMs)
            m_inFlightTimeMs = -1;
        return;
    }

    bool wasPrefetch = m_inFlightIsPrefetch;
    m_inFlightIsPrefetch = false;
    m_inFlightTimeMs = -1;

    if (!img.isNull())
    {
        m_cache.insert(timeMs, img);
        trimCache();
        emit thumbnailReady(timeMs, img);
    }

    /* User-driven request preempts everything. */
    if (m_pendingMs >= 0)
    {
        startNextRequest();
        return;
    }

    /* Idle: direction-prefetch first, then warmup-queue drain. */
    if (!wasPrefetch || m_prefetchCount < PREFETCH_MAX)
    {
        prefetchAhead();
        if (m_inFlightTimeMs >= 0)
            return;
    }

    while (!m_warmupQueue.isEmpty())
    {
        qint64 t = m_warmupQueue.dequeue();
        if (m_cache.contains(t)) continue;
        m_pendingMs = t;
        m_inFlightIsPrefetch = true;
        startNextRequest();
        return;
    }
}

void ThumbnailProvider::onOpenFailed(int generation)
{
    if (generation != m_generation)
        return;
    m_workerOpen = false;
    msg_Dbg(m_intf, "[hover-thumb] worker reported openFile failure");
}

void ThumbnailProvider::seedWarmup(qint64 lengthMs)
{
    /* Idempotent: bail if the duration is unknown, hasn't changed,
     * or we have no media. */
    if (lengthMs <= 0 || m_currentPath.isEmpty())
        return;
    if (lengthMs == m_warmupLengthMs)
        return;
    m_warmupLengthMs = lengthMs;
    m_warmupQueue.clear();

    /* Evenly-spaced positions. Skip the very ends — first frames are
     * often black, and the last is rarely interesting. */
    for (int i = 1; i <= WARMUP_BUCKETS; ++i)
    {
        qint64 t = (lengthMs * i) / (WARMUP_BUCKETS + 1);
        if (!m_cache.contains(t))
            m_warmupQueue.enqueue(t);
    }

    /* libav-direct is intrinsically serial (one decoder context, one
     * in-flight at a time). Kick off one warmup if idle; the rest
     * drain from onFrameReady. */
    if (m_workerOpen && m_inFlightTimeMs < 0 && m_pendingMs < 0
        && !m_warmupQueue.isEmpty())
    {
        qint64 t = m_warmupQueue.dequeue();
        m_pendingMs = t;
        m_inFlightIsPrefetch = true;
        startNextRequest();
    }
}

void ThumbnailProvider::prefetchAhead()
{
    /* Need a known direction. m_lastDeltaMs is set when we see two
     * consecutive distinct user hovers — that's our step + direction. */
    if (m_lastDeltaMs == 0 || m_lastUserMs < 0 || m_currentPath.isEmpty())
        return;
    if (m_inFlightTimeMs >= 0)
        return;

    /* Walk in the cursor's direction, skipping cached buckets. Take
     * the first uncached hit. Cap the walk so we don't loop over a
     * fully-covered region. */
    qint64 candidate = m_lastUserMs + m_lastDeltaMs;
    int hops = 0;
    while (hops < 16 && candidate >= 0 && m_cache.contains(candidate))
    {
        candidate += m_lastDeltaMs;
        hops++;
    }
    if (candidate < 0 || hops >= 16 || m_cache.contains(candidate))
        return;

    m_prefetchCount++;
    m_inFlightIsPrefetch = true;
    m_pendingMs = candidate;
    startNextRequest();
}

void ThumbnailProvider::clearCache()
{
    m_cache.clear();
    m_pendingMs = -1;
    /* Don't touch the in-flight request — it may still produce a
     * useful thumbnail for the bucket the user is currently hovering.
     * The generation counter isn't bumped here because the file is the
     * same; only rendering parameters changed. */
}

void ThumbnailProvider::trimCache()
{
    /* Cheap: when over the cap, drop ~10% of arbitrary entries. Cursor
     * locality means most evictions will be old timestamps anyway. */
    if (m_cache.size() <= CACHE_MAX)
        return;

    int toDrop = m_cache.size() - (CACHE_MAX * 9 / 10);
    auto it = m_cache.begin();
    while (toDrop-- > 0 && it != m_cache.end())
        it = m_cache.erase(it);
}
