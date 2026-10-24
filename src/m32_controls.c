/*
 * Carbon menus and controls for i386 guests (include/ocerz/m32.h), on what Apple silicon's HIToolbox keeps.  The menu
 * and control calls that arm64 HIToolbox still exports cross through the databases (tools/sdkgen, declare_hidden);
 * these are the ones it dropped, made from their HIView and menu successors, with Carbon's documented contracts:
 *
 *   menus      EnableMenuCommand and IsMenuCommandEnabled find the item by command (GetIndMenuItemWithCommandID);
 *              SetItemCmd and GetItemCmd are its command key; the hierarchical ID calls go through the submenu's
 *              MenuRef; GetMenuBar, SetMenuBar and DisposeMenuBar keep the menus of the menu bar in a handle of
 *              their own
 *   controls   EmbedControl and AutoEmbedControl add a subview; a control's reference value is a control property;
 *              GetControlRegion is its frame in the window's coordinates; the 32-bit value, minimum, maximum and
 *              view size are the HIView's; SetControlBounds sets its frame; CountSubControls counts subviews;
 *              GetControlDataSize asks GetControlData; Draw1Control, DrawControls and UpdateControls ask for a redraw;
 *              TestControl and FindControl send the view a hit-test event; HandleControlClick and TrackControl
 *              click the part hit (HIViewSimulateClick); IsControlEnabled is HIViewIsEnabled
 *   user panes a view of a class of m32's own (CreateUserPaneControl) that keeps the guest's draw, hit-test,
 *              tracking, activate and focus procedures (SetControlData's kControlUserPane...ProcTag) and calls them;
 *              a draw procedure draws with QuickDraw into the window's port (src/m32_qd.c), which the pane then
 *              shows
 *
 * Most of the old control kinds (radio buttons, bevel buttons, sliders, tabs, pop-up buttons, edit text, scroll bars,
 * group boxes, the Data Browser) are gone from arm64 HIToolbox and m32 does not make them yet (docs/m32.md).
 */
#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0   /* Carbon's check() and require() */
#include <Carbon/Carbon.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)
#define A(i) m32_arg(cpu, i)
#define H(i) m32_host(A(i))

typedef void *Ref;
typedef struct { int16_t v, h; } QdPoint;
typedef struct { int16_t top, left, bottom, right; } QdRect;

/* ---- the host functions: their documented prototypes, as arm64's headers declare few of them ---- */

