/* A stand-in for Steam's client, built for arm64 as tests/m32/bin/steamclient.dylib (tests/m32/build.sh), so the
 * i386 Steam bridge (src/m32_steam.c) can be tested without Steam: CreateInterface hands out a SteamClient017,
 * which hands out a SteamUser018 and a SteamUtils007, each a C object whose first word is a table of functions in
 * the slots the Steamworks interfaces of those versions put them, `this` first as an Itanium virtual call takes
 * it.  Slots tests/m32/steambridge.c does not call answer 0.  The callback queue holds one message. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Obj { void *const *vt; } Obj;
static uint64_t zero(void) { return 0; }

static int client_create_pipe(Obj *self) { (void)self; return 7; }
static int client_connect_global(Obj *self, int pipe) { (void)self; return pipe * 10 + 1; }
static Obj g_user, g_utils;
static void *client_user(Obj *self, int user, int pipe, const char *ver)
{
    (void)self, (void)user, (void)pipe;
    return strcmp(ver, "SteamUser018") ? NULL : &g_user;
}
static void *client_utils(Obj *self, int pipe, const char *ver)
{
    (void)self, (void)pipe;
    return strcmp(ver, "SteamUtils007") ? NULL : &g_utils;
}
static void *client_vt[40];

static uint64_t user_steamid(Obj *self) { (void)self; return 0x0110000100000042ull; }
static bool user_folder(Obj *self, char *buf, int cb) { (void)self; snprintf(buf, (size_t)cb, "/fake/userdata"); return true; }
static void *user_vt[40];

static uint32_t utils_realtime(Obj *self) { (void)self; return 1234567890u; }
static const char *utils_country(Obj *self) { (void)self; return "NZ"; }
static bool utils_image(Obj *self, int image, uint32_t *w, uint32_t *h) { (void)self; *w = (uint32_t)image * 2; *h = (uint32_t)image * 3; return true; }
static uint32_t utils_appid(Obj *self) { (void)self; return 35140; }
static int utils_reason(Obj *self, uint64_t call) { (void)self; return (int)(call >> 32) * 100 + (int)(uint32_t)call; }
static void *utils_vt[40];

static Obj g_client;
__attribute__((constructor)) static void init(void)
{
    for (int i = 0; i < 40; i++)
        client_vt[i] = user_vt[i] = utils_vt[i] = (void *)zero;
    client_vt[0] = (void *)client_create_pipe;
    client_vt[2] = (void *)client_connect_global;
    client_vt[5] = (void *)client_user;
    client_vt[9] = (void *)client_utils;
    user_vt[2] = (void *)user_steamid;
    user_vt[6] = (void *)user_folder;
    utils_vt[3] = (void *)utils_realtime;
    utils_vt[4] = (void *)utils_country;
    utils_vt[5] = (void *)utils_image;
    utils_vt[9] = (void *)utils_appid;
    utils_vt[12] = (void *)utils_reason;
    g_client.vt = client_vt;
    g_user.vt = user_vt;
    g_utils.vt = utils_vt;
}

void *CreateInterface(const char *ver, int *rc)
{
    int ok = !strcmp(ver, "SteamClient017");
    if (rc) *rc = ok ? 0 : 1;
    return ok ? &g_client : NULL;
}

typedef struct { int32_t user, id; uint8_t *param; int32_t size; } CallbackMsg;
static uint8_t g_payload[12] = { 0x44, 0x89, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 };   /* {uint64 game 35140, int result 1} */
static int g_pending = 1;
bool Steam_BGetCallback(int pipe, CallbackMsg *m)
{
    if (!g_pending)
        return false;
    *m = (CallbackMsg){ pipe * 10 + 1, 1101, g_payload, (int32_t)sizeof g_payload };
    return true;
}
bool Steam_FreeLastCallback(int pipe) { (void)pipe; g_pending = 0; return true; }
bool Steam_GetAPICallResult(int pipe, uint64_t call, void *buf, int cub, int expected, bool *failed)
{
    (void)pipe;
    for (int i = 0; i < cub; i++)
        ((uint8_t *)buf)[i] = (uint8_t)(call + (uint64_t)i);
    *failed = false;
    return expected == 1101;
}
