/* Full reference implementation for the assignment requirements. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum { CMD_SUPERVISOR = 0, CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP } command_kind_t;
typedef enum { CONTAINER_STARTING = 0, CONTAINER_RUNNING, CONTAINER_STOPPED, CONTAINER_KILLED, CONTAINER_EXITED } container_state_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head, tail, count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    int stop_requested;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int finished;
    pthread_mutex_t lock;
    pthread_cond_t done_cv;
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    char log_path[PATH_MAX];
    /* Must not free until the cloned child has exited, since child_fn reads cfg/stack. */
    void *child_stack;
    child_config_t *child_cfg;
    struct container_record *next;
} container_record_t;

typedef struct {
    int read_fd;
    bounded_buffer_t *buffer;
    char id[CONTAINER_ID_LEN];
} producer_arg_t;

typedef struct producer_node {
    pthread_t tid;
    producer_arg_t *arg;
    struct producer_node *next;
} producer_node_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    volatile sig_atomic_t should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    producer_node_t *producers;
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes);
int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid);

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    while (!buffer->shutting_down && buffer->count == LOG_BUFFER_CAPACITY)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg) {
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        int fd;
        ssize_t written = 0;
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0)
            continue;
        while ((size_t)written < item.length) {
            ssize_t n = write(fd, item.data + written, item.length - (size_t)written);
            if (n <= 0)
                break;
            written += n;
        }
        close(fd);
    }
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg) {
    child_config_t *cfg = (child_config_t *)arg;
    /* Route early errors into the per-container log so debugging is possible. */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0)
        perror("dup2 stdout");
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0)
        perror("dup2 stderr");
    close(cfg->log_write_fd);

    if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        perror("setpriority");
    if (sethostname(cfg->id, strnlen(cfg->id, sizeof(cfg->id))) < 0)
        perror("sethostname");
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount private");
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount proc");
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 127;
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id) {
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strncmp(cur->id, id, CONTAINER_ID_LEN) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static void set_exit_status(container_record_t *rec, int status) {
    pthread_mutex_lock(&rec->lock);
    rec->finished = 1;
    if (WIFEXITED(status)) {
        rec->exit_code = WEXITSTATUS(status);
        rec->exit_signal = 0;
        rec->state = rec->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
    } else if (WIFSIGNALED(status)) {
        rec->exit_code = 128 + WTERMSIG(status);
        rec->exit_signal = WTERMSIG(status);
        if (rec->stop_requested)
            rec->state = CONTAINER_STOPPED;
        else if (rec->exit_signal == SIGKILL)
            rec->state = CONTAINER_KILLED;
        else
            rec->state = CONTAINER_EXITED;
    }
    pthread_cond_broadcast(&rec->done_cv);
    pthread_mutex_unlock(&rec->lock);
}

static void reap_children(supervisor_ctx_t *ctx) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *cur;
        pthread_mutex_lock(&ctx->metadata_lock);
        cur = ctx->containers;
        while (cur) {
            if (cur->host_pid == pid) {
                set_exit_status(cur, status);
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, cur->id, cur->host_pid);
                if (cur->child_stack) {
                    free(cur->child_stack);
                    cur->child_stack = NULL;
                }
                if (cur->child_cfg) {
                    free(cur->child_cfg);
                    cur->child_cfg = NULL;
                }
                break;
            }
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static void sigchld_handler(int signo) { (void)signo; }
static void stop_handler(int signo) {
    (void)signo;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, int status, int exit_status, const char *msg) {
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    resp.exit_status = exit_status;
    if (msg)
        strncpy(resp.message, msg, sizeof(resp.message) - 1);
    write_all(fd, &resp, sizeof(resp));
}

static void *producer_thread(void *arg) {
    producer_arg_t *pa = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    for (;;) {
        ssize_t n = read(pa->read_fd, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pa->id, sizeof(item.container_id) - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        if (bounded_buffer_push(pa->buffer, &item) != 0)
            break;
    }
    close(pa->read_fd);
    return NULL;
}

static int spawn_container(supervisor_ctx_t *ctx, const control_request_t *req, container_record_t **out) {
    int pipefd[2];
    void *stack = NULL;
    child_config_t *cfg = NULL;
    container_record_t *rec = NULL;
    producer_arg_t *parg = NULL;
    producer_node_t *pnode = NULL;
    pid_t pid;
    if (pipe(pipefd) < 0)
        return -1;
    cfg = calloc(1, sizeof(*cfg));
    rec = calloc(1, sizeof(*rec));
    parg = calloc(1, sizeof(*parg));
    pnode = calloc(1, sizeof(*pnode));
    if (!cfg || !rec || !parg || !pnode)
        goto fail;
    stack = malloc(STACK_SIZE);
    if (!stack)
        goto fail;
    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    cfg->id[sizeof(cfg->id) - 1] = '\0';
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    cfg->rootfs[sizeof(cfg->rootfs) - 1] = '\0';
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->command[sizeof(cfg->command) - 1] = '\0';
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];
    pid = clone(child_fn, (char *)stack + STACK_SIZE, CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD, cfg);
    if (pid < 0)
        goto fail;
    close(pipefd[1]);
    strncpy(parg->id, req->container_id, sizeof(parg->id) - 1);
    parg->id[sizeof(parg->id) - 1] = '\0';
    parg->read_fd = pipefd[0];
    parg->buffer = &ctx->log_buffer;
    if (pthread_create(&pnode->tid, NULL, producer_thread, parg) != 0)
        goto fail_pid;
    pnode->arg = parg;
    pthread_mutex_lock(&ctx->metadata_lock);
    pnode->next = ctx->producers;
    ctx->producers = pnode;
    pthread_mutex_unlock(&ctx->metadata_lock);
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->id[sizeof(rec->id) - 1] = '\0';
    strncpy(rec->rootfs, req->rootfs, sizeof(rec->rootfs) - 1);
    rec->rootfs[sizeof(rec->rootfs) - 1] = '\0';
    strncpy(rec->command, req->command, sizeof(rec->command) - 1);
    rec->command[sizeof(rec->command) - 1] = '\0';
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req->container_id);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    pthread_mutex_init(&rec->lock, NULL);
    pthread_cond_init(&rec->done_cv, NULL);
    /* Retain child stack/cfg until the cloned child exits (reaped via SIGCHLD). */
    rec->child_stack = stack;
    rec->child_cfg = cfg;
    stack = NULL;
    cfg = NULL;
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, rec->id, rec->host_pid, rec->soft_limit_bytes, rec->hard_limit_bytes);
    *out = rec;
    return 0;
