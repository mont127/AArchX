/*
 * AArchX.app: runs the Intel programs dropped on it under ocerz.
 *
 * A program reaches it by being dropped on its window or its Dock icon, by
 * Open With in Finder, or through Choose.  Each one runs as
 * `ocerz [-native] <path>` with its output in ~/Library/Logs/AArchX, and the
 * window lists every run with how it ended.  ocerz is looked for inside the
 * bundle, then beside it, then in ~/.aarchx and Homebrew's bin.  A Windows
 * program runs as `ocerz <wine> <path>` with a Wine build found on the Mac,
 * in a prefix of the app's own.  CrossOver bottles can be switched to AArchX
 * one by one: the bottle's [Wine] BinPath, CrossOver's documented way to use
 * another Wine, puts two small scripts ahead of CrossOver's own bin, and they
 * run CrossOver's wineloader and wineserver under ocerz instead of Rosetta.
 * CrossOver's own files are not modified.
 */
#import <AppKit/AppKit.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <signal.h>
#include <sys/xattr.h>

enum { ARCH_X86_64 = 1, ARCH_I386 = 2, ARCH_ARM64 = 4 };

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

/* Which architectures a Mach-O file carries, from its header alone. */
static int macho_arches(NSString *path)
{
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!fh)
        return 0;
    NSData *d = [fh readDataOfLength:4096];
    [fh closeFile];
    if (d.length < sizeof(uint32_t) * 2)
        return 0;
    const uint8_t *p = d.bytes;
    uint32_t magic;
    memcpy(&magic, p, 4);
    int arches = 0;
    if (magic == FAT_CIGAM || magic == FAT_CIGAM_64) {
        uint32_t n;
        memcpy(&n, p + 4, 4);
        n = be32(n);
        size_t each = magic == FAT_CIGAM_64 ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
        for (uint32_t i = 0; i < n && 8 + (i + 1) * each <= d.length; i++) {
            int32_t cpu;
            memcpy(&cpu, p + 8 + i * each, 4);
            cpu = (int32_t)be32((uint32_t)cpu);
            if (cpu == CPU_TYPE_X86_64) arches |= ARCH_X86_64;
            if (cpu == CPU_TYPE_I386) arches |= ARCH_I386;
            if (cpu == CPU_TYPE_ARM64) arches |= ARCH_ARM64;
        }
    } else if (magic == MH_MAGIC_64 || magic == MH_MAGIC) {
        int32_t cpu;
        memcpy(&cpu, p + 4, 4);
        if (cpu == CPU_TYPE_X86_64) arches |= ARCH_X86_64;
        if (cpu == CPU_TYPE_I386) arches |= ARCH_I386;
        if (cpu == CPU_TYPE_ARM64) arches |= ARCH_ARM64;
    }
    return arches;
}

/* The executable a path stands for: an app bundle's, or the file itself. */
static NSString *executable_of(NSString *path)
{
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir])
        return nil;
    if (!dir)
        return path;
    return [NSBundle bundleWithPath:path].executablePath;
}

static BOOL rosetta_cache_present(void)
{
    for (NSString *d in @[ @"/System/Volumes/Preboot/Cryptexes/Rosetta", @"/System/Volumes/Preboot/Cryptexes/OS" ]) {
        NSString *f = [d stringByAppendingPathComponent:@"System/Library/dyld/dyld_shared_cache_x86_64"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:f])
            return YES;
    }
    return NO;
}

static NSString *find_ocerz(void)
{
    NSString *bundle = [NSBundle mainBundle].bundlePath;
    NSString *custom = [[NSUserDefaults standardUserDefaults] stringForKey:@"OcerzPath"];
    NSMutableArray *candidates = [NSMutableArray array];
    if (custom.length)
        [candidates addObject:custom];
    [candidates addObjectsFromArray:@[
        [bundle stringByAppendingPathComponent:@"Contents/Resources/aarchx/ocerz"],
        [[bundle stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"ocerz"],
        [NSHomeDirectory() stringByAppendingPathComponent:@".aarchx/ocerz"],
        @"/opt/homebrew/bin/ocerz",
        @"/usr/local/bin/ocerz",
    ]];
    for (NSString *c in candidates)
        if ([[NSFileManager defaultManager] isExecutableFileAtPath:c])
            return c;
    return nil;
}

/* A Wine build the app can hand Windows programs to. */
@interface WineBuild : NSObject
@property(copy) NSString *name;
@property(copy) NSString *wine;
@end
@implementation WineBuild
@end

/* Wine builds on this Mac: WineHQ's app bundles wherever Launch Services or the
 * usual folders have them (MacNdCheese keeps its own in its deps), Whisky's,
 * the Game Porting Toolkit's and a wine on Homebrew's PATH.  Only x86_64 ones
 * count, and each appears once however many routes lead to it. */
static NSArray<WineBuild *> *find_wines(void)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableArray<WineBuild *> *found = [NSMutableArray array];
    NSMutableSet *seen = [NSMutableSet set];
    void (^add)(NSString *, NSString *) = ^(NSString *name, NSString *wine) {
        NSString *real = wine.stringByResolvingSymlinksInPath;
        if ([seen containsObject:real] || ![fm isExecutableFileAtPath:real] || !(macho_arches(real) & ARCH_X86_64))
            return;
        [seen addObject:real];
        WineBuild *b = [WineBuild new];
        b.name = name;
        b.wine = real;
        [found addObject:b];
    };

    NSString *home = NSHomeDirectory();
    NSMutableArray *apps = [NSMutableArray array];
    for (NSString *dir in @[ @"/Applications", [home stringByAppendingPathComponent:@"Applications"],
                             [home stringByAppendingPathComponent:@"Library/Application Support/MacNCheese/deps"] ])
        for (NSString *n in @[ @"Wine Staging.app", @"Wine Stable.app", @"Wine Devel.app" ])
            [apps addObject:[dir stringByAppendingPathComponent:n]];
    for (NSString *bid in @[ @"org.winehq.wine-staging.wine", @"org.winehq.wine-stable.wine", @"org.winehq.wine-devel.wine" ])
        for (NSURL *u in [[NSWorkspace sharedWorkspace] URLsForApplicationsWithBundleIdentifier:bid])
            [apps addObject:u.path];
    for (NSString *app in apps) {
        NSString *wine = [app stringByAppendingPathComponent:@"Contents/Resources/wine/bin/wine"];
        if (![fm fileExistsAtPath:wine])
            continue;
        NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Contents/Info.plist"]];
        NSString *version = info[@"CFBundleShortVersionString"];
        NSString *name = app.lastPathComponent.stringByDeletingPathExtension;
        if (version.length)
            name = [NSString stringWithFormat:@"%@ %@", name, version];
        if ([app rangeOfString:@"/MacNCheese/"].location != NSNotFound)
            name = [name stringByAppendingString:@" (MacNdCheese)"];
        add(name, wine);
    }
    add(@"Whisky's Wine", [home stringByAppendingPathComponent:@"Library/Application Support/com.isaacmarovitz.Whisky/Libraries/Wine/bin/wine64"]);
    add(@"Game Porting Toolkit", @"/usr/local/opt/game-porting-toolkit/bin/wine64");
    for (NSString *p in @[ @"/opt/homebrew/bin/wine", @"/usr/local/bin/wine", @"/usr/local/bin/wine64" ])
        add([NSString stringWithFormat:@"%@ (%@)", p.lastPathComponent, p.stringByDeletingLastPathComponent], p);
    return found;
}

