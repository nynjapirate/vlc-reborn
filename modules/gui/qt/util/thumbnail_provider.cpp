/*****************************************************************************
 * thumbnail_provider.cpp: ffmpeg-subprocess seek-time thumbnail provider
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * Licensed under GPLv2+ matching the rest of the Qt UI module.
 *****************************************************************************/

#include "thumbnail_provider.hpp"
#include "input_manager.hpp"

#include <vlc_input.h>
#include <vlc_url.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <unistd.h>  /* getpid, getuid */

ThumbnailProvider::ThumbnailProvider(intf_thread_t *p_intf, QObject *parent)
    : QObject(parent)
    , m_intf(p_intf)
{
    m_backend = detectBackend();
    if (m_backend == BACKEND_MPV)
    {
        /* /run/user/<uid> is tmpfs on systemd-managed sessions. The
         * socket and screenshot paths are unique per spawn (mpvStart
         * generates them with an incrementing serial) so a stale mpv
         * from a previous spawn can't collide with a fresh one. */
        m_mpvRunDir = QStringLiteral("/run/user/%1").arg(getuid());
        if (!QFileInfo::exists(m_mpvRunDir))
            m_mpvRunDir = QStringLiteral("/tmp");
        msg_Dbg(m_intf, "[hover-thumb] backend=mpv runDir=%s",
                m_mpvRunDir.toUtf8().constData());
    }
    else
        msg_Dbg(m_intf, "[hover-thumb] backend=ffmpegthumbnailer (mpv not found)");

    /* Pick up the currently loaded media if any (the input may already be
     * running by the time we register). */
    onInputChanged();
}

ThumbnailProvider::~ThumbnailProvider()
{
    killInFlight();
    killWarmupWorkers();
    mpvStop();
}

ThumbnailProvider::Backend ThumbnailProvider::detectBackend() const
{
    /* Prefer mpv if installed. ffmpegthumbnailer is the fallback. If
     * neither is present, requestThumbnail() becomes a no-op. */
    QFileInfo mpvBin("/usr/bin/mpv");
    if (mpvBin.exists() && mpvBin.isExecutable())
        return BACKEND_MPV;
    QFileInfo ftBin("/usr/bin/ffmpegthumbnailer");
    if (ftBin.exists() && ftBin.isExecutable())
        return BACKEND_FFMPEGTHUMBNAILER;
    return BACKEND_NONE;
}

void ThumbnailProvider::onInputChanged()
{
    /* MainInputManager fires inputChanged many times during file setup
     * and again on every auto-advance. We debounce: don't actually
     * respawn mpv until the input has been stable for 800 ms. Each fresh
     * inputChanged restarts the timer. */
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

    /* Same path AND mpv subprocess alive: no work to do. Common case. */
    if (!newPath.isEmpty() && newPath == m_currentPath
        && (m_backend != BACKEND_MPV || m_mpv != nullptr))
    {
        if (m_inputDebouncer) m_inputDebouncer->stop();  /* cancel any
                              * pending respawn for an interim path */
        return;
    }

    /* Stash the latest path; restart the debounce timer. */
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
    /* Input has been stable for 800 ms. Now do the per-file reset. */
    QString newPath = m_debouncedNewPath;

    /* Re-check dedupe — m_currentPath could have caught up if a transient
     * path bounced through. */
    if (!newPath.isEmpty() && newPath == m_currentPath
        && (m_backend != BACKEND_MPV || m_mpv != nullptr))
    {
        return;
    }

    killInFlight();
    killWarmupWorkers();
    m_pendingMs = -1;
    m_lastUserMs = -1;
    m_lastDeltaMs = 0;
    m_prefetchCount = 0;
    m_warmupLengthMs = -1;
    m_warmupQueue.clear();
    m_cache.clear();
    m_currentPath = newPath;
    m_generation++;

    if (m_backend == BACKEND_MPV && !m_currentPath.isEmpty())
    {
        /* Hot-swap the file in the existing mpv if we have one — much
         * cheaper than respawn, and avoids the kill/start race that was
         * causing the user's "10 mpv spawns in a row" behavior. */
        if (m_mpv != nullptr)
            mpvLoadFile(m_currentPath);
        else
            mpvStart(m_currentPath);
    }
}

