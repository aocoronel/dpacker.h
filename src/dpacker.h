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

#define COLOR_MAGENTA "\x1b[35m"
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

typedef struct {
    size_t initial_command;
    size_t count;
    size_t cap;
    char **data;
} DPacker_Pkg_List;

typedef struct {
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
    const char *(*init)(void);
    // Find all packages to be installed, to be removed and dependencies
    const char *(*collect)(char **manual, char **user);
} DPacker_Interface;

struct DPacker_Config {
    char sudo[256];
    bool yes;
    bool debug;
    bool dry;
};

DPacker DPACKER;
DPacker_Pkg_Metadata PACKAGE_METADATA;
struct DPacker_Config DPACKER_CONFIG;

static void dpacker_usage(const char *program) {
    char *message = "Usage: %s [OPTION]\n"
                    "\n"
                    "Options:\n"
                    "  -d          Enable debug\n"
                    "  -D          Enable dry mode\n"
                    "  -h          Display this message and exits\n"
                    "  -p [sudo]   Sets privileage escalation tool\n"
                    "  -y          Accepts everything\n";
    printf(message, program);
    exit(0);
}

static bool dpacker_parse_args(int argc, char **argv) {
    dpacker_assert_nonnull(argv);

    char *env = getenv("SUDO");
    if (env) {
        dpacker_assert(strlen(env) < 255, "$SUDO is too large");
        strcpy(DPACKER_CONFIG.sudo, env);
    }

    int opt = 0;
    while ((opt = getopt(argc, argv, ":p:yhdD")) != -1) {
        switch (opt) {
        case 'p':
            // p --> Privileage Escalation Tool (sudo/doas)
            dpacker_assert(strlen(optarg) < 255, "'%s' is too large", optarg);
            strcpy(DPACKER_CONFIG.sudo, optarg);
            break;
        case 'y': // accepts everything
            DPACKER_CONFIG.yes = true;
            break;
        case 'd':
            DPACKER_CONFIG.debug = true;
            break;
        case 'D':
            DPACKER_CONFIG.dry = true;
            break;
        case 'h':
            dpacker_usage(argv[0]);
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

#define range(begin, end) for (size_t it = begin; it < end; it++)
static const char *dpacker_sync(void) {
    da_append_null(&DPACKER.installed_native);
    da_append_null(&DPACKER.to_remove);

    DPacker_Pkg_List native = DPACKER.installed_native;
    DPacker_Pkg_List user = DPACKER.installed_user;
    DPacker_Pkg_List to_remove = DPACKER.to_remove;

    // Increment last time for NULL
    native.count -= 1;
    user.count -= 1;
    to_remove.count -= 1;

    int installing_native = native.count - native.initial_command;
    int installing_user = user.count - user.initial_command;
    int uninstalling = to_remove.count - to_remove.initial_command;

    if (DPACKER_CONFIG.debug) {
        if (native.count > native.initial_command) {
            printf("%sInstalling native packages:%s %d\n",
                   COLOR_MAGENTA,
                   COLOR_RESET,
                   installing_native);
            range(native.initial_command, native.count) {
                printf("%s ", native.data[it]);
            }
            putc('\n', stdout);
        }
    }

    if (native.count > native.initial_command) {
        if (!DPACKER_CONFIG.dry) {
            printf("%sInstalling native packages:%s %d\n",
                   COLOR_GREEN,
                   COLOR_RESET,
                   installing_native);
            dpacker_sh(native.data);
        }
    } else {
        printf("%snative packages:%s there is nothing to do\n", COLOR_BOLD, COLOR_RESET);
    }

    da_free(&DPACKER.installed_native);

    bool has_user = user.cap > 0;

    if (has_user) {
        da_append_null(&DPACKER.installed_user);
        if (DPACKER_CONFIG.debug) {
            if (user.count > user.initial_command) {
                printf("%sInstalling user packages:%s %d\n",
                       COLOR_MAGENTA,
                       COLOR_RESET,
                       installing_user);
                printf("User packages to be installed:\n");
                range(user.initial_command, user.count) {
                    printf("%s ", user.data[it]);
                }
                putc('\n', stdout);
            }
        }

        if (user.count > user.initial_command) {
            if (!DPACKER_CONFIG.dry) {
                printf("%sInstalling user packages:%s %d\n",
                       COLOR_GREEN,
                       COLOR_RESET,
                       installing_user);
                dpacker_sh(user.data);
            }
        } else {
            printf("%suser packages:%s there is nothing to do\n", COLOR_BOLD, COLOR_RESET);
        }

        da_free(&DPACKER.installed_user);
    }

    if (DPACKER_CONFIG.debug) {
        if (to_remove.count > to_remove.initial_command) {
            printf("%sRemoving packages:%s %d\n", COLOR_MAGENTA, COLOR_RESET, uninstalling);
            range(to_remove.initial_command, to_remove.count) {
                printf("%s ", to_remove.data[it]);
            }
            putc('\n', stdout);
        }
    }

    if (to_remove.count > to_remove.initial_command) {
        if (!DPACKER_CONFIG.dry) {
            printf("%sRemoving packages:%s %d\n", COLOR_GREEN, COLOR_RESET, uninstalling);
            dpacker_sh(to_remove.data);
        }
    }

    da_free(&DPACKER.to_remove);

    return NULL;
}

int dpacker(DPacker_Interface interface, char **native, char **user, int argc, char **argv) {
    dpacker_assert_nonnull(native);
    dpacker_assert_nonnull(argv);

    DPACKER_CONFIG.sudo[0] = '\0';
    DPACKER_CONFIG.yes = false;

    if (dpacker_parse_args(argc, argv) == false) return 1;

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

    // default to 'sudo'
    if (DPACKER_CONFIG.sudo[0] == '\0') {
        strcpy(DPACKER_CONFIG.sudo, "sudo");
    }

    call(interface.init());
    call(interface.collect(native, user));

    printf("%d manual packages, and %d dependencies installed, using a total of %zu MBs\n",
           PACKAGE_METADATA.manual,
           PACKAGE_METADATA.dependency,
           PACKAGE_METADATA.total_used_size / 1024 / 1024);

    dpacker_sync();

    return 0;
}
#undef range

#endif // DPACKER_H_
