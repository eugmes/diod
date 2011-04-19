/*****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security, LLC.
 *  Written by Jim Garlick <garlick@llnl.gov> LLNL-CODE-423279
 *  All Rights Reserved.
 *
 *  This file is part of the Distributed I/O Daemon (diod).
 *  For details, see <http://code.google.com/p/diod/>.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License (as published by the
 *  Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA or see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/

/* diod_conf.c - config registry for distributed I/O daemon */

/* Attributes are set with the following precedence:
 *
 *    command line, config file, compiled-in default
 *
 * Users should call:
 * 1) diod_conf_init () - sets initial defaults
 * 2) diod_config_init_config_file () - override defaults with config file
 * 3) diod_conf_set_* - override config file with command line
 *
 * Config file can be reloaded on SIGHUP - any command line settings
 * will be protected from update with readonly flag (see below)
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#if HAVE_LUA_H
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#endif
#include <stdarg.h>
#include <ctype.h>
#include <signal.h>
#include <pthread.h>

#include "list.h"

#include "diod_conf.h"
#include "diod_log.h"

#ifndef _PATH_PROC_MOUNTS
#define _PATH_PROC_MOUNTS "/proc/mounts"
#endif

/* ro_mask values to protect attribute from overwrite by config file */
#define RO_DEBUGLEVEL       0x0001
#define RO_NWTHREADS        0x0002
#define RO_FOREGROUND       0x0004
#define RO_AUTH_REQUIRED    0x0008
#define RO_RUNASUID         0x0010
#define RO_DIODPATH         0x0020
#define RO_DIODLISTEN       0x0040
#define RO_DIODCTLLISTEN    0x0080
#define RO_EXPORTS          0x0100
#define RO_STATSLOG         0x0200
#define RO_CONFIGPATH       0x0400
#define RO_LOGDEST          0x0800
#define RO_EXPORTALL        0x1000
#define RO_ALLSQUASH        0x2000

typedef struct {
    int          debuglevel;
    int          nwthreads;
    int          foreground;
    int          auth_required;
    int          allsquash;
    uid_t        runasuid;
    char        *diodpath;
    List         diodlisten;
    List         diodctllisten;
    int          exportall;
    List         exports;
    char        *configpath;
    char        *logdest;
    int         ro_mask; 
} Conf;

static Conf config;

static char *
_xstrdup (char *s)
{
    char *cpy = strdup (s);
    if (!cpy)
        msg_exit ("out of memory");
    return cpy;
}

static List
_xlist_create (ListDelF f)
{
    List new = list_create (f);
    if (!new)
        msg_exit ("out of memory");
    return new;
}

static void
_xlist_append (List l, void *item)
{
    if (!list_append (l, item))
        msg_exit ("out of memory");
}

static Export *
_create_export (char *path)
{
    Export *x = malloc (sizeof (*x));
    if (!x)
        return NULL;
    if (!(x->path = strdup (path))) {
        free (x);
        return NULL;
    }
    x->opts = NULL;
    x->hosts = NULL;
    x->users = NULL;
    return x;
}

static Export *
_xcreate_export (char *path)
{
    Export *x = _create_export (path);
    if (!x)
        msg_exit ("out of memory");
    return x;
}

static void
_destroy_export (Export *x)
{
    free (x->path);
    if (x->opts)
        free (x->opts);
    if (x->hosts)
        free (x->hosts);
    if (x->users)
        free (x->users);
    free (x);
}

void
diod_conf_init (void)
{
    config.debuglevel = 0;
    config.nwthreads = 16;
    config.foreground = 0;
    config.auth_required = 1;
    config.allsquash = 0;
    config.runasuid = 0;
    config.diodpath = _xstrdup (X_SBINDIR "/diod");
    config.diodlisten = _xlist_create ((ListDelF)free);
    config.diodctllisten = _xlist_create ((ListDelF)free);
    _xlist_append (config.diodctllisten, _xstrdup ("0.0.0.0:10005"));
    config.exports = _xlist_create ((ListDelF)_destroy_export);
    config.exportall = 0;
#ifdef HAVE_LUA_H
    config.configpath = _xstrdup (X_SYSCONFDIR "/diod.conf");
#else
    config.configpath = NULL;
#endif
    config.logdest = NULL;
    config.ro_mask = 0;
}

