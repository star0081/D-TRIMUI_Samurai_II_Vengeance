#define _GNU_SOURCE
#include "xport.h"
#include "ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CMD_FLUSH (32u * 1024u)
#define XPORT_PATH "/tmp/tspgl-xport"
#define XPORT_MAGIC 0x58504f52u

static struct tspgl_shared *X;

struct tspgl_shared *tspgl_shared(void)
{
    int fd;
    int32_t pid;

    if (X)
        return X;
    fd = open(XPORT_PATH, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return NULL;
    if (ftruncate(fd, (off_t)sizeof(*X)) != 0) {
        close(fd);
        return NULL;
    }
    X = mmap(NULL, sizeof(*X), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (X == MAP_FAILED) {
        X = NULL;
        return NULL;
    }
    pid = (int32_t)getpid();
    if (X->magic != XPORT_MAGIC || X->pid != pid) {
        if (__sync_bool_compare_and_swap(&X->lock, 0, 1)) {
            if (X->magic != XPORT_MAGIC || X->pid != pid) {
                memset(X, 0, sizeof(*X));
                X->lock = 1;
                X->sock = -1;
                X->pid = pid;
                X->unpack_align = 4;
                X->st_viewport[2] = 1280;
                X->st_viewport[3] = 720;
                X->st_scissor[2] = 1280;
                X->st_scissor[3] = 720;
                {
                    const char *ew = getenv("NFSMW_WIDTH");
                    const char *eh = getenv("NFSMW_HEIGHT");
                    int width;
                    int height;

                    if (ew == NULL || ew[0] == '\0')
                        ew = getenv("TSPGL_WIDTH");
                    if (eh == NULL || eh[0] == '\0')
                        eh = getenv("TSPGL_HEIGHT");
                    width = ew != NULL && ew[0] != '\0' ? atoi(ew) : 1280;
                    height = eh != NULL && eh[0] != '\0' ? atoi(eh) : 720;
                    if (width >= 160)
                        X->st_viewport[2] = X->st_scissor[2] = width;
                    if (height >= 120)
                        X->st_viewport[3] = X->st_scissor[3] = height;
                }
                X->magic = XPORT_MAGIC;
                fprintf(stderr, "tspgl: shared xport pid=%d\n", (int)pid);
            }
            __sync_lock_release(&X->lock);
        } else {
            while (X->magic != XPORT_MAGIC || X->pid != pid)
                usleep(200);
        }
    }
    return X;
}

static void tspgl_lock(void)
{
    struct tspgl_shared *s = tspgl_shared();
    if (!s)
        return;
    while (__sync_lock_test_and_set(&s->lock, 1))
        ;
}

static void tspgl_unlock(void)
{
    struct tspgl_shared *s = tspgl_shared();
    if (s)
        __sync_lock_release(&s->lock);
}

static void tspgl_disconnect(void)
{
    struct tspgl_shared *s = tspgl_shared();
    if (!s)
        return;
    s->cmd_used = 0;
    if (s->sock >= 0) {
        close(s->sock);
        s->sock = -1;
    }
}

static int tspgl_connect(void)
{
    struct tspgl_shared *s = tspgl_shared();
    struct sockaddr_un addr;
    int fd;
    int n;

    if (!s)
        return -1;
    if (s->sock >= 0)
        return 0;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TSPGL_SOCK, sizeof(addr.sun_path) - 1);
    for (n = 0; n < 50; ++n) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            int buf = 1024 * 1024;
            s->sock = fd;
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
            fprintf(stderr, "tspgl: shared sock=%d\n", fd);
            return 0;
        }
        usleep(100000);
    }
    fprintf(stderr, "tspgl: connect %s errno=%d\n", TSPGL_SOCK, errno);
    close(fd);
    return -1;
}

