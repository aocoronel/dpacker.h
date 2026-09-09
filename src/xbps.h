#include "dpacker.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <sys/types.h>
#include <xbps.h>

struct list_pkgver_cb {
    unsigned int pkgver_len;
    unsigned int maxcols;
    char *linebuf;
};

DPacker_Pkg_List config_native_packages = { 0 };
DPacker_Pkg_List config_user_packages = { 0 };

int foreach_dict(struct xbps_handle *xhp,
                 xbps_object_t obj,
                 const char *key,
                 void *arg,
                 bool *loop_done) {
    const char *pkgname = NULL, *state_str = NULL;
    pkg_state_t state;
    uint64_t instsize;
    size_t i = 0;
    bool found;

    xbps_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
    xbps_dictionary_get_uint64(obj, "installed_size", &instsize);

    if (!pkgname) return EINVAL;

    PACKAGE_METADATA.total_used_size += instsize;

    xbps_pkg_state_dictionary(obj, &state);

    if (state != XBPS_PKG_STATE_INSTALLED) return 0;

    {
        bool automatic = false;
        xbps_dictionary_get_bool(obj, "automatic-install", &automatic);
        if (automatic) {
            PACKAGE_METADATA.dependency += 1;
            return 0;
        }
        PACKAGE_METADATA.manual += 1;
    }

    const char *repository = NULL;

    xbps_dictionary_get_cstring_nocopy(obj, "repository", &repository);
    if (!repository) {
        exit(1);
    }

    found = false;
    if (repository[0] == '/') { // User
        for (i = 0; i < config_user_packages.count; i++) {
            const char *pkg = config_user_packages.data[i];

            if (strcmp(pkg, pkgname) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            da_append(&DPACKER.to_remove, pkgname);
        }
    } else { // Native
        for (i = 0; i < config_native_packages.count; i++) {
            const char *pkg = config_native_packages.data[i];

            if (strcmp(pkg, pkgname) == 0) {
                found = true;
                break;
            }
        }

        if (!found) {
            da_append(&DPACKER.to_remove, pkgname);
        }
    }

    return 0;
}

const char *dpacker_xbps_init(void) {
    bool has_sudo = DPACKER_CONFIG.sudo[0] != '\0';

    if (has_sudo) {
        da_append(&DPACKER.installed_native, DPACKER_CONFIG.sudo);
        DPACKER.installed_native.initial_command += 1;

        da_append(&DPACKER.to_remove, DPACKER_CONFIG.sudo);
        DPACKER.to_remove.initial_command += 1;
    }

    if (VOID_CONFIG.xbps_src_root) {
        da_append(&DPACKER.installed_user, "dpacker-xbps-src");
        da_append(&DPACKER.installed_user, VOID_CONFIG.xbps_src_root);
        da_append(&DPACKER.installed_user, VOID_CONFIG.user);

        DPACKER.installed_user.initial_command += 3;
    } else {
        da_append(&DPACKER.installed_user, "notify-send");
        da_append(&DPACKER.installed_user, "VOID_CONFIG not set");
        DPACKER.installed_user.initial_command += 1;
    }

    da_append(&DPACKER.installed_native, "xbps-install");
    DPACKER.installed_native.initial_command += 1;
    if (DPACKER_CONFIG.yes) {
        da_append(&DPACKER.installed_native, "-y");
        DPACKER.installed_native.initial_command += 1;
    }

    da_append(&DPACKER.to_remove, "xbps-remove");
    da_append(&DPACKER.to_remove, "-R");
    DPACKER.to_remove.initial_command += 2;
    if (DPACKER_CONFIG.yes) {
        da_append(&DPACKER.to_remove, "-y");
        DPACKER.to_remove.initial_command += 1;
    }

    return NULL;
}

static const char *dpacker_xbps_collect(char **native, char **user) {
    dpacker_assert_nonnull(native);

    int rv = 0;
    struct list_pkgver_cb lpc;
    struct xbps_handle xh;
    const char *rootdir = NULL, *cachedir = NULL, *confdir = NULL;
    size_t i = 0;

    memset(&xh, 0, sizeof(xh));

    lpc.maxcols = 0;
    lpc.linebuf = NULL;

    // TODO: currently always NULL
    if (rootdir) xbps_strlcpy(xh.rootdir, rootdir, sizeof(xh.rootdir));
    if (cachedir) xbps_strlcpy(xh.cachedir, cachedir, sizeof(xh.cachedir));
    if (confdir) xbps_strlcpy(xh.confdir, confdir, sizeof(xh.confdir));

    if (user) {
        for (i = 0; user[i]; i++) {
            dpacker_split_string_into_da(&config_user_packages, user[i]);
        }
    }

    for (i = 0; native[i]; i++) {
        dpacker_split_string_into_da(&config_native_packages, native[i]);
    }

    if ((rv = xbps_init(&xh)) != 0) {
        xbps_error_printf("failed to initialize libxbps: %s\n", strerror(rv));
        exit(1);
    }

    if (xbps_pkgdb_lock(&xh) != 0) return "failed to lock database";
    rv = xbps_pkgdb_foreach_cb(&xh, foreach_dict, &lpc);

    const char *pkg;
    for (i = 0; i < config_user_packages.count; i++) {
        pkg = config_user_packages.data[i];
        dpacker_assert(pkg);

        xbps_dictionary_t dic = xbps_pkgdb_get_pkg(&xh, pkg);
        // Assume that an invalid dictionary is never installed in the system
        if (!dic) {
            da_append(&DPACKER.installed_user, pkg);
            continue;
        }

        bool dependency = false;
        xbps_dictionary_get_bool(dic, "automatic-install", &dependency);

        if (dependency) {
            xbps_dictionary_set_bool(dic, "automatic-install", false);
            continue;
        }

        // size_t pkg_len = strlen(pkg);
        // char *path = (char *)malloc(pkg_len + 256 + 1); // + 1 NULL
        // snprintf(path, pkg_len + 256, "./xbps-src/%s", pkg);
        // da_append(&DPACKER.installed_user, path);
        //
        // free(path);
        // continue;
    }

    for (i = 0; i < config_native_packages.count; i++) {
        pkg = config_native_packages.data[i];
        dpacker_assert(pkg);

        xbps_dictionary_t dic = xbps_pkgdb_get_pkg(&xh, pkg);
        // Assume that an invalid dictionary is never installed in the system
        if (!dic) {
            da_append(&DPACKER.installed_native, pkg);
            continue;
        }

        bool dependency = false;
        xbps_dictionary_get_bool(dic, "automatic-install", &dependency);

        if (dependency) {
            xbps_dictionary_set_bool(dic, "automatic-install", false);
            continue;
        }
    }

    if (user) {
        da_free(&config_user_packages);
    }

    da_free(&config_native_packages);

    if (rv == 0) xbps_pkgdb_update(&xh, true, false);
    xbps_pkgdb_unlock(&xh);
    xbps_end(&xh);

    return rv == 0 ? NULL : strerror(rv);
}
