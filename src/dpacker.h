#ifndef DPACKER_H_
#define DPACKER_H_

#define _GNU_SOURCE
#define _XOPEN_SOURCE 600
#include <sys/types.h>
#include <stdio.h>
#include <getopt.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define COLOR_GREEN "\x1b[32m"
#define COLOR_BOLD "\x1b[1m"
#define COLOR_RESET "\x1b[0m"

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define errorf(...) fprintf(stderr, "error: " __VA_ARGS__)

#define dpacker_abort(msg, ...)                                    \
    (fprintf(stderr, "%s: %s:%u: ", __func__, __FILE__, __LINE__), \
     fprintf(stderr, msg " " __VA_ARGS__),                         \
     fputc('\n', stderr),                                          \
     abort())

// Asserts an expression, and prints a formatted message
#ifdef NDEBUG
#define dpacker_assert(...)
#else
#define dpacker_assert(expr, ...) \
    ((expr) ? (void)0 : dpacker_abort("Assertion failed: " #expr, __VA_ARGS__))
#endif

#define dpacker_assert_nonnull(exp) \
    dpacker_assert((exp), "passing NULL pointer to Nonnull parameter")

#define DA_INIT_CAPACITY 256

#define da_reserve(da, new_cap)                                                                   \
    do {                                                                                          \
        if ((new_cap) > (da)->cap) {                                                              \
            if ((da)->cap < DA_INIT_CAPACITY) {                                                   \
                (da)->cap = DA_INIT_CAPACITY;                                                     \
            }                                                                                     \
            while ((new_cap) > (da)->cap) {                                                       \
                (da)->cap *= 2;                                                                   \
            }                                                                                     \
            (da)->data = realloc((da)->data, (da)->cap * sizeof(char *));                         \
            dpacker_assert((da)->data, "out of memory while reserving memory for dynamic array"); \
        }                                                                                         \
    } while (0)

#define da_append(da, name)            \
    da_reserve((da), (da)->count + 1); \
    (da)->data[(da)->count++] = strdup(name);

#define da_append_null(da)             \
    da_reserve((da), (da)->count + 1); \
    dpacker_assert((da)->data);        \
    (da)->data[(da)->count++] = NULL;

#define da_free(da)                                \
    do {                                           \
        for (size_t i = 0; i < (da)->count; i++) { \
            free((da)->data[i]);                   \
        }                                          \
        free((da)->data);                          \
    } while (0)

extern char SUDO[256];

typedef struct {
    size_t count;
    size_t cap;
    char **data;
} DPacker_Pkg_List;

typedef struct {
    // Context to be passed from init() to collect() and sync()
    // This is optional.
    void *ctx;
    // Official packages from distribution
    DPacker_Pkg_List installed_native;
    // User provided packages, like AUR
    DPacker_Pkg_List installed_user;
    // Packages to be uninstalled
    DPacker_Pkg_List to_remove;
} DPacker;

// This serves only for analytical purposes
typedef struct {
    int manual; // manually installed packages, a.k.a explicit
    int dependency;

    off_t total_used_size;
} DPacker_Pkg_Metadata;

typedef struct {
    // Initialize dynamic arrays
    const char *(*init)(DPacker *);
    // Find all packages to be installed, to be removed and dependencies
    const char *(*collect)(DPacker *, char **manual, char **user, DPacker_Pkg_Metadata *out);
    // Install/remove packages
    const char *(*sync)(DPacker *);
} DPacker_Interface;

static bool dpacker_parse_args(int argc, char **argv) {
    dpacker_assert_nonnull(argv);

    int opt = 0;
    while ((opt = getopt(argc, argv, ":p:")) != -1) {
        switch (opt) {
        case 'p':
            // p --> Privileage Escalation Tool (sudo/doas)
            strcpy(SUDO, optarg);
            break;
        case ':':
            errorf("option '%c' needs a value\n", opt);
            return false;
        case '?':
            errorf("unknown option: '%c'\n", optopt);
            return false;
        }
    }

    return true;
}

static void dpacker_split_string_into_da(DPacker_Pkg_List *da, const char *str) {
    dpacker_assert_nonnull(da);
    dpacker_assert_nonnull(str);
    char *copy, *token;
    copy = strdup(str);
    token = strtok(copy, " ");
    while (token) {
        da_append(da, token);
        token = strtok(NULL, " ");
    }
    free(copy);
}

static int dpacker_sh(char **argv) {
    dpacker_assert_nonnull(argv);

    pid_t pid = fork();
    if (pid == -1) {
        errorf("failed to run %s\n", argv[0]);
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

int dpacker(DPacker_Interface interface, char **manual, char **user, int argc, char **argv) {
    dpacker_assert_nonnull(manual);
    dpacker_assert_nonnull(argv);

    char *env = getenv("SUDO");
    if (env) {
        dpacker_assert(strlen(env) < sizeof(SUDO), "$SUDO is too large");
        strcpy(SUDO, env);
    }

    if (dpacker_parse_args(argc, argv) == false) return 1;

    DPacker pkgs = { 0 };
    DPacker_Pkg_Metadata pkg_metadata = { 0 };

#define call(...)                      \
    do {                               \
        {                              \
            const char *err = NULL;    \
            if ((err = __VA_ARGS__)) { \
                errorf("%s\n", err);   \
                return 1;              \
            }                          \
        }                              \
    } while (0)

    call(interface.init(&pkgs));
    call(interface.collect(&pkgs, manual, user, &pkg_metadata));

    printf("%d manual packages, and %d dependencies installed, using a total of %zu MBs\n",
           pkg_metadata.manual,
           pkg_metadata.dependency,
           pkg_metadata.total_used_size / 1024 / 1024);

    call(interface.sync(&pkgs));

    return 0;
}

#endif // DPACKER_H_
