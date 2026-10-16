/*
 * tsearch, tfind, tdelete and twalk for i386 guests (include/ocerz/m32.h).  libc's trees are made of nodes it
 * allocates, whose first word is the key a caller reads straight out of the node tsearch hands back: on arm64 that
 * word is 8 bytes and the node host memory, and the root the caller passes is a 4-byte guest cell libc would read 8
 * bytes from (VLC 1.1's tfind faulted in libc).  So the tree is kept in guest memory instead, an AVL tree of 16-byte
 * nodes {key, left, right, height} from the guest heap, with the caller's comparison and action functions called as
 * guest code.  tdelete answers the deleted node's parent, or 1 for the root, as BSD's does.
 */
#include <stdint.h>
#include <string.h>

#include "ocerz/interp.h"
#include "ocerz/m32.h"

#define RET(v) do { m32_ret(cpu, (uint32_t)(v), 0, 0); return OCERZ_STEP_OK; } while (0)

enum { KEY = 0, LEFT = 4, RIGHT = 8, HEIGHT = 12, NODE_BYTES = 16 };

typedef struct Tree { struct OcerzVM *vm; uint32_t compar, key; } Tree;

static uint32_t at(uint32_t n, uint32_t f) { return m32_rd(n + f); }
static void set(uint32_t n, uint32_t f, uint32_t v) { m32_wr(n + f, v); }
static uint32_t height(uint32_t n) { return n ? at(n, HEIGHT) : 0; }
static int32_t compare(const Tree *t, uint32_t node)
{
    const uint32_t args[2] = { t->key, at(node, KEY) };
    return (int32_t)m32_call(t->vm, t->compar, args, 2, NULL, NULL);
}

static void fix(uint32_t n)
{
    uint32_t l = height(at(n, LEFT)), r = height(at(n, RIGHT));
    set(n, HEIGHT, 1 + (l > r ? l : r));
}

static uint32_t rotate_right(uint32_t n)
{
    uint32_t l = at(n, LEFT);
    set(n, LEFT, at(l, RIGHT));
    set(l, RIGHT, n);
    fix(n);
    fix(l);
    return l;
}

static uint32_t rotate_left(uint32_t n)
{
    uint32_t r = at(n, RIGHT);
    set(n, RIGHT, at(r, LEFT));
    set(r, LEFT, n);
    fix(n);
    fix(r);
    return r;
}

static uint32_t balance(uint32_t n)
{
    fix(n);
    int32_t b = (int32_t)height(at(n, LEFT)) - (int32_t)height(at(n, RIGHT));
    if (b > 1) {
        uint32_t l = at(n, LEFT);
        if (height(at(l, LEFT)) < height(at(l, RIGHT)))
            set(n, LEFT, rotate_left(l));
        return rotate_right(n);
    }
    if (b < -1) {
        uint32_t r = at(n, RIGHT);
        if (height(at(r, RIGHT)) < height(at(r, LEFT)))
            set(n, RIGHT, rotate_right(r));
        return rotate_left(n);
    }
    return n;
}

/* the subtree n with key in it; *node is the key's node, new or found */
static uint32_t insert(const Tree *t, uint32_t n, uint32_t *node)
{
    if (!n) {
        n = m32_malloc(NODE_BYTES);
        if (n) {
            set(n, KEY, t->key);
            set(n, LEFT, 0);
            set(n, RIGHT, 0);
            set(n, HEIGHT, 1);
        }
        *node = n;
        return n;
    }
    int32_t c = compare(t, n);
    if (c == 0) {
        *node = n;
        return n;
    }
    if (c < 0)
        set(n, LEFT, insert(t, at(n, LEFT), node));
    else
        set(n, RIGHT, insert(t, at(n, RIGHT), node));
    return balance(n);
}

static uint32_t remove_min(uint32_t n, uint32_t *min)
{
    if (!at(n, LEFT)) {
        *min = n;
        return at(n, RIGHT);
    }
    set(n, LEFT, remove_min(at(n, LEFT), min));
    return balance(n);
}