/* An .msi, or anything that starts the way a DOS or Windows executable does. */
static BOOL is_windows_program(NSString *path)
{
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] || dir)
        return NO;
    if ([path.pathExtension.lowercaseString isEqualToString:@"msi"])
        return YES;
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    NSData *d = [fh readDataOfLength:2];
    [fh closeFile];
    return d.length == 2 && memcmp(d.bytes, "MZ", 2) == 0;
}

/* The WinePrefix default, else $WINEPREFIX, else one of the app's own so no
 * other Wine setup's prefix is touched. */
static NSString *wine_prefix(void)
{
    NSString *p = [[NSUserDefaults standardUserDefaults] stringForKey:@"WinePrefix"];
    if (!p.length)
        p = [NSProcessInfo processInfo].environment[@"WINEPREFIX"];
    if (!p.length)
        p = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/AArchX/Wine"];
    return p.stringByExpandingTildeInPath;
}

/* CrossOver.app, wherever Launch Services or the usual folders have it. */
static NSString *find_crossover(void)
{
    NSMutableArray *candidates = [NSMutableArray array];
    for (NSURL *u in [[NSWorkspace sharedWorkspace] URLsForApplicationsWithBundleIdentifier:@"com.codeweavers.CrossOver"])
        [candidates addObject:u.path];
    [candidates addObjectsFromArray:@[
        @"/Applications/CrossOver.app", [NSHomeDirectory() stringByAppendingPathComponent:@"Applications/CrossOver.app"]
    ]];
    for (NSString *c in candidates)
        if ([[NSFileManager defaultManager]
                isExecutableFileAtPath:[c stringByAppendingPathComponent:@"Contents/SharedSupport/CrossOver/bin/wineloader"]])
            return c;
    return nil;
}

static NSString *crossover_bottles(void)
{
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/CrossOver/Bottles"];
}

static NSString *crossover_shims(void)
{
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/AArchX/CrossOver/bin"];
}

static NSString *crossover_binpath(void)
{
    return [crossover_shims() stringByAppendingString:@":${CX_ROOT}/bin"];
}