void
diod_conf_fini (void)
{
    if (config.diodpath)
        free (config.diodpath);
    if (config.diodlisten)
        list_destroy (config.diodlisten);
    if (config.diodctllisten)
        list_destroy (config.diodctllisten);
    if (config.exports)
        list_destroy (config.exports);
    if (config.configpath)
        free (config.configpath);
    if (config.logdest)
        free (config.logdest);
}

/* logdest - logging destination
 */
char *diod_conf_get_logdest (void) { return config.logdest; }
int diod_conf_opt_logdest (void) { return config.ro_mask & RO_LOGDEST; }
void diod_conf_set_logdest (char *s)
{
    if (config.logdest)
        free (config.logdest);
    config.logdest = _xstrdup (s);
    config.ro_mask |= RO_LOGDEST;
}

/* configpath - config file path
 *    (set in diod_conf_init_config_file)
 */
char *diod_conf_get_configpath (void) { return config.configpath; }
int diod_conf_opt_configpath (void) { return config.ro_mask & RO_CONFIGPATH; }

/* debuglevel - turn debug channels on/off
 */
int diod_conf_get_debuglevel (void) { return config.debuglevel; }
int diod_conf_opt_debuglevel (void) { return config.ro_mask & RO_DEBUGLEVEL; }
void diod_conf_set_debuglevel (int i)
{
    config.debuglevel = i;
    config.ro_mask |= RO_DEBUGLEVEL;
}

/* nwthreads - number of worker threads to spawn in libnpfs
 */
int diod_conf_get_nwthreads (void) { return config.nwthreads; }
int diod_conf_opt_nwthreads (void) { return config.ro_mask & RO_NWTHREADS; }
void diod_conf_set_nwthreads (int i)
{
    config.nwthreads = i;
    config.ro_mask |= RO_NWTHREADS;
}

/* foreground - run daemon in foreground
 */
int diod_conf_get_foreground (void) { return config.foreground; }
int diod_conf_opt_foreground (void) { return config.ro_mask & RO_FOREGROUND; }
void diod_conf_set_foreground (int i)
{
    config.foreground = i;
    config.ro_mask |= RO_FOREGROUND;
}

/* auth_required - whether to accept unauthenticated attaches
 */
int diod_conf_get_auth_required (void) { return config.auth_required; }
int diod_conf_opt_auth_required (void) { return config.ro_mask & RO_AUTH_REQUIRED; }
void diod_conf_set_auth_required (int i)
{
    config.auth_required = i;
    config.ro_mask |= RO_AUTH_REQUIRED;
}

/* allsquash - run server as nobody:nobody and remap all attaches
 */
int diod_conf_get_allsquash (void) { return config.allsquash; }
int diod_conf_opt_allsquash (void) { return config.ro_mask & RO_ALLSQUASH; }
void diod_conf_set_allsquash (int i)
{
    config.allsquash = i;
    config.ro_mask |= RO_AUTH_REQUIRED;
}

/* runasuid - set to run server as one user (mount -o access=uid)
 */
uid_t diod_conf_get_runasuid (void) { return config.runasuid; }
int diod_conf_opt_runasuid (void) { return config.ro_mask & RO_RUNASUID; }
void diod_conf_set_runasuid (uid_t uid)
{
    config.runasuid = uid;
    config.ro_mask |= RO_RUNASUID;
}

/* diodpath - path to diod executable for diodctl
 */
char *diod_conf_get_diodpath (void) { return config.diodpath; }
int diod_conf_opt_diodpath (void) { return config.ro_mask & RO_DIODPATH; }
void diod_conf_set_diodpath (char *s)
{
    free (config.diodpath);
    config.diodpath = _xstrdup (s);
    config.ro_mask |= RO_DIODPATH;
}