/* the subtree n without key; *parent the deleted node's parent, *gone whether one was */
static uint32_t remove_key(const Tree *t, uint32_t n, uint32_t up, uint32_t *parent, int *gone)
{
    if (!n)
        return 0;
    int32_t c = compare(t, n);
    if (c < 0) {
        set(n, LEFT, remove_key(t, at(n, LEFT), n, parent, gone));
    } else if (c > 0) {
        set(n, RIGHT, remove_key(t, at(n, RIGHT), n, parent, gone));
    } else {
        uint32_t l = at(n, LEFT), r = at(n, RIGHT), min = 0;
        *gone = 1;
        *parent = up;
        m32_free(n);
        if (!r)
            return l;
        r = remove_min(r, &min);
        set(min, LEFT, l);
        set(min, RIGHT, r);
        return balance(min);
    }
    return balance(n);
}

/* tsearch(const void *key, void **rootp, int (*compar)(const void *, const void *)) */
static int sp_tsearch(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t rootp = m32_arg(cpu, 1), node = 0;
    if (!rootp)
        RET(0);
    Tree t = { vm, m32_arg(cpu, 2), m32_arg(cpu, 0) };
    m32_wr(rootp, insert(&t, m32_rd(rootp), &node));
    RET(node);
}

/* tfind(const void *key, void *const *rootp, compar) */
static int sp_tfind(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t rootp = m32_arg(cpu, 1);
    Tree t = { vm, m32_arg(cpu, 2), m32_arg(cpu, 0) };
    for (uint32_t n = rootp ? m32_rd(rootp) : 0; n;) {
        int32_t c = compare(&t, n);
        if (c == 0)
            RET(n);
        n = at(n, c < 0 ? LEFT : RIGHT);
    }
    RET(0);
}

/* tdelete(const void *key, void **rootp, compar) */
static int sp_tdelete(struct OcerzVM *vm, OcerzCPU *cpu)
{
    uint32_t rootp = m32_arg(cpu, 1), parent = 0;
    int gone = 0;
    if (!rootp || !m32_rd(rootp))
        RET(0);
    Tree t = { vm, m32_arg(cpu, 2), m32_arg(cpu, 0) };
    m32_wr(rootp, remove_key(&t, m32_rd(rootp), 0, &parent, &gone));
    RET(!gone ? 0 : parent ? parent : 1);
}

/* twalk(const void *root, void (*action)(const void *node, VISIT, int level)): preorder 0, postorder 1, endorder 2,
 * leaf 3 */
static void walk(struct OcerzVM *vm, uint32_t action, uint32_t n, uint32_t level)
{
    uint32_t l = at(n, LEFT), r = at(n, RIGHT);
    if (!l && !r) {
        m32_call(vm, action, (const uint32_t[]){ n, 3, level }, 3, NULL, NULL);
        return;
    }
    m32_call(vm, action, (const uint32_t[]){ n, 0, level }, 3, NULL, NULL);
    if (l)
        walk(vm, action, l, level + 1);
    m32_call(vm, action, (const uint32_t[]){ n, 1, level }, 3, NULL, NULL);
    if (r)
        walk(vm, action, r, level + 1);
    m32_call(vm, action, (const uint32_t[]){ n, 2, level }, 3, NULL, NULL);
}

static int sp_twalk(struct OcerzVM *vm, OcerzCPU *cpu)
{
    if (m32_arg(cpu, 0) && m32_arg(cpu, 1))
        walk(vm, m32_arg(cpu, 1), m32_arg(cpu, 0), 0);
    RET(0);
}

const M32SpecialEntry m32_tsearch_specials[] = {
    { "_tsearch", sp_tsearch }, { "_tfind", sp_tfind }, { "_tdelete", sp_tdelete }, { "_twalk", sp_twalk },
    { NULL, NULL }
};