static struct {
    OSStatus (*get_ind_item)(Ref, UInt32, UInt32, Ref *, UInt16 *);
    void (*enable_item)(Ref, UInt16);
    Boolean (*item_enabled)(Ref, UInt16);
    OSStatus (*set_cmd_key)(Ref, UInt16, Boolean, UInt16);
    OSStatus (*get_cmd_key)(Ref, UInt16, Boolean, UInt16 *);
    OSStatus (*set_submenu)(Ref, UInt16, Ref);
    OSStatus (*get_submenu)(Ref, UInt16, Ref *);
    Ref (*menu_handle)(int16_t);
    int16_t (*menu_id)(Ref);
    Ref (*root_menu)(void);
    UInt16 (*count_items)(Ref);
    void (*clear_menu_bar)(void);
    void (*insert_menu)(Ref, int16_t);
    OSStatus (*add_subview)(Ref, Ref);
    OSStatus (*set_property)(Ref, OSType, OSType, UInt32, const void *);
    OSStatus (*get_property)(Ref, OSType, OSType, UInt32, UInt32 *, void *);
    OSStatus (*get_frame)(Ref, CGRect *);
    OSStatus (*set_frame)(Ref, const CGRect *);
    Ref (*superview)(Ref);
    OSStatus (*convert_rect)(CGRect *, Ref, Ref);
    OSStatus (*convert_point)(CGPoint *, Ref, Ref);
    Ref (*view_root)(Ref);
    OSStatus (*find_by_id)(Ref, HIViewID, Ref *);
    Ref (*view_window)(Ref);
    OSStatus (*needs_display)(Ref, Boolean);
    SInt32 (*get_value)(Ref);
    OSStatus (*set_value)(Ref, SInt32);
    SInt32 (*get_min)(Ref);
    OSStatus (*set_min)(Ref, SInt32);
    SInt32 (*get_max)(Ref);
    OSStatus (*set_max)(Ref, SInt32);
    SInt32 (*get_view_size)(Ref);
    OSStatus (*set_view_size)(Ref, SInt32);
    Ref (*first_subview)(Ref);
    Ref (*next_view)(Ref);
    Boolean (*is_enabled)(Ref, UInt16 *);
    OSStatus (*simulate_click)(Ref, int16_t, UInt32, int16_t *);
    OSStatus (*get_control_data)(Ref, int16_t, OSType, Size, void *, Size *);
    OSStatus (*create_event)(CFAllocatorRef, UInt32, UInt32, EventTime, EventAttributes, EventRef *);
    OSStatus (*set_param)(EventRef, OSType, OSType, ByteCount, const void *);
    OSStatus (*get_param)(EventRef, OSType, OSType, OSType *, ByteCount, ByteCount *, void *);
    OSStatus (*send_event)(EventRef, EventTargetRef);
    EventTargetRef (*control_target)(Ref);
    OSStatus (*register_subclass)(CFStringRef, CFStringRef, OptionBits, EventHandlerUPP, ItemCount, const EventTypeSpec *,
                                  void *, void **);
    OSStatus (*hiobject_create)(CFStringRef, EventRef, Ref *);
    OSStatus (*set_visible)(Ref, Boolean);
    OSStatus (*get_bounds)(Ref, CGRect *);
    UInt32 (*event_class)(EventRef);
    UInt32 (*event_kind)(EventRef);
    void (*cf_release)(CFTypeRef);
} hi;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_ok;
static const HIViewID *g_content_id;
static void load(void)
{
    void *h = dlopen("/System/Library/Frameworks/Carbon.framework/Carbon", RTLD_LAZY | RTLD_LOCAL);
    int ok = h != NULL;
#define L(field, name) ok &= (*(void **)&hi.field = h ? dlsym(h, name) : NULL) != NULL
    L(get_ind_item, "GetIndMenuItemWithCommandID"); L(enable_item, "EnableMenuItem"); L(item_enabled, "IsMenuItemEnabled");
    L(set_cmd_key, "SetMenuItemCommandKey"); L(get_cmd_key, "GetMenuItemCommandKey");
    L(set_submenu, "SetMenuItemHierarchicalMenu"); L(get_submenu, "GetMenuItemHierarchicalMenu");
    L(menu_handle, "GetMenuHandle"); L(menu_id, "GetMenuID"); L(root_menu, "AcquireRootMenu");
    L(count_items, "CountMenuItems"); L(clear_menu_bar, "ClearMenuBar"); L(insert_menu, "InsertMenu");
    L(add_subview, "HIViewAddSubview"); L(set_property, "SetControlProperty"); L(get_property, "GetControlProperty");
    L(get_frame, "HIViewGetFrame"); L(set_frame, "HIViewSetFrame"); L(superview, "HIViewGetSuperview");
    L(convert_rect, "HIViewConvertRect"); L(convert_point, "HIViewConvertPoint"); L(view_root, "HIViewGetRoot");
    L(find_by_id, "HIViewFindByID"); L(view_window, "HIViewGetWindow"); L(needs_display, "HIViewSetNeedsDisplay");
    L(get_value, "HIViewGetValue"); L(set_value, "HIViewSetValue"); L(get_min, "HIViewGetMinimum");
    L(set_min, "HIViewSetMinimum"); L(get_max, "HIViewGetMaximum"); L(set_max, "HIViewSetMaximum");
    L(get_view_size, "HIViewGetViewSize"); L(set_view_size, "HIViewSetViewSize");
    L(first_subview, "HIViewGetFirstSubview"); L(next_view, "HIViewGetNextView"); L(is_enabled, "HIViewIsEnabled");
    L(simulate_click, "HIViewSimulateClick"); L(get_control_data, "GetControlData"); L(create_event, "CreateEvent");
    L(set_param, "SetEventParameter"); L(get_param, "GetEventParameter"); L(send_event, "SendEventToEventTarget");
    L(control_target, "GetControlEventTarget"); L(register_subclass, "HIObjectRegisterSubclass");
    L(hiobject_create, "HIObjectCreate"); L(set_visible, "HIViewSetVisible"); L(get_bounds, "HIViewGetBounds");
    L(event_class, "GetEventClass"); L(event_kind, "GetEventKind");
#undef L
    *(void **)&hi.cf_release = dlsym(RTLD_DEFAULT, "CFRelease");
    g_content_id = h ? (const HIViewID *)dlsym(h, "kHIViewWindowContentID") : NULL;
    g_ok = ok && hi.cf_release && g_content_id;
    if (!g_ok)
        fprintf(stderr, "ocerz: m32: Carbon controls: a HIToolbox function is missing\n");
}
static int ready(void) { pthread_once(&g_once, load); return g_ok; }