fail_pid:
    kill(pid, SIGKILL);
fail:
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    free(stack);
    free(cfg);
    free(rec);
    free(parg);
    free(pnode);
    return -1;
}

static void stream_ps(supervisor_ctx_t *ctx, int fd) {
    char line[512];
    container_record_t *cur;
    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        snprintf(line, sizeof(line),
                 "id=%s pid=%d state=%s soft=%luMiB hard=%luMiB exit=%d signal=%d\n",
                 cur->id,
                 cur->host_pid,
                 state_to_string(cur->state),
                 cur->soft_limit_bytes >> 20,
                 cur->hard_limit_bytes >> 20,
                 cur->exit_code,
                 cur->exit_signal);
        write_all(fd, line, strlen(line));
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void stream_logs(const char *id, int fd) {
    char path[PATH_MAX], buf[2048];
    int in;
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, id);
    in = open(path, O_RDONLY);
    if (in < 0) {
        write_all(fd, "No logs found\n", 14);
        return;
    }
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n <= 0)
            break;
        write_all(fd, buf, (size_t)n);
    }
    close(in);
}

static void handle_request(supervisor_ctx_t *ctx, int cfd, const control_request_t *req) {
    container_record_t *rec = NULL;
    if (req->kind == CMD_START || req->kind == CMD_RUN) {
        pthread_mutex_lock(&ctx->metadata_lock);
        if (find_container_locked(ctx, req->container_id) != NULL) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(cfd, 1, 0, "container id already exists");
            return;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (spawn_container(ctx, req, &rec) != 0) {
            send_response(cfd, 1, 0, "failed to spawn container");
            return;
        }
        if (req->kind == CMD_RUN) {
            pthread_mutex_lock(&rec->lock);
            while (!rec->finished)
                pthread_cond_wait(&rec->done_cv, &rec->lock);
            send_response(cfd, 0, rec->exit_code, "run completed");
            pthread_mutex_unlock(&rec->lock);
        } else {
            send_response(cfd, 0, 0, "started");
        }
        return;
    }
    if (req->kind == CMD_STOP) {
        int i;
        int kill_rc;
        pid_t target_pid;
        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_locked(ctx, req->container_id);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(cfd, 1, 0, "container not found");
            return;
        }
        rec->stop_requested = 1;
        target_pid = rec->host_pid;
        kill_rc = kill(target_pid, SIGTERM);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (kill_rc < 0 && errno != ESRCH) {
            send_response(cfd, 1, 0, "failed to send SIGTERM");
            return;
        }

        /* Give graceful stop a short window, then force kill if still alive. */
        for (i = 0; i < 20; i++) {
            reap_children(ctx);
            if (kill(target_pid, 0) < 0 && errno == ESRCH)
                break;
            usleep(100000);
        }
        if (kill(target_pid, 0) == 0) {
            kill(target_pid, SIGKILL);
            reap_children(ctx);
            send_response(cfd, 0, 0, "stop escalated to SIGKILL");
            return;
        }

        send_response(cfd, 0, 0, "stop sent");
        return;
    }
    if (req->kind == CMD_PS) {
        /* Refresh child statuses before rendering metadata to avoid stale "running". */
        reap_children(ctx);
        /* Keep client output clean: table rows only (no leading "ok"). */
        send_response(cfd, 0, 0, NULL);
        stream_ps(ctx, cfd);
        return;
    }
    if (req->kind == CMD_LOGS) {
        /* Keep client output clean: log contents only. */
        send_response(cfd, 0, 0, NULL);
        stream_logs(req->container_id, cfd);
        return;
    }
    send_response(cfd, 1, 0, "unsupported command");
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc, cfd;
    struct sockaddr_un addr;
    struct sigaction sa;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    mkdir(LOG_DIR, 0755);
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }
    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(ctx.server_fd, 32) < 0) {
        perror("listen");
        return 1;
    }
    g_ctx = &ctx;
    fprintf(stderr, "Supervisor listening on %s\n", CONTROL_PATH);
    fprintf(stderr, "Run CLI commands in a separate terminal (this supervisor occupies stdin).\n");
    fflush(stderr);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = stop_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        return 1;
    }
    while (!ctx.should_stop) {
        control_request_t req;
        cfd = accept(ctx.server_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                reap_children(&ctx);
                continue;
            }
            perror("accept");
            break;
        }
        if (read_all(cfd, &req, sizeof(req)) == 0)
            handle_request(&ctx, cfd, &req);
        close(cfd);
        reap_children(&ctx);
    }
    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *cur = ctx.containers;
        while (cur) {
            if (!cur->finished) {
                cur->stop_requested = 1;
                kill(cur->host_pid, SIGTERM);
            }
            cur = cur->next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    sleep(1);
    reap_children(&ctx);
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    if (ctx.server_fd >= 0) {
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
    }
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd, rc = 1;
    struct sockaddr_un addr;
    control_response_t resp;
    char extra[2048];
    ssize_t n;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    if (write_all(fd, req, sizeof(*req)) != 0 || read_all(fd, &resp, sizeof(resp)) != 0) {
        perror("ipc");
        close(fd);
        return 1;
    }
    if (resp.message[0] != '\0')
        printf("%s\n", resp.message);
    if (resp.status == 0)
        rc = 0;
    while ((n = read(fd, extra, sizeof(extra))) > 0)
        fwrite(extra, 1, (size_t)n, stdout);
    close(fd);
    if (req->kind == CMD_RUN)
        return resp.exit_status;
    return rc;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
