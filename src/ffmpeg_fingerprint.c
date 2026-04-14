/*
 * ffmpeg_fingerprint - Single-command FFmpeg + DVB subtitle fingerprint
 *
 * C wrapper that forks FFmpeg and ts_fingerprint into a managed pipeline,
 * presenting a single process to the user. Handles signals, pipe management,
 * and proper cleanup of both child processes.
 *
 * Architecture:
 *   ffmpeg_fingerprint
 *     |-- fork() --> FFmpeg  (demux/remux, outputs MPEG-TS to pipe)
 *     |-- fork() --> ts_fingerprint (reads pipe, injects DVB subtitles)
 *     \-- (parent) monitors children, forwards signals, manages exit
 *
 * Usage:
 *   ffmpeg_fingerprint -i SOURCE [--zmq ADDR] [--text TEXT] [--position N] [-f FMT] OUTPUT
 *
 * Examples:
 *   ffmpeg_fingerprint -i "udp://239.1.1.1:1234" --zmq tcp://127.0.0.1:5555 -f mpegts pipe:1
 *   ffmpeg_fingerprint -i input.ts --text USERNAME --position 0 -f mpegts output.ts
 *
 * Copyright (c) 2026 - Custom FFmpeg Plugin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define MAX_ARGS        128
#define MAX_PATH_LEN    4096
#define MAX_TEXT_LEN    256

/* ------------------------------------------------------------------ */
/*  Global state for signal handling                                   */
/* ------------------------------------------------------------------ */

static volatile pid_t g_ffmpeg_pid = 0;
static volatile pid_t g_tsfp_pid = 0;
static volatile int   g_shutdown = 0;

/* ------------------------------------------------------------------ */
/*  Signal handler                                                     */
/* ------------------------------------------------------------------ */

static void signal_handler(int sig)
{
    g_shutdown = 1;

    /* Forward signal to children */
    if (g_ffmpeg_pid > 0)
        kill(g_ffmpeg_pid, sig);
    if (g_tsfp_pid > 0)
        kill(g_tsfp_pid, sig);
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* Ignore SIGPIPE - we handle broken pipes via write errors */
    signal(SIGPIPE, SIG_IGN);
}

/* ------------------------------------------------------------------ */
/*  Helper: find ts_fingerprint binary                                 */
/* ------------------------------------------------------------------ */

/*
 * Search for ts_fingerprint in:
 *   1. Same directory as this binary (../bin/ts_fingerprint relative to src/)
 *   2. Directory of this binary + /bin/
 *   3. PATH
 */
