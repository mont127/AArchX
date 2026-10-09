/*
 * A dependency marked delayed-init (DYLIB_USE_DELAYED_INIT, macOS 15) is not
 * part of the launch.  dyld leaves it off the image list, runs none of its
 * initializers and does not tell libobjc about it, unless an ordinary link
 * reaches it as well; a client that needs it dlopens it first.
 *
 * So every image on the list at main must be reachable from the main
 * executable through links that are not delayed-init.  Under Rosetta it is.
 * ocerz followed the delayed links too: CoreGraphics' delayed link to
 * TextRecognition put CoreML and Vision on the list and ran their
 * initializers, 505 images where dyld has 346.
 *
 * Then first use: Foundation reaches its markdown parser, libcmark-gfm, through
 * a delayed link.  Parsing markdown has to work, and has to leave the parser on
 * the list if it was not there before.
 */
#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <string.h>

static int image_index(const char *path)
{
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
        if (strcmp(_dyld_get_image_name(i), path) == 0)
            return (int)i;
    return -1;
}

static int has_leaf(const char *leaf)
{
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
        if (strstr(_dyld_get_image_name(i), leaf))
            return 1;
    return 0;
}

int main(void)
{
    int bad = 0;
    uint32_t n = _dyld_image_count();
    char *reached = calloc(n, 1);
    int queue[4096], qn = 0;
    reached[0] = 1;
    queue[qn++] = 0;
    for (int q = 0; q < qn; q++) {
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header((uint32_t)queue[q]);
        const uint8_t *lc = (const uint8_t *)(mh + 1);
        for (uint32_t k = 0; k < mh->ncmds; k++, lc += ((const struct load_command *)lc)->cmdsize) {
            uint32_t cmd = ((const struct load_command *)lc)->cmd;
            if (cmd != LC_LOAD_DYLIB && cmd != LC_LOAD_WEAK_DYLIB && cmd != LC_REEXPORT_DYLIB &&
                cmd != LC_LOAD_UPWARD_DYLIB)
                continue;
            const struct dylib_command *d = (const void *)lc;
            const struct dylib_use_command *u = (const void *)lc;
            if (d->dylib.name.offset == sizeof *u && u->marker == DYLIB_USE_MARKER &&
                (u->flags & DYLIB_USE_DELAYED_INIT) && !(u->flags & DYLIB_USE_REEXPORT))
                continue;
            int at = image_index((const char *)lc + d->dylib.name.offset);
            if (at >= 0 && !reached[at] && qn < 4096) {
                reached[at] = 1;
                queue[qn++] = at;
            }
        }
    }
    int unreached = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!reached[i]) {
            if (unreached++ < 5)
                printf("on the list but reached only through delayed links: %s\n", _dyld_get_image_name(i));
        }
    if (unreached) {
        printf("%d image(s) in all\n", unreached);
        bad = 1;
    }

    int before = has_leaf("/libcmark-gfm");
    @autoreleasepool {
        NSError *err = nil;
        NSAttributedString *s =
            [[NSAttributedString alloc] initWithMarkdownString:@"**bold** and _it_ [link](https://example.com)"
                                                       options:nil baseURL:nil error:&err];
        __block int runs = 0;
        [s enumerateAttributesInRange:NSMakeRange(0, s.length) options:0
                           usingBlock:^(NSDictionary *a, NSRange r, BOOL *stop) { runs++; }];
        if (!s || strcmp(s.string.UTF8String, "bold and it link") != 0 || runs != 5) {
            printf("markdown: %s, %d attribute runs\n", s ? s.string.UTF8String : "(nil)", runs);
            bad = 1;
        }
    }
    if (!before && !has_leaf("/libcmark-gfm")) {
        printf("markdown parsed but libcmark-gfm never joined the list\n");
        bad = 1;
    }
    if (!bad)
        printf("OK\n");
    return bad;
}
