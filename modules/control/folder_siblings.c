/*****************************************************************************
 * folder_siblings.c: auto-load other media files from the playing file's
 *                    folder into the playlist
 *****************************************************************************
 * Copyright (C) 2026 vlc-reborn
 *
 * Licensed under GPLv2+ matching the rest of VLC.
 *
 * Module type: secondary interface (capability 0). Loaded via
 *   --extraintf=folder_siblings
 * or by adding "extraintf=folder_siblings" to ~/.config/vlc/vlcrc.
 *
 * Hooks the playlist's "input-current" variable. Each time the current
 * playing item changes, if the user-toggleable option fsib-enable is on,
 * the module enumerates the directory of the current item, filters by
 * file extension (configurable), optionally recursive, and appends the
 * non-duplicate items to the playlist.
 *
 * Three Preferences-registered options:
 *   fsib-enable      bool   master on/off
 *   fsib-recursive   bool   descend into subdirectories
 *   fsib-extensions  string comma-separated list of extensions (no dots)
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input.h>
#include <vlc_input_item.h>
#include <vlc_playlist.h>
#include <vlc_url.h>
#include <vlc_fs.h>
#include <vlc_strings.h>

#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

/*****************************************************************************
 * Module declaration / Preferences entries
 *****************************************************************************/

#define ENABLE_TEXT       N_( "Auto-load folder siblings" )
#define ENABLE_LONGTEXT   N_( "When enabled, every time you open a media file " \
    "the other media files in the same folder are appended to the playlist." )

#define RECURSIVE_TEXT     N_( "Recurse into subdirectories" )
#define RECURSIVE_LONGTEXT N_( "If checked, also append media files found in " \
    "subdirectories of the current file's folder." )

#define EXTS_TEXT     N_( "File extensions to include" )
#define EXTS_LONGTEXT N_( "Comma-separated list of file extensions, without " \
    "leading dots. Items with a different extension are ignored." )

#define DEFAULT_EXTS \
    "mp4,mkv,avi,mov,wmv,flv,webm,mpg,mpeg,m4v,3gp,ts,vob,ogv,m2ts," \
    "mp3,flac,wav,m4a,aac,ogg,opus,wma"

static int  Open  ( vlc_object_t * );
static void Close ( vlc_object_t * );

vlc_module_begin ()
    set_shortname( N_( "Folder Siblings" ) )
    set_description( N_( "Auto-add other media in the playing file's folder" ) )
    set_category( CAT_PLAYLIST )
    set_subcategory( SUBCAT_PLAYLIST_GENERAL )

    add_bool  ( "fsib-enable",     true,         ENABLE_TEXT,    ENABLE_LONGTEXT,    false )
    add_bool  ( "fsib-recursive",  false,        RECURSIVE_TEXT, RECURSIVE_LONGTEXT, false )
    add_string( "fsib-extensions", DEFAULT_EXTS, EXTS_TEXT,      EXTS_LONGTEXT,      false )

    set_capability( "interface", 0 )
    set_callbacks( Open, Close )
vlc_module_end ()

/*****************************************************************************
 * Internals
 *****************************************************************************/

typedef struct
{
    intf_thread_t *p_intf;
} fsib_sys_t;

static int PlaylistEvent( vlc_object_t *p_this, char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval,
                          void *p_data );

/* Lowercased copy of the file's extension (without the dot), or NULL if
 * the path has no extension. Caller free()s. */
static char *get_ext( const char *path )
{
    const char *base = strrchr( path, '/' );
    base = base ? base + 1 : path;
    const char *dot = strrchr( base, '.' );
    if( !dot || dot == base ) return NULL;
    char *e = strdup( dot + 1 );
    if( !e ) return NULL;
    for( char *p = e; *p; p++ ) *p = tolower( (unsigned char)*p );
    return e;
}

/* Test whether ext is in the comma-separated csv. Match is case-insensitive. */
static bool ext_in_list( const char *ext, const char *csv )
{
    if( !ext || !csv ) return false;
    const char *p = csv;
    size_t need = strlen( ext );
    while( *p )
    {
        while( *p == ',' || *p == ' ' || *p == '\t' ) p++;
        const char *tok = p;
        while( *p && *p != ',' ) p++;
        size_t len = p - tok;
        /* trim trailing spaces */
        while( len && (tok[len-1] == ' ' || tok[len-1] == '\t') ) len--;
        if( len == need && strncasecmp( tok, ext, len ) == 0 )
            return true;
    }
    return false;
}

