/* Raspberry Pi TCP gateway. All mutable state is protected by state_mutex.
 * Main owns connection lifetimes; each socket has exactly one RX worker and
 * one TX owner. Nonblocking worker jobs have a bounded I/O budget so that
 * control, shutdown, and reconnection continue under continuous traffic.
 */
#include "json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_FRAME (1024U * 1024U)
#define MAX_CONTROL 128U
#define MAX_TOKENS 4096
#define IO_BUDGET 64
#define BYTE_BUDGET (1024U * 1024U)
#define REASON_SIZE 256
#define QUEUE_BYTE_LIMIT (16U * 1024U * 1024U)

typedef struct Message {
    struct Message *next;
    size_t size, offset;
    bool vision, resume;
    unsigned char bytes[];
} Message;

typedef struct {
    int fd;
    bool connecting, fault;
    unsigned char prefix[4];
    size_t prefix_used, length, used;
    char *payload;
    bool discard;
    Message *head, *tail;
    size_t visions, controls, vision_bytes;
    int64_t progress;
} Link;

typedef struct {
    pthread_t thread;
    pthread_cond_t wake;
    int role;
    bool job, drained;
    bool write_jetson, write_wsl;
} Worker;

enum { JETSON_RX, WSL_RX, SOCKET_TX };
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_done = PTHREAD_COND_INITIALIZER;
static Worker workers[3];
static bool stop_workers;
static Link jetson = { .fd = -1 }, wsl = { .fd = -1 };
static bool wsl_known, wsl_ready, running, upstream_clean = true;
static bool pending_jetson_restored;
static volatile sig_atomic_t interrupted;
static size_t queue_limit = 256;
static uint64_t received, forwarded, overflow, discarded, sessions_j, sessions_w, sequence;
static char device[96] = "gateway-pi-01", boot[64];
static char pause_reason[REASON_SIZE] = "jetson_connection_lost";
static char wsl_reason[REASON_SIZE] = "wsl_state_pending";
static char resume_reason[REASON_SIZE] = "all_links_ready";
static char *pending_resume;
static size_t pending_resume_size;
static int64_t reconnect_at;
static int reconnect_ms = 1000, send_timeout_ms = 5000;
static struct addrinfo *next_address;