static NSString *shell_quote(NSString *s)
{
    return [NSString stringWithFormat:@"'%@'", [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

/* wineloader and wineserver in the shim directory, each running CrossOver's
 * own binary of that name under ocerz, or as it is when ocerz has gone.  WINELOADER and WINESERVER go back to
 * CrossOver's real binaries, so the processes Wine starts itself are x86 ones,
 * which ocerz keeps under itself. */
static BOOL write_crossover_shims(NSString *ocerz)
{
    NSString *dir = crossover_shims();
    if (![[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil])
        return NO;
    for (NSString *name in @[ @"wineloader", @"wineserver" ]) {
        NSString *script = [NSString stringWithFormat:
            @"#!/bin/sh\n"
            @"# Written by AArchX.app: CrossOver's own %@, run by ocerz instead of Rosetta.\n"
            @"export WINELOADER=\"$CX_ROOT/bin/wineloader\"\n"
            @"export WINESERVER=\"$CX_ROOT/bin/wineserver\"\n"
            @"[ -x %@ ] || exec \"$CX_ROOT/bin/%@\" \"$@\"\n"
            @"exec %@ -- \"$CX_ROOT/bin/%@\" \"$@\"\n",
            name, shell_quote(ocerz), name, shell_quote(ocerz), name];
        NSString *path = [dir stringByAppendingPathComponent:name];
        if (![script writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil])
            return NO;
        [[NSFileManager defaultManager] setAttributes:@{ NSFilePosixPermissions : @0755 } ofItemAtPath:path error:nil];
    }
    return YES;
}

/* The [Wine] "BinPath" line of a cxbottle.conf, as an index into its lines, or
 * -1; *wine_at gets the index of the [Wine] header, or -1. */
static NSInteger binpath_line(NSArray<NSString *> *lines, NSInteger *wine_at)
{
    NSString *section = @"";
    NSInteger found = -1;
    *wine_at = -1;
    for (NSInteger i = 0; i < (NSInteger)lines.count; i++) {
        NSString *t = [lines[i] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([t hasPrefix:@"["]) {
            section = t;
            if ([t caseInsensitiveCompare:@"[Wine]"] == NSOrderedSame && *wine_at < 0)
                *wine_at = i;
            continue;
        }
        if ([section caseInsensitiveCompare:@"[Wine]"] == NSOrderedSame && [t hasPrefix:@"\"BinPath\""])
            found = i;
    }
    return found;
}

static BOOL bottle_uses_aarchx(NSString *conf)
{
    NSString *text = [NSString stringWithContentsOfFile:conf encoding:NSUTF8StringEncoding error:nil];
    NSArray *lines = [text componentsSeparatedByString:@"\n"];
    NSInteger wine_at;
    NSInteger i = text ? binpath_line(lines, &wine_at) : -1;
    return i >= 0 && [lines[i] rangeOfString:crossover_shims()].location != NSNotFound;
}

/* Sets or removes the bottle's BinPath.  A BinPath someone else set is left
 * alone and reported as a failure. */
static BOOL set_bottle_aarchx(NSString *conf, BOOL on)
{
    NSString *text = [NSString stringWithContentsOfFile:conf encoding:NSUTF8StringEncoding error:nil];
    if (!text)
        return NO;
    NSMutableArray<NSString *> *lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];
    NSInteger wine_at;
    NSInteger i = binpath_line(lines, &wine_at);
    BOOL ours = i >= 0 && [lines[i] rangeOfString:crossover_shims()].location != NSNotFound;
    if (i >= 0 && !ours)
        return NO;
    NSString *line = [NSString stringWithFormat:@"\"BinPath\" = \"%@\"", crossover_binpath()];
    if (on && ours)
        lines[i] = line;
    else if (on && wine_at >= 0)
        [lines insertObject:line atIndex:wine_at + 1];
    else if (on)
        [lines addObjectsFromArray:@[ @"[Wine]", line, @"" ]];
    else if (ours)
        [lines removeObjectAtIndex:i];
    else
        return YES;
    return [[lines componentsJoinedByString:@"\n"] writeToFile:conf atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

/* A copy downloaded with a browser carries the quarantine flag on every file;
 * once the user has let the app open, ocerz inside it must not be stopped
 * again on its own. */
static void clear_own_quarantine(void)
{
    NSString *res = [[NSBundle mainBundle].bundlePath stringByAppendingPathComponent:@"Contents/Resources/aarchx"];
    NSDirectoryEnumerator *e = [[NSFileManager defaultManager] enumeratorAtPath:res];
    removexattr(res.fileSystemRepresentation, "com.apple.quarantine", 0);
    for (NSString *rel in e) {
        NSString *p = [res stringByAppendingPathComponent:rel];
        removexattr(p.fileSystemRepresentation, "com.apple.quarantine", XATTR_NOFOLLOW);
    }
}

/* What `ocerz version` prints, e.g. "AArchX 0.7". */
static NSString *ocerz_version(NSString *ocerz)
{
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:ocerz];
    t.arguments = @[ @"version" ];
    NSPipe *pipe = [NSPipe pipe];
    t.standardOutput = pipe;
    t.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![t launchAndReturnError:nil])
        return @"ocerz";
    NSData *d = [pipe.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    NSString *v = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
    v = [v stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    return v.length ? v : @"ocerz";
}


enum { KIND_INTEL, KIND_I386, KIND_WINDOWS };

/* One run of one program: what the Activity list shows. */
@interface Run : NSObject
@property(copy) NSString *name;
@property(copy) NSString *path;
@property(copy) NSString *status;
@property(copy) NSString *log;
@property(copy) NSString *mode;
@property(copy) NSString *wine;
@property(copy) NSString *winePrefix;
@property(strong) NSDate *started;
@property(strong) NSTask *task;
@end
@implementation Run
@end

/* A program in the library, and its latest run. */
@interface Item : NSObject
@property(copy) NSString *path;
@property(copy) NSString *name;
@property int kind;
@property(strong) NSImage *icon;
@property(strong) Run *run;
@end
@implementation Item
@end

/* What a path is to AArchX, or -1 with *why set when it cannot run it. */
static int program_kind(NSString *path, NSString **why)
{
    if (is_windows_program(path))
        return KIND_WINDOWS;
    NSString *exe = executable_of(path);
    int arches = exe ? macho_arches(exe) : 0;
    if (!arches) {
        *why = @"AArchX runs Intel macOS apps and command-line programs, and Windows programs through Wine.";
        return -1;
    }
    if (!(arches & (ARCH_X86_64 | ARCH_I386))) {
        *why = @"It is already an Apple silicon program and runs on this Mac by itself.";
        return -1;
    }
    return arches & ARCH_X86_64 ? KIND_INTEL : KIND_I386;
}

@class AppDelegate;

/* A scroll view's content starts at the top only in a flipped view. */
@interface FlippedView : NSView
@end
@implementation FlippedView
- (BOOL)isFlipped
{
    return YES;
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate,
                                   NSCollectionViewDataSource, NSCollectionViewDelegate, NSToolbarDelegate>
@property(strong) NSWindow *window;
@property(strong) NSTabView *tabs;
@property(strong) NSSegmentedControl *viewSwitch;
@property(strong) NSCollectionView *grid;
@property(strong) NSView *emptyState;
@property(strong) NSTableView *table;
@property(strong) NSButton *nativeBox;
@property(strong) NSPopUpButton *winePopup;
@property(strong) NSButton *crossoverButton;
@property(strong) NSArray<WineBuild *> *wines;
@property(strong) NSTextField *footer;
@property(strong) NSMutableArray<Run *> *runs;
@property(strong) NSMutableArray<Item *> *items;
- (void)addPaths:(NSArray<NSString *> *)paths run:(BOOL)run;
- (void)runItem:(Item *)item native:(BOOL)native;
- (NSMenu *)menuForItem:(Item *)item;
@end

/* The library's area: takes dropped files, and outlines itself while empty or
 * while something is dragged over it. */
@interface DropArea : NSView
@property(weak) AppDelegate *owner;
@property BOOL hot;
@property BOOL empty;
@end

@implementation DropArea

- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame]))
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return self;
}

- (void)drawRect:(NSRect)dirty
{
    if (!self.hot && !self.empty)
        return;
    NSBezierPath *b = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 10, 10) xRadius:12 yRadius:12];
    CGFloat dash[] = { 6, 5 };
    [b setLineDash:dash count:2 phase:0];
    b.lineWidth = self.hot ? 2 : 1.5;
    [(self.hot ? [NSColor controlAccentColor] : [NSColor tertiaryLabelColor]) setStroke];
    [b stroke];
}

- (NSArray<NSString *> *)pathsFrom:(id<NSDraggingInfo>)info
{
    NSArray *urls = [info.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                           options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    NSMutableArray *paths = [NSMutableArray array];
    for (NSURL *u in urls)
        [paths addObject:u.path];
    return paths;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info
{
    if (![self pathsFrom:info].count)
        return NSDragOperationNone;
    self.hot = YES;
    [self setNeedsDisplay:YES];
    return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)info
{
    self.hot = NO;
    [self setNeedsDisplay:YES];
}

/* One program dropped is added and run, as a droplet would; several are only
 * added, so a whole folder of apps does not start at once. */
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    self.hot = NO;
    [self setNeedsDisplay:YES];
    NSArray *paths = [self pathsFrom:info];
    [self.owner addPaths:paths run:paths.count == 1];
    return paths.count > 0;
}

@end

/* A library tile: the program's icon, its name, and one line saying what it
 * is or that it is running. */
@interface TileView : NSView
@property(weak) AppDelegate *owner;
@property(weak) Item *item;
@property BOOL selected;
@end

@implementation TileView

- (BOOL)wantsUpdateLayer
{
    return YES;
}

- (void)updateLayer
{
    self.layer.cornerRadius = 10;
    self.layer.backgroundColor =
        self.selected ? [[NSColor controlAccentColor] colorWithAlphaComponent:0.22].CGColor : NSColor.clearColor.CGColor;
}

- (void)mouseDown:(NSEvent *)e
{
    [super mouseDown:e];
    if (e.clickCount == 2 && self.item)
        [self.owner runItem:self.item native:NO];
}

- (NSMenu *)menuForEvent:(NSEvent *)e
{
    return self.item ? [self.owner menuForItem:self.item] : nil;
}

@end

@interface Tile : NSCollectionViewItem
@property(strong) NSTextField *caption;
@property(weak) AppDelegate *owner;
- (void)show:(Item *)item;
@end

@implementation Tile

- (void)loadView
{
    TileView *v = [[TileView alloc] initWithFrame:NSMakeRect(0, 0, 132, 136)];
    v.wantsLayer = YES;
    NSImageView *icon = [NSImageView new];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    NSTextField *name = [NSTextField labelWithString:@""];
    name.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    name.alignment = NSTextAlignmentCenter;
    name.maximumNumberOfLines = 2;
    name.lineBreakMode = NSLineBreakByTruncatingMiddle;
    name.cell.wraps = YES;
    self.caption = [NSTextField labelWithString:@""];
    self.caption.font = [NSFont systemFontOfSize:11];
    self.caption.alignment = NSTextAlignmentCenter;
    self.caption.textColor = [NSColor secondaryLabelColor];
    for (NSView *sub in @[ icon, name, self.caption ]) {
        sub.translatesAutoresizingMaskIntoConstraints = NO;
        [v addSubview:sub];
    }
    [NSLayoutConstraint activateConstraints:@[
        [icon.topAnchor constraintEqualToAnchor:v.topAnchor constant:10],
        [icon.centerXAnchor constraintEqualToAnchor:v.centerXAnchor],
        [icon.widthAnchor constraintEqualToConstant:64],
        [icon.heightAnchor constraintEqualToConstant:64],
        [name.topAnchor constraintEqualToAnchor:icon.bottomAnchor constant:6],
        [name.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:6],
        [name.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-6],
        [self.caption.topAnchor constraintEqualToAnchor:name.bottomAnchor constant:2],
        [self.caption.leadingAnchor constraintEqualToAnchor:v.leadingAnchor constant:6],
        [self.caption.trailingAnchor constraintEqualToAnchor:v.trailingAnchor constant:-6],
    ]];
    self.view = v;
    self.imageView = icon;
    self.textField = name;
}

- (void)setSelected:(BOOL)selected
{
    [super setSelected:selected];
    TileView *v = (TileView *)self.view;
    v.selected = selected;
    [v setNeedsDisplay:YES];
}

- (void)show:(Item *)item
{
    TileView *v = (TileView *)self.view;
    v.item = item;
    v.owner = self.owner;
    self.imageView.image = item.icon;
    self.textField.stringValue = item.name;
    self.view.toolTip = item.path;
    if (item.run.task.running) {
        self.caption.stringValue = @"Running";
        self.caption.textColor = [NSColor systemGreenColor];
    } else {
        self.caption.stringValue = item.kind == KIND_WINDOWS ? @"Windows" : item.kind == KIND_I386 ? @"32-bit" : @"Intel";
        self.caption.textColor = [NSColor secondaryLabelColor];
    }
}

@end

@implementation AppDelegate

- (void)applicationWillFinishLaunching:(NSNotification *)n
{
    self.runs = [NSMutableArray array];
    self.items = [NSMutableArray array];
    clear_own_quarantine();
    [self buildMenu];
    [self buildWindow];
    [self loadLibrary];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)applicationDidBecomeActive:(NSNotification *)n
{
    if (self.winePopup)
        [self fillWines];
    if (self.crossoverButton)
        self.crossoverButton.hidden = find_crossover() == nil;
}

/* Open With and the Dock icon: each program is added and run. */
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls
{
    NSMutableArray *paths = [NSMutableArray array];
    for (NSURL *u in urls)
        if (u.isFileURL)
            [paths addObject:u.path];
    for (NSString *p in paths)
        [self addPaths:@[ p ] run:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    return YES;
}

- (void)buildMenu
{
    NSMenu *bar = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"About AArchX" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide AArchX" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItemWithTitle:@"Quit AArchX" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSMenuItem *fileItem = [NSMenuItem new];
    [bar addItem:fileItem];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenu addItemWithTitle:@"Add to Library…" action:@selector(choose:) keyEquivalent:@"o"];
    [fileMenu addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
    fileItem.submenu = fileMenu;
    NSMenuItem *viewItem = [NSMenuItem new];
    [bar addItem:viewItem];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *lib = [viewMenu addItemWithTitle:@"Library" action:@selector(showLibrary:) keyEquivalent:@"1"];
    NSMenuItem *act = [viewMenu addItemWithTitle:@"Activity" action:@selector(showActivity:) keyEquivalent:@"2"];
    lib.target = act.target = self;
    viewItem.submenu = viewMenu;
    NSApp.mainMenu = bar;
}

- (void)buildWindow
{
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 720, 500)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"AArchX";
    self.window.minSize = NSMakeSize(520, 380);
    self.window.releasedWhenClosed = NO;
    self.window.frameAutosaveName = @"Main";
    if (!self.window.frameAutosaveName.length || ![self.window setFrameUsingName:@"Main"])
        [self.window center];
    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"main"];
    toolbar.delegate = self;
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    if (@available(macOS 13.0, *))
        toolbar.centeredItemIdentifiers = [NSSet setWithObject:@"view"];
    self.window.toolbar = toolbar;
    if (@available(macOS 11.0, *))
        self.window.toolbarStyle = NSWindowToolbarStyleUnified;
    NSView *root = self.window.contentView;

    self.tabs = [NSTabView new];
    self.tabs.tabViewType = NSNoTabsNoBorder;
    self.tabs.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:self.tabs];

    NSTabViewItem *libTab = [[NSTabViewItem alloc] initWithIdentifier:@"library"];
    libTab.view = [self buildLibrary];
    [self.tabs addTabViewItem:libTab];
    NSTabViewItem *actTab = [[NSTabViewItem alloc] initWithIdentifier:@"activity"];
    actTab.view = [self buildActivity];
    [self.tabs addTabViewItem:actTab];

    NSBox *line = [NSBox new];
    line.boxType = NSBoxSeparator;
    line.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:line];
    NSView *bar = [self buildBottomBar];
    [root addSubview:bar];

    NSDictionary *v = @{ @"tabs" : self.tabs, @"line" : line, @"bar" : bar };
    for (NSString *f in @[ @"H:|[tabs]|", @"H:|[line]|", @"H:|-14-[bar]-14-|", @"V:|[tabs][line(1)]-8-[bar]-10-|" ])
        [root addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:f options:0 metrics:nil views:v]];
    [self updateFooter];
}

