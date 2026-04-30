/*****************************************************************************
 * playlist_thumbnail_manager.cpp
 *****************************************************************************
 * Copyright (C) 2026 VLC authors and VideoLAN
 *
 * Licensed under GPLv2+ matching the rest of the Qt UI module.
 *****************************************************************************/

/* QThread MUST come before any VLC header (vlc_threads.h's msleep
 * macro vs QThread::msleep — see thumbnail_provider.cpp). */
#include <QThread>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include "qt.hpp"
#include "playlist_thumbnail_manager.hpp"

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
 * XDG thumbnail cache lookup
 *
 * Many file managers (nemo, nautilus, dolphin, thunar) and helper tools
 * (totem-video-thumbnailer, ffmpegthumbnailer) pre-populate
 * ~/.cache/thumbnails/<size>/<md5(uri)>.png as users browse media. If a
 * thumbnail already exists there, decoding it (a small PNG) is several
 * times faster than seeking + decoding a video frame from scratch.
 *
 * Spec: https://specifications.freedesktop.org/thumbnail-spec/
 *****************************************************************************/

static QString xdgThumbPath( const QString &uri, const QString &sizeDir )
{
    QByteArray hash = QCryptographicHash::hash( uri.toUtf8(),
                                                QCryptographicHash::Md5 ).toHex();
    QString cacheBase = QStandardPaths::writableLocation(
                            QStandardPaths::GenericCacheLocation );
    return QStringLiteral( "%1/thumbnails/%2/%3.png" )
            .arg( cacheBase, sizeDir, QString::fromLatin1( hash ) );
}

static QImage tryXdgCache( const QString &uri, int targetWidth )
{
    QStringList sizes;
    if      ( targetWidth <= 128 ) sizes << "normal" << "large" << "x-large";
    else if ( targetWidth <= 256 ) sizes << "large" << "x-large" << "xx-large";
    else if ( targetWidth <= 512 ) sizes << "x-large" << "xx-large" << "large";
    else                            sizes << "xx-large" << "x-large";

    for ( const QString &sz : sizes )
    {
        const QString p = xdgThumbPath( uri, sz );
        if ( !QFileInfo::exists( p ) ) continue;
        QImage img( p, "PNG" );
        if ( !img.isNull() )
            return img;
    }
    return QImage();
}

/*****************************************************************************
 * Per-task libav decode (each worker call uses its own scratch state).
 *****************************************************************************/

