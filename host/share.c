/* The folder a virtual Mac shares with the device it runs on.

   The folder is a small WebDAV server, because macOS has a WebDAV client built in, so it mounts like any
   network drive with nothing to install for that part. The network hands each connection the guest makes to
   10.0.2.4:80 to a Unix socket in the app's own storage: no TCP port is open, so other apps can't reach this.

   /.henge/ serves the optional helper (tools/henge-agent.m) that mounts the folder inside macOS.

   Locks are accepted but not enforced: macOS only writes to a server that offers them, and there is one client. */
#include "share.h"

#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define HEAD_MAX (32 * 1024)
#define TEMP_PREFIX ".henge-put-"

static int root_fd = -1;
static char root_path[PATH_MAX];
static pthread_mutex_t namespace_lock = PTHREAD_MUTEX_INITIALIZER;
static char agent_path[PATH_MAX];

/* ---- connections ---- */

typedef struct Conn {
    int fd;
    char buf[64 * 1024]; /* read ahead */
    size_t at, have;
} Conn;

typedef struct Request {
    char method[16];
    char target[HEAD_MAX]; /* as sent, without the query */
    char path[PATH_MAX];   /* decoded */
    char destination[PATH_MAX]; /* decoded path */
    char if_header[256];
    long long length;
    int chunked, expect_continue, close, overwrite, depth;
    long long range_from, range_to; /* -1: none */
    long chunk_left;
    int body_done, chunk_needs_crlf;
} Request;

/* A growing string for XML. */
typedef struct Text {
    char *s;
    size_t len, cap;
    int failed;
} Text;

static void add(Text *t, const char *s, size_t n) {
    if (t->failed) return;
    if (t->len + n + 1 > t->cap) {
        const size_t max = 8 * 1024 * 1024;
        if (n >= max - t->len) { t->failed = 1; return; }
        size_t cap = (t->len + n + 1) * 2;
        if (cap > max) cap = max;
        char *next = realloc(t->s, cap);
        if (!next) { t->failed = 1; return; }
        t->s = next;
        t->cap = cap;
    }
    memcpy(t->s + t->len, s, n);
    t->len += n;
    t->s[t->len] = 0;
}

static void adds(Text *t, const char *s) { add(t, s, strlen(s)); }

static void addf(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void addf(Text *t, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) add(t, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

static void add_xml(Text *t, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': adds(t, "&amp;"); break;
        case '<': adds(t, "&lt;"); break;
        case '>': adds(t, "&gt;"); break;
        case '"': adds(t, "&quot;"); break;
        default: add(t, s, 1);
        }
    }
}

static void add_url(Text *t, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~/", c)) add(t, s, 1);
        else addf(t, "%%%02X", c);
    }
}

static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        c += w, n -= (size_t)w;
    }
    return 0;
}