static int64_t now_ms(clockid_t clock)
{
    struct timespec t;
    clock_gettime(clock, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void on_signal(int sig) { (void)sig; interrupted = 1; }
static bool up(const Link *l) { return l->fd >= 0 && !l->connecting; }

static void cancel_resume(void)
{
    free(pending_resume);
    pending_resume = NULL;
    pending_resume_size = 0;
}

static void free_output(Link *l)
{
    while (l->head) {
        Message *m = l->head;
        l->head = m->next;
        free(m);
    }
    l->tail = NULL;
    l->visions = l->controls = l->vision_bytes = 0;
}

/* Only main calls this, with no outstanding worker jobs. */
static void close_link(Link *l)
{
    if (l->fd >= 0) {
        shutdown(l->fd, SHUT_RDWR);
        close(l->fd);
    }
    free(l->payload);
    free_output(l);
    *l = (Link){ .fd = -1 };
}

static void clear_vision(void)
{
    Message **at = &wsl.head;
    wsl.tail = NULL;
    while (*at) {
        Message *m = *at;
        if (m->vision) {
            discarded++;
            wsl.visions--;
            wsl.vision_bytes -= m->size;
            /* Cannot remove half a frame from a live byte stream. Main will
             * reset this WSL session before any further writes. */
            if (m->offset) wsl.fault = true;
            *at = m->next;
            free(m);
        } else {
            wsl.tail = m;
            at = &m->next;
        }
    }
    if (jetson.prefix_used || jetson.payload) jetson.discard = true;
}

static int enqueue(Link *l, const char *payload, size_t n, bool vision, bool resume)
{
    if (!up(l) || l->fault) return -1;
    if (vision && (l->visions >= queue_limit ||
                   n + 4 > QUEUE_BYTE_LIMIT - l->vision_bytes)) {
        overflow++;
        return 0; /* Bounded queue: drop the arriving Vision. */
    }
    if (!vision && l->controls >= MAX_CONTROL) {
        l->fault = true; /* Never silently lose a control on a live session. */
        return -1;
    }
    Message *m = malloc(sizeof(*m) + n + 4);
    if (!m) { l->fault = true; return -1; }
    *m = (Message){ .size = n + 4, .vision = vision, .resume = resume };
    uint32_t length = htonl((uint32_t)n);
    memcpy(m->bytes, &length, 4);
    memcpy(m->bytes + 4, payload, n);
    if (!l->head) {
        l->head = l->tail = m;
        l->progress = now_ms(CLOCK_MONOTONIC);
    } else if (vision) {
        l->tail->next = m;
        l->tail = m;
    } else {
        /* Prioritize control after a partially written frame, preserving
         * FIFO within the control queue and framing across all writes. */
        Message **at = &l->head;
        if ((*at)->offset) at = &(*at)->next;
        while (*at && !(*at)->vision) at = &(*at)->next;
        m->next = *at;
        *at = m;
        if (!m->next) l->tail = m;
    }
    if (vision) { l->visions++; l->vision_bytes += m->size; }
    else l->controls++;
    return 0;
}

static void control(Link *l, const char *action, const char *reason)
{
    if (!up(l) || l->fault) return;
    char quoted[REASON_SIZE * 6 + 3], buffer[2048];
    if (json_quote(reason, quoted, sizeof(quoted)) < 0) {
        l->fault = true;
        return;
    }
    int n = snprintf(buffer, sizeof(buffer),
        "{\"version\":1,\"type\":\"control\",\"device_id\":\"%s\","
        "\"message_id\":\"%s-%s-%012" PRIu64 "\",\"data\":{"
        "\"timestamp_ms\":%" PRId64 ",\"action\":\"%s\",\"reason\":%s}}",
        device, device, boot, ++sequence, now_ms(CLOCK_REALTIME), action, quoted);
    if (n < 0 || (size_t)n >= sizeof(buffer)) l->fault = true;
    else enqueue(l, buffer, (size_t)n, false, !strcmp(action, "resume"));
}

static void remove_resumes(Link *l)
{
    Message **at = &l->head;
    l->tail = NULL;
    while (*at) {
        Message *m = *at;
        if (m->resume) {
            if (m->offset) l->fault = true;
            *at = m->next;
            l->controls--;
            free(m);
        } else {
            l->tail = m;
            at = &m->next;
        }
    }
}

static bool path_ready(void)
{
    return up(&jetson) && !jetson.fault && up(&wsl) && !wsl.fault &&
           wsl_known && wsl_ready;
}

static void evaluate(bool notify_pause)
{
    bool next = path_ready() && (running || upstream_clean);
    if (!next) {
        const char *reason;
        if (jetson.fault || wsl.fault) reason = "pi_data_path_failed";
        else if (!up(&jetson)) reason = "jetson_connection_lost";
        else if (!up(&wsl)) reason = "wsl_connection_lost";
        else if (!wsl_known || !wsl_ready) reason = wsl_reason;
        else reason = "upstream_drain_pending";
        snprintf(pause_reason, sizeof(pause_reason), "%s", reason);
    }
    if (next == running) return;
    running = next;
    if (!running) {
        upstream_clean = false;
        clear_vision();
        remove_resumes(&jetson);
        remove_resumes(&wsl);
        if (notify_pause) control(&jetson, "pause", pause_reason);
    } else {
        snprintf(pause_reason, sizeof(pause_reason), "none");
        if (pending_resume) {
            enqueue(&jetson, pending_resume, pending_resume_size, false, true);
            cancel_resume();
        } else control(&jetson, "resume", resume_reason);
        if (pending_jetson_restored) {
            control(&wsl, "resume", "jetson_connection_restored");
            pending_jetson_restored = false;
        }
    }
    fprintf(stderr, "state=%s reason=%s\n", running ? "RUNNING" : "PAUSED",
            pause_reason);
}

static void lost(Link *l)
{
    bool was_up = up(l), is_wsl = l == &wsl;
    if (is_wsl) discarded += l->visions;
    close_link(l);
    if (is_wsl) {
        wsl_known = wsl_ready = false;
        cancel_resume();
        reconnect_at = now_ms(CLOCK_MONOTONIC) + reconnect_ms;
        snprintf(wsl_reason, sizeof(wsl_reason), "wsl_state_pending");
    } else pending_jetson_restored = false;
    evaluate(true);
    if (was_up) {
        const char *reason = is_wsl ? "wsl_connection_lost" : "jetson_connection_lost";
        fprintf(stderr, "event=%s timestamp_ms=%" PRId64 "\n",
                reason, now_ms(CLOCK_REALTIME));
        control(is_wsl ? &jetson : &wsl, "pause", reason);
    }
}

static int field(const char *s, const Token *t, int n, int object, const char *key)
{
    return json_field(s, t, n, object, key);
}

static bool equals(const char *s, const Token *t, int index, const char *v)
{
    return index >= 0 && json_equal(s, &t[index], v);
}

static bool text_field(const Token *t, int i)
{
    return i >= 0 && t[i].type == JSON_STRING && t[i].end > t[i].start;
}

static void handle(Link *l, const char *s, size_t len, bool drop)
{
    Token tokens[MAX_TOKENS];
    int n = json_parse(s, len, tokens, MAX_TOKENS);
    if (n < 0 || tokens[0].type != JSON_OBJECT) { l->fault = true; return; }
    int version = field(s, tokens, n, 0, "version");
    int type = field(s, tokens, n, 0, "type");
    int data = field(s, tokens, n, 0, "data");
    if (version < 0 || tokens[version].type != JSON_PRIMITIVE ||
        !equals(s, tokens, version, "1") || !text_field(tokens, type) ||
        !text_field(tokens, field(s, tokens, n, 0, "device_id")) ||
        !text_field(tokens, field(s, tokens, n, 0, "message_id")) ||
        data < 0 || tokens[data].type != JSON_OBJECT) {
        l->fault = true;
        return;
    }
    if (equals(s, tokens, type, "vision")) {
        if (l != &jetson) { l->fault = true; return; }
        received++;
        if (running && !drop) {
            if (enqueue(&wsl, s, len, true, false) < 0) {
                discarded++;
                evaluate(true);
            }
        } else discarded++;
    } else if (equals(s, tokens, type, "control")) {
        int action = field(s, tokens, n, data, "action");
        int reason = field(s, tokens, n, data, "reason");
        char decoded_reason[REASON_SIZE];
        if (!text_field(tokens, action) || !text_field(tokens, reason) ||
            json_string(s, &tokens[reason], decoded_reason, sizeof(decoded_reason)) < 0) {
            l->fault = true;
            return;
        }
        /* Connection observations never establish WSL storage readiness,
         * regardless of the action used to carry the event. */
        if (l == &wsl && !strcmp(decoded_reason, "pi_connection_restored")) {
            fprintf(stderr, "event=pi_connection_restored source=wsl\n");
            return;
        }
        if (l == &wsl && equals(s, tokens, action, "pause")) {
            wsl_known = true;
            wsl_ready = false;
            cancel_resume();
            snprintf(wsl_reason, sizeof(wsl_reason), "%s", decoded_reason);
            clear_vision();
            evaluate(false);
            enqueue(&jetson, s, len, false, false); /* Preserve the original envelope. */
        } else if (l == &wsl && equals(s, tokens, action, "resume")) {
            /* Every new session must first report resume/server_ready or an
             * actual internal PAUSE. Subsequent internal recoveries retain
             * their original reasons (database_recovered, queue_recovered...). */
            if (!wsl_known && strcmp(decoded_reason, "server_ready")) {
                fprintf(stderr, "event=wsl_initial_status_pending\n");
                return;
            }
            wsl_known = wsl_ready = true;
            cancel_resume();
            pending_resume = malloc(len);
            if (!pending_resume) { l->fault = true; return; }
            memcpy(pending_resume, s, len);
            pending_resume_size = len;
            evaluate(true);
            if (running && pending_resume) {
                enqueue(&jetson, pending_resume, pending_resume_size, false, true);
                cancel_resume();
            } else if (!running) control(&jetson, "pause", pause_reason);
        } else enqueue(l == &wsl ? &jetson : &wsl, s, len, false,
                       equals(s, tokens, action, "resume"));
    } else l->fault = true;
}

/* True means all immediately available bytes were consumed. A PAUSED frame
 * remains marked for discard even if completed after RESUME. */
static bool receive(Link *l)
{
    size_t bytes = 0;
    for (int frames = 0; frames < IO_BUDGET && bytes < BYTE_BUDGET && !l->fault;) {
        void *buffer;
        size_t remaining;
        bool prefix = l->prefix_used < 4;
        if (prefix) {
            buffer = l->prefix + l->prefix_used;
            remaining = 4 - l->prefix_used;
        } else {
            buffer = l->payload + l->used;
            remaining = l->length - l->used;
        }
        if (remaining > BYTE_BUDGET - bytes) remaining = BYTE_BUDGET - bytes;
        ssize_t got = recv(l->fd, buffer, remaining, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            l->fault = true;
            return true;
        }
        if (!got) { l->fault = true; return true; }
        bytes += (size_t)got;
        if (l == &jetson && !running) l->discard = true;
        if (prefix) {
            l->prefix_used += (size_t)got;
            if (l->prefix_used < 4) continue;
            uint32_t length;
            memcpy(&length, l->prefix, 4);
            l->length = ntohl(length);
            if (!l->length || l->length > MAX_FRAME) { l->fault = true; return true; }
            l->payload = malloc(l->length + 1);
            if (!l->payload) { l->fault = true; return true; }
        } else {
            l->used += (size_t)got;
            if (l->used < l->length) continue;
            l->payload[l->length] = 0;
            handle(l, l->payload, l->length, l->discard);
            free(l->payload);
            l->payload = NULL;
            l->prefix_used = l->length = l->used = 0;
            l->discard = false;
            frames++;
        }
    }
    return false;
}

static void transmit(Link *l)
{
    size_t bytes = 0;
    for (int count = 0; l->head && !l->fault && count < IO_BUDGET &&
         bytes < BYTE_BUDGET; count++) {
        Message *m = l->head;
        size_t remaining = m->size - m->offset;
        if (remaining > BYTE_BUDGET - bytes) remaining = BYTE_BUDGET - bytes;
        ssize_t sent = send(l->fd, m->bytes + m->offset, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            l->fault = true;
            return;
        }
        if (!sent) { l->fault = true; return; }
        bytes += (size_t)sent;
        m->offset += (size_t)sent;
        l->progress = now_ms(CLOCK_MONOTONIC);
        if (m->offset < m->size) return;
        l->head = m->next;
        if (!l->head) l->tail = NULL;
        if (m->vision) {
            l->visions--;
            l->vision_bytes -= m->size;
            forwarded++;
        } else l->controls--;
        free(m);
    }
}

static void *worker_main(void *argument)
{
    Worker *worker = argument;
    pthread_mutex_lock(&state_mutex);
    for (;;) {
        while (!worker->job && !stop_workers)
            pthread_cond_wait(&worker->wake, &state_mutex);
        if (stop_workers) break;
        if (worker->role == JETSON_RX) worker->drained = receive(&jetson);
        else if (worker->role == WSL_RX) receive(&wsl);
        else {
            if (worker->write_jetson && up(&jetson) && !jetson.fault) transmit(&jetson);
            if (worker->write_wsl && up(&wsl) && !wsl.fault) transmit(&wsl);
        }
        worker->job = false;
        pthread_cond_signal(&job_done);
    }
    pthread_mutex_unlock(&state_mutex);
    return NULL;
}

/* Called with state_mutex held. Main never closes an fd while a job runs. */
static void dispatch(int role)
{
    Worker *worker = &workers[role];
    worker->job = true;
    pthread_cond_signal(&worker->wake);
    while (worker->job) pthread_cond_wait(&job_done, &state_mutex);
}

static int configure(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) return -1;
#ifdef TCP_KEEPIDLE
    int idle = 10, interval = 3, count = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0) return -1;
#endif
    return 0;
}

