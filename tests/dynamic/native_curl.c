/*
 * libcurl's variadic setters under native mode: curl_easy_setopt with a long,
 * a string, a curl_off_t and callbacks (write and transfer-info), with the
 * resume offset the off_t carries visible in the bytes that arrive;
 * curl_easy_getinfo answering a long and a curl_off_t; curl_share_setopt with
 * lock functions; curl_multi_setopt with a long and a timer callback.  The
 * transfer reads a file this program wrote, through a file:// URL, so no
 * network is involved.  The output must match the arm64 build's.
 */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct sink { char buf[256]; size_t n; unsigned calls; };

static size_t on_write(char *data, size_t size, size_t nitems, void *userp)
{
    struct sink *s = userp;
    size_t len = size * nitems;
    if (s->n + len < sizeof s->buf) {
        memcpy(s->buf + s->n, data, len);
        s->n += len;
    }
    s->calls++;
    return len;
}

static int xferinfo_calls;
static int on_xferinfo(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    if (clientp == (void *)&xferinfo_calls && dlnow <= dltotal + 1)
        xferinfo_calls++;
    return 0;
}

static int locks, unlocks;
static void on_lock(CURL *h, curl_lock_data data, curl_lock_access access, void *userp)
{
    (void)h; (void)data; (void)access;
    if (userp == (void *)&locks)
        locks++;
}
static void on_unlock(CURL *h, curl_lock_data data, void *userp)
{
    (void)h; (void)data;
    if (userp == (void *)&locks)
        unlocks++;
}

static int timer_calls;
static int on_timer(CURLM *multi, long timeout_ms, void *userp)
{
    (void)multi; (void)timeout_ms;
    if (userp == (void *)&timer_calls)
        timer_calls++;
    return 0;
}

int main(void)
{
    char path[] = "/tmp/ocerz_native_curl_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    const char text[] = "0123456789 native curl crossing";
    if (write(fd, text, strlen(text)) != (ssize_t)strlen(text))
        return 1;
    close(fd);
    char url[128];
    snprintf(url, sizeof url, "file://%s", path);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURLSH *share = curl_share_init();
    int s1 = curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    int s2 = curl_share_setopt(share, CURLSHOPT_LOCKFUNC, on_lock);
    int s3 = curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, on_unlock);
    int s4 = curl_share_setopt(share, CURLSHOPT_USERDATA, (void *)&locks);
    printf("share setopt %d %d %d %d\n", s1, s2, s3, s4);

    struct sink sink = { .n = 0 };
    CURL *h = curl_easy_init();
    int e1 = curl_easy_setopt(h, CURLOPT_URL, url);
    int e2 = curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_write);
    int e3 = curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    int e4 = curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    int e5 = curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_xferinfo);
    int e6 = curl_easy_setopt(h, CURLOPT_XFERINFODATA, (void *)&xferinfo_calls);
    int e7 = curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)5);
    int e8 = curl_easy_setopt(h, CURLOPT_SHARE, share);
    int e9 = curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");
    printf("easy setopt %d %d %d %d %d %d %d %d %d\n", e1, e2, e3, e4, e5, e6, e7, e8, e9);
    int rc = curl_easy_perform(h);
    long code = -1;
    curl_off_t got = -1;
    int g1 = curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    int g2 = curl_easy_getinfo(h, CURLINFO_SIZE_DOWNLOAD_T, &got);
    sink.buf[sink.n] = 0;
    printf("perform %d, %zu bytes \"%s\", getinfo %d %d code %ld size %lld\n", rc, sink.n, sink.buf, g1, g2, code,
           (long long)got);
    printf("xferinfo called: %s, locks balanced: %s\n", xferinfo_calls > 0 ? "yes" : "no",
           locks == unlocks && locks > 0 ? "yes" : "no");

    CURLM *multi = curl_multi_init();
    int m1 = curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, 6L);
    int m2 = curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, on_timer);
    int m3 = curl_multi_setopt(multi, CURLMOPT_TIMERDATA, (void *)&timer_calls);
    CURL *h2 = curl_easy_init();
    struct sink sink2 = { .n = 0 };
    curl_easy_setopt(h2, CURLOPT_URL, url);
    curl_easy_setopt(h2, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(h2, CURLOPT_WRITEDATA, &sink2);
    curl_multi_add_handle(multi, h2);
    int running = 1, rounds = 0;
    while (running && rounds++ < 1000) {
        curl_multi_perform(multi, &running);
        if (running)
            curl_multi_poll(multi, NULL, 0, 10, NULL);
    }
    sink2.buf[sink2.n] = 0;
    printf("multi setopt %d %d %d, %zu bytes \"%s\", timer called: %s\n", m1, m2, m3, sink2.n, sink2.buf,
           timer_calls > 0 ? "yes" : "no");
    curl_multi_remove_handle(multi, h2);
    curl_easy_cleanup(h2);
    curl_multi_cleanup(multi);
    curl_easy_cleanup(h);
    curl_share_cleanup(share);
    curl_global_cleanup();
    unlink(path);
    return 0;
}