static void wr16(uint32_t g, int16_t v) { memcpy(m32_h(g), &v, 2); }

/* ---- menus ---- */

/* EnableMenuCommand(MenuRef, MenuCommand): the first item with the command, in the menu or, NULL, the menu bar */
static int sp_EnableMenuCommand(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    Ref menu = NULL;
    UInt16 item = 0;
    if (ready() && hi.get_ind_item(H(0), A(1), 1, &menu, &item) == noErr)
        hi.enable_item(menu, item);
    RET(0);
}
static int sp_IsMenuCommandEnabled(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    Ref menu = NULL;
    UInt16 item = 0;
    RET(ready() && hi.get_ind_item(H(0), A(1), 1, &menu, &item) == noErr && hi.item_enabled(menu, item));
}
/* SetItemCmd(MenuRef, MenuItemIndex, CharParameter) and GetItemCmd(MenuRef, MenuItemIndex, CharParameter *) */
static int sp_SetItemCmd(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (ready())
        hi.set_cmd_key(H(0), (UInt16)A(1), false, (UInt16)(A(2) & 0xff));
    RET(0);
}
static int sp_GetItemCmd(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    UInt16 key = 0;
    if (ready())
        hi.get_cmd_key(H(0), (UInt16)A(1), false, &key);
    if (A(2))
        wr16(A(2), (int16_t)key);
    RET(0);
}
/* SetMenuItemHierarchicalID(MenuRef, MenuItemIndex, MenuID): the menu inserted under that ID, or none for 0 */
static int sp_SetMenuItemHierarchicalID(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (!ready())
        RET((uint32_t)-50);
    int16_t id = (int16_t)A(2);
    Ref sub = id ? hi.menu_handle(id) : NULL;
    RET((uint32_t)(int32_t)hi.set_submenu(H(0), (UInt16)A(1), sub));
}
static int sp_GetMenuItemHierarchicalID(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    Ref sub = NULL;
    OSStatus st = ready() ? hi.get_submenu(H(0), (UInt16)A(1), &sub) : -50;
    if (A(2))
        wr16(A(2), sub ? hi.menu_id(sub) : 0);
    RET((uint32_t)(int32_t)st);
}

/* a menu bar handle: a guest handle of a list of the root menu's menus.  A program may build its own (Kega Fusion
 * makes an empty one with NewHandleClear and sets it), which is a Memory Manager handle and is left alone */
enum { MENU_BAR_MAGIC = 0x6d426172 /* 'mBar' */ };
typedef struct { uint32_t magic; int n; Ref menus[64]; } MenuBar;

static MenuBar *menu_bar(uint32_t g)
{
    MenuBar *b = g && m32_is_handle(g) ? m32_host(g) : NULL;
    return b && b->magic == MENU_BAR_MAGIC ? b : NULL;
}

static int sp_GetMenuBar(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    MenuBar *b = calloc(1, sizeof *b);
    Ref root = ready() && b ? hi.root_menu() : NULL;
    if (!root) {
        free(b);
        RET(0);
    }
    b->magic = MENU_BAR_MAGIC;
    UInt16 n = hi.count_items(root);
    for (UInt16 i = 1; i <= n && b->n < 64; i++) {
        Ref sub = NULL;
        if (hi.get_submenu(root, i, &sub) == noErr && sub)
            b->menus[b->n++] = sub;
    }
    hi.cf_release(root);
    RET(m32_handle(b));
}
static int sp_SetMenuBar(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    MenuBar *b = menu_bar(A(0));
    if (!ready() || !b)
        RET(0);
    hi.clear_menu_bar();
    for (int i = 0; i < b->n; i++)
        hi.insert_menu(b->menus[i], 0);
    RET(0);
}
static int sp_DisposeMenuBar(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    MenuBar *b = menu_bar(A(0));
    if (b) {
        b->magic = 0;
        free(b);
    }
    RET(0);
}