static QImage decodeOneFrame( const QString &path, int width )
{
    AVFormatContext *fmt = nullptr;
    if ( avformat_open_input( &fmt, path.toUtf8().constData(),
                              nullptr, nullptr ) < 0 )
        return QImage();

    if ( avformat_find_stream_info( fmt, nullptr ) < 0 )
    {
        avformat_close_input( &fmt );
        return QImage();
    }

    int videoIdx = -1;
    for ( unsigned i = 0; i < fmt->nb_streams; ++i )
    {
        if ( fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
        {
            videoIdx = static_cast<int>( i );
            break;
        }
    }
    if ( videoIdx < 0 )
    {
        avformat_close_input( &fmt );
        return QImage();
    }

    AVStream *st = fmt->streams[videoIdx];
    const AVCodec *codec = avcodec_find_decoder( st->codecpar->codec_id );
    if ( !codec )
    {
        avformat_close_input( &fmt );
        return QImage();
    }

    AVCodecContext *cctx = avcodec_alloc_context3( codec );
    if ( !cctx )
    {
        avformat_close_input( &fmt );
        return QImage();
    }
    if ( avcodec_parameters_to_context( cctx, st->codecpar ) < 0
         || avcodec_open2( cctx, codec, nullptr ) < 0 )
    {
        avcodec_free_context( &cctx );
        avformat_close_input( &fmt );
        return QImage();
    }
    cctx->thread_count = 1;
    cctx->skip_loop_filter = AVDISCARD_NONREF;

    int64_t targetUs = ( fmt->duration > 0 )
                         ? fmt->duration / 4
                         : 5LL * AV_TIME_BASE;
    AVRational tb = st->time_base;
    AVRational usBase; usBase.num = 1; usBase.den = AV_TIME_BASE;
    int64_t targetPts = av_rescale_q( targetUs, usBase, tb );

    if ( av_seek_frame( fmt, videoIdx, targetPts, AVSEEK_FLAG_BACKWARD ) < 0 )
    {
        avcodec_free_context( &cctx );
        avformat_close_input( &fmt );
        return QImage();
    }
    avcodec_flush_buffers( cctx );

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SwsContext *sws = nullptr;

    bool got = false;
    bool eof = false;
    int loops = 0;
    while ( !got && loops++ < 400 )
    {
        if ( !eof )
        {
            int rc = av_read_frame( fmt, pkt );
            if ( rc < 0 )
            {
                eof = true;
                avcodec_send_packet( cctx, nullptr );
            }
            else if ( pkt->stream_index != videoIdx )
            {
                av_packet_unref( pkt );
                continue;
            }
            else
            {
                avcodec_send_packet( cctx, pkt );
                av_packet_unref( pkt );
            }
        }

        while ( true )
        {
            int rc = avcodec_receive_frame( cctx, frame );
            if ( rc == AVERROR( EAGAIN ) ) break;
            if ( rc < 0 ) { eof = true; goto decode_done; }

            int64_t pts = frame->best_effort_timestamp;
            if ( pts == AV_NOPTS_VALUE ) pts = frame->pts;
            if ( pts == AV_NOPTS_VALUE || pts >= targetPts )
            {
                got = true;
                break;
            }
            av_frame_unref( frame );
        }
    }
decode_done:

    QImage out;
    if ( got )
    {
        const int srcW = cctx->width;
        const int srcH = cctx->height;
        if ( srcW > 0 && srcH > 0 )
        {
            const int outW = qBound( 80, width, 480 );
            int outH = static_cast<int>( static_cast<qint64>( outW ) * srcH / srcW );
            if ( outH < 1 ) outH = 1;

            AVPixelFormat srcFmt = static_cast<AVPixelFormat>( frame->format );
            if ( srcFmt == AV_PIX_FMT_NONE ) srcFmt = cctx->pix_fmt;

            sws = sws_getContext( srcW, srcH, srcFmt,
                                  outW, outH, AV_PIX_FMT_RGB24,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr );
            if ( sws )
            {
                QImage img( outW, outH, QImage::Format_RGB888 );
                uint8_t *dst[1] = { img.bits() };
                int dstStride[1] = { static_cast<int>( img.bytesPerLine() ) };
                sws_scale( sws, frame->data, frame->linesize, 0,
                           srcH, dst, dstStride );
                out = img;
            }
        }
    }

    if ( sws )   sws_freeContext( sws );
    if ( frame ) av_frame_free( &frame );
    if ( pkt )   av_packet_free( &pkt );
    avcodec_free_context( &cctx );
    avformat_close_input( &fmt );
    return out;
}

/*****************************************************************************
 * PlaylistThumbnailWorker
 *
 * One per worker thread. The worker has no persistent libav state —
 * each generate() call owns its own AVFormatContext + decoder state
 * for the duration of the call. That keeps workers stateless and
 * trivially shutdown-safe.
 *****************************************************************************/

PlaylistThumbnailWorker::PlaylistThumbnailWorker() {}
PlaylistThumbnailWorker::~PlaylistThumbnailWorker() {}

void PlaylistThumbnailWorker::generate( QString uri, QString path,
                                        int width, int gen )
{
    /* Fast path: spec-compliant XDG thumbnail. PNG decode is several
     * times faster than a libav seek + frame decode. */
    QImage img = tryXdgCache( uri, width );
    if ( !img.isNull() )
    {
        const int outW = qBound( 80, width, 480 );
        if ( img.width() != outW )
            img = img.scaledToWidth( outW, Qt::SmoothTransformation );
    }
    else
    {
        img = decodeOneFrame( path, width );
    }
    emit thumbnailReady( uri, img, gen );
}

/*****************************************************************************
 * PlaylistThumbnailManager
 *****************************************************************************/

PlaylistThumbnailManager *PlaylistThumbnailManager::instance()
{
    static PlaylistThumbnailManager *s_inst = nullptr;
    if ( !s_inst )
        s_inst = new PlaylistThumbnailManager();
    return s_inst;
}

PlaylistThumbnailManager::PlaylistThumbnailManager()
    : QObject( qApp )
{
    /* Spin up N worker threads; each owns one PlaylistThumbnailWorker
     * with its own event loop. The workers stay around for the app
     * lifetime, idle when the queue drains. */
    m_workers.reserve( N_WORKERS );
    for ( int i = 0; i < N_WORKERS; ++i )
    {
        WorkerSlot slot;
        slot.thread = new QThread( this );
        slot.worker = new PlaylistThumbnailWorker();
        slot.worker->moveToThread( slot.thread );
        slot.busy = false;

        connect( slot.thread, &QThread::finished,
                 slot.worker, &QObject::deleteLater );
        connect( slot.worker,
                 SIGNAL( thumbnailReady( QString, QImage, int ) ),
                 this, SLOT( onWorkerDone( QString, QImage, int ) ) );

        slot.thread->start();
        m_workers.append( slot );
    }
}

PlaylistThumbnailManager::~PlaylistThumbnailManager()
{
    for ( WorkerSlot &s : m_workers )
    {
        s.thread->quit();
        s.thread->wait();
    }
}

QImage PlaylistThumbnailManager::cachedFor( const QString &uri ) const
{
    auto it = m_cache.constFind( uri );
    return it == m_cache.constEnd() ? QImage() : it.value();
}

QImage PlaylistThumbnailManager::requestAsync( const QString &uri, int width )
{
    if ( width != m_curWidth )
    {
        m_cache.clear();
        m_curWidth = width;
        m_generation++;
    }

    auto it = m_cache.constFind( uri );
    if ( it != m_cache.constEnd() )
        return it.value();

    if ( m_inFlight.contains( uri ) || m_pendingSet.contains( uri ) )
        return QImage();

    /* LIFO push: the most recently requested rows (typically what's
     * just been scrolled into view) get processed first by pumpQueue's
     * takeLast. */
    m_pending.append( uri );
    m_pendingSet.insert( uri );
    pumpQueue();
    return QImage();
}

void PlaylistThumbnailManager::pumpQueue()
{
    /* Hand out as many pending uris as we have idle workers. */
    for ( WorkerSlot &slot : m_workers )
    {
        if ( m_pending.isEmpty() ) return;
        if ( slot.busy ) continue;

        QString uri = m_pending.takeLast();   /* LIFO */
        m_pendingSet.remove( uri );

        QString path;
        QByteArray uriBytes = uri.toUtf8();
        char *p = vlc_uri2path( uriBytes.constData() );
        if ( p )
        {
            path = QString::fromUtf8( p );
            free( p );
        }
        if ( path.isEmpty() || !QFileInfo::exists( path ) )
            continue;   /* silently skip non-local; try next pending */

        slot.busy = true;
        m_inFlight.insert( uri );
        QMetaObject::invokeMethod( slot.worker, "generate",
            Qt::QueuedConnection,
            Q_ARG( QString, uri ),
            Q_ARG( QString, path ),
            Q_ARG( int, m_curWidth ),
            Q_ARG( int, m_generation ) );
    }
}

void PlaylistThumbnailManager::onWorkerDone( QString uri, const QImage &img, int gen )
{
    m_inFlight.remove( uri );

    /* Mark the originating worker idle. We identify by sender(): the
     * signal was emitted on the worker thread, queued through Qt's
     * meta-call system; sender() in this slot points at the emitting
     * QObject regardless of thread. */
    PlaylistThumbnailWorker *src =
        qobject_cast<PlaylistThumbnailWorker *>( sender() );
    for ( WorkerSlot &slot : m_workers )
    {
        if ( slot.worker == src ) { slot.busy = false; break; }
    }

    /* Stale: width changed (or cache was cleared) while this task was
     * decoding. Throw the result away — it's at the wrong size. */
    if ( gen != m_generation )
    {
        pumpQueue();
        return;
    }

    if ( !img.isNull() )
    {
        if ( m_cache.size() >= CACHE_MAX )
        {
            int toDrop = m_cache.size() - ( CACHE_MAX * 9 / 10 );
            auto it = m_cache.begin();
            while ( toDrop-- > 0 && it != m_cache.end() )
                it = m_cache.erase( it );
        }
        m_cache.insert( uri, img );
        emit thumbnailReady( uri );
    }

    pumpQueue();
}

void PlaylistThumbnailManager::clearCache()
{
    m_cache.clear();
    m_generation++;
}