static struct addrinfo *resolve(const char *host, const char *port, bool passive)
{
    struct addrinfo hints = {0}, *result = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | (passive ? AI_PASSIVE : 0);
    int error = getaddrinfo(host, port, &hints, &result);
    if (error) {
        fprintf(stderr, "address_resolution host=%s error=%s\n", host, gai_strerror(error));
        return NULL;
    }
    return result;
}

static int listen_socket(struct addrinfo *addresses)
{
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (configure(fd) == 0 && bind(fd, a->ai_addr, a->ai_addrlen) == 0 &&
            listen(fd, 8) == 0) return fd;
        close(fd);
    }
    return -1;
}

static void connected_wsl(void)
{
    wsl.connecting = false;
    sessions_w++;
    wsl_known = wsl_ready = false;
    snprintf(wsl_reason, sizeof(wsl_reason), "wsl_state_pending");
    snprintf(resume_reason, sizeof(resume_reason), "wsl_connection_restored");
    fprintf(stderr, "event=wsl_connection_restored timestamp_ms=%" PRId64 "\n",
            now_ms(CLOCK_REALTIME));
    /* Report restoration without authorizing data until WSL reports ready. */
    control(&jetson, "pause", "wsl_connection_restored");
    control(&wsl, "pause", up(&jetson) ? "jetson_connection_restored" : "jetson_connection_lost");
    pending_jetson_restored = up(&jetson);
    evaluate(true);
}

