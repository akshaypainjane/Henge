/* Exercise the WebDAV parser and handlers without binding a socket or starting a guest. */
#include "../host/share.c"
#include <assert.h>
#include <sys/xattr.h>

static unsigned checks;

static void request(const char *method, const char *path, const char *headers, const char *body,
                    int status, const char *contains_body) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    char message[4096], result[8192];
    char length[64] = "";
    if (!strstr(headers, "Transfer-Encoding:")) snprintf(length, sizeof length, "Content-Length: %zu\r\n", strlen(body));
    int n = snprintf(message, sizeof message, "%s %s HTTP/1.1\r\nConnection: close\r\n%s%s\r\n%s",
                     method, path, length, headers, body);
    assert(n > 0 && (size_t)n < sizeof message);
    assert(send_all(pair[0], message, (size_t)n) == 0);
    assert(shutdown(pair[0], SHUT_WR) == 0);
    Conn *c = calloc(1, sizeof *c);
    assert(c);
    c->fd = pair[1];
    __atomic_add_fetch(&active_connections, 1, __ATOMIC_RELAXED);
    connection(c);
    size_t len = 0;
    ssize_t got;
    while ((got = read(pair[0], result + len, sizeof result - len - 1)) > 0) len += (size_t)got;
    result[len] = 0;
    close(pair[0]);
    if (!status) {
        assert(!len);
    } else {
        int actual = 0;
        if (sscanf(result, "HTTP/1.1 %d", &actual) != 1 || actual != status) {
            fprintf(stderr, "%s %s: wanted %d, got %s\n", method, path, status, result);
            abort();
        }
    }
    if (contains_body) assert(strstr(result, contains_body));
    checks++;
}