- (NSView *)buildLibrary
{
    DropArea *area = [DropArea new];
    area.owner = self;

    NSCollectionViewFlowLayout *layout = [NSCollectionViewFlowLayout new];
    layout.itemSize = NSMakeSize(132, 136);
    layout.minimumInteritemSpacing = 6;
    layout.minimumLineSpacing = 10;
    layout.sectionInset = NSEdgeInsetsMake(18, 18, 18, 18);
    self.grid = [NSCollectionView new];
    self.grid.collectionViewLayout = layout;
    self.grid.dataSource = self;
    self.grid.delegate = self;
    self.grid.selectable = YES;
    self.grid.backgroundColors = @[ NSColor.clearColor ];
    [self.grid registerClass:[Tile class] forItemWithIdentifier:@"tile"];
    NSScrollView *scroll = [NSScrollView new];
    scroll.documentView = self.grid;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [area addSubview:scroll];

    NSTextField *title = [NSTextField labelWithString:@"Drop an Intel app or a Windows program here"];
    title.font = [NSFont systemFontOfSize:15 weight:NSFontWeightMedium];
    title.textColor = [NSColor secondaryLabelColor];
    NSTextField *sub = [NSTextField labelWithString:@"It joins your library and runs under AArchX. Double-click it to run it again."];
    sub.font = [NSFont systemFontOfSize:12];
    sub.textColor = [NSColor tertiaryLabelColor];
    NSButton *choose = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(choose:)];
    NSStackView *empty = [NSStackView stackViewWithViews:@[ title, sub, choose ]];
    empty.orientation = NSUserInterfaceLayoutOrientationVertical;
    empty.spacing = 8;
    [empty setCustomSpacing:16 afterView:sub];
    empty.translatesAutoresizingMaskIntoConstraints = NO;
    [area addSubview:empty];
    self.emptyState = empty;

    NSDictionary *v = @{ @"scroll" : scroll };
    for (NSString *f in @[ @"H:|[scroll]|", @"V:|[scroll]|" ])
        [area addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:f options:0 metrics:nil views:v]];
    [NSLayoutConstraint activateConstraints:@[
        [empty.centerXAnchor constraintEqualToAnchor:area.centerXAnchor],
        [empty.centerYAnchor constraintEqualToAnchor:area.centerYAnchor],
    ]];
    return area;
}