/* ---- controls ---- */

static Ref content_view(Ref window)
{
    Ref content = NULL;
    if (window && ready())
        hi.find_by_id(hi.view_root(window), *g_content_id, &content);
    return content;
}

/* a control's frame in its window's content coordinates, QuickDraw's local ones */
static int frame_in_window(Ref control, CGRect *r)
{
    if (!control || hi.get_frame(control, r) != noErr)
        return 0;
    Ref content = content_view(hi.view_window(control)), super = hi.superview(control);
    if (content && super && super != content)
        hi.convert_rect(r, super, content);
    return 1;
}

static int sp_EmbedControl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    RET(ready() && A(0) && A(1) ? (uint32_t)(int32_t)hi.add_subview(H(1), H(0)) : (uint32_t)-50);
}
static int sp_AutoEmbedControl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    Ref content = ready() ? content_view(H(1)) : NULL;
    RET(content && A(0) ? (uint32_t)(int32_t)hi.add_subview(content, H(0)) : (uint32_t)-50);
}

enum { PROP_CREATOR = 'ocrz', PROP_REFCON = 'rfcn' };
static int sp_SetControlReference(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    SInt32 v = (SInt32)A(1);
    if (ready() && A(0))
        hi.set_property(H(0), PROP_CREATOR, PROP_REFCON, sizeof v, &v);
    RET(0);
}
static int sp_GetControlReference(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    SInt32 v = 0;
    if (ready() && A(0))
        hi.get_property(H(0), PROP_CREATOR, PROP_REFCON, sizeof v, NULL, &v);
    RET((uint32_t)v);
}

/* GetControlRegion(ControlRef, ControlPartCode, RgnHandle): every part is the control's frame */
static int sp_GetControlRegion(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static void (*rect_rgn)(void *, const QdRect *);
    if (!rect_rgn)
        *(void **)&rect_rgn = dlsym(RTLD_DEFAULT, "RectRgn");
    CGRect r;
    if (!ready() || !rect_rgn || !A(2) || !frame_in_window(H(0), &r))
        RET((uint32_t)-50);
    QdRect q = { (int16_t)r.origin.y, (int16_t)r.origin.x, (int16_t)(r.origin.y + r.size.height),
                 (int16_t)(r.origin.x + r.size.width) };
    rect_rgn(H(2), &q);
    RET(0);
}

/* SetControlBounds(ControlRef, const Rect *): its frame, in its parent's coordinates as GetControlBounds answers */
static int sp_SetControlBounds(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (ready() && A(0) && A(1)) {
        QdRect q;
        memcpy(&q, m32_h(A(1)), 8);
        CGRect r = CGRectMake(q.left, q.top, q.right - q.left, q.bottom - q.top);
        hi.set_frame(H(0), &r);
    }
    RET(0);
}

static int sp_GetValue(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? (uint32_t)hi.get_value(H(0)) : 0); }
static int sp_SetValue(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) hi.set_value(H(0), (SInt32)A(1)); RET(0); }
static int sp_GetMinimum(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? (uint32_t)hi.get_min(H(0)) : 0); }
static int sp_SetMinimum(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) hi.set_min(H(0), (SInt32)A(1)); RET(0); }
static int sp_GetMaximum(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? (uint32_t)hi.get_max(H(0)) : 0); }
static int sp_SetMaximum(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) hi.set_max(H(0), (SInt32)A(1)); RET(0); }
static int sp_GetViewSize(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? (uint32_t)hi.get_view_size(H(0)) : 0); }
static int sp_SetViewSize(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) hi.set_view_size(H(0), (SInt32)A(1)); RET(0); }
/* the 16-bit forms: the value clamped to a short */
static uint32_t clamp16(SInt32 v) { return (uint32_t)(int32_t)(int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static int sp_GetMinimum16(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? clamp16(hi.get_min(H(0))) : 0); }
static int sp_GetMaximum16(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) ? clamp16(hi.get_max(H(0))) : 0); }
static int sp_SetMinimum16(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) hi.set_min(H(0), (int16_t)A(1)); RET(0); }
static int sp_SetMaximum16(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) hi.set_max(H(0), (int16_t)A(1)); RET(0); }