void ThumbnailProvider::requestThumbnail(qint64 timeMs)
{
    if (m_currentPath.isEmpty() || timeMs < 0)
        return;

    /* User-toggleable. Tools → Preferences → Show All → Interface → Main
     * interfaces → Qt → "Show preview thumbnails when hovering the seek
     * bar". Default on; off-switch is honored on each request. */
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
    if (!m_proc)
        startNextRequest();
}

QStringList ThumbnailProvider::buildArgs(qint64 timeMs) const
{
    /* ffmpegthumbnailer is a small Debian-packaged tool built specifically
     * for this job; benchmarks ~128 ms per thumbnail vs. ~513 ms for raw
     * ffmpeg (~4x faster) on the same hardware. JPEG to stdout. */
    const qint64 totalSecs = timeMs / 1000;
    const qint64 hh = totalSecs / 3600;
    const qint64 mm = (totalSecs % 3600) / 60;
    const qint64 ss = totalSecs % 60;
    const qint64 fff = timeMs % 1000;
    QString seekTime = QString::asprintf("%02lld:%02lld:%02lld.%03lld",
                                         hh, mm, ss, fff);
    /* Configurable size + quality, read fresh every request so changes in
     * Tools → Preferences apply without restart. */
    int width = static_cast<int>(var_InheritInteger(m_intf, "qt-hover-thumb-width"));
    if (width < 80) width = 80;
    if (width > 480) width = 480;
    int quality = static_cast<int>(var_InheritInteger(m_intf, "qt-hover-thumb-quality"));
    if (quality < 0) quality = 0;
    if (quality > 10) quality = 10;

    QStringList args;
    args << "-i" << m_currentPath
         << "-o" << QStringLiteral("/dev/stdout")
         << "-t" << seekTime
         << "-s" << QString::number(width)
         << "-c" << "jpeg"
         << "-q" << QString::number(quality);
    return args;
}

void ThumbnailProvider::startNextRequest()
{
    if (m_pendingMs < 0 || m_currentPath.isEmpty())
        return;

    m_inFlightTimeMs = m_pendingMs;
    m_pendingMs = -1;
    /* m_inFlightIsPrefetch is set by prefetchAhead() before it calls into
     * here; clear it on every user-driven start. */
    if (m_inFlightTimeMs == m_lastUserMs)
        m_inFlightIsPrefetch = false;

    if (m_backend == BACKEND_MPV)
    {
        /* mpv backend: defer to mpv if it's ready; otherwise stash the
         * request and we'll fire it from onMpvSocketConnected once the
         * file has loaded. The pending slot already holds the request. */
        if (m_mpvReady)
            mpvIssueRequest(m_inFlightTimeMs);
        else
        {
            /* Put it back in the pending slot — onMpvSocketConnected will
             * pick it up when ready. */
            m_pendingMs = m_inFlightTimeMs;
            m_inFlightTimeMs = -1;
        }
        return;
    }

    /* ffmpegthumbnailer backend (fallback). */
    if (m_backend != BACKEND_FFMPEGTHUMBNAILER)
    {
        m_inFlightTimeMs = -1;
        return;
    }
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_proc, SIGNAL(finished(int)),
            this, SLOT(onProcessFinished(int)));
    m_proc->start("ffmpegthumbnailer", buildArgs(m_inFlightTimeMs));
}

void ThumbnailProvider::launchWarmupWorker()
{
    if (m_warmupQueue.isEmpty() || m_currentPath.isEmpty())
        return;
    if (m_warmupProcs.size() >= WARMUP_PARALLEL)
        return;

    /* Skip any cache-already-covered entries up front. */
    qint64 timeMs;
    do {
        if (m_warmupQueue.isEmpty()) return;
        timeMs = m_warmupQueue.dequeue();
    } while (m_cache.contains(timeMs));

    QProcess *p = new QProcess(this);
    p->setProcessChannelMode(QProcess::SeparateChannels);
    m_warmupProcs.append(p);

    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, p, timeMs](int code, QProcess::ExitStatus) {
        QImage image;
        if (code == 0)
        {
            QByteArray data = p->readAllStandardOutput();
            if (!data.isEmpty())
                image.loadFromData(data, "JPEG");
        }
        m_warmupProcs.removeOne(p);
        p->deleteLater();
        if (!image.isNull())
        {
            m_cache.insert(timeMs, image);
            trimCache();
            /* No emit here — warmup thumbnails are filled silently; the
             * tooltip's onThumbnailReady filter would drop them anyway
             * since they typically aren't the user's hovered bucket. */
        }
        /* Keep the worker pool busy until queue drains. */
        launchWarmupWorker();
    });
    p->start("ffmpegthumbnailer", buildArgs(timeMs));
}