- (NSView *)buildActivity
{
    self.table = [NSTableView new];
    if (@available(macOS 11.0, *))
        self.table.style = NSTableViewStylePlain;
    NSArray *cols = @[ @[ @"name", @"Program", @220 ], @[ @"status", @"Status", @260 ], @[ @"started", @"Started", @90 ] ];
    for (NSArray *c in cols) {
        NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:c[0]];
        col.title = c[1];
        col.width = [c[2] doubleValue];
        [self.table addTableColumn:col];
    }
    self.table.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    self.table.rowHeight = 24;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.target = self;
    self.table.doubleAction = @selector(openLog:);
    self.table.usesAlternatingRowBackgroundColors = YES;
    NSMenu *menu = [NSMenu new];
    [menu addItemWithTitle:@"Show Log" action:@selector(openLog:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Show in Finder" action:@selector(reveal:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Run Again" action:@selector(runAgain:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Stop" action:@selector(stop:) keyEquivalent:@""];
    for (NSMenuItem *item in menu.itemArray)
        item.target = self;
    self.table.menu = menu;
    NSScrollView *scroll = [NSScrollView new];
    scroll.documentView = self.table;
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSNoBorder;
    return scroll;
}

- (NSView *)buildBottomBar
{
    self.nativeBox = [NSButton checkboxWithTitle:@"Native mode" target:self action:@selector(nativeChanged:)];
    self.nativeBox.toolTip = @"Run Intel Mac programs against the Mac's own frameworks instead of Apple's Intel ones.";
    self.nativeBox.state = [[NSUserDefaults standardUserDefaults] boolForKey:@"Native"] ? NSControlStateValueOn
                                                                                       : NSControlStateValueOff;
    NSTextField *wineLabel = [NSTextField labelWithString:@"Windows programs:"];
    wineLabel.textColor = [NSColor secondaryLabelColor];
    self.winePopup = [NSPopUpButton new];
    self.winePopup.target = self;
    self.winePopup.action = @selector(wineChanged:);
    self.winePopup.controlSize = NSControlSizeSmall;
    self.winePopup.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    [self fillWines];
    self.crossoverButton = [NSButton buttonWithTitle:@"CrossOver Bottles…" target:self action:@selector(crossover:)];
    self.crossoverButton.controlSize = NSControlSizeSmall;
    self.crossoverButton.font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    self.crossoverButton.toolTip = @"Choose which CrossOver bottles run under AArchX instead of Rosetta.";
    self.crossoverButton.hidden = find_crossover() == nil;
    NSView *gap = [NSView new];
    [gap setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = [NSStackView stackViewWithViews:@[ self.nativeBox, self.crossoverButton, gap, wineLabel,
                                                          self.winePopup ]];
    row.spacing = 8;
    self.footer = [NSTextField labelWithString:@""];
    self.footer.textColor = [NSColor tertiaryLabelColor];
    self.footer.font = [NSFont systemFontOfSize:11];
    self.footer.selectable = YES;
    self.footer.lineBreakMode = NSLineBreakByTruncatingMiddle;
    for (NSView *c in @[ self.nativeBox, wineLabel ])
        ((NSControl *)c).font = [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
    self.nativeBox.controlSize = NSControlSizeSmall;
    NSStackView *bar = [NSStackView stackViewWithViews:@[ row, self.footer ]];
    bar.orientation = NSUserInterfaceLayoutOrientationVertical;
    bar.alignment = NSLayoutAttributeLeading;
    bar.spacing = 4;
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    [row.widthAnchor constraintEqualToAnchor:bar.widthAnchor].active = YES;
    return bar;
}

/* ---- toolbar ---- */

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
    return @[ @"view", NSToolbarFlexibleSpaceItemIdentifier, @"add" ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
    return [self toolbarDefaultItemIdentifiers:toolbar];
}

- (NSToolbarItem *)toolbar:(NSToolbar *)toolbar
        itemForItemIdentifier:(NSToolbarItemIdentifier)ident
    willBeInsertedIntoToolbar:(BOOL)flag
{
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    if ([ident isEqualToString:@"view"]) {
        self.viewSwitch = [NSSegmentedControl segmentedControlWithLabels:@[ @"Library", @"Activity" ]
                                                            trackingMode:NSSegmentSwitchTrackingSelectOne
                                                                  target:self
                                                                  action:@selector(switchView:)];
        self.viewSwitch.selectedSegment = 0;
        item.view = self.viewSwitch;
        item.label = @"View";
    } else {
        NSButton *add = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"plus"
                                                            accessibilityDescription:@"Add to Library"]
                                           target:self
                                           action:@selector(choose:)];
        add.bezelStyle = NSBezelStyleTexturedRounded;
        add.toolTip = @"Add to Library";
        item.view = add;
        item.label = @"Add";
    }
    return item;
}

- (void)switchView:(id)sender
{
    [self.tabs selectTabViewItemAtIndex:self.viewSwitch.selectedSegment];
}

- (void)showLibrary:(id)sender
{
    self.viewSwitch.selectedSegment = 0;
    [self switchView:nil];
}

- (void)showActivity:(id)sender
{
    self.viewSwitch.selectedSegment = 1;
    [self switchView:nil];
}

/* ---- library ---- */

- (void)loadLibrary
{
    for (NSString *p in [[NSUserDefaults standardUserDefaults] stringArrayForKey:@"Library"])
        [self itemForPath:p create:YES];
    [self reloadLibrary];
}

- (void)saveLibrary
{
    NSMutableArray *paths = [NSMutableArray array];
    for (Item *i in self.items)
        [paths addObject:i.path];
    [[NSUserDefaults standardUserDefaults] setObject:paths forKey:@"Library"];
}

- (void)reloadLibrary
{
    [self.items sortUsingComparator:^NSComparisonResult(Item *a, Item *b) {
        return [a.name localizedStandardCompare:b.name];
    }];
    [self.grid reloadData];
    self.emptyState.hidden = self.items.count > 0;
    DropArea *area = (DropArea *)self.grid.enclosingScrollView.superview;
    area.empty = self.items.count == 0;
    [area setNeedsDisplay:YES];
}

/* The library's entry for a path; with create, one is made for a program that
 * exists and AArchX can run.  Programs that went missing are dropped. */
- (Item *)itemForPath:(NSString *)path create:(BOOL)create
{
    for (Item *i in self.items)
        if ([i.path isEqualToString:path])
            return i;
    if (!create || ![[NSFileManager defaultManager] fileExistsAtPath:path])
        return nil;
    NSString *why = nil;
    int kind = program_kind(path, &why);
    if (kind < 0)
        return nil;
    Item *i = [Item new];
    i.path = path;
    i.name = [[NSFileManager defaultManager] displayNameAtPath:path];
    if ([i.name.pathExtension.lowercaseString isEqualToString:@"exe"])
        i.name = i.name.stringByDeletingPathExtension;
    i.kind = kind;
    i.icon = [[NSWorkspace sharedWorkspace] iconForFile:path];
    [self.items addObject:i];
    return i;
}

- (void)addPaths:(NSArray<NSString *> *)paths run:(BOOL)run
{
    for (NSString *p in paths) {
        NSString *why = nil;
        if (program_kind(p, &why) < 0) {
            [self tell:[NSString stringWithFormat:@"%@ cannot run under AArchX",
                                                  [[NSFileManager defaultManager] displayNameAtPath:p]]
                detail:why];
            continue;
        }
        Item *i = [self itemForPath:p create:YES];
        if (run && i)
            [self runItem:i native:NO];
    }
    [self saveLibrary];
    [self reloadLibrary];
}

- (NSMenu *)menuForItem:(Item *)item
{
    NSMenu *m = [NSMenu new];
    NSMenuItem *(^add)(NSString *, SEL) = ^(NSString *title, SEL sel) {
        NSMenuItem *mi = [m addItemWithTitle:title action:sel keyEquivalent:@""];
        mi.target = self;
        mi.representedObject = item;
        return mi;
    };
    add(@"Run", @selector(itemRun:));
    if (item.kind == KIND_INTEL)
        add(@"Run in Native Mode", @selector(itemRunNative:));
    if (item.run.task.running)
        add(@"Stop", @selector(itemStop:));
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem *log = add(@"Show Last Log", @selector(itemLog:));
    log.enabled = item.run != nil;
    add(@"Show in Finder", @selector(itemReveal:));
    [m addItem:[NSMenuItem separatorItem]];
    add(@"Remove from Library", @selector(itemRemove:));
    m.autoenablesItems = NO;
    return m;
}

- (void)itemRun:(NSMenuItem *)mi
{
    [self runItem:mi.representedObject native:NO];
}

- (void)itemRunNative:(NSMenuItem *)mi
{
    [self runItem:mi.representedObject native:YES];
}

- (void)itemStop:(NSMenuItem *)mi
{
    [self stopRun:((Item *)mi.representedObject).run];
}

- (void)itemLog:(NSMenuItem *)mi
{
    [self openLogOf:((Item *)mi.representedObject).run];
}

- (void)itemReveal:(NSMenuItem *)mi
{
    [[NSWorkspace sharedWorkspace]
        activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:((Item *)mi.representedObject).path] ]];
}

