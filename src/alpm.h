#ifndef DPACKER_ALPM_H_
#define DPACKER_ALPM_H_

#include "dpacker.h"
#include <alpm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Copy pasted from pacman source code:
// https://gitlab.archlinux.org/pacman/pacman/-/blob/master/src/pacman/query.c
static unsigned short dpacker_alpm_pkg_get_locality(alpm_pkg_t *pkg, alpm_handle_t *handle) {
    dpacker_assert_nonnull(pkg);
    dpacker_assert_nonnull(handle);
    const char *pkgname = alpm_pkg_get_name(pkg);
    alpm_list_t *j;
    alpm_list_t *sync_dbs = alpm_get_syncdbs(handle);

    for (j = sync_dbs; j; j = alpm_list_next(j)) {
        if (alpm_db_get_pkg(j->data, pkgname)) {
            return 0; // native
        }
    }
    return 1; // foreign
}

static bool dpacker_alpm_is_installed(const char *name, alpm_list_t *list) {
    dpacker_assert_nonnull(name);
    dpacker_assert_nonnull(list);
    for (alpm_list_t *node = list; node; node = alpm_list_next(node)) {
        alpm_pkg_t *pkg = node->data;
        if (strcmp(alpm_pkg_get_name(pkg), name) == 0) {
            return true;
        }
    }
    return false;
}

const char *dpacker_alpm_init(void) {
    bool has_sudo = DPACKER_CONFIG.sudo[0] != '\0';

    if (has_sudo) {
        da_append(&DPACKER.installed_native, DPACKER_CONFIG.sudo);
        DPACKER.installed_native.initial_command += 1;

        da_append(&DPACKER.to_remove, DPACKER_CONFIG.sudo);
        DPACKER.to_remove.initial_command += 1;
    }

    da_append(&DPACKER.installed_user, "makepkg");
    da_append(&DPACKER.installed_user, "-si");
    da_append(&DPACKER.installed_user, "--dir");
    DPACKER.installed_user.initial_command += 3;

    da_append(&DPACKER.installed_native, "pacman");
    da_append(&DPACKER.installed_native, "-S");
    DPACKER.installed_native.initial_command += 2;

    da_append(&DPACKER.to_remove, "pacman");
    da_append(&DPACKER.to_remove, "-Rns");
    DPACKER.to_remove.initial_command += 2;

    return NULL;
}

static const char *dpacker_alpm_collect(char **pacman, char **aur) {
    dpacker_assert_nonnull(pacman);

    // pacman and aur static arrays:
    DPacker_Pkg_List pacman_config_pkgs = { 0 };
    DPacker_Pkg_List aur_config_pkgs = { 0 };

    // "0" is ignored, so we can iterate till NULL instead
    if (aur) {
        for (size_t i = 0; aur[i]; i++) {
            if (strcmp(aur[i], "0") == 0) continue;
            dpacker_split_string_into_da(&aur_config_pkgs, aur[i]);
        }
    }

    for (size_t i = 0; pacman[i]; i++) {
        if (strcmp(pacman[i], "0") == 0) continue;
        dpacker_split_string_into_da(&pacman_config_pkgs, pacman[i]);
    }

    alpm_errno_t error;
    alpm_handle_t *handle = alpm_initialize("/", "/var/lib/pacman", &error);
    if (!handle) return alpm_strerror(error);

    // Registering is important so alpm can figure out if a package is not from
    // any of these repositories
    //
    // For now, it's just hardcoded, because a proper solution would require
    // parsing the pacman.conf

    // Arch Linux
#ifdef ARCH
    alpm_register_syncdb(handle, "core", 0);
    alpm_register_syncdb(handle, "extra", 0);

#ifdef MULTILIB
    alpm_register_syncdb(handle, "multilib", 0);
#endif // MULTILIB
#endif // ARCH

    // Artix Linux
#ifdef ARTIX
    alpm_register_syncdb(handle, "galaxy", 0);
    alpm_register_syncdb(handle, "lib32", 0);
    alpm_register_syncdb(handle, "system", 0);
    alpm_register_syncdb(handle, "world", 0);
#ifdef ARTIX_GREMLINS
    alpm_register_syncdb(handle, "galaxy-gremlins", 0);
    alpm_register_syncdb(handle, "lib32-gremlins", 0);
    alpm_register_syncdb(handle, "system-gremlins", 0);
    alpm_register_syncdb(handle, "world-gremlins", 0);
#endif // ARTIX_GREMLINS
#endif // ARTIX

    alpm_db_t *localdb = alpm_get_localdb(handle);
    alpm_list_t *list = alpm_db_get_pkgcache(localdb);
    if (!list) {
        alpm_release(handle);
        return "failed to get the package cache from the database";
    }

    for (alpm_list_t *node = list; node; node = alpm_list_next(node)) {
        bool found = false;
        alpm_pkg_t *pkg = node->data;
        const char *name = alpm_pkg_get_name(pkg);

        off_t pkg_size = alpm_pkg_get_isize(pkg);

        PACKAGE_METADATA.total_used_size += pkg_size;

        if (alpm_pkg_get_reason(pkg) != ALPM_PKG_REASON_EXPLICIT) {
            PACKAGE_METADATA.dependency += 1;
            continue;
        }

        PACKAGE_METADATA.manual += 1;

        if (dpacker_alpm_pkg_get_locality(pkg, handle)) { // Foreign
            for (size_t i = 0; i < aur_config_pkgs.count; i++) {
                const char *cfg = aur_config_pkgs.data[i];

                if (strcmp(cfg, name) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                da_append(&DPACKER.to_remove, name);
            }
        } else { // Native
            for (size_t i = 0; i < pacman_config_pkgs.count; i++) {
                const char *cfg = pacman_config_pkgs.data[i];

                if (strcmp(cfg, name) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                da_append(&DPACKER.to_remove, name);
            }
        }
    }

    for (size_t i = 0; i < aur_config_pkgs.count; i++) {
        const char *cfg = aur_config_pkgs.data[i];
        size_t cfg_len = strlen(cfg);
        if (!dpacker_alpm_is_installed(cfg, list)) {
            char *path = (char *)malloc(cfg_len + 256);
            snprintf(path, cfg_len + 256, "./pkg/%s", cfg);
            da_append(&DPACKER.installed_user, path);
            free(path);
        }
    }

    for (size_t i = 0; i < pacman_config_pkgs.count; i++) {
        const char *cfg = pacman_config_pkgs.data[i];
        if (!dpacker_alpm_is_installed(cfg, list)) {
            da_append(&DPACKER.installed_native, cfg);
        }
    }

    // Cleanup
    alpm_unregister_all_syncdbs(handle);
    alpm_release(handle);

    da_free(&aur_config_pkgs);
    da_free(&pacman_config_pkgs);

    return NULL;
}
#endif // DPACKER_ALPM_H_
