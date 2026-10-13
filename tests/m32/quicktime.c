/* QuickTime and QuickDraw are gone from macOS. Their calls Left 4 Dead 2's video code makes answer as QuickTime did on a
 * Mac that cannot play the file: EnterMovies succeeds, opening a movie fails with unimpErr (-4) and leaves no movie, and
 * the GWorld calls do nothing. The game then shows no menu movie. Expected output written by hand. */
#include <stdio.h>

typedef short OSErr;
OSErr EnterMovies(void);
OSErr GetMoviesError(void);
OSErr OpenMovieFile(const void *spec, short *ref, signed char permission);
OSErr NewMovieFromFile(void **movie, short ref, short *id, unsigned char *name, short flags, unsigned char *changed);
void SetGWorld(void *port, void *device);
void *GetGWorldPixMap(void *world);
unsigned char LockPixels(void *pixmap);
long GetPixRowBytes(void *pixmap);

int main(void)
{
    short ref = 123;
    void *movie = (void *)1;
    char spec[70] = { 0 };
    printf("EnterMovies %d\n", EnterMovies());
    OSErr open = OpenMovieFile(spec, &ref, 1);
    printf("OpenMovieFile %d ref %d\n", open, ref);
    OSErr made = NewMovieFromFile(&movie, ref, NULL, NULL, 0, NULL);
    printf("NewMovieFromFile %d movie %p error %d\n", made, movie, GetMoviesError());
    SetGWorld(NULL, NULL);
    printf("GWorld %p %d %ld\n", GetGWorldPixMap(NULL), LockPixels(NULL), GetPixRowBytes(NULL));
    return 0;
}