/* CountSubControls(ControlRef, UInt16 *) */
static int sp_CountSubControls(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    UInt16 n = 0;
    if (ready() && A(0))
        for (Ref v = hi.first_subview(H(0)); v; v = hi.next_view(v))
            n++;
    if (A(1))
        wr16(A(1), (int16_t)n);
    RET(0);
}

/* GetControlDataSize(ControlRef, ControlPartCode, ResType, Size *) */
static int sp_GetControlDataSize(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    Size n = 0;
    OSStatus st = ready() && A(0) ? hi.get_control_data(H(0), (int16_t)A(1), A(2), 0, NULL, &n) : -50;
    if (A(3))
        m32_wr(A(3), (uint32_t)n);
    RET((uint32_t)(int32_t)st);
}

static int sp_Draw1Control(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; if (ready() && A(0)) hi.needs_display(H(0), true); RET(0); }
static int sp_DrawControls(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    Ref content = ready() ? content_view(H(0)) : NULL;
    if (content)
        hi.needs_display(content, true);
    RET(0);
}
static int sp_IsControlEnabled(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() && A(0) && hi.is_enabled(H(0), NULL)); }

/* the part of control at a point in its window's coordinates: the control is asked with a hit-test event */
static int16_t part_at(Ref control, QdPoint where)
{
    CGRect frame;
    if (!control || !frame_in_window(control, &frame))
        return 0;
    CGPoint local = CGPointMake(where.h - frame.origin.x, where.v - frame.origin.y);
    EventRef e = NULL;
    if (hi.create_event(NULL, kEventClassControl, kEventControlHitTest, 0, 0, &e) != noErr)
        return 0;
    int16_t part = 0;
    hi.set_param(e, kEventParamDirectObject, typeControlRef, sizeof control, &control);
    hi.set_param(e, kEventParamMouseLocation, typeHIPoint, sizeof local, &local);
    if (hi.send_event(e, hi.control_target(control)) == noErr)
        hi.get_param(e, kEventParamControlPart, typeControlPartCode, NULL, sizeof part, NULL, &part);
    hi.cf_release(e);
    return part;
}

static QdPoint point_arg(uint32_t w) { QdPoint p = { (int16_t)(w & 0xffff), (int16_t)(w >> 16) }; return p; }

/* TestControl(ControlRef, Point) -> ControlPartCode */
static int sp_TestControl(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(ready() ? (uint32_t)(int32_t)part_at(H(0), point_arg(A(1))) : 0); }

/* the deepest visible subview of view whose frame holds where (window coordinates) */
static Ref subview_at(Ref view, QdPoint where)
{
    for (Ref v = hi.first_subview(view); v; v = hi.next_view(v)) {
        CGRect r;
        if (frame_in_window(v, &r) && where.h >= r.origin.x && where.h < r.origin.x + r.size.width &&
            where.v >= r.origin.y && where.v < r.origin.y + r.size.height) {
            Ref deeper = subview_at(v, where);
            return deeper ? deeper : v;
        }
    }
    return NULL;
}

/* FindControl(Point, WindowRef, ControlRef *) -> ControlPartCode */
static int sp_FindControl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    QdPoint where = point_arg(A(0));
    Ref content = ready() ? content_view(H(1)) : NULL, hit = content ? subview_at(content, where) : NULL;
    int16_t part = hit ? part_at(hit, where) : 0;
    if (A(2))
        m32_wr(A(2), part ? m32_handle(hit) : 0);
    RET((uint32_t)(int32_t)part);
}

/* HandleControlClick(ControlRef, Point, EventModifiers, ControlActionUPP) and TrackControl(ControlRef, Point,
 * ControlActionUPP): the part under the point clicked, the part released in answered; the action procedure is not
 * called during the click */
static int click(OcerzCPU *cpu, uint32_t modifiers)
{
    if (!ready() || !A(0))
        RET(0);
    int16_t part = part_at(H(0), point_arg(A(1))), out = 0;
    if (part)
        hi.simulate_click(H(0), part, modifiers, &out);
    RET((uint32_t)(int32_t)out);
}
static int sp_HandleControlClick(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return click(cpu, A(2) & 0xffff); }
static int sp_TrackControl(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; return click(cpu, 0); }