static ssize_t conn_read(Conn *c, void *out, size_t n) {
    if (c->at < c->have) {
        size_t take = c->have - c->at < n ? c->have - c->at : n;
        memcpy(out, c->buf + c->at, take);
        c->at += take;
        return (ssize_t)take;
    }
    for (;;) {
        ssize_t r = recv(c->fd, out, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

/* A line of a chunked body's framing. */
static int conn_line(Conn *c, char *line, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        if (conn_read(c, &ch, 1) != 1) return -1;
        if (ch == '\n') {
            if (!n || line[n - 1] != '\r') return -1;
            line[--n] = 0;
            return 0;
        }
        if (!ch || (n && line[n - 1] == '\r')) return -1;
        line[n++] = ch;
    }
    return -1;
}

static int hex(int c);
static int root_is_current(void);

/* The next piece of the request's body: >0 bytes, 0 at its end, <0 if the connection broke. */
static ssize_t body_read(Conn *c, Request *r, void *out, size_t n) {
    if (r->body_done) return 0;
    if (r->chunked) {
        if (r->chunk_left == 0) {
            char line[HEAD_MAX];
            if (r->chunk_needs_crlf && (conn_line(c, line, sizeof line) != 0 || line[0])) return -1;
            if (conn_line(c, line, sizeof line) != 0 || hex((unsigned char)line[0]) < 0) return -1;
            const unsigned char *p = (const unsigned char *)line;
            int digit;
            while ((digit = hex(*p)) >= 0) {
                if (r->chunk_left > (LONG_MAX - digit) / 16) return -1;
                r->chunk_left = r->chunk_left * 16 + digit;
                p++;
            }
            if (*p && *p != ';') return -1;
            if (r->chunk_left == 0) {
                size_t trailers = 0;
                do {
                    if (conn_line(c, line, sizeof line) != 0) return -1;
                    trailers += strlen(line) + 2;
                    if (trailers > HEAD_MAX) return -1;
                } while (line[0]);
                r->body_done = 1;
                return 0;
            }
            r->chunk_needs_crlf = 1;
        }
        size_t take = (size_t)r->chunk_left < n ? (size_t)r->chunk_left : n;
        ssize_t got = conn_read(c, out, take);
        if (got <= 0) return -1;
        r->chunk_left -= got;
        return got;
    }
    if (r->length <= 0) {
        r->body_done = 1;
        return 0;
    }
    size_t take = (unsigned long long)r->length < n ? (size_t)r->length : n;
    ssize_t got = conn_read(c, out, take);
    if (got <= 0) return -1;
    r->length -= got;
    return got;
}

static void body_ready(Conn *c, Request *r) {
    if (r->expect_continue) {
        r->expect_continue = 0;
        static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(c->fd, go, sizeof go - 1);
    }
}

/* Reads what is left of a body, so the connection can carry on. Returns -1 if it can't. */
static int body_discard(Conn *c, Request *r) {
    char junk[8192];
    body_ready(c, r);
    for (;;) {
        ssize_t got = body_read(c, r, junk, sizeof junk);
        if (got == 0) return 0;
        if (got < 0) return -1;
    }
}

static int respond(Conn *c, Request *r, int status, const char *reason, const char *headers, const char *body, size_t len) {
    char head[2048];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nServer: Henge\r\nContent-Length: %zu\r\n%s%s\r\n", status, reason, len,
                     headers ? headers : "", r->close ? "Connection: close\r\n" : "");
    if (send_all(c->fd, head, (size_t)n) != 0) return -1;
    return (body && len && strcmp(r->method, "HEAD") != 0) ? send_all(c->fd, body, len) : 0;
}

static int respond_empty(Conn *c, Request *r, int status, const char *reason) {
    return respond(c, r, status, reason, NULL, NULL, 0);
}

/* ---- paths ---- */

static int hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decodes a URL path, and refuses any that could leave the folder. */
static int decode_path(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int a = hex(p[1]), b = a < 0 ? -1 : hex(p[2]);
            if (a < 0 || b < 0) return -1;
            c = a * 16 + b;
            p += 2;
            if (c == 0) return -1;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (out[0] != '/') return -1;
    /* Use one spelling of the root and refuse traversal and internal staging names. */
    char *read = out + 1, *write = out + 1;
    while (*read) {
        if (*read == '/') { read++; continue; }
        char *end = strchr(read, '/');
        size_t len = end ? (size_t)(end - read) : strlen(read);
        if ((len == 2 && !memcmp(read, "..", 2)) ||
            (len >= sizeof TEMP_PREFIX - 1 && !strncasecmp(read, TEMP_PREFIX, sizeof TEMP_PREFIX - 1))) return -1;
        if (!(len == 1 && read[0] == '.')) {
            if (write != out + 1) *write++ = '/';
            memmove(write, read, len);
            write += len;
        }
        if (!end) break;
        read = end + 1;
    }
    *write = 0;
    return 0;
}

static int internal_path(const char *path) {
    return !strncasecmp(path, "/.henge", 7) && (!path[7] || path[7] == '/');
}

static int shared_type(const struct stat *st) {
    /* Do not expose symlinks, special files, or hard links into other directories. */
    return S_ISDIR(st->st_mode) || (S_ISREG(st->st_mode) && st->st_nlink == 1);
}

static int open_shared(const char *path, struct stat *st) {
    int fd = openat(root_fd, path[1] ? path + 1 : ".", O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, st) != 0 || !shared_type(st))) {
        close(fd);
        return -1;
    }
    return fd;
}

typedef struct Parent {
    int fd;
    const char *name;
} Parent;

static void close_parent(Parent *p) { if (p->fd >= 0) close(p->fd); }