void ThumbnailProvider::killWarmupWorkers()
{
    for (QProcess *p : m_warmupProcs)
    {
        disconnect(p, nullptr, this, nullptr);
        p->kill();
        p->waitForFinished(50);
        p->deleteLater();
    }
    m_warmupProcs.clear();
}

void ThumbnailProvider::onProcessFinished(int exitCode)
{
    QImage image;
    qint64 timeMs = m_inFlightTimeMs;

    if (exitCode == 0)
    {
        QByteArray data = m_proc->readAllStandardOutput();
        if (!data.isEmpty())
            image.loadFromData(data, "JPEG");
    }

    m_proc->deleteLater();
    m_proc = nullptr;
    m_inFlightTimeMs = -1;

    if (!image.isNull())
    {
        m_cache.insert(timeMs, image);
        trimCache();
        emit thumbnailReady(timeMs, image);
    }

    bool wasPrefetch = m_inFlightIsPrefetch;
    m_inFlightIsPrefetch = false;

    /* User-driven request preempts everything. */
    if (m_pendingMs >= 0)
    {
        startNextRequest();
        return;
    }

    /* Idle. First, fan out direction-aware prefetch (capped) so the next
     * step in the cursor's last-seen direction is already cached. */
    if (!wasPrefetch || m_prefetchCount < PREFETCH_MAX)
    {
        prefetchAhead();
        if (m_proc)
            return;  /* prefetchAhead actually started something */
    }

    /* Warmup is handled by separate parallel workers (m_warmupProcs)
     * launched in seedWarmup; nothing to do here. */
}

void ThumbnailProvider::killInFlight()
{
    if (m_proc)
    {
        disconnect(m_proc, nullptr, this, nullptr);
        m_proc->kill();
        m_proc->waitForFinished(50);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_inFlightTimeMs = -1;
}

void ThumbnailProvider::seedWarmup(qint64 lengthMs)
{
    /* Idempotent: bail out if the duration is unknown, hasn't changed,
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

    if (m_backend == BACKEND_FFMPEGTHUMBNAILER)
    {
        /* Spawn parallel workers up to WARMUP_PARALLEL. They drain the
         * queue themselves; user-driven requests run on the separate
         * m_proc slot. */
        while (m_warmupProcs.size() < WARMUP_PARALLEL && !m_warmupQueue.isEmpty())
            launchWarmupWorker();
    }
    else if (m_backend == BACKEND_MPV)
    {
        /* mpv is intrinsically serial (one socket, one in-flight at a
         * time), so we just kick off one and the onScreenshotResult
         * handler will pull the next from m_warmupQueue. */
        if (m_mpvReady && m_inFlightTimeMs < 0 && m_pendingMs < 0
            && !m_warmupQueue.isEmpty())
        {
            qint64 t = m_warmupQueue.dequeue();
            m_inFlightTimeMs = t;
            m_inFlightIsPrefetch = true;
            mpvIssueRequest(t);
        }
    }
}

void ThumbnailProvider::prefetchAhead()
{
    /* Need a known direction. m_lastDeltaMs is set when we see two
     * consecutive distinct user hovers — that's our step + direction. */
    if (m_lastDeltaMs == 0 || m_lastUserMs < 0 || m_currentPath.isEmpty())
        return;
    if (m_proc)
        return;  /* will retry from onProcessFinished when idle */

    /* Walk forward in the cursor's direction, skipping any bucket that's
     * already cached. Take the first uncached hit. Cap the walk so we
     * don't loop forever on a region that's fully covered. */
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
    /* Don't kill an in-flight request — it may still produce a useful
     * thumbnail for the bucket the user is currently hovering. The
     * generation counter isn't bumped here because the file is the same;
     * only the rendering parameters changed. */
}