/* diodlisten - list of host:port strings for diod to listen on.
 */
List diod_conf_get_diodlisten (void) { return config.diodlisten; }
int diod_conf_opt_diodlisten (void) { return config.ro_mask & RO_DIODLISTEN; }
void diod_conf_clr_diodlisten (void)
{
    list_destroy (config.diodlisten);
    config.diodlisten = _xlist_create ((ListDelF)free);
    config.ro_mask |= RO_DIODLISTEN;
}

/* diodctllisten - list of host:port strings for diodctl to listen on.
 */
List diod_conf_get_diodctllisten (void) { return config.diodctllisten; }
int diod_conf_opt_diodctllisten (void)
{
    return config.ro_mask & RO_DIODCTLLISTEN;
}
void diod_conf_add_diodlisten (char *s)
{
    _xlist_append (config.diodlisten, _xstrdup (s));
    config.ro_mask |= RO_DIODLISTEN;
}
void diod_conf_clr_diodctllisten (void)
{
    list_destroy (config.diodctllisten);
    config.diodctllisten = _xlist_create ((ListDelF)free);
    config.ro_mask |= RO_DIODCTLLISTEN;
}
void diod_conf_add_diodctllisten (char *s)
{
    _xlist_append (config.diodctllisten, _xstrdup (s));
    config.ro_mask |= RO_DIODCTLLISTEN;
}

/* exportall - export everything in /proc/mounts
 */
int diod_conf_get_exportall (void) { return config.exportall; }
int diod_conf_opt_exportall (void) { return config.ro_mask & RO_EXPORTALL; }
void diod_conf_set_exportall (int i)
{
    config.exportall = i;
    config.ro_mask |= RO_EXPORTALL;
}
List diod_conf_get_mounts (void)
{
    List l = NULL;
    FILE *f = NULL;
    Export *x;
    char *p, *path, buf[1024];

    if (!config.exportall)
        goto error;
    if (!(l = list_create ((ListDelF)_destroy_export)))
        goto error;
    if (!(f = fopen (_PATH_PROC_MOUNTS, "r")))
        goto error;
    while (fgets (buf, sizeof(buf), f) != NULL) {
        if (buf[strlen(buf) - 1] == '\n')
            buf[strlen(buf) - 1] = '\0';
        if (!(p = strchr (buf, ' ')))
            continue;
        path = p + 1;
        if (!(p = strchr (path, ' ')))
            continue;
        *p = '\0';
        if (!(x = _create_export (path)))
            goto error;
        if (!list_append (l, x)) {
            _destroy_export (x);
            goto error;
        }
    }
    fclose (f);
    return l;
error:
    if (f)
        fclose (f);
    if (l)
        list_destroy (l);
    return NULL;
}

/* exports - list of paths of exported file systems
 */
List diod_conf_get_exports (void) { return config.exports; }
int diod_conf_opt_exports (void) { return config.ro_mask & RO_EXPORTS; }
void diod_conf_clr_exports (void)
{
    list_destroy (config.exports);
    config.exports = _xlist_create ((ListDelF)_destroy_export);
    config.ro_mask |= RO_EXPORTS;
}
void diod_conf_add_exports (char *path)
{
    Export *x = _xcreate_export (path);
    _xlist_append (config.exports, x);
    config.ro_mask |= RO_EXPORTS;
}
void diod_conf_validate_exports (void)
{
    ListIterator itr;
    Export *x;

    if (config.exportall == 0 && list_count (config.exports) == 0)
        msg_exit ("no exports defined");
    if ((itr = list_iterator_create (config.exports)) == NULL)
        msg_exit ("out of memory");
    while ((x = list_next (itr))) {
        if (*x->path != '/')
            msg_exit ("exports should begin with '/'");
        if (strstr (x->path, "/..") != 0)
            msg_exit ("exports should not contain '/..'"); /* FIXME */
    }
    list_iterator_destroy (itr);
}