static int find_ts_fingerprint(const char *argv0, char *out_path, int max_len)
{
    char self_dir[MAX_PATH_LEN];
    char candidate[MAX_PATH_LEN];

    /* Get directory of this executable */
    /* Try /proc/self/exe first (Linux) */
    ssize_t len = readlink("/proc/self/exe", self_dir, sizeof(self_dir) - 1);
    if (len > 0) {
        self_dir[len] = '\0';
        /* Get directory part */
        char *slash = strrchr(self_dir, '/');
        if (slash) *slash = '\0';
    } else {
        /* Fall back to argv[0] */
        strncpy(self_dir, argv0, sizeof(self_dir) - 1);
        self_dir[sizeof(self_dir) - 1] = '\0';
        char *slash = strrchr(self_dir, '/');
        if (slash) {
            *slash = '\0';
        } else {
            strcpy(self_dir, ".");
        }
    }

    /* 1. Same directory as binary */
    snprintf(candidate, sizeof(candidate), "%s/ts_fingerprint", self_dir);
    if (access(candidate, X_OK) == 0) {
        snprintf(out_path, max_len, "%s", candidate);
        return 0;
    }

    /* 2. Sibling bin/ directory (if we're in bin/ already, try same dir) */
    snprintf(candidate, sizeof(candidate), "%s/../bin/ts_fingerprint", self_dir);
    if (access(candidate, X_OK) == 0) {
        /* Resolve to absolute path */
        char *resolved = realpath(candidate, NULL);
        if (resolved) {
            snprintf(out_path, max_len, "%s", resolved);
            free(resolved);
            return 0;
        }
    }

    /* 3. Parent directory's bin/ */
    snprintf(candidate, sizeof(candidate), "%s/bin/ts_fingerprint", self_dir);
    if (access(candidate, X_OK) == 0) {
        snprintf(out_path, max_len, "%s", candidate);
        return 0;
    }

    /* 4. Try PATH via which-like search */
    const char *path_env = getenv("PATH");
    if (path_env) {
        char path_copy[MAX_PATH_LEN * 4];
        strncpy(path_copy, path_env, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        char *dir = strtok(path_copy, ":");
        while (dir) {
            snprintf(candidate, sizeof(candidate), "%s/ts_fingerprint", dir);
            if (access(candidate, X_OK) == 0) {
                snprintf(out_path, max_len, "%s", candidate);
                return 0;
            }
            dir = strtok(NULL, ":");
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  Helper: find ffmpeg binary                                         */
/* ------------------------------------------------------------------ */

static int find_ffmpeg(char *out_path, int max_len)
{
    /* Check FFMPEG_BIN environment variable first */
    const char *env_ffmpeg = getenv("FFMPEG_BIN");
    if (env_ffmpeg && access(env_ffmpeg, X_OK) == 0) {
        snprintf(out_path, max_len, "%s", env_ffmpeg);
        return 0;
    }

    /* Try common locations */
    const char *candidates[] = {
        "/usr/local/bin/ffmpeg",
        "/usr/bin/ffmpeg",
        "/opt/ffmpeg/bin/ffmpeg",
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(out_path, max_len, "%s", candidates[i]);
            return 0;
        }
    }

    /* Search PATH */
    const char *path_env = getenv("PATH");
    if (path_env) {
        char path_copy[MAX_PATH_LEN * 4];
        strncpy(path_copy, path_env, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        char candidate[MAX_PATH_LEN];
        char *dir = strtok(path_copy, ":");
        while (dir) {
            snprintf(candidate, sizeof(candidate), "%s/ffmpeg", dir);
            if (access(candidate, X_OK) == 0) {
                snprintf(out_path, max_len, "%s", candidate);
                return 0;
            }
            dir = strtok(NULL, ":");
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  Parsed arguments                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Fingerprint-specific */
    const char *zmq_addr;
    const char *fp_text;
    const char *fp_position;

    /* FFmpeg-specific */
    const char *input;
    const char *output;
    const char *output_fmt;

    /* Extra FFmpeg args (reconnect, etc.) */
    const char *ffmpeg_extra[MAX_ARGS];
    int         ffmpeg_extra_count;

    /* Flags */
    int verbose;
    int output_is_pipe;     /* output is pipe:1 or - */
    int output_is_network;  /* output is udp://, rtmp://, etc. */
} ParsedArgs;

/* ------------------------------------------------------------------ */
/*  Argument parsing                                                   */
/* ------------------------------------------------------------------ */

static void print_usage(const char *progname)
{
    fprintf(stderr,
        "ffmpeg_fingerprint - FFmpeg + DVB subtitle fingerprint overlay\n"
        "\n"
        "Usage:\n"
        "  %s -i SOURCE [options] [-f FMT] OUTPUT\n"
        "\n"
        "Fingerprint Options:\n"
        "  --zmq ADDR       ZeroMQ bind address for dynamic control\n"
        "                   (default: tcp://127.0.0.1:5556)\n"
        "  --text TEXT      Initial/static fingerprint text\n"
        "  --position N     Position 0-8 (-1=random, default=-1)\n"
        "\n"
        "FFmpeg Options (passed through to FFmpeg):\n"
        "  -i SOURCE        Input URL/file (required)\n"
        "  -f FMT           Output format (default: mpegts)\n"
        "  -reconnect 1     Enable HTTP reconnect\n"
        "  -reconnect_streamed 1\n"
        "                   Reconnect on streamed input\n"
        "  -reconnect_delay_max N\n"
        "                   Max reconnect delay in seconds\n"
        "  Any other FFmpeg input/output options are passed through\n"
        "\n"
        "General Options:\n"
        "  --verbose, -v    Show detailed pipeline info\n"
        "  --help, -h       Show this help\n"
        "\n"
        "Output:\n"
        "  pipe:1           Output to stdout (for piping)\n"
        "  FILE             Output to file\n"
        "  udp://...        Output to UDP multicast\n"
        "  rtmp://...       Output to RTMP server\n"
        "\n"
        "ZMQ Commands (send to --zmq address):\n"
        "  SHOW <text>          Show fingerprint text\n"
        "  SHOW <text> <pos>    Show at position (0-8)\n"
        "  HIDE                 Hide fingerprint\n"
        "  STATUS               Get current status\n"
        "\n"
        "Positions: 0=top_left  1=top_center  2=top_right\n"
        "           3=mid_left  4=center      5=mid_right\n"
        "           6=bot_left  7=bot_center  8=bot_right\n"
        "\n"
        "Examples:\n"
        "  # ZMQ-controlled live restream:\n"
        "  %s -i \"udp://239.1.1.1:1234\" \\\n"
        "    --zmq tcp://127.0.0.1:5555 -f mpegts pipe:1\n"
        "\n"
        "  # Static text to file:\n"
        "  %s -i input.ts --text VIEWER42 -f mpegts output.ts\n"
        "\n"
        "  # HLS with reconnect:\n"
        "  %s -i \"https://cdn.example.com/live.m3u8\" \\\n"
        "    -reconnect 1 -reconnect_streamed 1 \\\n"
        "    --zmq tcp://127.0.0.1:5555 \\\n"
        "    -f mpegts \"udp://239.1.1.2:1234?pkt_size=1316\"\n"
        "\n",
        progname, progname, progname, progname);
}

static int parse_args(int argc, char *argv[], ParsedArgs *args)
{
    memset(args, 0, sizeof(*args));
    args->output_fmt = "mpegts";

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--zmq") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --zmq requires an address\n");
                return -1;
            }
            args->zmq_addr = argv[++i];
        } else if (strcmp(argv[i], "--text") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --text requires a value\n");
                return -1;
            }
            args->fp_text = argv[++i];
        } else if (strcmp(argv[i], "--position") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --position requires a value\n");
                return -1;
            }
            args->fp_position = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            args->verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -i requires a source\n");
                return -1;
            }
            args->input = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -f requires a format\n");
                return -1;
            }
            args->output_fmt = argv[++i];
        } else if (argv[i][0] == '-') {
            /* FFmpeg extra arg - collect flag and value */
            if (args->ffmpeg_extra_count >= MAX_ARGS - 2) {
                fprintf(stderr, "Error: too many FFmpeg arguments\n");
                return -1;
            }
            args->ffmpeg_extra[args->ffmpeg_extra_count++] = argv[i];
            /* Check if next arg is a value (not a flag) */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                args->ffmpeg_extra[args->ffmpeg_extra_count++] = argv[++i];
            }
        } else {
            /* Positional = output */
            args->output = argv[i];
        }
        i++;
    }

    /* Validate */
    if (!args->input) {
        fprintf(stderr, "Error: -i SOURCE is required\n");
        return -1;
    }
    if (!args->output) {
        fprintf(stderr, "Error: output destination is required\n");
        return -1;
    }

    /* Classify output type */
    if (strcmp(args->output, "pipe:1") == 0 || strcmp(args->output, "-") == 0) {
        args->output_is_pipe = 1;
    } else if (strncmp(args->output, "udp://", 6) == 0 ||
               strncmp(args->output, "rtp://", 6) == 0 ||
               strncmp(args->output, "rtmp://", 7) == 0 ||
               strncmp(args->output, "rtsp://", 7) == 0 ||
               strncmp(args->output, "srt://", 6) == 0) {
        args->output_is_network = 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Pipeline execution                                                 */
/* ------------------------------------------------------------------ */

/*
 * Pipeline topology:
 *
 *   Case 1: pipe:1 output (stdout)
 *     FFmpeg --> pipe_fd --> ts_fingerprint --> stdout
 *
 *   Case 2: file output
 *     FFmpeg --> pipe_fd --> ts_fingerprint --> output_fd (file)
 *
 *   Case 3: network output (udp://, rtmp://)
 *     FFmpeg --> pipe1 --> ts_fingerprint --> pipe2 --> FFmpeg_out --> network
 *     (second FFmpeg for proper network muxing with pacing)
 */

static int run_pipeline(const ParsedArgs *args,
                        const char *ffmpeg_path,
                        const char *tsfp_path)
{
    int pipe_ff_to_tsfp[2];  /* FFmpeg stdout -> ts_fingerprint stdin */
    int pipe_tsfp_to_out[2]; /* ts_fingerprint stdout -> output ffmpeg stdin (network only) */
    int output_fd = -1;

    /* Create first pipe: FFmpeg -> ts_fingerprint */
    if (pipe(pipe_ff_to_tsfp) < 0) {
        perror("pipe");
        return 1;
    }

    /* For network output, create second pipe: ts_fingerprint -> FFmpeg output */
    if (args->output_is_network) {
        if (pipe(pipe_tsfp_to_out) < 0) {
            perror("pipe");
            close(pipe_ff_to_tsfp[0]);
            close(pipe_ff_to_tsfp[1]);
            return 1;
        }
    }

    /* For file output, open the output file */
    if (!args->output_is_pipe && !args->output_is_network) {
        output_fd = open(args->output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (output_fd < 0) {
            fprintf(stderr, "Error: cannot open output file '%s': %s\n",
                    args->output, strerror(errno));
            close(pipe_ff_to_tsfp[0]);
            close(pipe_ff_to_tsfp[1]);
            return 1;
        }
    }

    /* ---- Fork FFmpeg (producer) ---- */
    g_ffmpeg_pid = fork();
    if (g_ffmpeg_pid < 0) {
        perror("fork (ffmpeg)");
        return 1;
    }

    if (g_ffmpeg_pid == 0) {
        /* Child: FFmpeg process */

        /* Redirect stdout to pipe write end */
        dup2(pipe_ff_to_tsfp[1], STDOUT_FILENO);
        close(pipe_ff_to_tsfp[0]);
        close(pipe_ff_to_tsfp[1]);

        /* Close other pipe fds if they exist */
        if (args->output_is_network) {
            close(pipe_tsfp_to_out[0]);
            close(pipe_tsfp_to_out[1]);
        }
        if (output_fd >= 0) close(output_fd);

        /* Redirect stderr to /dev/null for cleaner output (FFmpeg is noisy) */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        /* Build FFmpeg argv */
        const char *ff_argv[MAX_ARGS];
        int ff_argc = 0;

        ff_argv[ff_argc++] = ffmpeg_path;
        ff_argv[ff_argc++] = "-hide_banner";
        ff_argv[ff_argc++] = "-nostdin";

        /* Insert extra FFmpeg args (reconnect options, etc.) */
        for (int j = 0; j < args->ffmpeg_extra_count; j++) {
            if (ff_argc < MAX_ARGS - 8)
                ff_argv[ff_argc++] = args->ffmpeg_extra[j];
        }

        ff_argv[ff_argc++] = "-i";
        ff_argv[ff_argc++] = args->input;
        ff_argv[ff_argc++] = "-c:v";
        ff_argv[ff_argc++] = "copy";
        ff_argv[ff_argc++] = "-c:a";
        ff_argv[ff_argc++] = "copy";
        ff_argv[ff_argc++] = "-f";
        ff_argv[ff_argc++] = "mpegts";
        ff_argv[ff_argc++] = "pipe:1";
        ff_argv[ff_argc] = NULL;

        execvp(ff_argv[0], (char *const *)ff_argv);
        /* If we get here, exec failed */
        fprintf(stderr, "Error: failed to exec ffmpeg at '%s': %s\n",
                ffmpeg_path, strerror(errno));
        _exit(127);
    }

    /* ---- Fork ts_fingerprint (processor) ---- */
    g_tsfp_pid = fork();
    if (g_tsfp_pid < 0) {
        perror("fork (ts_fingerprint)");
        kill(g_ffmpeg_pid, SIGTERM);
        return 1;
    }

    if (g_tsfp_pid == 0) {
        /* Child: ts_fingerprint process */

        /* Redirect stdin from pipe read end */
        dup2(pipe_ff_to_tsfp[0], STDIN_FILENO);
        close(pipe_ff_to_tsfp[0]);
        close(pipe_ff_to_tsfp[1]);

        /* Redirect stdout based on output type */
        if (args->output_is_pipe) {
            /* stdout stays as-is (inherited from parent -> terminal/pipe) */
        } else if (args->output_is_network) {
            dup2(pipe_tsfp_to_out[1], STDOUT_FILENO);
            close(pipe_tsfp_to_out[0]);
            close(pipe_tsfp_to_out[1]);
        } else {
            /* File output */
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }

        /* Build ts_fingerprint argv */
        const char *tsfp_argv[16];
        int tsfp_argc = 0;

        tsfp_argv[tsfp_argc++] = tsfp_path;

        if (args->zmq_addr) {
            tsfp_argv[tsfp_argc++] = "--zmq";
            tsfp_argv[tsfp_argc++] = args->zmq_addr;
        }
        if (args->fp_text) {
            tsfp_argv[tsfp_argc++] = "--text";
            tsfp_argv[tsfp_argc++] = args->fp_text;
        }
        if (args->fp_position) {
            tsfp_argv[tsfp_argc++] = "--position";
            tsfp_argv[tsfp_argc++] = args->fp_position;
        }

        tsfp_argv[tsfp_argc] = NULL;

        execvp(tsfp_argv[0], (char *const *)tsfp_argv);
        fprintf(stderr, "Error: failed to exec ts_fingerprint at '%s': %s\n",
                tsfp_path, strerror(errno));
        _exit(127);
    }

    /* ---- Parent: close unused pipe ends ---- */
    close(pipe_ff_to_tsfp[0]);
    close(pipe_ff_to_tsfp[1]);
    if (output_fd >= 0) close(output_fd);

    /* ---- For network output: fork a third FFmpeg for output muxing ---- */
    pid_t out_ffmpeg_pid = 0;

    if (args->output_is_network) {
        out_ffmpeg_pid = fork();
        if (out_ffmpeg_pid < 0) {
            perror("fork (output ffmpeg)");
            kill(g_ffmpeg_pid, SIGTERM);
            kill(g_tsfp_pid, SIGTERM);
            return 1;
        }

        if (out_ffmpeg_pid == 0) {
            /* Child: output FFmpeg process */
            dup2(pipe_tsfp_to_out[0], STDIN_FILENO);
            close(pipe_tsfp_to_out[0]);
            close(pipe_tsfp_to_out[1]);

            /* Redirect stderr to /dev/null */
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            const char *out_argv[] = {
                ffmpeg_path,
                "-hide_banner",
                "-nostdin",
                "-re",
                "-i", "pipe:0",
                "-c", "copy",
                "-f", args->output_fmt,
                args->output,
                NULL
            };

            execvp(out_argv[0], (char *const *)out_argv);
            fprintf(stderr, "Error: failed to exec output ffmpeg: %s\n",
                    strerror(errno));
            _exit(127);
        }

        close(pipe_tsfp_to_out[0]);
        close(pipe_tsfp_to_out[1]);
    }

    /* ---- Parent: wait for children ---- */
    int exit_status = 0;
    int children_alive = args->output_is_network ? 3 : 2;

    while (children_alive > 0 && !g_shutdown) {
        int status;
        pid_t wpid = waitpid(-1, &status, 0);

        if (wpid < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        children_alive--;

        const char *name = "unknown";
        if (wpid == g_ffmpeg_pid) {
            name = "ffmpeg";
            g_ffmpeg_pid = 0;
        } else if (wpid == g_tsfp_pid) {
            name = "ts_fingerprint";
            g_tsfp_pid = 0;
        } else if (wpid == out_ffmpeg_pid) {
            name = "output-ffmpeg";
            out_ffmpeg_pid = 0;
        }

        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code != 0 && code != 255) {
                fprintf(stderr, "[ffmpeg_fingerprint] %s exited with code %d\n",
                        name, code);
                if (exit_status == 0) exit_status = code;
            } else if (args->verbose) {
                fprintf(stderr, "[ffmpeg_fingerprint] %s exited normally\n", name);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            if (sig != SIGTERM && sig != SIGPIPE && sig != SIGINT) {
                fprintf(stderr, "[ffmpeg_fingerprint] %s killed by signal %d\n",
                        name, sig);
                if (exit_status == 0) exit_status = 128 + sig;
            }
        }

        /* If the input FFmpeg died, signal ts_fingerprint to finish */
        if (wpid == g_ffmpeg_pid || (g_ffmpeg_pid == 0 && children_alive > 0)) {
            /* Pipe close will send EOF to ts_fingerprint naturally */
        }

        /* If ts_fingerprint died unexpectedly, kill the pipeline */
        if (wpid == g_tsfp_pid || (g_tsfp_pid == 0 && g_ffmpeg_pid > 0)) {
            if (g_ffmpeg_pid > 0) {
                kill(g_ffmpeg_pid, SIGTERM);
            }
            if (out_ffmpeg_pid > 0) {
                kill(out_ffmpeg_pid, SIGTERM);
            }
        }
    }

    /* Cleanup: make sure all children are dead */
    if (g_ffmpeg_pid > 0) {
        kill(g_ffmpeg_pid, SIGTERM);
        waitpid(g_ffmpeg_pid, NULL, 0);
    }
    if (g_tsfp_pid > 0) {
        kill(g_tsfp_pid, SIGTERM);
        waitpid(g_tsfp_pid, NULL, 0);
    }
    if (out_ffmpeg_pid > 0) {
        kill(out_ffmpeg_pid, SIGTERM);
        waitpid(out_ffmpeg_pid, NULL, 0);
    }

    return exit_status;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Parse arguments */
    ParsedArgs args;
    if (parse_args(argc, argv, &args) < 0) {
        fprintf(stderr, "\nRun '%s --help' for usage information.\n", argv[0]);
        return 1;
    }

    /* Find binaries */
    char ffmpeg_path[MAX_PATH_LEN];
    char tsfp_path[MAX_PATH_LEN];

    if (find_ffmpeg(ffmpeg_path, sizeof(ffmpeg_path)) < 0) {
        fprintf(stderr, "Error: ffmpeg not found. Install ffmpeg or set FFMPEG_BIN.\n");
        return 1;
    }

    if (find_ts_fingerprint(argv[0], tsfp_path, sizeof(tsfp_path)) < 0) {
        fprintf(stderr, "Error: ts_fingerprint not found.\n"
                "Build it first: make tools\n");
        return 1;
    }

    /* Log startup info */
    fprintf(stderr, "[ffmpeg_fingerprint] Starting pipeline...\n");
    fprintf(stderr, "[ffmpeg_fingerprint]   FFmpeg:          %s\n", ffmpeg_path);
    fprintf(stderr, "[ffmpeg_fingerprint]   ts_fingerprint:  %s\n", tsfp_path);
    fprintf(stderr, "[ffmpeg_fingerprint]   Input:           %s\n", args.input);
    fprintf(stderr, "[ffmpeg_fingerprint]   Output:          %s\n", args.output);

    if (args.zmq_addr)
        fprintf(stderr, "[ffmpeg_fingerprint]   ZMQ:             %s\n", args.zmq_addr);
    if (args.fp_text)
        fprintf(stderr, "[ffmpeg_fingerprint]   Text:            %s\n", args.fp_text);
    if (args.fp_position)
        fprintf(stderr, "[ffmpeg_fingerprint]   Position:        %s\n", args.fp_position);

    if (args.output_is_network)
        fprintf(stderr, "[ffmpeg_fingerprint]   Mode:            network (%s)\n",
                args.output_fmt);
    else if (args.output_is_pipe)
        fprintf(stderr, "[ffmpeg_fingerprint]   Mode:            pipe (stdout)\n");
    else
        fprintf(stderr, "[ffmpeg_fingerprint]   Mode:            file\n");

    /* Setup signal handlers */
    setup_signals();

    /* Run the pipeline */
    int ret = run_pipeline(&args, ffmpeg_path, tsfp_path);

    if (ret == 0)
        fprintf(stderr, "[ffmpeg_fingerprint] Pipeline finished.\n");
    else
        fprintf(stderr, "[ffmpeg_fingerprint] Pipeline exited with status %d.\n", ret);

    return ret;
}