/* Returns a heap-allocated copy of the parent directory of `path`, with
 * trailing slash. Caller free()s. */
static char *parent_dir( const char *path )
{
    const char *slash = strrchr( path, '/' );
    if( !slash ) return strdup( "./" );
    size_t len = slash - path + 1;  /* include the slash */
    char *d = malloc( len + 1 );
    if( !d ) return NULL;
    memcpy( d, path, len );
    d[len] = '\0';
    return d;
}

/* Already-queued URIs as a NULL-terminated array. Caller frees each
 * element + the array. We compare by exact URI string match — VLC's
 * make_uri is canonical so two paths to the same file produce the same
 * URI and won't collide. */
static char **collect_existing_uris( playlist_t *p_playlist, size_t *n_out )
{
    playlist_Lock( p_playlist );
    playlist_item_t *p_root = p_playlist->p_playing;
    char **uris = NULL;
    size_t cap = 0, n = 0;
    if( p_root == NULL ) { playlist_Unlock( p_playlist ); *n_out = 0; return NULL; }

    /* Iterative DFS using an array. */
    playlist_item_t **stack = malloc( sizeof(*stack) * 64 );
    size_t scap = 64, sn = 0;
    if( !stack ) { playlist_Unlock( p_playlist ); *n_out = 0; return NULL; }
    stack[sn++] = p_root;
    while( sn > 0 )
    {
        playlist_item_t *node = stack[--sn];
        if( node->p_input && node->p_input->psz_uri )
        {
            if( n == cap )
            {
                size_t nc = cap ? cap * 2 : 32;
                char **nu = realloc( uris, sizeof(*uris) * nc );
                if( !nu ) break;
                uris = nu; cap = nc;
            }
            uris[n++] = strdup( node->p_input->psz_uri );
        }
        for( int i = 0; i < node->i_children; i++ )
        {
            if( sn == scap )
            {
                size_t nc = scap * 2;
                playlist_item_t **ns = realloc( stack, sizeof(*ns) * nc );
                if( !ns ) goto done;
                stack = ns; scap = nc;
            }
            stack[sn++] = node->pp_children[i];
        }
    }
done:
    free( stack );
    playlist_Unlock( p_playlist );
    *n_out = n;
    return uris;
}

static bool already_queued( const char *uri, char **list, size_t n )
{
    if( !uri ) return false;
    for( size_t i = 0; i < n; i++ )
        if( list[i] && strcmp( list[i], uri ) == 0 )
            return true;
    return false;
}

/* Recursively scan dir for files matching exts; append URIs to playlist
 * end if not already queued. */