/* ---- user panes ---- */

enum { TAG_DRAW = 'draw', TAG_HITTEST = 'hitt', TAG_TRACKING = 'trak', TAG_IDLE = 'idle', TAG_KEYDOWN = 'keyd',
       TAG_ACTIVATE = 'acti', TAG_FOCUS = 'foci', TAG_BACKGROUND = 'back' };
static const uint32_t g_pane_tags[8] = { TAG_DRAW, TAG_HITTEST, TAG_TRACKING, TAG_IDLE, TAG_KEYDOWN, TAG_ACTIVATE,
                                         TAG_FOCUS, TAG_BACKGROUND };

typedef struct UserPane {
    Ref view;
    uint32_t proc[8];   /* the guest's procedures, by g_pane_tags */
} UserPane;

static uint32_t pane_proc(const UserPane *p, uint32_t tag)
{
    for (int i = 0; i < 8; i++)
        if (g_pane_tags[i] == tag)
            return p->proc[i];
    return 0;
}

/* where, a point in the view's coordinates, in its window's */
static QdPoint to_window(Ref view, CGPoint local)
{
    CGRect frame = CGRectMake(0, 0, 0, 0);
    frame_in_window(view, &frame);
    QdPoint p = { (int16_t)(local.y + frame.origin.y), (int16_t)(local.x + frame.origin.x) };
    return p;
}

