#ifndef PACMIRROR_H_
#define PACMIRROR_H_

#define _GNU_SOURCE
#define _XOPEN_SOURCE 600
#include <alpm.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define COLOR_GREEN "\x1b[32m"
#define COLOR_BOLD "\x1b[1m"
#define COLOR_RESET "\x1b[0m"

#define da_append(da, name)                                             \
        if (da->count >= da->cap) {                                     \
                da->cap = da->cap == 0 ? 8 : da->cap * 2;               \
                da->data = realloc(da->data, da->cap * sizeof(char *)); \
        }                                                               \
        da->data[da->count++] = strdup(name);

#define da_append_l(da, name)                                        \
        if (da.count >= da.cap) {                                    \
                da.cap = da.cap == 0 ? 8 : da.cap * 2;               \
                da.data = realloc(da.data, da.cap * sizeof(char *)); \
        }                                                            \
        da.data[da.count++] = strdup(name);

#define da_append_null(da)                                              \
        if (da->count >= da->cap) {                                     \
                da->cap = da->cap == 0 ? 8 : da->cap * 2;               \
                da->data = realloc(da->data, da->cap * sizeof(char *)); \
        }                                                               \
        da->data[da->count++] = NULL;

#define da_append_null_l(da)                                         \
        if (da.count >= da.cap) {                                    \
                da.cap = da.cap == 0 ? 8 : da.cap * 2;               \
                da.data = realloc(da.data, da.cap * sizeof(char *)); \
        }                                                            \
        da.data[da.count++] = NULL;

#define da_free(da)                             \
        for (size_t i = 0; i < da.count; i++) { \
                free(da.data[i]);               \
        }

char AUR_HELPER[256] = "none";
char SUDO[256] = "sudo";

typedef struct {
        size_t count;
        size_t cap;
        char **data;
} DynArray;

typedef struct {
        DynArray pacman;
        DynArray aur;
        DynArray rm;
} Packages;

static bool parse_args(int argc, char **argv) {
        int opt = 0;
        // a --> AUR Helper (paru/yay/pikaur)
        // p --> Privileage Escalation Tool (sudo/doas)
        while ((opt = getopt(argc, argv, ":a:p:")) != -1) {
                switch (opt) {
                case 'a':
                        strcpy(AUR_HELPER, optarg);
                        break;
                case 'p':
                        strcpy(SUDO, optarg);
                        break;
                case ':':
                        printf("option '%c' needs a value\n", opt);
                        return false;
                case '?':
                        printf("unknown option: %c\n", optopt);
                        return false;
                }
        }
        return true;
}

static void split_string_into_da(DynArray *da, const char *str) {
        char *copy = strdup(str);
        char *token = strtok(copy, " ");
        while (token) {
                da_append(da, token);
                token = strtok(NULL, " ");
        }
        free(copy);
}

/*
 * Initialize a generic dynamic array
 */
void init_da(DynArray *da) {
        da->count = 0;
        da->cap = 16;
        da->data = malloc(da->cap * sizeof *da->data);
}

static Packages *init_packages(Packages *p, bool use_aur) {
        init_da(&p->pacman);
        init_da(&p->rm);

        if (use_aur) {
                init_da(&p->aur);
                da_append_l(p->aur, AUR_HELPER);
                da_append_l(p->aur, "-S");
        }

        da_append_l(p->pacman, SUDO);
        da_append_l(p->pacman, "pacman");
        da_append_l(p->pacman, "-S");

        da_append_l(p->rm, SUDO);
        da_append_l(p->rm, "pacman");
        da_append_l(p->rm, "-Rns");

        return p;
}

