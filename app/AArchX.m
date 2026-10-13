/*
 * AArchX.app: runs the Intel programs dropped on it under ocerz.
 *
 * A program reaches it by being dropped on its window or its Dock icon, by
 * Open With in Finder, or through Choose.  Each one runs as
 * `ocerz [-native] <path>` with its output in ~/Library/Logs/AArchX, and the
 * window lists every run with how it ended.  ocerz is looked for inside the
 * bundle, then beside it, then in ~/.aarchx and Homebrew's bin.
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

@interface Run : NSObject
@property(copy) NSString *name;
@property(copy) NSString *path;
@property(copy) NSString *status;
@property(copy) NSString *log;
@property(copy) NSString *mode;
@property(strong) NSTask *task;
@end
@implementation Run
@end

@class AppDelegate;

@interface DropView : NSView
@property(weak) AppDelegate *owner;
@property BOOL hot;
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property(strong) NSWindow *window;
@property(strong) NSTableView *table;
@property(strong) NSButton *nativeBox;
@property(strong) NSTextField *footer;
@property(strong) NSMutableArray<Run *> *runs;
- (void)runPaths:(NSArray<NSString *> *)paths;
@end

@implementation DropView

- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame]))
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return self;
}

- (void)drawRect:(NSRect)dirty
{
    NSRect r = NSInsetRect(self.bounds, 6, 6);
    NSBezierPath *b = [NSBezierPath bezierPathWithRoundedRect:r xRadius:10 yRadius:10];
    CGFloat dash[] = { 6, 4 };
    [b setLineDash:dash count:2 phase:0];
    b.lineWidth = 2;
    [(self.hot ? [NSColor controlAccentColor] : [NSColor tertiaryLabelColor]) setStroke];
    [b stroke];
    NSString *s = @"Drop an Intel app or program here";
    NSDictionary *a = @{
        NSFontAttributeName : [NSFont systemFontOfSize:15 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
    };
    NSSize sz = [s sizeWithAttributes:a];
    [s drawAtPoint:NSMakePoint(NSMidX(self.bounds) - sz.width / 2, NSMidY(self.bounds) - sz.height / 2 + 12)
        withAttributes:a];
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
    return NSDragOperationGeneric;
}

- (void)draggingExited:(id<NSDraggingInfo>)info
{
    self.hot = NO;
    [self setNeedsDisplay:YES];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    self.hot = NO;
    [self setNeedsDisplay:YES];
    NSArray *paths = [self pathsFrom:info];
    [self.owner runPaths:paths];
    return paths.count > 0;
}

@end

@implementation AppDelegate

- (void)applicationWillFinishLaunching:(NSNotification *)n
{
    self.runs = [NSMutableArray array];
    clear_own_quarantine();
    [self buildMenu];
    [self buildWindow];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls
{
    NSMutableArray *paths = [NSMutableArray array];
    for (NSURL *u in urls)
        if (u.isFileURL)
            [paths addObject:u.path];
    [self runPaths:paths];
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
    [fileMenu addItemWithTitle:@"Open…" action:@selector(choose:) keyEquivalent:@"o"];
    [fileMenu addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
    fileItem.submenu = fileMenu;
    NSApp.mainMenu = bar;
}

- (void)buildWindow
{
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 420)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"AArchX";
    self.window.minSize = NSMakeSize(420, 340);
    [self.window center];
    self.window.releasedWhenClosed = NO;
    NSView *root = self.window.contentView;

    DropView *drop = [DropView new];
    drop.owner = self;
    drop.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:drop];

    NSButton *choose = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(choose:)];
    choose.translatesAutoresizingMaskIntoConstraints = NO;
    [drop addSubview:choose];

    self.nativeBox = [NSButton checkboxWithTitle:@"Native mode: use the Mac's own frameworks instead of Apple's Intel ones"
                                          target:self
                                          action:@selector(nativeChanged:)];
    self.nativeBox.translatesAutoresizingMaskIntoConstraints = NO;
    self.nativeBox.state = [[NSUserDefaults standardUserDefaults] boolForKey:@"Native"] ? NSControlStateValueOn
                                                                                       : NSControlStateValueOff;
    [root addSubview:self.nativeBox];

    self.table = [NSTableView new];
    if (@available(macOS 11.0, *))
        self.table.style = NSTableViewStylePlain;
    NSTableColumn *nameCol = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    nameCol.title = @"Program";
    nameCol.width = 200;
    NSTableColumn *statusCol = [[NSTableColumn alloc] initWithIdentifier:@"status"];
    statusCol.title = @"Status";
    statusCol.width = 260;
    [self.table addTableColumn:nameCol];
    [self.table addTableColumn:statusCol];
    self.table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
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
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:scroll];

    self.footer = [NSTextField wrappingLabelWithString:@""];
    self.footer.textColor = [NSColor secondaryLabelColor];
    self.footer.font = [NSFont systemFontOfSize:11];
    self.footer.selectable = YES;
    self.footer.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:self.footer];

    NSDictionary *v = @{ @"drop" : drop, @"box" : self.nativeBox, @"scroll" : scroll, @"footer" : self.footer };
    for (NSString *f in @[
             @"H:|-12-[drop]-12-|", @"H:|-16-[box]-16-|", @"H:|-12-[scroll]-12-|", @"H:|-16-[footer]-16-|",
             @"V:|-12-[drop(150)]-8-[box]-10-[scroll(>=100)]-8-[footer]-12-|"
         ])
        [root addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:f options:0 metrics:nil views:v]];
    [drop addConstraints:@[
        [choose.centerXAnchor constraintEqualToAnchor:drop.centerXAnchor],
        [choose.bottomAnchor constraintEqualToAnchor:drop.bottomAnchor constant:-36],
    ]];
    [self updateFooter];
}

- (void)updateFooter
{
    NSString *ocerz = find_ocerz();
    if (!ocerz) {
        self.footer.stringValue = @"ocerz was not found. Install it with:\n"
                                  @"curl -fsSL https://mont127.github.io/AArchX/install.sh | sh";
        return;
    }
    NSString *s = [NSString stringWithFormat:@"%@ at %@. Double-click a run to see its log.", ocerz_version(ocerz),
                                             ocerz.stringByAbbreviatingWithTildeInPath];
    if (!rosetta_cache_present())
        s = [s stringByAppendingString:@"\nRosetta is not installed, so programs run in native mode."];
    self.footer.stringValue = s;
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
    p.message = @"Choose an Intel app or program to run under AArchX";
    [p beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse r) {
                  if (r != NSModalResponseOK)
                      return;
                  NSMutableArray *paths = [NSMutableArray array];
                  for (NSURL *u in p.URLs)
                      [paths addObject:u.path];
                  [self runPaths:paths];
              }];
}

- (void)tell:(NSString *)title detail:(NSString *)detail
{
    NSAlert *a = [NSAlert new];
    a.messageText = title;
    a.informativeText = detail;
    [a beginSheetModalForWindow:self.window completionHandler:nil];
}

- (void)runPaths:(NSArray<NSString *> *)paths
{
    for (NSString *p in paths)
        [self runPath:p];
}

- (void)runPath:(NSString *)path
{
    [self.window makeKeyAndOrderFront:nil];
    NSString *name = [[NSFileManager defaultManager] displayNameAtPath:path];
    NSString *ocerz = find_ocerz();
    if (!ocerz) {
        [self tell:@"ocerz was not found"
            detail:@"Install AArchX in Terminal with\ncurl -fsSL https://mont127.github.io/AArchX/install.sh | sh\n"
                   @"and drop the program here again."];
        return;
    }
    NSString *exe = executable_of(path);
    int arches = exe ? macho_arches(exe) : 0;
    if (!exe || !arches) {
        [self tell:[NSString stringWithFormat:@"%@ is not a Mac program", name]
            detail:@"AArchX runs Intel macOS apps and command-line programs. For Windows programs use MacNdCheese."];
        return;
    }
    if (!(arches & (ARCH_X86_64 | ARCH_I386))) {
        [self tell:[NSString stringWithFormat:@"%@ is already an Apple silicon program", name]
            detail:@"It runs on this Mac by itself; open it as usual."];
        return;
    }

    BOOL native = self.nativeBox.state == NSControlStateValueOn;
    NSString *why = nil;
    if (!(arches & ARCH_X86_64)) {
        native = YES;
        why = @"32-bit";
    } else if (!native && !rosetta_cache_present()) {
        native = YES;
        why = @"no Rosetta";
    }

    NSString *logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/AArchX"];
    [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *df = [NSDateFormatter new];
    df.dateFormat = @"yyyy-MM-dd HH.mm.ss";
    NSString *safe = [name stringByReplacingOccurrencesOfString:@"/" withString:@"-"];
    NSString *log = [logs stringByAppendingPathComponent:
                              [NSString stringWithFormat:@"%@ %@.log", safe, [df stringFromDate:[NSDate date]]]];
    [[NSFileManager defaultManager] createFileAtPath:log contents:nil attributes:nil];
    NSFileHandle *out = [NSFileHandle fileHandleForWritingAtPath:log];

    NSMutableArray *args = [NSMutableArray array];
    if (native)
        [args addObject:@"-native"];
    [args addObjectsFromArray:@[ @"--", path ]];
    NSString *head = [NSString stringWithFormat:@"%@ %@\n\n", ocerz, [args componentsJoinedByString:@" "]];
    [out writeData:[head dataUsingEncoding:NSUTF8StringEncoding]];

    Run *run = [Run new];
    run.name = name;
    run.path = path;
    run.log = log;
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:ocerz];
    t.arguments = args;
    t.currentDirectoryURL = [NSURL fileURLWithPath:NSHomeDirectory()];
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
        });
    };
    NSError *err = nil;
    run.mode = why ? [NSString stringWithFormat:@"native, %@", why] : native ? @"native" : nil;
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
}

- (Run *)clickedRun
{
    NSInteger row = self.table.clickedRow;
    if (row < 0 || row >= (NSInteger)self.runs.count)
        return nil;
    return self.runs[row];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(choose:) || item.action == @selector(performClose:))
        return YES;
    Run *r = [self clickedRun];
    if (!r)
        return NO;
    if (item.action == @selector(stop:))
        return r.task.running;
    return YES;
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
    if (r)
        [self runPath:r.path];
}

/* SIGTERM first, as quitting would; SIGKILL if it is still there 5 s later. */
- (void)stop:(id)sender
{
    Run *r = [self clickedRun];
    NSTask *t = r.task;
    if (!t.running)
        return;
    [t terminate];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (t.running)
            kill(t.processIdentifier, SIGKILL);
    });
}

- (void)openLog:(id)sender
{
    Run *r = [self clickedRun];
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
    Run *r = self.runs[row];
    BOOL name = [col.identifier isEqualToString:@"name"];
    cell.textField.stringValue = name ? r.name : r.status;
    cell.toolTip = name ? r.path : r.log;
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