static void connect_wsl(struct addrinfo *addresses)
{
    if (!next_address) next_address = addresses;
    struct addrinfo *a = next_address;
    next_address = a->ai_next ? a->ai_next : addresses;
    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd >= 0) {
        if (configure(fd) == 0) {
            int rc = connect(fd, a->ai_addr, a->ai_addrlen);
            if (rc == 0 || errno == EINPROGRESS) {
                wsl.fd = fd;
                wsl.connecting = rc != 0;
                wsl.progress = now_ms(CLOCK_MONOTONIC);
                if (!wsl.connecting) connected_wsl();
                return;
            }
        }
        close(fd);
    }
    reconnect_at = now_ms(CLOCK_MONOTONIC) + reconnect_ms;
}

static void accept_jetson(int server)
{
    int fd = accept(server, NULL, NULL);
    if (fd < 0) return;
    if (up(&jetson) || configure(fd) < 0) { close(fd); return; }
    jetson.fd = fd;
    sessions_j++;
    pending_jetson_restored = true;
    snprintf(resume_reason, sizeof(resume_reason), "jetson_connection_restored");
    fprintf(stderr, "event=jetson_connection_restored timestamp_ms=%" PRId64 "\n",
            now_ms(CLOCK_REALTIME));
    /* Check for pre-session bytes before authorizing this new producer. */
    upstream_clean = false;
    evaluate(true);
    control(&jetson, "pause", pause_reason);
    control(&wsl, "pause", "jetson_connection_restored");
}