static OSStatus pane_event(EventHandlerCallRef next, EventRef e, void *data)
{
    (void)next;
    UInt32 cls = hi.event_class(e), kind = hi.event_kind(e);
    if (cls == kEventClassHIObject && kind == kEventHIObjectConstruct) {
        UserPane *p = calloc(1, sizeof *p);
        hi.get_param(e, kEventParamHIObjectInstance, typeHIObjectRef, NULL, sizeof p->view, NULL, &p->view);
        hi.set_param(e, kEventParamHIObjectInstance, typeVoidPtr, sizeof p, &p);
        return noErr;
    }
    UserPane *p = data;
    if (cls == kEventClassHIObject && kind == kEventHIObjectDestruct) {
        free(p);
        return noErr;
    }
    if (cls != kEventClassControl || !p)
        return eventNotHandledErr;
    switch (kind) {
    case kEventControlSetData:
    case kEventControlGetData: {
        OSType tag = 0;
        void *buf = NULL;
        hi.get_param(e, kEventParamControlDataTag, typeEnumeration, NULL, sizeof tag, NULL, &tag);
        hi.get_param(e, kEventParamControlDataBuffer, typePtr, NULL, sizeof buf, NULL, &buf);
        for (int i = 0; i < 8; i++)
            if (g_pane_tags[i] == tag && buf) {
                if (kind == kEventControlSetData)
                    memcpy(&p->proc[i], buf, 4);   /* a guest procedure, 4 bytes in guest memory */
                else
                    memcpy(buf, &p->proc[i], 4);
                return noErr;
            }
        return eventNotHandledErr;
    }
    case kEventControlDraw: {
        uint32_t draw = pane_proc(p, TAG_DRAW);
        CGContextRef ctx = NULL;
        hi.get_param(e, kEventParamCGContextRef, typeCGContextRef, NULL, sizeof ctx, NULL, &ctx);
        Ref window = hi.view_window(p->view);
        CGRect frame;
        if (!draw || !window || !frame_in_window(p->view, &frame))
            return noErr;
        /* the procedure draws into the window's QuickDraw port, which the pane then shows */
        void (*proc)(Ref, int16_t) = (void (*)(Ref, int16_t))m32_callback(draw, "v(ph)", "v(ph)");
        uint32_t port = m32_qd_window_port(window), was = m32_qd_current_port();
        m32_qd_set_current_port(port);
        m32_qd_quiet(1);
        if (proc)
            proc(p->view, 0);
        m32_qd_quiet(0);
        m32_qd_set_current_port(was);
        if (ctx)
            m32_qd_show_window_rect(window, ctx, frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
        return noErr;
    }
    case kEventControlHitTest:
    case kEventControlTrack: {
        CGPoint at;
        if (hi.get_param(e, kEventParamMouseLocation, typeHIPoint, NULL, sizeof at, NULL, &at) != noErr)
            return eventNotHandledErr;
        QdPoint where = to_window(p->view, at);
        int16_t part;
        uint32_t proc = pane_proc(p, kind == kEventControlHitTest ? TAG_HITTEST : TAG_TRACKING);
        if (kind == kEventControlHitTest && !proc) {
            CGRect b;
            hi.get_bounds(p->view, &b);
            part = at.x >= b.origin.x && at.x < b.origin.x + b.size.width && at.y >= b.origin.y &&
                   at.y < b.origin.y + b.size.height ? 1 : 0;
        } else if (!proc) {
            return eventNotHandledErr;
        } else if (kind == kEventControlHitTest) {
            int16_t (*f)(Ref, QdPoint) = (int16_t (*)(Ref, QdPoint))m32_callback(proc, "h(p{hh})", "h(p{hh})");
            part = f ? f(p->view, where) : 0;
        } else {
            int16_t (*f)(Ref, QdPoint, void *) = (int16_t (*)(Ref, QdPoint, void *))m32_callback(proc, "h(p{hh}p)", "h(p{hh}p)");
            part = f ? f(p->view, where, NULL) : 0;
        }
        hi.set_param(e, kEventParamControlPart, typeControlPartCode, sizeof part, &part);
        return noErr;
    }
    case kEventControlActivate:
    case kEventControlDeactivate: {
        uint32_t proc = pane_proc(p, TAG_ACTIVATE);
        void (*f)(Ref, Boolean) = proc ? (void (*)(Ref, Boolean))m32_callback(proc, "v(pB)", "v(pB)") : NULL;
        if (f)
            f(p->view, kind == kEventControlActivate);
        return f ? noErr : eventNotHandledErr;
    }
    case kEventControlSetFocusPart: {
        uint32_t proc = pane_proc(p, TAG_FOCUS);
        int16_t part = 0;
        hi.get_param(e, kEventParamControlPart, typeControlPartCode, NULL, sizeof part, NULL, &part);
        int16_t (*f)(Ref, int16_t) = proc ? (int16_t (*)(Ref, int16_t))m32_callback(proc, "h(ph)", "h(ph)") : NULL;
        if (!f)
            return eventNotHandledErr;
        part = f(p->view, part);
        hi.set_param(e, kEventParamControlPart, typeControlPartCode, sizeof part, &part);
        return noErr;
    }
    default:
        return eventNotHandledErr;
    }
}

static CFStringRef pane_class(void)
{
    static CFStringRef id;
    static const EventTypeSpec events[] = {
        { kEventClassHIObject, kEventHIObjectConstruct }, { kEventClassHIObject, kEventHIObjectDestruct },
        { kEventClassControl, kEventControlDraw }, { kEventClassControl, kEventControlHitTest },
        { kEventClassControl, kEventControlTrack }, { kEventClassControl, kEventControlSetData },
        { kEventClassControl, kEventControlGetData }, { kEventClassControl, kEventControlActivate },
        { kEventClassControl, kEventControlDeactivate }, { kEventClassControl, kEventControlSetFocusPart },
    };
    if (!id && ready()) {   /* CFSTR needs CoreFoundation linked, which ocerz is not */
        CFStringRef (*make)(CFAllocatorRef, const char *, CFStringEncoding) =
            (CFStringRef (*)(CFAllocatorRef, const char *, CFStringEncoding))dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
        CFStringRef want = make ? make(NULL, "org.ocerz.m32.UserPane", kCFStringEncodingUTF8) : NULL;
        CFStringRef base = make ? make(NULL, "com.apple.hiview", kCFStringEncodingUTF8) : NULL;
        void *ref = NULL;
        if (want && base && hi.register_subclass(want, base, 0, pane_event, sizeof events / sizeof events[0], events,
                                                 NULL, &ref) == noErr)
            id = want;
    }
    return id;
}

/* CreateUserPaneControl(WindowRef, const Rect *, UInt32 features, ControlRef *) */
static int sp_CreateUserPaneControl(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    Ref window = H(0), view = NULL;
    CFStringRef cls = ready() ? pane_class() : NULL;
    Ref content = content_view(window);
    if (!cls || !content || hi.hiobject_create(cls, NULL, &view) != noErr || !view)
        RET((uint32_t)-50);
    QdRect q = { 0, 0, 0, 0 };
    if (A(1))
        memcpy(&q, m32_h(A(1)), 8);
    CGRect r = CGRectMake(q.left, q.top, q.right - q.left, q.bottom - q.top);
    hi.add_subview(content, view);
    hi.set_frame(view, &r);
    hi.set_visible(view, true);
    if (A(3))
        m32_wr(A(3), m32_handle(view));
    RET(0);
}

/* ---- the rest ---- */

static int sp_Nothing(struct OcerzVM *vm, OcerzCPU *cpu) { (void)vm; RET(0); }

/* StillDown: whether the mouse button is down, from the session's state (CGEventSourceButtonState, button 0) */
static int sp_StillDown(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    static bool (*state)(int32_t, uint32_t);
    if (!state)
        state = (bool (*)(int32_t, uint32_t))dlsym(RTLD_DEFAULT, "CGEventSourceButtonState");
    RET(state && state(0, 0));   /* kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft */
}

/* GetThemeScrollBarArrowStyle and GetThemeScrollBarThumbStyle: Aqua's own, arrows together at the lower right (1)
 * and a proportional thumb (1), as the Macs these programs were written for had them */
static int sp_ThemeStyleOne(struct OcerzVM *vm, OcerzCPU *cpu)
{
    (void)vm;
    if (A(0))
        *(uint16_t *)m32_h(A(0)) = 1;
    RET(0);
}

const M32SpecialEntry m32_controls_specials[] = {
    { "_EnableMenuCommand", sp_EnableMenuCommand }, { "_IsMenuCommandEnabled", sp_IsMenuCommandEnabled },
    { "_SetItemCmd", sp_SetItemCmd }, { "_GetItemCmd", sp_GetItemCmd },
    { "_SetMenuItemHierarchicalID", sp_SetMenuItemHierarchicalID },
    { "_GetMenuItemHierarchicalID", sp_GetMenuItemHierarchicalID },
    { "_GetMenuBar", sp_GetMenuBar }, { "_SetMenuBar", sp_SetMenuBar }, { "_DisposeMenuBar", sp_DisposeMenuBar },
    { "_EmbedControl", sp_EmbedControl }, { "_AutoEmbedControl", sp_AutoEmbedControl },
    { "_SetControlReference", sp_SetControlReference }, { "_GetControlReference", sp_GetControlReference },
    { "_GetControlRegion", sp_GetControlRegion }, { "_SetControlBounds", sp_SetControlBounds },
    { "_GetControl32BitValue", sp_GetValue }, { "_SetControl32BitValue", sp_SetValue },
    { "_GetControl32BitMinimum", sp_GetMinimum }, { "_SetControl32BitMinimum", sp_SetMinimum },
    { "_GetControl32BitMaximum", sp_GetMaximum }, { "_SetControl32BitMaximum", sp_SetMaximum },
    { "_GetControlMinimum", sp_GetMinimum16 }, { "_SetControlMinimum", sp_SetMinimum16 },
    { "_GetControlMaximum", sp_GetMaximum16 }, { "_SetControlMaximum", sp_SetMaximum16 },
    { "_GetControlViewSize", sp_GetViewSize }, { "_SetControlViewSize", sp_SetViewSize },
    { "_CountSubControls", sp_CountSubControls }, { "_GetControlDataSize", sp_GetControlDataSize },
    { "_Draw1Control", sp_Draw1Control }, { "_DrawControls", sp_DrawControls }, { "_UpdateControls", sp_DrawControls },
    { "_IsControlEnabled", sp_IsControlEnabled }, { "_TestControl", sp_TestControl }, { "_FindControl", sp_FindControl },
    { "_HandleControlClick", sp_HandleControlClick }, { "_TrackControl", sp_TrackControl },
    { "_CreateUserPaneControl", sp_CreateUserPaneControl },
    { "_SetEventMask", sp_Nothing }, { "_TXNInitTextension", sp_Nothing }, { "_SetWindowGroupParent", sp_Nothing },
    { "_StillDown", sp_StillDown }, { "_GetThemeScrollBarArrowStyle", sp_ThemeStyleOne },
    { "_GetThemeScrollBarThumbStyle", sp_ThemeStyleOne },
    { NULL, NULL }
};
