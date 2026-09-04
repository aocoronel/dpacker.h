#ifndef DPACKER_ALPM_H_
#define DPACKER_ALPM_H_

#include "dpacker.h"
#include <alpm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char SUDO[256] = "sudo";

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

const char *dpacker_alpm_init(DPacker *p) {
    dpacker_assert_nonnull(p);

    da_append(&p->installed_user, "makepkg");
    da_append(&p->installed_user, "-si");
    da_append(&p->installed_user, "--dir");

    da_append(&p->installed_native, SUDO);
    da_append(&p->installed_native, "pacman");
    da_append(&p->installed_native, "-S");

    da_append(&p->to_remove, SUDO);
    da_append(&p->to_remove, "pacman");
    da_append(&p->to_remove, "-Rns");

    return NULL;
}

static const char *
dpacker_alpm_collect(DPacker *packages, char **pacman, char **aur, DPacker_Pkg_Metadata *out) {
    dpacker_assert_nonnull(packages);
    dpacker_assert_nonnull(pacman);

    // pacman and aur static arrays:
    DPacker_Pkg_List pacman_config_pkgs = { 0, 0, NULL };
    DPacker_Pkg_List aur_config_pkgs = { 0, 0, NULL };

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

        out->total_used_size += pkg_size;

        if (alpm_pkg_get_reason(pkg) != ALPM_PKG_REASON_EXPLICIT) {
            out->dependency += 1;
            continue;
        }

        out->manual += 1;

        if (dpacker_alpm_pkg_get_locality(pkg, handle)) { // Foreign
            for (size_t i = 0; i < aur_config_pkgs.count; i++) {
                const char *cfg = aur_config_pkgs.data[i];

                if (strcmp(cfg, name) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                da_append(&packages->to_remove, name);
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
                da_append(&packages->to_remove, name);
            }
        }
    }

    for (size_t i = 0; i < aur_config_pkgs.count; i++) {
        const char *cfg = aur_config_pkgs.data[i];
        size_t cfg_len = strlen(cfg);
        if (!dpacker_alpm_is_installed(cfg, list)) {
            char *path = (char *)malloc(cfg_len + 256);
            snprintf(path, cfg_len + 256, "./pkg/%s", cfg);
            da_append(&packages->installed_user, path);
            free(path);
        }
    }

    for (size_t i = 0; i < pacman_config_pkgs.count; i++) {
        const char *cfg = pacman_config_pkgs.data[i];
        if (!dpacker_alpm_is_installed(cfg, list)) {
            da_append(&packages->installed_native, cfg);
        }
    }

    // Cleanup
    alpm_unregister_all_syncdbs(handle);
    alpm_release(handle);

    da_free(&aur_config_pkgs);
    da_free(&pacman_config_pkgs);

    da_append_null(&packages->installed_native);
    da_append_null(&packages->installed_user);
    da_append_null(&packages->to_remove);

    return NULL;
}

static const char *dpacker_alpm_sync(DPacker *pkgs) {
    dpacker_assert_nonnull(pkgs);
    // 4 --> sudo pacman -S ... NULL
    if (pkgs->installed_native.count > 4) {
        printf("%sInstalling pacman packages:%s %zu\n",
               COLOR_GREEN,
               COLOR_RESET,
               pkgs->installed_native.count - 4);
        dpacker_sh(pkgs->installed_native.data);
    } else {
        printf("%spacman packages:%s there is nothing to do\n", COLOR_BOLD, COLOR_RESET);
    }

    // 3 --> makepkg -si --dir ... NULL
    if (pkgs->installed_user.count > 4) {
        printf("%sInstalling AUR packages:%s %zu\n",
               COLOR_GREEN,
               COLOR_RESET,
               pkgs->installed_user.count - 4);
        dpacker_sh(pkgs->installed_user.data);
    } else {
        printf("%sAUR packages:%s there is nothing to do\n", COLOR_BOLD, COLOR_RESET);
    }

    // 4 --> sudo pacman -Rns ... NULL
    if (pkgs->to_remove.count > 4) {
        printf("%sRemoving packages:%s %zu\n", COLOR_GREEN, COLOR_RESET, pkgs->to_remove.count - 4);
        da_append_null(&pkgs->to_remove);
        dpacker_sh(pkgs->to_remove.data);
    }

    da_free(&pkgs->installed_user);
    da_free(&pkgs->installed_native);
    da_free(&pkgs->to_remove);

    return NULL;
}
#endif // DPACKER_ALPM_H_