static int number(const char *s, int minimum, int maximum)
{
    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (errno || !*s || *end || value < minimum || value > maximum) return -1;
    return (int)value;
}

static void metrics(int64_t now, int64_t *last, uint64_t *prev_rx, uint64_t *prev_tx)
{
    if (now - *last < 1000) return;
    double seconds = (now - *last) / 1000.0;
    char quoted[REASON_SIZE * 6 + 3];
    json_quote(pause_reason, quoted, sizeof(quoted));
    fprintf(stderr,
        "metrics rx_per_s=%.1f tx_per_s=%.1f received=%" PRIu64
        " forwarded=%" PRIu64 " queue=%zu queue_bytes=%zu overflow_dropped=%" PRIu64
        " discarded_on_pause=%" PRIu64 " jetson=%s wsl=%s wsl_internal=%s pi=%s"
        " reason=%s jetson_reconnects=%" PRIu64 " wsl_reconnects=%" PRIu64 "\n",
        (received - *prev_rx) / seconds, (forwarded - *prev_tx) / seconds,
        received, forwarded, wsl.visions, wsl.vision_bytes, overflow, discarded,
        up(&jetson) ? "UP" : "DOWN", up(&wsl) ? "UP" : "DOWN",
        !wsl_known ? "UNKNOWN" : wsl_ready ? "READY" : "PAUSED",
        running ? "RUNNING" : "PAUSED", quoted,
        sessions_j ? sessions_j - 1 : 0, sessions_w ? sessions_w - 1 : 0);
    *last = now; *prev_rx = received; *prev_tx = forwarded;
}