#ifdef HAVE_LUA_H
static void
_parse_expopt (char *s, int *fp)
{
    int flags = 0;
    char *cpy, *item;
    char *saveptr = NULL;

    if (!(cpy = strdup (s)))
        msg_exit ("out of memory");
    item = strtok_r (cpy, ",", &saveptr);
    while (item) {
        if (!strcmp (item, "ro"))
            flags |= XFLAGS_RO;
        else
            msg_exit ("unknown export option: %s", item);
        item = strtok_r (NULL, ",", &saveptr);
    }
    free (cpy);
    *fp = flags;
}

static int
_lua_getglobal_int (char *path, lua_State *L, char *key, int *ip)
{
    int res = 0;

    lua_getglobal (L, key);
    if (!lua_isnil (L, -1)) {
        if (!lua_isnumber (L, -1))
            msg_exit ("%s: `%s' should be number", path, key);
        if (ip)
            *ip = (int)lua_tonumber (L, -1);
        res = 1;
    }
    lua_pop (L, 1);

    return res;
}

static int
_lua_getglobal_string (char *path, lua_State *L, char *key, char **sp)
{
    int res = 0;
    char *cpy;

    lua_getglobal (L, key);
    if (!lua_isnil (L, -1)) {
        if (!lua_isstring (L, -1))
            msg_exit ("%s: `%s' should be string", path, key);
        if (sp) {
            cpy = _xstrdup ((char *)lua_tostring (L, -1));
            if (*sp != NULL)
                free (*sp);
            *sp = cpy;
        }
        res = 1;
    }
    lua_pop (L, 1);

    return res;
}

static int
_lua_getglobal_list_of_strings (char *path, lua_State *L, char *key, List *lp)
{
    int res = 0;
    int i;
    List l;

    lua_getglobal (L, key);
    if (!lua_isnil (L, -1)) {
        if (!lua_istable(L, -1))
            msg_exit ("%s: `%s' should be table", path, key);
        l = _xlist_create ((ListDelF)free);
        for (i = 1; ;i++) {
            lua_pushinteger(L, (lua_Integer)i);
            lua_gettable (L, -2);
            if (lua_isnil (L, -1))
                break;
            if (!lua_isstring (L, -1))
                msg_exit ("%s: `%s[%d]' should be string", path, key, i);
            _xlist_append (l, _xstrdup ((char *)lua_tostring (L, -1)));
            lua_pop (L, 1);
        }
        lua_pop (L, 1);
        if (lp) {
            if (*lp != NULL)
                list_destroy (*lp);
            *lp = l;
        } else
            list_destroy (l);
        res = 1;
    }
    lua_pop (L, 1);

    return res;
}

static void
_lua_get_expattr (char *path, int i, lua_State *L, char *key, char **sp)
{
    lua_getfield (L, -1, key);
    if (!lua_isnil (L, -1)) {
         if (!lua_isstring (L, -1))
            msg_exit ("%s: `exports[%d].%s' requires string value",
                      path, i, key);
         *sp = _xstrdup ((char *)lua_tostring (L, -1));
    }
    lua_pop (L, 1);
}