/*****************************************************************************
 * mpv backend
 *****************************************************************************/

void ThumbnailProvider::mpvStart(const QString &filePath)
{
    /* If we already have an mpv running, kill it before respawning. */
    mpvStop();

    /* Generate per-spawn unique paths so even if the previous mpv hasn't
     * fully been reaped by the kernel, our new mpv's socket and
     * screenshot paths can't collide with it. */
    m_mpvSpawnSerial++;
    m_mpvSocketPath = QStringLiteral("%1/vlc-reborn-mpv-%2-%3.sock")
                      .arg(m_mpvRunDir).arg(getpid()).arg(m_mpvSpawnSerial);
    m_mpvScreenshotPath = QStringLiteral("%1/vlc-reborn-thumb-%2-%3.jpg")
                          .arg(m_mpvRunDir).arg(getpid()).arg(m_mpvSpawnSerial);
    msg_Dbg(m_intf, "[hover-thumb] mpvStart serial=%d socket=%s",
            m_mpvSpawnSerial, m_mpvSocketPath.toUtf8().constData());

    /* mpv hasn't loaded the file yet — mpvProcessLine will flip this
     * true on the file-loaded event. Hover requests gate on this. */
    m_mpvFileLoaded = false;
    m_mpv = new QProcess(this);
    m_mpv->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_mpv, SIGNAL(finished(int)),
            this, SLOT(onMpvProcessFinished(int)));

    /* Headless mpv: video decoder runs (--vid=auto) so we can grab frames,
     * but no display output (--vo=null) and no audio. --idle is required
     * because the file may be unloaded before mpv exits. */
    QStringList args;
    args << "--idle=yes"
         << "--no-config"
         << "--vid=auto"
         << "--vo=null"
         << "--no-audio"
         << "--no-input-terminal"
         << "--quiet"
         << "--no-osc"
         << QStringLiteral("--input-ipc-server=%1").arg(m_mpvSocketPath)
         /* Default screenshot params; we override per-request via the
          * screenshot-format/-jpeg-quality properties if needed. */
         << "--screenshot-format=jpg"
         << "--screenshot-jpeg-quality=85"
         << filePath;
    m_mpv->start("/usr/bin/mpv", args);

    /* Poll for the socket to appear, then connect. mpv typically opens it
     * within ~50-200 ms after launch. */
    if (!m_mpvConnectTimer)
    {
        m_mpvConnectTimer = new QTimer(this);
        m_mpvConnectTimer->setInterval(50);
        connect(m_mpvConnectTimer, SIGNAL(timeout()),
                this, SLOT(onMpvConnectRetry()));
    }
    m_mpvConnectAttempts = 0;
    m_mpvConnectTimer->start();
}

void ThumbnailProvider::onMpvConnectRetry()
{
    if (!QFileInfo::exists(m_mpvSocketPath))
    {
        if (++m_mpvConnectAttempts > 60)  /* 3 seconds total */
        {
            m_mpvConnectTimer->stop();
            msg_Err(m_intf, "[hover-thumb] mpv socket never appeared, giving up");
        }
        return;
    }
    m_mpvConnectTimer->stop();

    if (!m_mpvSocket)
    {
        m_mpvSocket = new QLocalSocket(this);
        connect(m_mpvSocket, SIGNAL(connected()),
                this, SLOT(onMpvSocketConnected()));
        connect(m_mpvSocket, SIGNAL(readyRead()),
                this, SLOT(onMpvSocketReadyRead()));
        connect(m_mpvSocket, SIGNAL(disconnected()),
                this, SLOT(onMpvSocketDisconnected()));
    }
    m_mpvSocket->connectToServer(m_mpvSocketPath);
}

void ThumbnailProvider::onMpvSocketConnected()
{
    msg_Dbg(m_intf, "[hover-thumb] mpv IPC connected");
    m_mpvReady = true;
    /* If a request was queued before the socket was ready, fire it now. */
    if (m_pendingMs >= 0 && m_inFlightTimeMs < 0)
        startNextRequest();
}