- (void)itemRemove:(NSMenuItem *)mi
{
    [self.items removeObject:mi.representedObject];
    [self saveLibrary];
    [self reloadLibrary];
}

- (NSInteger)collectionView:(NSCollectionView *)cv numberOfItemsInSection:(NSInteger)section
{
    return (NSInteger)self.items.count;
}

- (NSCollectionViewItem *)collectionView:(NSCollectionView *)cv itemForRepresentedObjectAtIndexPath:(NSIndexPath *)ip
{
    Tile *t = [cv makeItemWithIdentifier:@"tile" forIndexPath:ip];
    t.owner = self;
    [t show:self.items[(NSUInteger)ip.item]];
    return t;
}

/* ---- settings in the bottom bar ---- */

- (void)updateFooter
{
    NSString *ocerz = find_ocerz();
    if (!ocerz) {
        self.footer.stringValue = @"ocerz was not found. Install it with: curl -fsSL https://mont127.github.io/AArchX/install.sh | sh";
        self.footer.textColor = [NSColor systemOrangeColor];
        return;
    }
    NSString *s = [NSString stringWithFormat:@"%@ · %@", ocerz_version(ocerz), ocerz.stringByAbbreviatingWithTildeInPath];
    if (!rosetta_cache_present())
        s = [s stringByAppendingString:@" · Rosetta is not installed, so Intel Mac programs run in native mode"];
    self.footer.stringValue = s;
}

- (void)fillWines
{
    self.wines = find_wines();
    [self.winePopup removeAllItems];
    if (!self.wines.count) {
        [self.winePopup addItemWithTitle:@"no Wine found"];
        self.winePopup.enabled = NO;
        return;
    }
    self.winePopup.enabled = YES;
    NSString *chosen = [[NSUserDefaults standardUserDefaults] stringForKey:@"Wine"];
    for (WineBuild *b in self.wines) {
        [self.winePopup addItemWithTitle:b.name];
        self.winePopup.lastItem.toolTip = b.wine;
        if ([b.wine isEqualToString:chosen])
            [self.winePopup selectItem:self.winePopup.lastItem];
    }
}

- (void)wineChanged:(id)sender
{
    NSInteger i = self.winePopup.indexOfSelectedItem;
    if (i >= 0 && i < (NSInteger)self.wines.count)
        [[NSUserDefaults standardUserDefaults] setObject:self.wines[i].wine forKey:@"Wine"];
}

- (WineBuild *)chosenWine
{
    NSInteger i = self.winePopup.indexOfSelectedItem;
    return i >= 0 && i < (NSInteger)self.wines.count ? self.wines[i] : nil;
}

/* A sheet with a box per CrossOver bottle: ticked means the bottle runs under
 * AArchX the next time it starts. */
