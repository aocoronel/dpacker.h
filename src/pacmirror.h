#ifndef PACMIRROR_H_
#define PACMIRROR_H_

#define _GNU_SOURCE
#define _XOPEN_SOURCE 600
#include <assert.h>
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

#define DA_INIT_CAPACITY 256

#define eprintf(...) fprintf(stderr, __VA_ARGS__)

#define AOCLIBS_ABORT(msg, ...)                                    \
    (fprintf(stderr, "%s: %s:%u: ", __func__, __FILE__, __LINE__), \
     fprintf(stderr, msg " " __VA_ARGS__),                         \
     fputc('\n', stderr),                                          \
     abort())

// Asserts an expression, and prints a formatted message
#ifdef NDEBUG
#define ASSERT(...)
#else
#define ASSERT(expr, ...) \
    ((expr) ? (void)0 : AOCLIBS_ABORT("Assertion failed: " #expr, __VA_ARGS__))
#endif

#define ASSERT_NONNULL(exp) ASSERT((exp), "passing NULL pointer to Nonnull parameter")

#define da_reserve(da, new_cap)                                                             \
    do {                                                                                    \
        if ((new_cap) > (da)->cap) {                                                        \
            if ((da)->cap < DA_INIT_CAPACITY) {                                             \
                (da)->cap = DA_INIT_CAPACITY;                                               \
            }                                                                               \
            while ((new_cap) > (da)->cap) {                                                 \
                (da)->cap *= 2;                                                             \
            }                                                                               \
            (da)->data = realloc((da)->data, (da)->cap * sizeof(char *));                   \
            assert((da)->data && "out of memory while reserving memory for dynamic array"); \
        }                                                                                   \
    } while (0)

#define da_append(da, name)            \
    da_reserve((da), (da)->count + 1); \
    (da)->data[(da)->count++] = strdup(name);

#define da_append_null(da)             \
    da_reserve((da), (da)->count + 1); \
    assert((da)->data);                \
    (da)->data[(da)->count++] = NULL;

#define da_free(da)                                \
    do {                                           \
        for (size_t i = 0; i < (da)->count; i++) { \
            free((da)->data[i]);                   \
        }                                          \
        free((da)->data);                          \
    } while (0)

char SUDO[256] = "sudo";

typedef struct {
    size_t count;
    size_t cap;
    char **data;
} PackageList;

typedef struct {
    PackageList pacman;
    PackageList aur;
    PackageList rm;
} Packages;

typedef struct {
    int explicit;
    int dependency;

    off_t used_size;
} pkg_metadata;

static bool parse_args(int argc, char **argv) {
    ASSERT_NONNULL(argv);

    int opt = 0;
    while ((opt = getopt(argc, argv, ":p:")) != -1) {
        switch (opt) {
        case 'p':
            // p --> Privileage Escalation Tool (sudo/doas)
            strcpy(SUDO, optarg);
            break;
        case ':':
            eprintf("error: option '%c' needs a value\n", opt);
            return false;
        case '?':
            eprintf("error: unknown option: '%c'\n", optopt);
            return false;
        }
    }
    return true;
}

static void split_string_into_da(PackageList *da, const char *str) {
    ASSERT_NONNULL(da);
    ASSERT_NONNULL(str);
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
PackageList init_da(void) {
    return (PackageList){ 0 };
}

static Packages *init_packages(Packages *p) {
    ASSERT_NONNULL(p);
    p->pacman = init_da();
    p->rm = init_da();
    p->aur = init_da();

    da_append(&p->aur, "makepkg");
    da_append(&p->aur, "-si");
    da_append(&p->aur, "--dir");

    da_append(&p->pacman, SUDO);
    da_append(&p->pacman, "pacman");
    da_append(&p->pacman, "-S");

    da_append(&p->rm, SUDO);
    da_append(&p->rm, "pacman");
    da_append(&p->rm, "-Rns");

    return p;
}

// Copy pasted from pacman source code:
// https://gitlab.archlinux.org/pacman/pacman/-/blob/master/src/pacman/query.c
static unsigned short pkg_get_locality(alpm_pkg_t *pkg, alpm_handle_t *handle) {
    ASSERT_NONNULL(pkg);
    ASSERT_NONNULL(handle);
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
    ASSERT_NONNULL(name);
    ASSERT_NONNULL(list);
    for (alpm_list_t *node = list; node; node = alpm_list_next(node)) {
        alpm_pkg_t *pkg = node->data;
        if (strcmp(alpm_pkg_get_name(pkg), name) == 0) {
            return true;
        }
    }
    return false;
}

static int
get_explicitly_installed_pkgs(Packages *packages, char **pacman, char **aur, pkg_metadata *out) {
    ASSERT_NONNULL(packages);
    ASSERT_NONNULL(pacman);
    init_packages(packages);

    // pacman and aur static arrays:
    PackageList pacman_config_pkgs = { 0, 0, NULL };
    PackageList aur_config_pkgs = { 0, 0, NULL };

    // "0" is ignored, so we can iterate till NULL instead
    if (aur) {
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
        eprintf("error: %s\n", alpm_strerror(error));
        return false;
    }

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
        eprintf("error: failed to get the package cache from the database\n");
        alpm_release(handle);
        return false;
    }

    for (alpm_list_t *node = list; node; node = alpm_list_next(node)) {
        bool found = false;
        alpm_pkg_t *pkg = node->data;
        const char *name = alpm_pkg_get_name(pkg);

        off_t pkg_size = alpm_pkg_get_isize(pkg);

        out->used_size += pkg_size;

        if (alpm_pkg_get_reason(pkg) != ALPM_PKG_REASON_EXPLICIT) {
            out->dependency += 1;
            continue;
        }

        out->explicit += 1;

        if (pkg_get_locality(pkg, handle)) { // Foreign
            for (size_t i = 0; i < aur_config_pkgs.count; i++) {
                const char *cfg = aur_config_pkgs.data[i];

                if (strcmp(cfg, name) == 0) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                da_append(&packages->rm, name);
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
                da_append(&packages->rm, name);
            }
        }
    }

    for (size_t i = 0; i < aur_config_pkgs.count; i++) {
        const char *cfg = aur_config_pkgs.data[i];
        size_t cfg_len = strlen(cfg);
        if (!is_installed(cfg, list)) {
            char *path = (char *)malloc(cfg_len + 256);
            snprintf(path, cfg_len + 256, "./pkg/%s", cfg);
            da_append(&packages->aur, path);
            free(path);
        }
    }

    for (size_t i = 0; i < pacman_config_pkgs.count; i++) {
        const char *cfg = pacman_config_pkgs.data[i];
        if (!is_installed(cfg, list)) {
            da_append(&packages->pacman, cfg);
        }
    }

    // Cleanup
    alpm_unregister_all_syncdbs(handle);
    alpm_release(handle);

    da_free(&aur_config_pkgs);
    da_free(&pacman_config_pkgs);

    da_append_null(&packages->pacman);
    da_append_null(&packages->aur);
    da_append_null(&packages->rm);

    return true;
}

static int run(char **argv) {
    ASSERT_NONNULL(argv);
    pid_t pid = fork();
    if (pid == -1) {
        eprintf("error: failed to run %s\n", argv[0]);
        exit(1);
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
}

static void synchronize_packages(Packages *pkgs) {
    ASSERT_NONNULL(pkgs);
    // 4 --> sudo pacman -S ... NULL
    if (pkgs->pacman.count > 4) {
        printf("%sInstalling pacman packages:%s %zu\n",
               COLOR_GREEN,
               COLOR_RESET,
               pkgs->pacman.count - 4);
        run(pkgs->pacman.data);
    } else {
        printf("%spacman packages:%s there is nothing to do\n", COLOR_BOLD, COLOR_RESET);
    }

    // 3 --> makepkg -si --dir ... NULL
    if (pkgs->aur.count > 4) {
        printf("%sInstalling AUR packages:%s %zu\n", COLOR_GREEN, COLOR_RESET, pkgs->aur.count - 4);
        run(pkgs->aur.data);
    } else {
        printf("%sAUR packages:%s there is nothing to do\n", COLOR_BOLD, COLOR_RESET);
    }

    // 4 --> sudo pacman -Rns ... NULL
    if (pkgs->rm.count > 4) {
        printf("%sRemoving packages:%s %zu\n", COLOR_GREEN, COLOR_RESET, pkgs->rm.count - 4);
        da_append_null(&pkgs->rm);
        run(pkgs->rm.data);
    }

    da_free(&pkgs->aur);
    da_free(&pkgs->pacman);
    da_free(&pkgs->rm);
}

int pacmirror(char **pacman, char **aur, int argc, char **argv) {
    ASSERT_NONNULL(pacman);
    ASSERT_NONNULL(argv);

    char *env = getenv("SUDO");
    if (env) {
        assert(strlen(env) < sizeof(SUDO));
        strcpy(SUDO, env);
    }

    if (parse_args(argc, argv) == false) return 1;

    Packages pkgs = { 0 };
    pkg_metadata pkgc = { 0 };
    bool err = get_explicitly_installed_pkgs(&pkgs, pacman, aur, &pkgc);
    if (!err) return 1;

    printf("%d explicit packages, and %d dependencies installed, using a total of %zu MBs\n",
           pkgc.explicit,
           pkgc.dependency,
           pkgc.used_size / 1024 / 1024);

    synchronize_packages(&pkgs);

    return 0;
}
#endif