static void scan_and_append( intf_thread_t *p_intf, const char *dir,
                             const char *exts, bool recursive,
                             const char *current_uri,
                             char **existing, size_t n_existing,
                             int *n_added )
{
    DIR *d = vlc_opendir( dir );
    if( !d ) return;

    /* Collect entries first into a sorted list so playlist order is
     * deterministic. */
    char **names = NULL;
    size_t nn = 0, ncap = 0;
    const char *name;
    while( (name = vlc_readdir( d )) != NULL )
    {
        if( name[0] == '.' ) continue;  /* skip hidden + . / .. */
        if( nn == ncap )
        {
            size_t nc = ncap ? ncap * 2 : 32;
            char **nu = realloc( names, sizeof(*nu) * nc );
            if( !nu ) break;
            names = nu; ncap = nc;
        }
        names[nn++] = strdup( name );
    }
    closedir( d );  /* vlc_fs.h re-defines closedir as vlc_closedir on Windows */

    /* Lexicographic sort. */
    for( size_t i = 1; i < nn; i++ )
    {
        char *cur = names[i];
        size_t j = i;
        while( j > 0 && strcmp( names[j-1], cur ) > 0 )
        {
            names[j] = names[j-1];
            j--;
        }
        names[j] = cur;
    }

    playlist_t *p_playlist = pl_Get( p_intf );
    for( size_t i = 0; i < nn; i++ )
    {
        char *full;
        if( asprintf( &full, "%s%s", dir, names[i] ) < 0 ) continue;

        struct stat st;
        if( vlc_stat( full, &st ) != 0 ) { free( full ); continue; }

        if( S_ISDIR( st.st_mode ) )
        {
            if( recursive )
            {
                char *sub;
                if( asprintf( &sub, "%s/", full ) >= 0 )
                {
                    scan_and_append( p_intf, sub, exts, recursive,
                                     current_uri, existing, n_existing,
                                     n_added );
                    free( sub );
                }
            }
            free( full );
            continue;
        }
        if( !S_ISREG( st.st_mode ) ) { free( full ); continue; }

        char *ext = get_ext( names[i] );
        if( !ext || !ext_in_list( ext, exts ) )
        { free( ext ); free( full ); continue; }
        free( ext );

        char *uri = vlc_path2uri( full, NULL );
        free( full );
        if( !uri ) continue;

        if( current_uri && strcmp( uri, current_uri ) == 0 )
        { free( uri ); continue; }
        if( already_queued( uri, existing, n_existing ) )
        { free( uri ); continue; }

        /* playlist_Add(playlist, uri, play_now) — third arg is *play_now*,
         * not "is-playlist" as the header signature might suggest. Setting
         * play_now=true causes playback to immediately jump to the newly-
         * added item, which would yank the user away from the file they
         * just opened. Use play_now=false so the originally-opened file
         * keeps playing while siblings queue up behind it. */
        playlist_Add( p_playlist, uri, false );
        (*n_added)++;
        free( uri );
    }

    for( size_t i = 0; i < nn; i++ ) free( names[i] );
    free( names );
}

/* Called by VLC when the playlist's current input changes. */
static int PlaylistEvent( vlc_object_t *p_this, char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval,
                          void *p_data )
{
    VLC_UNUSED( p_this ); VLC_UNUSED( psz_var ); VLC_UNUSED( oldval );
    intf_thread_t *p_intf = (intf_thread_t *)p_data;

    if( !var_InheritBool( p_intf, "fsib-enable" ) )
        return VLC_SUCCESS;

    input_thread_t *p_input = newval.p_address;
    if( p_input == NULL ) return VLC_SUCCESS;

    input_item_t *p_item = input_GetItem( p_input );
    if( !p_item ) return VLC_SUCCESS;

    char *uri = input_item_GetURI( p_item );
    if( !uri ) return VLC_SUCCESS;

    char *path = vlc_uri2path( uri );
    if( !path )
    {
        msg_Dbg( p_intf, "[fsib] non-local URI, skipping: %s", uri );
        free( uri );
        return VLC_SUCCESS;
    }

    char *dir = parent_dir( path );
    free( path );
    if( !dir ) { free( uri ); return VLC_SUCCESS; }

    char *exts = var_InheritString( p_intf, "fsib-extensions" );
    bool recursive = var_InheritBool( p_intf, "fsib-recursive" );
    if( !exts ) exts = strdup( DEFAULT_EXTS );

    playlist_t *p_pl = pl_Get( p_intf );
    size_t n_existing = 0;
    char **existing = collect_existing_uris( p_pl, &n_existing );

    int n_added = 0;
    scan_and_append( p_intf, dir, exts, recursive,
                     uri, existing, n_existing, &n_added );

    msg_Dbg( p_intf, "[fsib] scanned %s — added %d sibling(s)", dir, n_added );

    for( size_t i = 0; i < n_existing; i++ ) free( existing[i] );
    free( existing );
    free( exts );
    free( dir );
    free( uri );
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Open / Close
 *****************************************************************************/

static int Open( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;
    fsib_sys_t *p_sys = malloc( sizeof( *p_sys ) );
    if( !p_sys ) return VLC_ENOMEM;
    p_sys->p_intf = p_intf;
    p_intf->p_sys = (intf_sys_t *)p_sys;

    var_AddCallback( pl_Get( p_intf ), "input-current",
                     PlaylistEvent, p_intf );

    msg_Dbg( p_intf, "[fsib] folder-siblings interface module loaded" );
    return VLC_SUCCESS;
}

static void Close( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;
    var_DelCallback( pl_Get( p_intf ), "input-current",
                     PlaylistEvent, p_intf );
    free( p_intf->p_sys );
}