static Parent open_parent(const char *path) {
    Parent p = {.fd = -1};
    if (!path[1] || internal_path(path)) { errno = EACCES; return p; }
    const char *last = strrchr(path, '/');
    char dir[PATH_MAX] = ".";
    if (last != path) {
        memcpy(dir, path + 1, (size_t)(last - path - 1));
        dir[last - path - 1] = 0;
    }
    p.name = last + 1;
    p.fd = openat(root_fd, dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    return p;
}

#define AUTO_PARENT __attribute__((cleanup(close_parent))) Parent

static int hidden_name(const char *name) {
    return !strcmp(name, ".") || !strcmp(name, "..") ||
           !strncasecmp(name, TEMP_PREFIX, sizeof TEMP_PREFIX - 1);
}

static void temp_name(char *out, size_t cap) {
    uint64_t token;
    arc4random_buf(&token, sizeof token);
    snprintf(out, cap, TEMP_PREFIX "%016llx", (unsigned long long)token);
}

/* ---- the folder ---- */

static void add_entry(Text *t, const char *url_path, const char *name, const struct stat *st, int is_root) {
    int dir = S_ISDIR(st->st_mode);
    char modified[64], created[64];
    struct tm tm;
    gmtime_r(&st->st_mtimespec.tv_sec, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    gmtime_r(&st->st_birthtimespec.tv_sec, &tm);
    strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%SZ", &tm);
    adds(t, "<D:response><D:href>");
    add_url(t, url_path);
    if (dir && url_path[strlen(url_path) - 1] != '/') adds(t, "/");
    adds(t, "</D:href><D:propstat><D:prop><D:displayname>");
    add_xml(t, name);
    addf(t, "</D:displayname><D:getlastmodified>%s</D:getlastmodified><D:creationdate>%s</D:creationdate>", modified, created);
    if (dir) {
        adds(t, "<D:resourcetype><D:collection/></D:resourcetype>");
    } else {
        addf(t, "<D:resourcetype/><D:getcontentlength>%lld</D:getcontentlength><D:getcontenttype>application/octet-stream</D:getcontenttype>"
                "<D:getetag>\"%llx-%llx\"</D:getetag>",
             (long long)st->st_size, (unsigned long long)st->st_size, (unsigned long long)st->st_mtimespec.tv_sec);
    }
    adds(t, "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype></D:lockentry></D:supportedlock>"
            "<D:lockdiscovery/>");
    if (is_root) {
        struct statfs fs;
        if (fstatfs(root_fd, &fs) == 0) {
            addf(t, "<D:quota-available-bytes>%llu</D:quota-available-bytes><D:quota-used-bytes>%llu</D:quota-used-bytes>",
                 (unsigned long long)fs.f_bavail * fs.f_bsize, (unsigned long long)(fs.f_blocks - fs.f_bfree) * fs.f_bsize);
        }
    }
    adds(t, "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>");
}

static int do_propfind(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    struct stat st;
    int fd = open_shared(r->path, &st);
    if (fd < 0) return respond_empty(c, r, 404, "Not Found");
    Text t = {0};
    adds(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">");
    const char *slash = strrchr(r->path, '/');
    int is_root = strcmp(r->path, "/") == 0;
    add_entry(&t, r->path, is_root ? "Henge" : (slash && slash[1] ? slash + 1 : r->path), &st, is_root);
    if (S_ISDIR(st.st_mode) && r->depth != 0) {
        DIR *d = fdopendir(fd);
        if (d) fd = -1;
        struct dirent *e;
        while (d && !t.failed && (e = readdir(d))) {
            if (hidden_name(e->d_name) || (is_root && !strcasecmp(e->d_name, ".henge"))) continue;
            char url[PATH_MAX];
            struct stat cs;
            if (snprintf(url, sizeof url, "%s%s%s", r->path, r->path[strlen(r->path) - 1] == '/' ? "" : "/", e->d_name) >= (int)sizeof url) continue;
            if (fstatat(dirfd(d), e->d_name, &cs, AT_SYMLINK_NOFOLLOW) != 0 || !shared_type(&cs)) continue;
            add_entry(&t, url, e->d_name, &cs, 0);
        }
        if (d) closedir(d);
    }
    if (fd >= 0) close(fd);
    adds(&t, "</D:multistatus>");
    int rc = t.failed ? respond_empty(c, r, 507, "Insufficient Storage") :
        respond(c, r, 207, "Multi-Status", "Content-Type: application/xml; charset=utf-8\r\n", t.s, t.len);
    free(t.s);
    return rc;
}

static int do_proppatch(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    Text t = {0};
    adds(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>");
    add_url(&t, r->path);
    adds(&t, "</D:href><D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response></D:multistatus>");
    int rc = t.failed ? respond_empty(c, r, 507, "Insufficient Storage") :
        respond(c, r, 207, "Multi-Status", "Content-Type: application/xml; charset=utf-8\r\n", t.s, t.len);
    free(t.s);
    return rc;
}

static int do_get(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    struct stat st;
    int fd = open_shared(r->path, &st);
    if (fd < 0) return respond_empty(c, r, 404, "Not Found");
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return respond_empty(c, r, 405, "Method Not Allowed");
    }
    long long from = 0, to = st.st_size - 1;
    int partial = 0;
    if (r->range_from >= 0 || r->range_to >= 0) {
        if (r->range_from < 0) { /* the last N bytes */
            from = st.st_size > r->range_to ? st.st_size - r->range_to : 0;
        } else {
            from = r->range_from;
            if (r->range_to >= 0 && r->range_to < to) to = r->range_to;
        }
        if (from >= st.st_size && st.st_size > 0) {
            close(fd);
            char h[80];
            snprintf(h, sizeof h, "Content-Range: bytes */%lld\r\n", (long long)st.st_size);
            return respond(c, r, 416, "Range Not Satisfiable", h, NULL, 0);
        }
        partial = 1;
    }
    long long len = st.st_size ? to - from + 1 : 0;
    char modified[64];
    struct tm tm;
    gmtime_r(&st.st_mtimespec.tv_sec, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    char head[1024];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\nServer: Henge\r\nContent-Length: %lld\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n"
                     "Last-Modified: %s\r\nETag: \"%llx-%llx\"\r\n",
                     partial ? "206 Partial Content" : "200 OK", len, modified, (unsigned long long)st.st_size,
                     (unsigned long long)st.st_mtimespec.tv_sec);
    if (partial) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", from, to, (long long)st.st_size);
    n += snprintf(head + n, sizeof head - (size_t)n, "%s\r\n", r->close ? "Connection: close\r\n" : "");
    int rc = send_all(c->fd, head, (size_t)n);
    if (rc == 0 && strcmp(r->method, "HEAD") != 0) {
        static const size_t piece = 256 * 1024;
        char *buf = malloc(piece);
        if (!buf) rc = -1;
        long long at = from, left = len;
        while (rc == 0 && left > 0) {
            ssize_t got = pread(fd, buf, (size_t)left < piece ? (size_t)left : piece, at);
            if (got <= 0) rc = -1;
            else {
                rc = send_all(c->fd, buf, (size_t)got);
                at += got, left -= got;
            }
        }
        free(buf);
    }
    close(fd);
    return rc;
}

/* Into a temporary file beside it, then renamed, so a transfer cut short leaves the old file alone. */
static int do_put(Conn *c, Request *r) {
    AUTO_PARENT p = open_parent(r->path);
    if (p.fd < 0) {
        int gone = errno == ENOENT;
        if (body_discard(c, r) != 0) return -1;
        return gone ? respond_empty(c, r, 409, "Conflict") : respond_empty(c, r, 403, "Forbidden");
    }
    struct stat st;
    int existed = fstatat(p.fd, p.name, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (existed && !S_ISREG(st.st_mode)) {
        if (body_discard(c, r) != 0) return -1;
        return respond_empty(c, r, 405, "Method Not Allowed");
    }
    char temp[64];
    temp_name(temp, sizeof temp);
    int fd = openat(p.fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (body_discard(c, r) != 0) return -1;
        return respond_empty(c, r, 507, "Insufficient Storage");
    }
    fchmod(fd, 0644);
    body_ready(c, r);
    static const size_t piece = 256 * 1024;
    char *buf = malloc(piece);
    if (!buf) {
        close(fd);
        unlinkat(p.fd, temp, 0);
        return -1;
    }
    int full = 0;
    for (;;) {
        ssize_t got = body_read(c, r, buf, piece);
        if (got < 0) { /* the client went away: no response to give */
            close(fd);
            unlinkat(p.fd, temp, 0);
            free(buf);
            return -1;
        }
        if (got == 0) break;
        if (!full) {
            for (ssize_t done = 0; done < got;) {
                ssize_t w = write(fd, buf + done, (size_t)(got - done));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    full = 1;
                    break;
                }
                done += w;
            }
        }
    }
    free(buf);
    if (close(fd) != 0) full = 1;
    pthread_mutex_lock(&namespace_lock);
    int rc = full || !root_is_current() ? -1 : renameat(p.fd, temp, p.fd, p.name);
    if (rc) unlinkat(p.fd, temp, 0);
    pthread_mutex_unlock(&namespace_lock);
    if (rc) return respond_empty(c, r, 507, "Insufficient Storage");
    return existed ? respond_empty(c, r, 204, "No Content") : respond_empty(c, r, 201, "Created");
}

/* Never traverse a link in a tree. Bound recursion and use the pinned parent for every operation. */
#define TREE_DEPTH 128

static int remove_at(int parent, const char *name, unsigned depth) {
    if (unlinkat(parent, name, 0) == 0) return 0;
    if (depth == TREE_DEPTH) { errno = ELOOP; return -1; }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if (fd < 0) return -1;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return -1; }
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(dir);
        if (!e) { rc = errno ? -1 : 0; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (remove_at(fd, e->d_name, depth + 1) != 0) { rc = -1; break; }
    }
    int error = errno;
    closedir(dir);
    if (rc) { errno = error; return -1; }
    return unlinkat(parent, name, AT_REMOVEDIR);
}

static int do_delete(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    AUTO_PARENT p = open_parent(r->path);
    if (p.fd < 0) return errno == ENOENT ? respond_empty(c, r, 404, "Not Found") : respond_empty(c, r, 403, "Forbidden");
    pthread_mutex_lock(&namespace_lock);
    int rc = remove_at(p.fd, p.name, 0), error = errno;
    pthread_mutex_unlock(&namespace_lock);
    if (rc) return error == ENOENT ? respond_empty(c, r, 404, "Not Found") : respond_empty(c, r, 403, "Forbidden");
    return respond_empty(c, r, 204, "No Content");
}

static int do_mkcol(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    AUTO_PARENT p = open_parent(r->path);
    int error = errno;
    if (p.fd >= 0) {
        pthread_mutex_lock(&namespace_lock);
        int rc = mkdirat(p.fd, p.name, 0755);
        error = errno;
        pthread_mutex_unlock(&namespace_lock);
        if (!rc) return respond_empty(c, r, 201, "Created");
    }
    if (error == EEXIST) return respond_empty(c, r, 405, "Method Not Allowed");
    if (error == ENOENT) return respond_empty(c, r, 409, "Conflict");
    return respond_empty(c, r, 403, "Forbidden");
}

static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/* Moving Shared in Files revokes the guest's access to that directory. */
static int root_is_current(void) {
    int fd = open(root_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat pinned, named;
    int current = fstat(root_fd, &pinned) == 0 && fstat(fd, &named) == 0 && same_file(&pinned, &named);
    close(fd);
    return current;
}

/* Compare directory identities so APFS case and normalization aliases are covered. */
static int contains(int parent, const struct stat *target) {
    struct stat root, st;
    if (fstat(root_fd, &root) != 0) return 1;
    int fd = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (unsigned i = 0; fd >= 0 && i <= TREE_DEPTH; i++) {
        if (fstat(fd, &st) != 0) break;
        if (same_file(&st, target)) { close(fd); return 1; }
        if (same_file(&st, &root)) { close(fd); return 0; }
        int next = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
        close(fd);
        fd = next;
    }
    if (fd >= 0) close(fd);
    return 1;
}

/* A failed copy leaves the destination in place. The caller removes the staged tree if needed. */
static int copy_at(int from, const char *src, int to, const char *dst, unsigned depth, int *created) {
    int fd = openat(from, src, O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !shared_type(&st) || depth > TREE_DEPTH) { close(fd); return -1; }
    if (S_ISREG(st.st_mode)) {
        int out = openat(to, dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (out >= 0) *created = 1;
        int rc = out < 0 ? -1 : fcopyfile(fd, out, NULL, COPYFILE_DATA | COPYFILE_XATTR);
        if (out >= 0 && close(out) != 0) rc = -1;
        close(fd);
        return rc;
    }
    if (mkdirat(to, dst, 0755) != 0) { close(fd); return -1; }
    *created = 1;
    int out = openat(to, dst, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    DIR *dir = out >= 0 ? fdopendir(fd) : NULL;
    int rc = dir ? 0 : -1;
    while (!rc) {
        errno = 0;
        struct dirent *e = readdir(dir);
        if (!e) { rc = errno ? -1 : 0; break; }
        if (hidden_name(e->d_name)) continue;
        int child_created = 0;
        rc = copy_at(fd, e->d_name, out, e->d_name, depth + 1, &child_created);
    }
    if (dir) closedir(dir);
    else close(fd);
    if (out >= 0) close(out);
    return rc;
}

/* Existing collections cannot be overwritten with an ordinary rename. Swap
   them first, then hide the old destination under a private name for cleanup. */
static int move_at(Parent from, Parent to, int overwrite, int existed, int same) {
    if (!overwrite) return renameatx_np(from.fd, from.name, to.fd, to.name, RENAME_EXCL);
    if (!existed || same) return renameat(from.fd, from.name, to.fd, to.name);
    char temp[64];
    temp_name(temp, sizeof temp);
    if (renameatx_np(from.fd, from.name, to.fd, to.name, RENAME_SWAP) != 0) return -1;
    if (renameatx_np(from.fd, from.name, from.fd, temp, RENAME_EXCL) != 0) {
        int error = errno;
        renameatx_np(from.fd, from.name, to.fd, to.name, RENAME_SWAP);
        errno = error;
        return -1;
    }
    remove_at(from.fd, temp, 0);
    return 0;
}

/* Return an HTTP status without writing to the socket under the namespace lock. */
static int move_copy_status(Request *r, int move) {
    AUTO_PARENT from = open_parent(r->path), to = open_parent(r->destination);
    struct stat src, dst;
    if (from.fd < 0 || fstatat(from.fd, from.name, &src, AT_SYMLINK_NOFOLLOW) != 0 || !shared_type(&src))
        return 404;
    if (to.fd < 0) return 409;
    int existed = fstatat(to.fd, to.name, &dst, AT_SYMLINK_NOFOLLOW) == 0;
    if (existed && !r->overwrite) return 412;
    int same = existed && same_file(&src, &dst);
    if (same && !move) return 403;
    if ((S_ISDIR(src.st_mode) && contains(to.fd, &src)) ||
        (existed && S_ISDIR(dst.st_mode) && contains(from.fd, &dst))) return 409;

    int rc;
    if (move) {
        rc = move_at(from, to, r->overwrite, existed, same);
    } else {
        char temp[64];
        int created = 0;
        temp_name(temp, sizeof temp);
        rc = copy_at(from.fd, from.name, to.fd, temp, 0, &created);
        if (!rc) rc = renameatx_np(to.fd, temp, to.fd, to.name, existed ? RENAME_SWAP : RENAME_EXCL);
        int error = errno;
        if (created && (rc || existed)) remove_at(to.fd, temp, 0);
        errno = error;
    }
    if (rc) {
        if (errno == EEXIST && !r->overwrite) return 412;
        if (errno == ENOENT || errno == ENOTEMPTY || errno == EISDIR || errno == ENOTDIR) return 409;
        return 403;
    }
    return existed ? 204 : 201;
}

static int do_move_copy(Conn *c, Request *r, int move) {
    if (body_discard(c, r) != 0) return -1;
    if (!r->destination[0] || !r->path[1] || !r->destination[1] || internal_path(r->destination))
        return respond_empty(c, r, 400, "Bad Request");
    pthread_mutex_lock(&namespace_lock);
    int status = move_copy_status(r, move);
    pthread_mutex_unlock(&namespace_lock);
    switch (status) {
        case 201: return respond_empty(c, r, 201, "Created");
        case 204: return respond_empty(c, r, 204, "No Content");
        case 404: return respond_empty(c, r, 404, "Not Found");
        case 409: return respond_empty(c, r, 409, "Conflict");
        case 412: return respond_empty(c, r, 412, "Precondition Failed");
        default: return respond_empty(c, r, 403, "Forbidden");
    }
}

static int do_lock(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    static unsigned long counter;
    char token[128];
    const char *open_ = strchr(r->if_header, '<'), *end = open_ ? strchr(open_, '>') : NULL;
    if (open_ && end && (size_t)(end - open_) < sizeof token) { /* a refresh keeps its token */
        snprintf(token, sizeof token, "%.*s", (int)(end - open_ - 1), open_ + 1);
    } else {
        snprintf(token, sizeof token, "opaquelocktoken:henge-%lx-%lx", (unsigned long)time(NULL), __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
    }
    int status = 200;
    if (r->path[1]) {
        AUTO_PARENT p = open_parent(r->path);
        struct stat st;
        if (p.fd < 0) return respond_empty(c, r, 409, "Conflict");
        if (fstatat(p.fd, p.name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) return respond_empty(c, r, 409, "Conflict");
            pthread_mutex_lock(&namespace_lock);
            int fd = openat(p.fd, p.name, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0644);
            pthread_mutex_unlock(&namespace_lock);
            if (fd < 0) return respond_empty(c, r, 409, "Conflict");
            close(fd);
            status = 201;
        } else if (!shared_type(&st)) return respond_empty(c, r, 403, "Forbidden");
    }
    Text t = {0};
    addf(&t, "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>"
             "<D:locktype><D:write/></D:locktype><D:lockscope><D:exclusive/></D:lockscope><D:depth>0</D:depth>"
             "<D:timeout>Second-3600</D:timeout><D:locktoken><D:href>%s</D:href></D:locktoken></D:activelock></D:lockdiscovery></D:prop>",
         token);
    char headers[256];
    snprintf(headers, sizeof headers, "Content-Type: application/xml; charset=utf-8\r\nLock-Token: <%s>\r\n", token);
    int rc = t.failed ? respond_empty(c, r, 507, "Insufficient Storage") :
        respond(c, r, status, status == 201 ? "Created" : "OK", headers, t.s, t.len);
    free(t.s);
    return rc;
}

/* ---- /.henge/ ---- */

/* What "curl -s http://10.0.2.4/.henge/install | sh" runs in macOS. */
static const char install_script[] =
    "#!/bin/sh\n"
    "D=\"$HOME/Library/Application Support/Henge\"\n"
    "P=\"$HOME/Library/LaunchAgents/com.henge.agent.plist\"\n"
    "mkdir -p \"$D\" \"$HOME/Library/LaunchAgents\"\n"
    "launchctl bootout \"gui/$(id -u)/com.henge.agent\" 2>/dev/null\n"
    "curl -fsS http://" HV_SHARE_ADDR "/.henge/agent | gunzip > \"$D/henge-agent\" || { echo 'Could not reach Henge.'; exit 1; }\n"
    "chmod 755 \"$D/henge-agent\"\n"
    "cat > \"$P\" <<EOF\n"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>\n"
    "<key>Label</key><string>com.henge.agent</string>\n"
    "<key>ProgramArguments</key><array><string>$D/henge-agent</string></array>\n"
    "<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>\n"
    "<key>LimitLoadToSessionType</key><string>Aqua</string>\n"
    "<key>ProcessType</key><string>Background</string>\n"
    "</dict></plist>\n"
    "EOF\n"
    "launchctl bootstrap \"gui/$(id -u)\" \"$P\"\n"
    "echo 'Henge is set up. You can close this window.'\n";

static int serve_agent(Conn *c, Request *r) {
    if (body_discard(c, r) != 0) return -1;
    int fd = agent_path[0] ? open(agent_path, O_RDONLY) : -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        return respond_empty(c, r, 404, "Not Found");
    }
    char *data = malloc((size_t)st.st_size);
    ssize_t got = data ? pread(fd, data, (size_t)st.st_size, 0) : -1;
    close(fd);
    int rc = got == st.st_size ? respond(c, r, 200, "OK", "Content-Type: application/octet-stream\r\n", data, (size_t)got)
                               : respond_empty(c, r, 500, "Internal Server Error");
    free(data);
    return rc;
}

static int serve_henge(Conn *c, Request *r) {
    const char *what = r->path[7] == '/' ? r->path + 8 : "";
    if (!strcmp(what, "install") && !strcmp(r->method, "GET")) {
        if (body_discard(c, r) != 0) return -1;
        return respond(c, r, 200, "OK", "Content-Type: text/plain\r\n", install_script, sizeof install_script - 1);
    }
    if (!strcmp(what, "agent") && !strcmp(r->method, "GET")) return serve_agent(c, r);
    /* An installed older helper backs off on a closed connection, but retries
       immediately after an HTTP response. */
    if (!strcmp(what, "clipboard")) return -1;
    if (body_discard(c, r) != 0) return -1;
    return respond_empty(c, r, 404, "Not Found");
}

/* ---- requests ---- */

static int header_is(const char *line, const char *name, const char **value) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return 0;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *value = v;
    return 1;
}

/* 1 with a request, 0 when the client is done, -1 on a malformed one. */
static int read_request(Conn *c, Request *r) {
    static __thread char head[HEAD_MAX];
    size_t n = 0;
    for (;;) {
        char ch;
        ssize_t got = conn_read(c, &ch, 1);
        if (got <= 0) return n ? -1 : 0;
        if (!ch || n + 1 >= sizeof head) return -1;
        head[n++] = ch;
        if (n >= 4 && !memcmp(head + n - 4, "\r\n\r\n", 4)) break;
        if (n == 2 && !memcmp(head, "\r\n", 2)) n = 0; /* blank lines between requests */
    }
    head[n] = 0;
    memset(r, 0, sizeof *r);
    r->range_from = r->range_to = -1;
    r->overwrite = 1;
    r->depth = 1;
    char *line = head, *eol = strstr(line, "\r\n");
    if (!eol) return -1;
    *eol = 0;
    char version[16];
    if (sscanf(line, "%15s %32767s %15s", r->method, r->target, version) != 3) return -1;
    char *query = strchr(r->target, '?');
    if (query) *query = 0;
    /* A client may send the whole URL. */
    if (!strncasecmp(r->target, "http://", 7)) {
        char *p = strchr(r->target + 7, '/');
        memmove(r->target, p ? p : "/", strlen(p ? p : "/") + 1);
    }
    if (decode_path(r->target, r->path, sizeof r->path) != 0) return -1;
    if (!strcmp(version, "HTTP/1.0")) r->close = 1;
    for (line = eol + 2; *line; line = eol + 2) {
        eol = strstr(line, "\r\n");
        if (!eol) break;
        *eol = 0;
        const char *v;
        if (header_is(line, "Content-Length", &v)) r->length = strtoll(v, NULL, 10);
        else if (header_is(line, "Transfer-Encoding", &v)) r->chunked = strcasestr(v, "chunked") != NULL;
        else if (header_is(line, "Expect", &v)) r->expect_continue = strcasestr(v, "100-continue") != NULL;
        else if (header_is(line, "Connection", &v)) r->close = strcasestr(v, "close") != NULL || r->close;
        else if (header_is(line, "Depth", &v)) r->depth = v[0] == '0' ? 0 : 1;
        else if (header_is(line, "Overwrite", &v)) r->overwrite = v[0] != 'F' && v[0] != 'f';
        else if (header_is(line, "If", &v)) snprintf(r->if_header, sizeof r->if_header, "%s", v);
        else if (header_is(line, "Destination", &v)) {
            const char *p = v;
            if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) {
                p = strchr(strstr(p, "//") + 2, '/');
                if (!p) p = "/";
            }
            if (decode_path(p, r->destination, sizeof r->destination) != 0) r->destination[0] = 0;
        } else if (header_is(line, "Range", &v) && !strncasecmp(v, "bytes=", 6)) {
            long long a = -1, b = -1;
            const char *s = v + 6;
            if (*s == '-') b = strtoll(s + 1, NULL, 10);
            else {
                char *end;
                a = strtoll(s, &end, 10);
                if (*end == '-' && end[1]) b = strtoll(end + 1, NULL, 10);
            }
            r->range_from = a, r->range_to = b;
        }
    }
    return 1;
}

static int handle(Conn *c, Request *r) {
    if (internal_path(r->path)) return serve_henge(c, r);
    const char *m = r->method;
    if (!strcmp(m, "OPTIONS")) {
        if (body_discard(c, r) != 0) return -1;
        return respond(c, r, 200, "OK",
                       "DAV: 1, 2\r\nMS-Author-Via: DAV\r\nAllow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, MOVE, COPY, LOCK, UNLOCK\r\n", NULL, 0);
    }
    if (!strcmp(m, "PROPFIND")) return do_propfind(c, r);
    if (!strcmp(m, "PROPPATCH")) return do_proppatch(c, r);
    if (!strcmp(m, "GET") || !strcmp(m, "HEAD")) return do_get(c, r);
    if (!strcmp(m, "PUT")) return do_put(c, r);
    if (!strcmp(m, "DELETE")) return do_delete(c, r);
    if (!strcmp(m, "MKCOL")) return do_mkcol(c, r);
    if (!strcmp(m, "MOVE")) return do_move_copy(c, r, 1);
    if (!strcmp(m, "COPY")) return do_move_copy(c, r, 0);
    if (!strcmp(m, "LOCK")) return do_lock(c, r);
    if (!strcmp(m, "UNLOCK")) {
        if (body_discard(c, r) != 0) return -1;
        return respond_empty(c, r, 204, "No Content");
    }
    if (body_discard(c, r) != 0) return -1;
    return respond_empty(c, r, 405, "Method Not Allowed");
}

static unsigned active_connections;

static void *connection(void *arg) {
    Conn *c = arg;
    Request *r = malloc(sizeof *r);
    while (r) {
        int got = read_request(c, r);
        if (got <= 0) {
            if (got < 0) {
                memset(r, 0, sizeof *r);
                strcpy(r->method, "GET");
                r->close = 1;
                respond_empty(c, r, 400, "Bad Request");
            }
            break;
        }
        if (!root_is_current()) {
            r->close = 1;
            respond_empty(c, r, 403, "Forbidden");
            break;
        }
        if (getenv("HV_SHARE_LOG")) fprintf(stderr, "share: %s %s\n", r->method, r->path);
        if (handle(c, r) != 0 || r->close) break;
    }
    close(c->fd);
    free(r);
    free(c);
    __atomic_sub_fetch(&active_connections, 1, __ATOMIC_RELAXED);
    return NULL;
}

static void *listener(void *arg) {
    int server = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            usleep(100000);
            continue;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        struct timeval idle = {.tv_sec = 30};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof idle);
        if (__atomic_load_n(&active_connections, __ATOMIC_RELAXED) >= 32) { close(fd); continue; }
        Conn *c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->fd = fd;
        __atomic_add_fetch(&active_connections, 1, __ATOMIC_RELAXED);
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, connection, c) != 0) {
            close(fd);
            free(c);
            __atomic_sub_fetch(&active_connections, 1, __ATOMIC_RELAXED);
        }
        pthread_attr_destroy(&a);
    }
    return NULL;
}

int hv_share_start(const char *folder, const char *socket_path, const char *agent) {
    static int started;
    if (started) return root_is_current() ? 0 : -1;
    if (!realpath(folder, root_path)) return -1;
    if (agent) snprintf(agent_path, sizeof agent_path, "%s", agent);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(socket_path) >= sizeof addr.sun_path) {
        fprintf(stderr, "hengevm: share: socket path too long: %s\n", socket_path);
        return -1;
    }
    root_fd = open(root_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if (root_fd < 0) return -1;
    strcpy(addr.sun_path, socket_path);
    unlink(socket_path);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(server, 16) != 0) {
        fprintf(stderr, "hengevm: share: %s: %s\n", socket_path, strerror(errno));
        if (server >= 0) close(server);
        close(root_fd);
        root_fd = -1;
        return -1;
    }
    chmod(socket_path, 0600);
    pthread_t t;
    if (pthread_create(&t, NULL, listener, (void *)(intptr_t)server) != 0) {
        close(server);
        close(root_fd);
        root_fd = -1;
        unlink(socket_path);
        return -1;
    }
    pthread_detach(t);
    started = 1;
    return 0;
}