static int
_lua_getglobal_exports (char *path, lua_State *L, List *lp)
{
    Export *x;
    int res = 0;
    int i;
    List l;

    lua_getglobal (L, "exports");
    if (!lua_isnil (L, -1)) {
        if (!lua_istable(L, -1))
            msg_exit ("%s: `exports' should be table", path);
        l = _xlist_create ((ListDelF)_destroy_export);
        for (i = 1; ;i++) {
            lua_pushinteger(L, (lua_Integer)i);
            lua_gettable (L, -2);
            if (lua_isnil (L, -1))
                break;
            if (lua_isstring (L, -1)) {
                x = _xcreate_export ((char *)lua_tostring (L, -1));
                _xlist_append (l, x);
            } else if (lua_istable(L, -1)) {
                char *p = NULL;
                _lua_get_expattr (path, i, L, "path", &p);
                if (!p)
                    msg_exit ("%s: `exports[%d]' path is a required attribute",
                              path, i);
                x = _xcreate_export (p);
                free (p);
                _lua_get_expattr (path, i, L, "opts", &x->opts);
                if (x->opts)
                    _parse_expopt (x->opts, &x->oflags);
                _lua_get_expattr (path, i, L, "users", &x->users);
                _lua_get_expattr (path, i, L, "hosts", &x->hosts);
                /* FIXME: check for illegal export attributes */
                _xlist_append (l, x);
            } else
                msg_exit ("%s: `exports[%d]' should be string/table", path, i);
            lua_pop (L, 1);
        }
        lua_pop (L, 1);
        if (lp) {
            if (*lp != NULL)
                list_destroy (*lp);
            *lp = l;
        } else
            list_destroy (l);
        res = 1;
    }
    lua_pop (L, 1);

    return res;
}

void
diod_conf_init_config_file (char *path) /* FIXME: ENOMEM is fatal */
{
    if (path) {
        if (config.configpath)
            free (config.configpath);
        config.configpath = _xstrdup (path);
        config.ro_mask |= RO_CONFIGPATH;
    } else {
        if (config.configpath && access (config.configpath, R_OK) == 0)
            path = config.configpath;  /* missing default file is not fatal */
    }
    if (path) {
    	lua_State *L = lua_open ();

        luaopen_base (L);
        luaopen_table (L);
        //luaopen_io (L);
        luaopen_string (L);
        luaopen_math (L);

        if (luaL_loadfile (L, path) || lua_pcall (L, 0, 0, 0))
            msg_exit ("%s", lua_tostring (L, -1));

        /* Don't override cmdline options when rereading config file.
         */
        if (!(config.ro_mask & RO_NWTHREADS)) {
            _lua_getglobal_int (path, L, "nwthreads",
                                &config.nwthreads);
        }
        if (!(config.ro_mask & RO_AUTH_REQUIRED)) {
            _lua_getglobal_int (path, L, "auth_required",
                                &config.auth_required);
        }
        if (!(config.ro_mask & RO_ALLSQUASH)) {
            _lua_getglobal_int (path, L, "allsquash",
                                &config.allsquash);
        }
        if (!(config.ro_mask & RO_DIODCTLLISTEN)) {
            _lua_getglobal_list_of_strings (path, L, "diodctllisten",
                                &config.diodctllisten);
        }
        if (!(config.ro_mask & RO_DIODLISTEN)) {
            _lua_getglobal_list_of_strings (path, L, "diodlisten",
                                &config.diodlisten);
        }
        if (!(config.ro_mask & RO_LOGDEST)) {
            _lua_getglobal_string (path, L, "logdest",
                                &config.logdest);
        }
        if (!(config.ro_mask & RO_EXPORTALL)) {
            _lua_getglobal_int (path, L, "exportall",
                                &config.exportall);
        }
        if (!(config.ro_mask & RO_EXPORTS))
            _lua_getglobal_exports (path, L, &config.exports);
        lua_close(L);
    }
}
#else
/* Allow no lua config + empty config file.
 * This is to allow regression tests to specify -c /dev/null and work
 * even if there is no LUA installed.
 */
void
diod_conf_init_config_file (char *path)
{
    struct stat sb;

    if (path) {
        if (config.configpath)
            free (config.configpath);
        config.configpath = _xstrdup (path);
        config.ro_mask |= RO_CONFIGPATH;
    } else {
        if (config.configpath && access (config.configpath, R_OK) == 0)
            path = config.configpath;  /* missing default file is not fatal */
    }
    if (path) {
        if (stat (path, &sb) < 0)
            err_exit ("%s", path);
        if (sb.st_size > 0)
            msg_exit ("no LUA suport - cannot parse contents of %s", path);
        if (config.configpath)
            free (config.configpath);
    }
}
#endif /* HAVE_LUA_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