static int full_write(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int full_read(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static int tspgl_flush_locked(void)
{
    struct tspgl_shared *s = tspgl_shared();
    if (!s || !s->cmd_used)
        return 0;
    if (full_write(s->sock, s->cmdbuf, s->cmd_used) != 0) {
        s->cmd_used = 0;
        return -1;
    }
    s->cmd_used = 0;
    return 0;
}

int tspgl_call(uint32_t op, const void *in, uint32_t in_len, void *out,
               uint32_t out_cap)
{
    struct tspgl_shared *s;
    struct tspgl_hdr h, r;
    static uint8_t scratch_s[2 * 1024 * 1024];
    uint8_t *scratch = scratch_s;
    uint8_t *scratch_h = NULL;
    uint32_t seq;
    int attempt;
    int rc = -1;
    int want_reply;
    uint32_t need;

    if (op == OP_glGetError) {
        if (out && out_cap >= 4)
            memset(out, 0, 4);
        return 0;
    }

    s = tspgl_shared();
    if (!s)
        return -1;
    want_reply = tspgl_needs_reply(op);
    need = (uint32_t)sizeof(h) + in_len;
    tspgl_lock();
    for (attempt = 0; attempt < 2; ++attempt) {
        if (tspgl_connect() != 0)
            break;
        seq = ++s->seq;
        h.magic = TSPGL_MAGIC;
        h.op = op;
        h.seq = seq;
        h.len = in_len;

        if (!want_reply && need <= TSPGL_CMD_CAP) {
            if (s->cmd_used + need > TSPGL_CMD_CAP && tspgl_flush_locked() != 0) {
                if (s->fail_log < 64) {
                    fprintf(stderr,
                            "tspgl: flush op=%u retry=%d errno=%d\n", op,
                            attempt, errno);
                    s->fail_log++;
                }
                tspgl_disconnect();
                continue;
            }
            memcpy(s->cmdbuf + s->cmd_used, &h, sizeof(h));
            s->cmd_used += (uint32_t)sizeof(h);
            if (in_len) {
                memcpy(s->cmdbuf + s->cmd_used, in, in_len);
                s->cmd_used += in_len;
            }
            if (s->cmd_used >= CMD_FLUSH || op == OP_glFlush) {
                if (tspgl_flush_locked() != 0) {
                    tspgl_disconnect();
                    continue;
                }
            }
            rc = 0;
            break;
        }

        if (tspgl_flush_locked() != 0 ||
            full_write(s->sock, &h, sizeof(h)) != 0 ||
            (in_len && full_write(s->sock, in, in_len) != 0)) {
            if (s->fail_log < 64) {
                fprintf(stderr, "tspgl: call op=%u retry=%d errno=%d in_len=%u\n",
                        op, attempt, errno, in_len);
                s->fail_log++;
            }
            tspgl_disconnect();
            continue;
        }
        if (!want_reply) {
            rc = 0;
            break;
        }
        if (full_read(s->sock, &r, sizeof(r)) != 0 ||
            r.magic != TSPGL_MAGIC || r.seq != seq || r.len > TSPGL_MAX_BLOB) {
            if (s->fail_log < 64) {
                fprintf(stderr, "tspgl: reply op=%u retry=%d errno=%d\n", op,
                        attempt, errno);
                s->fail_log++;
            }
            tspgl_disconnect();
            continue;
        }
        if (r.len) {
            if (r.len > sizeof(scratch_s)) {
                scratch_h = malloc(r.len);
                if (!scratch_h) {
                    tspgl_disconnect();
                    continue;
                }
                scratch = scratch_h;
            }
            if (full_read(s->sock, scratch, r.len) != 0) {
                free(scratch_h);
                scratch_h = NULL;
                scratch = scratch_s;
                tspgl_disconnect();
                continue;
            }
            if (out && out_cap) {
                uint32_t n = r.len < out_cap ? r.len : out_cap;
                memcpy(out, scratch, n);
            }
        }
        rc = r.op == TSPGL_OK ? 0 : -1;
        break;
    }
    free(scratch_h);
    tspgl_unlock();
    return rc;
}