static void stop_and_join(int started)
{
    pthread_mutex_lock(&state_mutex);
    stop_workers = true;
    for (int i = 0; i < started; i++) pthread_cond_signal(&workers[i].wake);
    if (jetson.fd >= 0) shutdown(jetson.fd, SHUT_RDWR);
    if (wsl.fd >= 0) shutdown(wsl.fd, SHUT_RDWR);
    pthread_mutex_unlock(&state_mutex);
    for (int i = 0; i < started; i++) {
        pthread_join(workers[i].thread, NULL);
        pthread_cond_destroy(&workers[i].wake);
    }
    close_link(&jetson);
    close_link(&wsl);
    cancel_resume();
    pthread_cond_destroy(&job_done);
    pthread_mutex_destroy(&state_mutex);
}

int main(int argc, char **argv)
{
    const char *listen_host = "0.0.0.0", *listen_port = "9000";
    const char *wsl_host = "127.0.0.1", *wsl_port = "9000";
    int option_start = 1;
    if (argc > 1 && argv[1][0] != '-') {
        if (argc < 3 || number(argv[2], 1, 65535) < 0) {
            fprintf(stderr, "usage: pi_gateway WSL_IP WSL_PORT [JETSON_PORT] [options]\n");
            return 2;
        }
        wsl_host = argv[1];
        wsl_port = argv[2];
        option_start = 3;
        if (argc > 3 && argv[3][0] != '-') {
            if (number(argv[3], 1, 65535) < 0) {
                fprintf(stderr, "invalid Jetson listen port: %s\n", argv[3]);
                return 2;
            }
            listen_port = argv[3];
            option_start = 4;
        }
    }
    for (int i = option_start; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            puts("pi_gateway WSL_IP WSL_PORT [JETSON_PORT] [options]\n"
                 "pi_gateway [--listen-host IP] [--listen-port PORT] [--wsl-host IP]"
                 " [--wsl-port PORT] [--device-id ID] [--queue-capacity N]"
                 " [--reconnect-ms N] [--send-timeout-ms N]");
            return 0;
        }
        if (i + 1 == argc) { fprintf(stderr, "missing option value\n"); return 2; }
        const char *key = argv[i], *value = argv[++i];
        int num;
        if (!strcmp(key, "--listen-host")) listen_host = value;
        else if (!strcmp(key, "--listen-port")) {
            if (number(value, 1, 65535) < 0) return 2;
            listen_port = value;
        } else if (!strcmp(key, "--wsl-host")) wsl_host = value;
        else if (!strcmp(key, "--wsl-port")) {
            if (number(value, 1, 65535) < 0) return 2;
            wsl_port = value;
        } else if (!strcmp(key, "--device-id")) {
            if (!*value || strlen(value) >= sizeof(device) ||
                strspn(value, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != strlen(value))
                return 2;
            snprintf(device, sizeof(device), "%s", value);
        } else if (!strcmp(key, "--queue-capacity")) {
            num = number(value, 1, 65536);
            if (num < 0) return 2;
            queue_limit = (size_t)num;
        } else if (!strcmp(key, "--reconnect-ms")) {
            num = number(value, 10, 60000);
            if (num < 0) return 2;
            reconnect_ms = num;
        } else if (!strcmp(key, "--send-timeout-ms")) {
            num = number(value, 100, 600000);
            if (num < 0) return 2;
            send_timeout_ms = num;
        } else { fprintf(stderr, "unknown option: %s\n", key); return 2; }
    }

    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    snprintf(boot, sizeof(boot), "%" PRId64 "-%ld", now_ms(CLOCK_REALTIME), (long)getpid());
    struct addrinfo *local = resolve(listen_host, listen_port, true);
    struct addrinfo *remote = resolve(wsl_host, wsl_port, false);
    if (!local || !remote) {
        if (local) freeaddrinfo(local);
        if (remote) freeaddrinfo(remote);
        return 1;
    }
    int server = listen_socket(local);
    freeaddrinfo(local);
    if (server < 0) { perror("listen"); freeaddrinfo(remote); return 1; }
    int started = 0;
    for (int i = 0; i < 3; i++) {
        workers[i].role = i;
        int error = pthread_cond_init(&workers[i].wake, NULL);
        if (!error) {
            error = pthread_create(&workers[i].thread, NULL, worker_main, &workers[i]);
            if (error) pthread_cond_destroy(&workers[i].wake);
        }
        if (error) {
            fprintf(stderr, "worker creation: %s\n", strerror(error));
            stop_and_join(started); close(server); freeaddrinfo(remote);
            return 1;
        }
        started++;
    }
    fprintf(stderr, "gateway listening=%s:%s downstream=%s:%s\n",
            listen_host, listen_port, wsl_host, wsl_port);
    int64_t metric_at = now_ms(CLOCK_MONOTONIC);
    uint64_t previous_rx = 0, previous_tx = 0;
    int exit_status = 0;
    while (!interrupted) {
        pthread_mutex_lock(&state_mutex);
        int64_t now = now_ms(CLOCK_MONOTONIC);
        if (wsl.fd < 0 && now >= reconnect_at) connect_wsl(remote);
        struct pollfd fds[3] = {
            {server, POLLIN, 0},
            {jetson.fd, POLLIN | (jetson.head ? POLLOUT : 0), 0},
            {wsl.fd, POLLIN | ((wsl.head || wsl.connecting) ? POLLOUT : 0), 0}
        };
        pthread_mutex_unlock(&state_mutex);
        int rc = poll(fds, 3, 100);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("poll"); exit_status = 1; break;
        }
        if (interrupted) break;
        pthread_mutex_lock(&state_mutex);
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            pthread_mutex_unlock(&state_mutex);
            exit_status = 1; break;
        }
        if (fds[0].revents & POLLIN) accept_jetson(server);
        /* Drain pre-RESUME upstream bytes before downstream RX can change state. */
        if (!running)
            upstream_clean = fds[1].fd == jetson.fd && !(fds[1].revents & POLLIN);
        if (up(&jetson) && (fds[1].revents & POLLIN)) {
            dispatch(JETSON_RX);
            upstream_clean = workers[JETSON_RX].drained;
        }
        if (wsl.connecting && (fds[2].revents & (POLLOUT | POLLIN | POLLERR | POLLHUP))) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(wsl.fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error)
                wsl.fault = true;
            else connected_wsl();
        }
        if (up(&wsl) && !wsl.fault && (fds[2].revents & POLLIN)) dispatch(WSL_RX);
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) jetson.fault = true;
        if (fds[2].revents & (POLLERR | POLLHUP | POLLNVAL)) wsl.fault = true;
        if (jetson.fault) lost(&jetson);
        if (wsl.fault) lost(&wsl);
        evaluate(true);
        workers[SOCKET_TX].write_jetson = up(&jetson) && (fds[1].revents & POLLOUT);
        workers[SOCKET_TX].write_wsl = up(&wsl) && (fds[2].revents & POLLOUT);
        if (workers[SOCKET_TX].write_jetson || workers[SOCKET_TX].write_wsl)
            dispatch(SOCKET_TX);
        now = now_ms(CLOCK_MONOTONIC);
        if ((wsl.connecting || wsl.head) && now - wsl.progress >= send_timeout_ms)
            wsl.fault = true;
        if (jetson.head && now - jetson.progress >= send_timeout_ms) jetson.fault = true;
        if (jetson.fault) lost(&jetson);
        if (wsl.fault) lost(&wsl);
        metrics(now, &metric_at, &previous_rx, &previous_tx);
        pthread_mutex_unlock(&state_mutex);
    }
    stop_and_join(started);
    close(server);
    freeaddrinfo(remote);
    return exit_status;
}