int main(void) {
    char work[PATH_MAX], scratch[PATH_MAX], path[PATH_MAX], outside[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    assert(snprintf(work, sizeof work, "%s/henge-share.XXXXXX", tmp ? tmp : "/tmp") < (int)sizeof work);
    assert(mkdtemp(work) && realpath(work, scratch));
    int fixture = open(scratch, O_RDONLY | O_DIRECTORY);
    assert(fixture >= 0 && mkdirat(fixture, "shared", 0700) == 0);
    root_fd = openat(fixture, "shared", O_RDONLY | O_DIRECTORY);
    assert(root_fd >= 0 && mkdirat(fixture, "outside", 0700) == 0);
    assert(snprintf(root_path, sizeof root_path, "%s/shared", scratch) < (int)sizeof root_path);
    assert(snprintf(outside, sizeof outside, "%s/outside", scratch) < (int)sizeof outside);
    int fd = openat(fixture, "outside/secret", O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, "private", 7) == 7 && close(fd) == 0);
    assert(symlinkat(outside, root_fd, "alias") == 0);
    assert(linkat(fixture, "outside/secret", root_fd, "hardlink", 0) == 0);
    assert(snprintf(path, sizeof path, "%s/missing", outside) < (int)sizeof path);
    assert(symlinkat(path, root_fd, "dangling") == 0);

    static const char malformed[] = "GET /\0note HTTP/1.1\r\n\r\n";
    Conn *invalid = calloc(1, sizeof *invalid);
    Request *parsed = malloc(sizeof *parsed);
    assert(invalid && parsed);
    memcpy(invalid->buf, malformed, sizeof malformed);
    invalid->have = sizeof malformed - 1;
    assert(read_request(invalid, parsed) == -1);
    free(parsed);
    free(invalid);
    checks++;

    request("PUT", "/note", "", "hello", 201, NULL);
    request("GET", "/note", "", "", 200, "hello");
    request("PROPFIND", "/", "Depth: 1\r\n", "", 207, "/note");
    request("PROPFIND", "//./", "Depth: 1\r\n", "", 207, "/note");
    const char *chunked = "Transfer-Encoding: chunked\r\n";
    request("PUT", "/chunked", chunked, "3;test=1\r\nnew\r\n0\r\nTest: trailer\r\n\r\n", 201, NULL);
    request("GET", "/chunked", "", "", 200, "new");
    request("PUT", "/note", chunked, "3\r\nnew\r\nxyz\r\n", 0, NULL);
    request("PUT", "/note", chunked, "3\r\nnew\r\n0\r\n", 0, NULL);
    request("PUT", "/note", chunked, "10000000000000000\r\n", 0, NULL);
    request("PUT", "/note", chunked, "3\r\nnew0\r\n\r\n", 0, NULL);
    request("GET", "/note", "", "", 200, "hello");
    request("MKCOL", "/docs/", "", "", 201, NULL);
    request("LOCK", "/docs/file", "", "", 201, NULL);
    request("PUT", "/docs/file", "", "contents", 204, NULL);
    request("COPY", "/docs", "Destination: http://10.0.2.4/copied/\r\n", "", 201, NULL);
    request("GET", "/copied/file", "", "", 200, "contents");
    request("COPY", "/docs", "Destination: /copied\r\nOverwrite: F\r\n", "", 412, NULL);
    request("PUT", "/docs/file", "", "replacement", 204, NULL);
    request("COPY", "/docs", "Destination: /copied\r\n", "", 204, NULL);
    request("GET", "/copied/file", "", "", 200, "replacement");
    request("MOVE", "/copied/file", "Destination: /moved\r\n", "", 201, NULL);
    request("DELETE", "/copied", "", "", 204, NULL);

    request("MKCOL", "/target", "", "", 201, NULL);
    request("PUT", "/target/old", "", "old", 201, NULL);
    fd = openat(root_fd, "docs/file", O_RDONLY);
    assert(fd >= 0 && fsetxattr(fd, "org.henge.fixture", "tag", 3, 0, 0) == 0 && close(fd) == 0);
    request("COPY", "/docs/file", "Destination: /tagged\r\n", "", 201, NULL);
    fd = openat(root_fd, "tagged", O_RDONLY);
    char tag[4] = {0};
    assert(fd >= 0 && fgetxattr(fd, "org.henge.fixture", tag, sizeof tag, 0, 0) == 3 && !strcmp(tag, "tag") && close(fd) == 0);
    request("MOVE", "/docs", "Destination: /target\r\n", "", 204, NULL);
    request("GET", "/target/old", "", "", 404, NULL);
    request("GET", "/docs/file", "", "", 404, NULL);
    request("GET", "/target/file", "", "", 200, "replacement");
    request("MOVE", "/target", "Destination: /docs\r\n", "", 201, NULL);

    request("GET", "/alias/secret", "", "", 404, NULL);
    request("HEAD", "/alias/secret", "", "", 404, NULL);
    request("PROPFIND", "/alias", "", "", 404, NULL);
    request("PUT", "/alias/new", "", "blocked", 403, NULL);
    request("MKCOL", "/alias/newdir", "", "", 403, NULL);
    request("DELETE", "/alias/secret", "", "", 403, NULL);
    request("LOCK", "/alias/new", "", "", 409, NULL);
    request("LOCK", "/dangling", "", "", 403, NULL);
    request("GET", "/hardlink", "", "", 404, NULL);
    request("COPY", "/alias/secret", "Destination: /note\r\n", "", 404, NULL);
    request("MOVE", "/alias/secret", "Destination: /note\r\n", "", 404, NULL);
    request("MOVE", "/note", "Destination: /alias/secret\r\n", "", 409, NULL);
    request("COPY", "/note", "Destination: /alias/secret\r\n", "", 409, NULL);

    assert(symlinkat(outside, root_fd, "docs/alias") == 0);
    request("COPY", "/docs", "Destination: /note\r\n", "", 403, NULL);
    request("GET", "/note", "", "", 200, "hello");
    request("MOVE", "/docs/file", "Destination: /docs\r\n", "", 409, NULL);
    request("GET", "/docs/file", "", "", 200, "replacement");
    request("DELETE", "/docs", "", "", 204, NULL);
    assert(faccessat(fixture, "outside/secret", F_OK, 0) == 0);
    assert(faccessat(fixture, "outside/missing", F_OK, 0) == -1 && errno == ENOENT);
    assert(faccessat(fixture, "outside/new", F_OK, 0) == -1 && errno == ENOENT);
    assert(faccessat(fixture, "outside/newdir", F_OK, 0) == -1 && errno == ENOENT);

    request("DELETE", "//%2e/", "", "", 403, NULL);
    request("MOVE", "/note", "Destination: //\r\n", "", 400, NULL);
    request("COPY", "/note", "Destination: /.HENGE/test\r\n", "", 400, NULL);
    request("GET", "/.HENGE/install", "", "", 200, "henge-agent");
    request("GET", "/.henge/clipboard", "", "", 0, NULL);
    request("PUT", "http://10.0.2.4/%2ehenge/clipboard", "", "blocked", 0, NULL);
    request("GET", "/note", "", "", 200, "hello");

    assert(renameat(fixture, "shared", fixture, "detached") == 0 && mkdirat(fixture, "shared", 0700) == 0);
    request("GET", "/note", "", "", 403, NULL);
    request("PUT", "/new", "", "blocked", 403, NULL);
    assert(faccessat(fixture, "detached/new", F_OK, 0) == -1 && errno == ENOENT);

    close(root_fd);
    close(fixture);
    int parent = openat(AT_FDCWD, tmp ? tmp : "/tmp", O_RDONLY | O_DIRECTORY);
    assert(parent >= 0 && remove_at(parent, strrchr(work, '/') + 1, 0) == 0);
    close(parent);
    printf("WebDAV: %u request checks passed\n", checks);
}