void ThumbnailProvider::onMpvSocketReadyRead()
{
    m_mpvRecvBuf.append(m_mpvSocket->readAll());
    /* JSON-RPC over the socket is newline-delimited. Process each
     * complete line and leave any partial line in the buffer. */
    int nl;
    while ((nl = m_mpvRecvBuf.indexOf('\n')) >= 0)
    {
        QByteArray line = m_mpvRecvBuf.left(nl);
        m_mpvRecvBuf.remove(0, nl + 1);
        if (!line.isEmpty())
            mpvProcessLine(line);
    }
}

void ThumbnailProvider::onMpvSocketDisconnected()
{
    msg_Dbg(m_intf, "[hover-thumb] mpv IPC disconnected");
    m_mpvReady = false;
}

void ThumbnailProvider::onMpvProcessFinished(int exitCode)
{
    msg_Dbg(m_intf, "[hover-thumb] mpv process exited with code %d", exitCode);
    if (m_mpv) { m_mpv->deleteLater(); m_mpv = nullptr; }
    if (m_mpvSocket) { m_mpvSocket->deleteLater(); m_mpvSocket = nullptr; }
    if (m_mpvConnectTimer) m_mpvConnectTimer->stop();
    m_mpvReady = false;
    m_inFlightTimeMs = -1;
    m_mpvScreenshotReqId = -1;
    /* Don't auto-respawn. onInputChanged will re-trigger on next file. */
}

void ThumbnailProvider::mpvLoadFile(const QString &filePath)
{
    /* Hot-swap the file in the running mpv via IPC. ~1-5 ms vs. spawning
     * a fresh mpv (~50-200 ms + file open + first frame decode). */
    if (!m_mpv)
        return;

    /* Mark file as not-yet-loaded; mpv emits "file-loaded" event when
     * ready, and mpvIssueRequest gates on m_mpvFileLoaded so a request
     * during the brief load gap is queued, not failed. */
    m_mpvFileLoaded = false;

    QJsonObject cmd;
    cmd["command"] = QJsonArray{ "loadfile", filePath, "replace" };
    cmd["request_id"] = m_mpvNextRequestId++;
    mpvSendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact));

    msg_Dbg(m_intf, "[hover-thumb] mpvLoadFile %s",
            filePath.toUtf8().constData());
}

void ThumbnailProvider::mpvStop()
{
    if (m_mpvConnectTimer) m_mpvConnectTimer->stop();
    if (m_mpvSocket)
    {
        disconnect(m_mpvSocket, nullptr, this, nullptr);
        m_mpvSocket->abort();
        delete m_mpvSocket;     /* synchronous; no deleteLater race */
        m_mpvSocket = nullptr;
    }
    if (m_mpv)
    {
        disconnect(m_mpv, nullptr, this, nullptr);
        /* SIGTERM first; if mpv ignores it, SIGKILL.
         * Synchronous waitForFinished blocks until the OS process is
         * actually dead — without this, the OLD mpv was still alive when
         * we spawned a NEW one for the next file, and they raced on the
         * shared screenshot path producing "Corrupt JPEG" reads. */
        m_mpv->terminate();
        if (!m_mpv->waitForFinished(500))
        {
            m_mpv->kill();
            m_mpv->waitForFinished(500);
        }
        delete m_mpv;
        m_mpv = nullptr;
    }
    if (!m_mpvSocketPath.isEmpty())
        QFile::remove(m_mpvSocketPath);
    if (!m_mpvScreenshotPath.isEmpty())
        QFile::remove(m_mpvScreenshotPath);
    m_mpvReady = false;
    m_mpvFileLoaded = false;
    m_mpvRecvBuf.clear();
    m_mpvScreenshotReqId = -1;
}

void ThumbnailProvider::mpvSendCommand(const QByteArray &jsonCmd)
{
    if (!m_mpvSocket || m_mpvSocket->state() != QLocalSocket::ConnectedState)
        return;
    m_mpvSocket->write(jsonCmd);
    if (!jsonCmd.endsWith('\n'))
        m_mpvSocket->write("\n");
    m_mpvSocket->flush();
}

