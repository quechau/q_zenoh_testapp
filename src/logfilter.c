/** logfilter.c — keep zenoh-pico's chatter out of the way unless it is asked for.
 *
 * zenoh-pico decides its log level at COMPILE time, so there is no library call to turn the
 * noise down at run time. Built quietly it cannot be made verbose when something goes wrong;
 * built verbosely it prints a line per keep-alive and per frame, which buries this tool's own
 * output and mangles the REPL prompt.
 *
 * So it is built verbose (`ZENOH_DEBUG=3`) and filtered here: stdout is replaced with a pipe
 * and a thread forwards only what the operator asked for. Without `--debug` the DEBUG and INFO
 * lines are dropped and everything else — this tool's output, and zenoh's WARN and ERROR — is
 * passed through untouched.
 *
 * WARN and ERROR are never suppressed. Their absence is exactly what makes a failed handshake
 * look like an unexplained return code, and several real defects in this project were found
 * only because those lines were visible.
 */
#include "qz.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int  g_orig_stdout = -1;
static bool g_verbose = false;

/** True for a zenoh-pico log line at a level we hide: "[<ISO-8601> DEBUG ::…" or "… INFO ::". */
static bool is_hidden_zenoh_line(const char *line)
{
    /* The shape is fixed: '[' then a 20-char timestamp, a space, the level, then " ::".
     * Anything that does not match is left alone rather than guessed at. */
    if (line[0] != '[') return false;
    const char *close = strchr(line, ']');
    if (close == NULL) return false;
    if (strstr(line, " ::") == NULL || strstr(line, " ::") > close) return false;
    return (strstr(line, " DEBUG ::") != NULL && strstr(line, " DEBUG ::") < close) ||
           (strstr(line, " INFO ::")  != NULL && strstr(line, " INFO ::")  < close);
}

static void emit(const char *s, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(g_orig_stdout, s + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void *pump(void *arg)
{
    int rfd = *(int *)arg;

    /* Raw reads, not fgets. The filter works on whole lines, but the REPL prompt is written
     * WITHOUT a trailing newline — and fgets blocks until it sees one, so the prompt never
     * reached the terminal and the tool looked like it had hung after printing its banner.
     *
     * So: buffer until a newline for anything that could be a log line, and release an
     * incomplete tail immediately when it cannot be one. Every line this tool and zenoh-pico
     * emit starts with '[', so a partial line that does not is the prompt (or something else
     * of ours) and is safe to pass straight through. */
    char pending[8192];
    size_t held = 0;
    char chunk[4096];

    for (;;) {
        ssize_t got = read(rfd, chunk, sizeof(chunk));
        if (got <= 0) break;

        for (ssize_t i = 0; i < got; i++) {
            if (held + 1 >= sizeof(pending)) {      /* absurdly long line: never grow forever */
                emit(pending, held);
                held = 0;
            }
            pending[held++] = chunk[i];
            if (chunk[i] != '\n') continue;

            pending[held] = '\0';
            if (g_verbose || !is_hidden_zenoh_line(pending)) emit(pending, held);
            held = 0;
        }

        if (held > 0 && pending[0] != '[') {        /* a prompt, not a half-written log line */
            emit(pending, held);
            held = 0;
        }
    }
    if (held > 0) emit(pending, held);
    close(rfd);
    return NULL;
}

void qz_log_filter_install(bool verbose)
{
    g_verbose = verbose;
    if (verbose) return;            /* nothing to do — let everything through as it is */

    static int rfd;
    int fds[2];
    if (pipe(fds) != 0) return;     /* filtering is a convenience; never fail the run for it */
    rfd = fds[0];

    g_orig_stdout = dup(STDOUT_FILENO);
    if (g_orig_stdout < 0) { close(fds[0]); close(fds[1]); return; }

    fflush(stdout);
    if (dup2(fds[1], STDOUT_FILENO) < 0) { close(fds[0]); close(fds[1]); return; }
    close(fds[1]);
    /* Line buffering, so a prompt written without a newline still reaches the pump promptly
     * and the REPL does not appear to hang. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    pthread_t t;
    if (pthread_create(&t, NULL, pump, &rfd) == 0) pthread_detach(t);
}