- (void)crossover:(id)sender
{
    NSString *ocerz = find_ocerz();
    if (!ocerz) {
        [self tell:@"ocerz was not found" detail:@"Install AArchX first, then switch bottles to it."];
        return;
    }
    NSString *dir = crossover_bottles();
    NSMutableArray *bottles = [NSMutableArray array];
    for (NSString *b in [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]
             sortedArrayUsingSelector:@selector(localizedStandardCompare:)])
        if ([[NSFileManager defaultManager] fileExistsAtPath:[[dir stringByAppendingPathComponent:b]
                                                                 stringByAppendingPathComponent:@"cxbottle.conf"]])
            [bottles addObject:b];
    if (!bottles.count) {
        [self tell:@"CrossOver has no bottles yet" detail:@"Create a bottle in CrossOver, then come back here."];
        return;
    }
    write_crossover_shims(ocerz);

    NSWindow *sheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 420, 300)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
    NSTextField *title = [NSTextField labelWithString:@"CrossOver Bottles"];
    title.font = [NSFont boldSystemFontOfSize:13];
    NSTextField *intro = [NSTextField wrappingLabelWithString:
        @"Ticked bottles run their Windows programs under AArchX instead of Rosetta. A change takes effect the "
        @"next time the bottle starts, so quit its programs first. CrossOver's own files are not changed."];
    NSStackView *boxes = [NSStackView new];
    boxes.orientation = NSUserInterfaceLayoutOrientationVertical;
    boxes.alignment = NSLayoutAttributeLeading;
    for (NSString *b in bottles) {
        NSButton *box = [NSButton checkboxWithTitle:b target:self action:@selector(bottleToggled:)];
        box.identifier = [[dir stringByAppendingPathComponent:b] stringByAppendingPathComponent:@"cxbottle.conf"];
        box.state = bottle_uses_aarchx(box.identifier) ? NSControlStateValueOn : NSControlStateValueOff;
        [boxes addArrangedSubview:box];
    }
    NSScrollView *scroll = [NSScrollView new];
    FlippedView *list = [FlippedView new];
    list.translatesAutoresizingMaskIntoConstraints = NO;
    boxes.translatesAutoresizingMaskIntoConstraints = NO;
    [list addSubview:boxes];
    scroll.documentView = list;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    NSButton *done = [NSButton buttonWithTitle:@"Done" target:self action:@selector(endSheet:)];
    done.keyEquivalent = @"\r";
    NSView *root = sheet.contentView;
    for (NSView *v in @[ title, intro, scroll, done ]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [root addSubview:v];
    }
    NSDictionary *views = @{ @"title" : title, @"intro" : intro, @"scroll" : scroll, @"done" : done };
    for (NSString *f in @[ @"H:|-20-[title]-20-|", @"H:|-20-[intro]-20-|", @"H:|-20-[scroll]-20-|", @"H:[done]-20-|",
                           @"V:|-20-[title]-8-[intro]-14-[scroll(>=120)]-14-[done]-20-|" ])
        [root addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:f options:0 metrics:nil views:views]];
    [NSLayoutConstraint activateConstraints:@[
        [list.topAnchor constraintEqualToAnchor:scroll.contentView.topAnchor],
        [list.leadingAnchor constraintEqualToAnchor:scroll.contentView.leadingAnchor],
        [list.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
        [boxes.topAnchor constraintEqualToAnchor:list.topAnchor],
        [boxes.leadingAnchor constraintEqualToAnchor:list.leadingAnchor],
        [boxes.bottomAnchor constraintEqualToAnchor:list.bottomAnchor],
    ]];
    [self.window beginSheet:sheet completionHandler:nil];
}

- (void)bottleToggled:(NSButton *)box
{
    BOOL on = box.state == NSControlStateValueOn;
    if (!set_bottle_aarchx(box.identifier, on)) {
        box.state = on ? NSControlStateValueOff : NSControlStateValueOn;
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithFormat:@"%@ could not be changed", box.title];
        a.informativeText = @"Its cxbottle.conf already sets [Wine] BinPath to something else, or could not be written.";
        [a beginSheetModalForWindow:box.window completionHandler:nil];
    }
}

- (void)endSheet:(NSButton *)sender
{
    [self.window endSheet:sender.window];
}

- (void)nativeChanged:(id)sender
{
    [[NSUserDefaults standardUserDefaults] setBool:self.nativeBox.state == NSControlStateValueOn forKey:@"Native"];
}

- (void)choose:(id)sender
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseFiles = YES;
    p.canChooseDirectories = NO;
    p.allowsMultipleSelection = YES;
    p.treatsFilePackagesAsDirectories = NO;
    p.directoryURL = [NSURL fileURLWithPath:@"/Applications"];
    p.message = @"Add Intel apps and programs, or Windows programs, to the library";
    p.prompt = @"Add";
    [p beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse r) {
                  if (r != NSModalResponseOK)
                      return;
                  NSMutableArray *paths = [NSMutableArray array];
                  for (NSURL *u in p.URLs)
                      [paths addObject:u.path];
                  [self addPaths:paths run:NO];
              }];
}

- (void)tell:(NSString *)title detail:(NSString *)detail
{
    NSAlert *a = [NSAlert new];
    a.messageText = title;
    a.informativeText = detail;
    [a beginSheetModalForWindow:self.window completionHandler:nil];
}

/* ---- running ---- */

- (void)runItem:(Item *)item native:(BOOL)forceNative
{
    Run *r = [self runPath:item.path native:forceNative];
    if (r)
        item.run = r;
    [self.grid reloadData];
}

- (Run *)runPath:(NSString *)path native:(BOOL)forceNative
{
    NSString *name = [[NSFileManager defaultManager] displayNameAtPath:path];
    NSString *ocerz = find_ocerz();
    if (!ocerz) {
        [self tell:@"ocerz was not found"
            detail:@"Install AArchX in Terminal with\ncurl -fsSL https://mont127.github.io/AArchX/install.sh | sh\n"
                   @"and run the program again."];
        return nil;
    }
    NSString *why = nil;
    int kind = program_kind(path, &why);
    if (kind < 0) {
        [self tell:[NSString stringWithFormat:@"%@ cannot run under AArchX", name] detail:why];
        return nil;
    }
    if (kind == KIND_WINDOWS)
        return [self runWindows:path name:name ocerz:ocerz];

    BOOL native = forceNative || self.nativeBox.state == NSControlStateValueOn;
    NSString *reason = nil;
    if (kind == KIND_I386) {
        native = YES;
        reason = @"32-bit";
    } else if (!native && !rosetta_cache_present()) {
        native = YES;
        reason = @"no Rosetta";
    }
    NSMutableArray *args = [NSMutableArray array];
    if (native)
        [args addObject:@"-native"];
    [args addObjectsFromArray:@[ @"--", path ]];
    NSString *mode = reason ? [NSString stringWithFormat:@"native, %@", reason] : native ? @"native" : nil;
    return [self launch:path name:name ocerz:ocerz args:args env:nil cwd:NSHomeDirectory() mode:mode wine:nil];
}