// Copy pasted from pacman source code:
// https://gitlab.archlinux.org/pacman/pacman/-/blob/master/src/pacman/query.c
static unsigned short pkg_get_locality(alpm_pkg_t *pkg, alpm_handle_t *handle) {
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

static bool is_installed(const char *name, alpm_list_t *list) {
        for (alpm_list_t *node = list; node; node = alpm_list_next(node)) {
                alpm_pkg_t *pkg = node->data;
                if (strcmp(alpm_pkg_get_name(pkg), name) == 0) {
                        return true;
                }
        }
        return false;
}

static bool get_explicitly_installed_pkgs(Packages *packages, char **pacman, char **aur,
                                          bool use_aur) {
        init_packages(packages, use_aur);

        // pacman and aur static arrays:
        DynArray pacman_config_pkgs = { 0, 0, NULL };
        DynArray aur_config_pkgs = { 0, 0, NULL };

        // "0" is ignored, so we can iterate till NULL instead
        if (use_aur) {
                for (size_t i = 0; aur[i]; i++) {
                        if (strcmp(aur[i], "0") == 0) continue;
                        split_string_into_da(&aur_config_pkgs, aur[i]);
                }
        }

        for (size_t i = 0; pacman[i]; i++) {
                if (strcmp(pacman[i], "0") == 0) continue;
                split_string_into_da(&pacman_config_pkgs, pacman[i]);
        }

        alpm_errno_t error;
        alpm_handle_t *handle = alpm_initialize("/", "/var/lib/pacman", &error);
        if (!handle) {
                fprintf(stderr,
                        "Database is locked. Maybe another pacman process is running? (%d)\n",
                        error);
                return false;
        }

        // Registering is important so alpm can figure out if a package is not from
        // any of these repositories
        //
        // For now, it's just hardcoded, because a proper solution would require
        // parsing the pacman.conf

        // Arch Linux
        alpm_register_syncdb(handle, "core", 0);
        alpm_register_syncdb(handle, "extra", 0);
        alpm_register_syncdb(handle, "multilib", 0);

        // Artix Linux
        alpm_register_syncdb(handle, "galaxy", 0);
        alpm_register_syncdb(handle, "lib32", 0);
        alpm_register_syncdb(handle, "system", 0);
        alpm_register_syncdb(handle, "world", 0);
#ifdef ARTIX_GREMLINS
        alpm_register_syncdb(handle, "galaxy-gremlins", 0);
        alpm_register_syncdb(handle, "lib32-gremlins", 0);
        alpm_register_syncdb(handle, "system-gremlins", 0);
        alpm_register_syncdb(handle, "world-gremlins", 0);
#endif

        alpm_db_t *localdb = alpm_get_localdb(handle);
        alpm_list_t *list = alpm_db_get_pkgcache(localdb);
        if (!list) {
                fprintf(stderr, "Failed to get the package cache from the database (%d)\n", error);
                alpm_release(handle);
                return false;
        }

        for (alpm_list_t *node = list; node; node = alpm_list_next(node)) {
                bool found = false;
                alpm_pkg_t *pkg = node->data;
                const char *name = alpm_pkg_get_name(pkg);

                if (alpm_pkg_get_reason(pkg) != ALPM_PKG_REASON_EXPLICIT) continue;

                if (pkg_get_locality(pkg, handle)) { // Foreign
                        if (use_aur) {
                                for (size_t i = 0; i < aur_config_pkgs.count; i++) {
                                        const char *cfg = aur_config_pkgs.data[i];

                                        if (strcmp(cfg, name) == 0) {
                                                found = true;
                                                break;
                                        }
                                }

                                if (!found) {
                                        da_append_l(packages->rm, name);
                                }
                        } else {
                                // Remove all AUR packages
                                da_append_l(packages->rm, name);
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
                                da_append_l(packages->rm, name);
                        }
                }
        }

        if (use_aur) {
                for (size_t i = 0; i < aur_config_pkgs.count; i++) {
                        const char *cfg = aur_config_pkgs.data[i];
                        if (!is_installed(cfg, list)) {
                                da_append_l(packages->aur, cfg);
                        }
                }
        }

        for (size_t i = 0; i < pacman_config_pkgs.count; i++) {
                const char *cfg = pacman_config_pkgs.data[i];
                if (!is_installed(cfg, list)) {
                        da_append_l(packages->pacman, cfg);
                }
        }

        // Cleanup
        alpm_unregister_all_syncdbs(handle);
        alpm_release(handle);

        if (use_aur) {
                da_free(aur_config_pkgs);
                free(aur_config_pkgs.data);
                da_append_null_l(packages->aur);
        }

        da_free(pacman_config_pkgs);
        free(pacman_config_pkgs.data);

        da_append_null_l(packages->pacman);
        da_append_null_l(packages->rm);

        return true;
}

static int wait_for(pid_t pid) {
        int status;
        waitpid(pid, &status, 0);
        return WEXITSTATUS(status);
}

static int fork_exec(char **argv) {
        pid_t pid = fork();
        if (pid == -1) {
                return false;
        }
        if (pid == 0) {
                execvp(argv[0], argv);
                perror("execvp");
                exit(127);
        }
        int status = wait_for(pid);
        return status;
}

static void synchronize_packages(Packages *pkgs, bool use_aur) {
        // 4 --> sudo pacman -S ... NULL
        if (pkgs->pacman.count > 4) {
                printf("%sInstalling pacman packages:%s %zu\n", COLOR_GREEN, COLOR_RESET,
                       pkgs->pacman.count - 4);
                fork_exec(pkgs->pacman.data);
        } else {
                printf("%spacman packages:%s there is nothing to do\n", COLOR_BOLD, COLOR_RESET);
        }

        // 3 --> yay -S ... NULL
        if (use_aur) {
                if (pkgs->aur.count > 3) {
                        printf("%sInstalling AUR packages:%s %zu\n", COLOR_GREEN, COLOR_RESET,
                               pkgs->aur.count - 3);
                        fork_exec(pkgs->aur.data);
                } else {
                        printf("%sAUR packages:%s there is nothing to do\n", COLOR_BOLD,
                               COLOR_RESET);
                }
        }

        // 4 --> sudo pacman -Rns ... NULL
        if (pkgs->rm.count > 4) {
                printf("%sRemoving packages:%s %zu\n", COLOR_GREEN, COLOR_RESET,
                       pkgs->rm.count - 4);
                da_append_null_l(pkgs->rm);
                fork_exec(pkgs->rm.data);
        }

        if (use_aur) {
                da_free(pkgs->aur);
                free(pkgs->aur.data);
        }

        da_free(pkgs->pacman);
        free(pkgs->pacman.data);
        da_free(pkgs->rm);
        free(pkgs->rm.data);
}

int pacmirror(char **pacman, char **aur, int argc, char **argv) {
        char *env = getenv("SUDO");
        if (env) strcpy(SUDO, env);

        if (parse_args(argc, argv) == false) return 1;

        bool use_aur = true;
        if (aur == NULL || strcmp(AUR_HELPER, "none") == 0) {
                use_aur = false;
        }

        Packages pkgs = { 0 };
        bool err = get_explicitly_installed_pkgs(&pkgs, pacman, aur, use_aur);
        if (!err) {
                fprintf(stderr, "[FATAL] Failed to get package list\n");
                return 1;
        }

        synchronize_packages(&pkgs, use_aur);

        return 0;
}
#endif
