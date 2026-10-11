/* An i386 render callback on the default output unit (FMOD Ex's CoreAudio output): AudioUnitSetProperty
 * takes the 8-byte i386 AURenderCallbackStruct, and the callback sees a guest buffer list it can fill (silence). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint32_t type, subtype, manufacturer, flags, mask; } Desc;
typedef struct { uint32_t channels, size; void *data; } Buffer;
typedef struct { uint32_t n; Buffer b[1]; } BufferList;
typedef int (*Render)(void *, uint32_t *, const void *, uint32_t, uint32_t, BufferList *);
void *AudioComponentFindNext(void *, const Desc *);
int AudioComponentInstanceNew(void *, void **);
int AudioComponentInstanceDispose(void *);
int AudioUnitSetProperty(void *, uint32_t, uint32_t, uint32_t, const void *, uint32_t);
int AudioUnitInitialize(void *);
int AudioOutputUnitStart(void *);
int AudioOutputUnitStop(void *);

static volatile int calls, sane = 1, refcon_ok = 1;
static int render(void *refcon, uint32_t *flags, const void *ts, uint32_t bus, uint32_t frames, BufferList *io)
{
    if (refcon != (void *)0x1234 || !flags || !ts)
        refcon_ok = 0;
    if (!io || io->n < 1 || !frames)
        sane = 0;
    for (uint32_t i = 0; io && i < io->n; i++) {
        if (!io->b[i].data || io->b[i].size < frames * 4)
            sane = 0;
        else
            memset(io->b[i].data, 0, io->b[i].size);
    }
    calls++;
    return 0;
}

int main(void)
{
    Desc d = { 'auou', 'def ', 'appl', 0, 0 };
    void *comp = AudioComponentFindNext(NULL, &d), *unit = NULL;
    printf("component %d\n", comp != NULL);
    printf("instance %d\n", AudioComponentInstanceNew(comp, &unit));
    struct { Render proc; void *refcon; } cb = { render, (void *)0x1234 };
    printf("set callback %d\n", AudioUnitSetProperty(unit, 23, 1, 0, &cb, sizeof cb));
    printf("initialize %d\n", AudioUnitInitialize(unit));
    printf("start %d\n", AudioOutputUnitStart(unit));
    for (int i = 0; i < 100 && calls < 4; i++)
        usleep(20000);
    AudioOutputUnitStop(unit);
    AudioComponentInstanceDispose(unit);
    printf("callbacks %d sane %d refcon %d\n", calls >= 4, sane, refcon_ok);
    return 0;
}