/* Wine runs in cache mode, in the program's own folder, with the chosen build. */
- (Run *)runWindows:(NSString *)path name:(NSString *)name ocerz:(NSString *)ocerz
{
    WineBuild *w = [self chosenWine];
    if (!w) {
        [self tell:@"No Wine was found"
            detail:@"Windows programs run through Wine. Install Wine Stable or Wine Staging from winehq.org, or "
                   @"MacNdCheese, which also runs Windows games under AArchX, and run the program again."];
        return nil;
    }
    if (!rosetta_cache_present()) {
        [self tell:@"Windows programs need Rosetta installed"
            detail:@"Wine runs in cache mode, which reads Apple's x86-64 shared cache from Rosetta's package. "
                   @"Install it in Terminal with\nsoftwareupdate --install-rosetta --agree-to-license"];
        return nil;
    }
    NSString *prefix = wine_prefix();
    [[NSFileManager defaultManager] createDirectoryAtPath:prefix.stringByDeletingLastPathComponent
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSArray *args = [path.pathExtension.lowercaseString isEqualToString:@"msi"]
                        ? @[ @"--", w.wine, @"msiexec", @"/i", path ]
                        : @[ @"--", w.wine, path ];
    return [self launch:path
                   name:name
                  ocerz:ocerz
                   args:args
                    env:@{ @"WINEPREFIX" : prefix }
                    cwd:path.stringByDeletingLastPathComponent
                   mode:w.name
                   wine:w.wine];
}

- (Run *)launch:(NSString *)path
           name:(NSString *)name
          ocerz:(NSString *)ocerz
           args:(NSArray *)args
            env:(NSDictionary *)env
            cwd:(NSString *)cwd
           mode:(NSString *)mode
           wine:(NSString *)wine
{
    NSString *logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/AArchX"];
    [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *df = [NSDateFormatter new];
    df.dateFormat = @"yyyy-MM-dd HH.mm.ss";
    NSString *safe = [name stringByReplacingOccurrencesOfString:@"/" withString:@"-"];
    NSString *log = [logs stringByAppendingPathComponent:
                              [NSString stringWithFormat:@"%@ %@.log", safe, [df stringFromDate:[NSDate date]]]];
    [[NSFileManager defaultManager] createFileAtPath:log contents:nil attributes:nil];
    NSFileHandle *out = [NSFileHandle fileHandleForWritingAtPath:log];

    NSMutableString *head = [NSMutableString string];
    for (NSString *k in env)
        [head appendFormat:@"%@=%@ ", k, env[k]];
    [head appendFormat:@"%@ %@\n\n", ocerz, [args componentsJoinedByString:@" "]];
    [out writeData:[head dataUsingEncoding:NSUTF8StringEncoding]];

    Run *run = [Run new];
    run.name = name;
    run.path = path;
    run.log = log;
    run.mode = mode;
    run.wine = wine;
    run.winePrefix = env[@"WINEPREFIX"];
    run.started = [NSDate date];
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:ocerz];
    t.arguments = args;
    if (env.count) {
        NSMutableDictionary *all = [[NSProcessInfo processInfo].environment mutableCopy];
        [all addEntriesFromDictionary:env];
        t.environment = all;
    }
    t.currentDirectoryURL = [NSURL fileURLWithPath:cwd];
    t.standardOutput = out;
    t.standardError = out;
    t.standardInput = [NSFileHandle fileHandleWithNullDevice];
    __weak AppDelegate *weakSelf = self;
    t.terminationHandler = ^(NSTask *done) {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString *how;
            if (done.terminationReason == NSTaskTerminationReasonUncaughtSignal)
                how = [NSString stringWithFormat:@"Stopped by signal %d", done.terminationStatus];
            else if (done.terminationStatus == 0)
                how = @"Exited";
            else
                how = [NSString stringWithFormat:@"Exited with code %d", done.terminationStatus];
            run.status = run.mode ? [NSString stringWithFormat:@"%@ (%@)", how, run.mode] : how;
            run.task = nil;
            [weakSelf.table reloadData];
            [weakSelf.grid reloadData];
        });
    };
    NSError *err = nil;
    if (![t launchAndReturnError:&err]) {
        run.status = [NSString stringWithFormat:@"Could not start: %@", err.localizedDescription];
    } else {
        run.task = t;
        run.status = run.mode ? [NSString stringWithFormat:@"Running (%@)", run.mode] : @"Running";
    }
    [out closeFile];
    [self.runs insertObject:run atIndex:0];
    [self.table reloadData];
    [[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:[NSURL fileURLWithPath:path]];
    return run;
}

/* SIGTERM first, as quitting would, and the prefix's wineserver for a Windows
 * program, which ends every Wine process in that prefix; SIGKILL if ocerz is
 * still there 5 s later. */
- (void)stopRun:(Run *)r
{
    NSTask *t = r.task;
    if (!t.running)
        return;
    [t terminate];
    if (r.wine && r.winePrefix) {
        NSString *ocerz = find_ocerz();
        NSString *server = [r.wine.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"wineserver"];
        if (ocerz && [[NSFileManager defaultManager] isExecutableFileAtPath:server]) {
            NSTask *k = [NSTask new];
            k.executableURL = [NSURL fileURLWithPath:ocerz];
            k.arguments = @[ @"--", server, @"-k" ];
            NSMutableDictionary *all = [[NSProcessInfo processInfo].environment mutableCopy];
            all[@"WINEPREFIX"] = r.winePrefix;
            k.environment = all;
            k.standardOutput = k.standardError = [NSFileHandle fileHandleWithNullDevice];
            [k launchAndReturnError:nil];
        }
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (t.running)
            kill(t.processIdentifier, SIGKILL);
    });
}

- (void)openLogOf:(Run *)r
{
    if (!r)
        return;
    NSURL *log = [NSURL fileURLWithPath:r.log];
    NSURL *console = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"com.apple.Console"];
    if (console)
        [[NSWorkspace sharedWorkspace] openURLs:@[ log ]
                           withApplicationAtURL:console
                                  configuration:[NSWorkspaceOpenConfiguration configuration]
                              completionHandler:nil];
    else
        [[NSWorkspace sharedWorkspace] openURL:log];
}

/* ---- activity ---- */

- (Run *)clickedRun
{
    NSInteger row = self.table.clickedRow;
    if (row < 0 || row >= (NSInteger)self.runs.count)
        return nil;
    return self.runs[(NSUInteger)row];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    SEL a = item.action;
    if (a == @selector(choose:) || a == @selector(performClose:) || a == @selector(showLibrary:) ||
        a == @selector(showActivity:))
        return YES;
    Run *r = [self clickedRun];
    if (!r)
        return NO;
    if (a == @selector(stop:))
        return r.task.running;
    return YES;
}

- (void)openLog:(id)sender
{
    [self openLogOf:[self clickedRun]];
}

- (void)reveal:(id)sender
{
    Run *r = [self clickedRun];
    if (r)
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:r.path] ]];
}

- (void)runAgain:(id)sender
{
    Run *r = [self clickedRun];
    if (!r)
        return;
    Item *i = [self itemForPath:r.path create:NO];
    if (i)
        [self runItem:i native:NO];
    else
        [self runPath:r.path native:NO];
}

- (void)stop:(id)sender
{
    [self stopRun:[self clickedRun]];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv
{
    return (NSInteger)self.runs.count;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row
{
    NSTableCellView *cell = [tv makeViewWithIdentifier:col.identifier owner:self];
    if (!cell) {
        cell = [NSTableCellView new];
        cell.identifier = col.identifier;
        NSTextField *label = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        label.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:label];
        cell.textField = label;
        [NSLayoutConstraint activateConstraints:@[
            [label.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:4],
            [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4],
            [label.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }
    Run *r = self.runs[(NSUInteger)row];
    NSString *ident = col.identifier;
    if ([ident isEqualToString:@"name"]) {
        cell.textField.stringValue = r.name;
        cell.toolTip = r.path;
    } else if ([ident isEqualToString:@"status"]) {
        cell.textField.stringValue = r.status;
        cell.toolTip = r.log;
    } else {
        cell.textField.stringValue = [NSDateFormatter localizedStringFromDate:r.started
                                                                    dateStyle:NSDateFormatterNoStyle
                                                                    timeStyle:NSDateFormatterShortStyle];
        cell.toolTip = nil;
    }
    cell.textField.textColor = [ident isEqualToString:@"started"] ? [NSColor secondaryLabelColor] : [NSColor labelColor];
    return cell;
}

@end

int main(int argc, const char **argv)
{
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *d = [AppDelegate new];
        app.delegate = d;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