void ThumbnailProvider::mpvIssueRequest(qint64 timeMs)
{
    if (!m_mpvReady) return;
    if (!m_mpvFileLoaded)
    {
        /* The file is still loading. Stash the request in the pending
         * slot so we serve it as soon as file-loaded fires. */
        m_pendingMs = timeMs;
        m_inFlightTimeMs = -1;
        return;
    }

    /* Seek with "absolute+exact" so mpv waits for the target frame to
     * decode before the next command runs. Without "exact", screenshot
     * could capture the pre-seek frame. */
    const double secs = static_cast<double>(timeMs) / 1000.0;

    QJsonObject seekCmd;
    seekCmd["command"] = QJsonArray{ "seek", secs, "absolute+exact" };
    seekCmd["request_id"] = m_mpvNextRequestId++;
    mpvSendCommand(QJsonDocument(seekCmd).toJson(QJsonDocument::Compact));

    /* Per-request unique screenshot path. Defends against any race where
     * the previous screenshot's file is still being flushed when the
     * next response arrives. We delete the file after we read it. */
    int reqId = m_mpvNextRequestId++;
    QString outPath = QStringLiteral("%1-%2.jpg")
                      .arg(m_mpvScreenshotPath.left(m_mpvScreenshotPath.length() - 4))
                      .arg(reqId);

    QJsonObject shotCmd;
    shotCmd["command"] = QJsonArray{ "screenshot-to-file", outPath, "video" };
    shotCmd["request_id"] = reqId;
    mpvSendCommand(QJsonDocument(shotCmd).toJson(QJsonDocument::Compact));

    m_mpvScreenshotReqId = reqId;
    m_mpvScreenshotInFlightPath = outPath;
}

void ThumbnailProvider::mpvProcessLine(const QByteArray &line)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    QJsonObject obj = doc.object();

    /* Async events (no request_id). We watch for file-loaded so requests
     * issued during a hot-swap loadfile get released as soon as the new
     * file's first frame is decodable. */
    if (obj.contains("event"))
    {
        const QString event = obj["event"].toString();
        if (event == QStringLiteral("file-loaded"))
        {
            m_mpvFileLoaded = true;
            /* If a request was queued during the load, fire it now. */
            if (m_pendingMs >= 0 && m_inFlightTimeMs < 0)
                startNextRequest();
        }
        return;
    }

    /* Command responses. */
    if (obj.contains("request_id"))
    {
        int reqId = obj["request_id"].toInt();
        if (reqId == m_mpvScreenshotReqId)
        {
            bool success = (obj["error"].toString() == "success");
            qint64 t = m_inFlightTimeMs;
            m_mpvScreenshotReqId = -1;
            mpvHandleScreenshotResult(t, success);
        }
    }
}

void ThumbnailProvider::mpvHandleScreenshotResult(qint64 timeMs, bool success)
{
    QImage image;
    QString readPath = m_mpvScreenshotInFlightPath;
    m_mpvScreenshotInFlightPath.clear();

    if (success && !readPath.isEmpty())
    {
        image.load(readPath);
        if (!image.isNull())
        {
            /* Honor user width preference. */
            int width = static_cast<int>(var_InheritInteger(m_intf, "qt-hover-thumb-width"));
            if (width < 80)  width = 80;
            if (width > 480) width = 480;
            if (image.width() > width)
                image = image.scaledToWidth(width, Qt::SmoothTransformation);
        }
        QFile::remove(readPath);  /* clean up immediately */
    }

    bool wasPrefetch = m_inFlightIsPrefetch;
    m_inFlightIsPrefetch = false;
    m_inFlightTimeMs = -1;

    if (!image.isNull())
    {
        m_cache.insert(timeMs, image);
        trimCache();
        emit thumbnailReady(timeMs, image);
    }

    /* User-driven request preempts everything. */
    if (m_pendingMs >= 0)
    {
        startNextRequest();
        return;
    }

    /* Idle: direction-prefetch, then warmup-queue drain. */
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
        m_inFlightTimeMs = t;
        m_inFlightIsPrefetch = true;
        mpvIssueRequest(t);
        return;
    }
}

/*****************************************************************************
 * back to common helpers
 *****************************************************************************/

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
