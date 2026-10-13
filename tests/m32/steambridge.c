/* The Steam bridge (src/m32_steam.c) against tests/m32/steamfake/steamclient.c, an arm64 stand-in for Steam's client
 * that tests/m32/build.sh puts beside the programs: an i386 program that dlopens a steamclient.dylib with no i386
 * slice gets the host's, and its interfaces cross by the notations in steam.api (make steam32). Plain results, a
 * 64-bit result and argument, a C string, output pointers, interfaces returned by an interface and by name, an unknown
 * version, one object under two versions (each gets its own vtable), the callback queue, a call result, string
 * results from two threads at once, and Steam Input's records returned by value (2 bytes in a register, 13 and 40
 * through a hidden pointer the method pops). steambridge.expected is this program built for arm64 and run
 * natively against the same stand-in. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct Obj { void **vt; } Obj;
#define SLOT(o, k, T) ((T)((Obj *)(o))->vt[k])

typedef struct { int32_t user, id; uint8_t *param; int32_t size; } CallbackMsg;
#pragma pack(push, 1)   /* as isteaminput.h declares them */
typedef struct { bool state, active; } Digital;
typedef struct { int32_t mode; float x, y; bool active; } Analog;
typedef struct { float f[10]; } Motion;
#pragma pack(pop)

static void *g_utils;
static void *languages(void *arg)
{
    (void)arg;
    long bad = 0;
    for (int i = 0; i < 3000; i++) {
        const char *s = SLOT(g_utils, 23, const char *(*)(void *))(g_utils);
        bad += !s || s[0] != 'L' || strlen(s) != 5;
    }
    return (void *)bad;
}

int main(void)
{
    char path[1024];
    if (!getcwd(path, sizeof path - 32)) return 1;
    snprintf(path + strlen(path), 32, "/steamclient.dylib");
    void *h = dlopen(path, RTLD_NOW);
    if (!h) { printf("dlopen failed\n"); return 1; }
    void *(*create)(const char *, int *) = (void *(*)(const char *, int *))dlsym(h, "CreateInterface");
    bool (*bget)(int, CallbackMsg *) = (bool (*)(int, CallbackMsg *))dlsym(h, "Steam_BGetCallback");
    bool (*bfree)(int) = (bool (*)(int))dlsym(h, "Steam_FreeLastCallback");
    bool (*result)(int, uint64_t, void *, int, int, bool *) =
        (bool (*)(int, uint64_t, void *, int, int, bool *))dlsym(h, "Steam_GetAPICallResult");
    printf("symbols %s\n", create && bget && bfree && result ? "found" : "missing");

    int rc = -1;
    void *none = create("SteamNope001", &rc);
    printf("unknown version: %s rc %d\n", none ? "object" : "null", rc);
    void *client = create("SteamClient017", &rc);
    printf("client: %s rc %d\n", client ? "object" : "null", rc);
    if (!client) return 1;
    int pipe = SLOT(client, 0, int (*)(void *))(client);
    int user = SLOT(client, 2, int (*)(void *, int))(client, pipe);
    printf("pipe %d user %d\n", pipe, user);

    void *u = SLOT(client, 5, void *(*)(void *, int, int, const char *))(client, user, pipe, "SteamUser018");
    void *t = SLOT(client, 9, void *(*)(void *, int, const char *))(client, pipe, "SteamUtils007");
    void *t2 = SLOT(client, 9, void *(*)(void *, int, const char *))(client, pipe, "SteamUtils007");
    void *bad = SLOT(client, 9, void *(*)(void *, int, const char *))(client, pipe, "SteamUtils999");
    printf("user %s utils %s same utils %s wrong version %s\n", u ? "object" : "null", t ? "object" : "null",
           t == t2 ? "yes" : "no", bad ? "object" : "null");
    if (!u || !t) return 1;
    void *t8 = SLOT(client, 9, void *(*)(void *, int, const char *))(client, pipe, "SteamUtils008");
    printf("utils 008 %s, big picture %d\n", t8 ? "object" : "null", t8 ? SLOT(t8, 26, bool (*)(void *))(t8) : -1);

    uint64_t id = SLOT(u, 2, uint64_t (*)(void *))(u);
    char folder[64] = "";
    bool got = SLOT(u, 6, bool (*)(void *, char *, int))(u, folder, sizeof folder);
    printf("steamid %016llx folder %d '%s'\n", (unsigned long long)id, got, folder);

    uint32_t w = 0, hgt = 0;
    bool img = SLOT(t, 5, bool (*)(void *, int, uint32_t *, uint32_t *))(t, 5, &w, &hgt);
    printf("appid %u realtime %u country %s image %d %ux%u reason %d\n", SLOT(t, 9, uint32_t (*)(void *))(t),
           SLOT(t, 3, uint32_t (*)(void *))(t), SLOT(t, 4, const char *(*)(void *))(t), img, w, hgt,
           SLOT(t, 12, int (*)(void *, uint64_t))(t, 0x0000000300000007ull));

    CallbackMsg m = { 0 };
    bool any = bget(pipe, &m);
    printf("callback %d: user %d id %d size %d payload", any, m.user, m.id, m.size);
    for (int i = 0; any && i < m.size; i++) printf(" %02x", m.param[i]);
    printf("\n");
    bfree(pipe);
    printf("after free: %d\n", bget(pipe, &m));

    uint8_t buf[6] = { 0 };
    bool failed = true;
    bool ok = result(pipe, 0x10000000full, buf, sizeof buf, 1101, &failed);
    printf("call result %d failed %d:", ok, failed);
    for (int i = 0; i < 6; i++) printf(" %02x", buf[i]);
    printf("\n");

    void *in = create("SteamInput005", &rc);
    if (!in) return 1;
    Digital dg = { 0, 0 };
    Analog an = { 0, 0, 0, 0 };
    Motion mo = { { 0 } };
    uintptr_t sp0 = 0, sp1 = 0;
#if defined(__i386__)   /* the methods pop the hidden pointer themselves, so the stack comes back where it was */
    __asm__ volatile("movl %%esp, %0" : "=r"(sp0));
#endif
    for (int i = 0; i < 100; i++) {
        dg = SLOT(in, 17, Digital (*)(void *, uint64_t, uint64_t))(in, 5, 3);
        an = SLOT(in, 21, Analog (*)(void *, uint64_t, uint64_t))(in, 7, 6);
        mo = SLOT(in, 29, Motion (*)(void *, uint64_t))(in, 2);
    }
#if defined(__i386__)
    __asm__ volatile("movl %%esp, %0" : "=r"(sp1));
#endif
    int stack_ok = sp0 == sp1;
    printf("input digital %d %d analog %d %.2f %.2f %d motion %.0f %.0f stack %d\n", dg.state, dg.active, an.mode,
           an.x, an.y, an.active, mo.f[0], mo.f[9], stack_ok);

    g_utils = t;
    pthread_t th[2];
    void *wrong[2] = { 0 };
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, languages, NULL);
    for (int i = 0; i < 2; i++) pthread_join(th[i], &wrong[i]);
    printf("strings from two threads: bad %ld\n", (long)wrong[0] + (long)wrong[1]);
    return 0;
}
