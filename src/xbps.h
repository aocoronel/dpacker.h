#include "dpacker.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <sys/types.h>
#include <xbps.h>

DPacker_Pkg_List config_native_packages = { 0 };
DPacker_Pkg_List config_user_packages = { 0 };

int foreach_dict(struct xbps_handle *xhp,
                 xbps_object_t obj,
                 const char *key,
                 void *arg,
                 bool *loop_done) {
    (void)xhp, (void)key, (void)arg, (void)loop_done;

    const char *pkgname = NULL;
    pkg_state_t state;
    uint64_t instsize;
    size_t i = 0;
    bool found = false;

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

    // Same as alpm version, user provided code gotta be vendored and built locally
    // This way it's more reliable to check rather the repository is a filepath or an URL
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
        da_append(&DPACKER.installed_user, DPACKER_CONFIG.sudo);

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

    size_t i = 0;
    int rv = 0;
    struct xbps_handle xh = { 0 };
    char *pkg;

#define prepare_change_cmd(pkg) cmd_change_mode[4] = pkg
    char *cmd_change_mode[6] = { DPACKER_CONFIG.sudo, "xbps-pkgdb", "-m", "manual", NULL, NULL };

    if (VOID_CONFIG.rootdir) xbps_strlcpy(xh.rootdir, VOID_CONFIG.rootdir, sizeof(xh.rootdir));
    if (VOID_CONFIG.cachedir) xbps_strlcpy(xh.cachedir, VOID_CONFIG.cachedir, sizeof(xh.cachedir));
    if (VOID_CONFIG.confdir) xbps_strlcpy(xh.confdir, VOID_CONFIG.confdir, sizeof(xh.confdir));

    if (user) {
        for (i = 0; user[i]; i++) {
            if (strcmp("0", user[i]) == 0) continue;
            dpacker_split_string_into_da(&config_user_packages, user[i]);
        }
    }

    for (i = 0; native[i]; i++) {
        if (strcmp("0", native[i]) == 0) continue;
        dpacker_split_string_into_da(&config_native_packages, native[i]);
    }

    if ((rv = xbps_init(&xh)) != 0) {
        xbps_error_printf("failed to initialize libxbps: %s\n", strerror(rv));
        exit(1);
    }

    rv = xbps_pkgdb_foreach_cb(&xh, foreach_dict, NULL);

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
            prepare_change_cmd(pkg);
            dpacker_sh(cmd_change_mode);
            continue;
        }
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
            prepare_change_cmd(pkg);
            dpacker_sh(cmd_change_mode);
            continue;
        }
    }

    if (user) {
        da_free(&config_user_packages);
    }

    da_free(&config_native_packages);

    xbps_end(&xh);

    return rv == 0 ? NULL : strerror(rv);
}
