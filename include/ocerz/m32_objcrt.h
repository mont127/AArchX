/*
 * The host Objective-C runtime for m32, resolved on first use (ocerz links no framework, as
 * src/objcbridge.c's OB_SYM does).  Include after <objc/runtime.h>: the names below then call these wrappers.
 */
#ifndef OCERZ_M32_OBJCRT_H
#define OCERZ_M32_OBJCRT_H

#include <dlfcn.h>
#include <objc/runtime.h>

static inline void *m32rt_sym(const char *name)
{
    static void *lib;
    if (!lib)
        lib = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
    return dlsym(lib ? lib : RTLD_DEFAULT, name);
}

#define M32RT(ret, name, params, args)                                        \
    static inline ret m32rt_##name params                                    \
    {                                                                        \
        static ret (*f) params;                                              \
        if (!f)                                                              \
            f = (ret (*) params)m32rt_sym(#name);                            \
        return f args;                                                       \
    }

M32RT(Protocol **, class_copyProtocolList, (Class c, unsigned *n), (c, n))
M32RT(id, class_createInstance, (Class c, size_t extra), (c, extra))
M32RT(Method, class_getInstanceMethod, (Class c, SEL s), (c, s))
M32RT(size_t, class_getInstanceSize, (Class c), (c))
M32RT(const char *, class_getName, (Class c), (c))
M32RT(Class, class_getSuperclass, (Class c), (c))
M32RT(BOOL, class_isMetaClass, (Class c), (c))
M32RT(IMP, class_replaceMethod, (Class c, SEL s, IMP i, const char *t), (c, s, i, t))
M32RT(BOOL, class_respondsToSelector, (Class c, SEL s), (c, s))
M32RT(IMP, method_getImplementation, (Method m), (m))
M32RT(SEL, method_getName, (Method m), (m))
M32RT(const char *, method_getTypeEncoding, (Method m), (m))
M32RT(IMP, method_setImplementation, (Method m, IMP i), (m, i))
M32RT(Class, object_getClass, (id o), (o))
M32RT(const char *, object_getClassName, (id o), (o))
M32RT(BOOL, object_isClass, (id o), (o))
M32RT(Class, objc_allocateClassPair, (Class s, const char *n, size_t e), (s, n, e))
M32RT(Class, objc_getClass, (const char *n), (n))
M32RT(void, objc_registerClassPair, (Class c), (c))
M32RT(int, objc_sync_enter, (id o), (o))
M32RT(int, objc_sync_exit, (id o), (o))
M32RT(const char *, protocol_getName, (Protocol *p), (p))
M32RT(const char *, sel_getName, (SEL s), (s))
M32RT(SEL, sel_registerName, (const char *n), (n))

#define class_copyProtocolList m32rt_class_copyProtocolList
#define class_createInstance m32rt_class_createInstance
#define class_getInstanceMethod m32rt_class_getInstanceMethod
#define class_getInstanceSize m32rt_class_getInstanceSize
#define class_getName m32rt_class_getName
#define class_getSuperclass m32rt_class_getSuperclass
#define class_isMetaClass m32rt_class_isMetaClass
#define class_replaceMethod m32rt_class_replaceMethod
#define class_respondsToSelector m32rt_class_respondsToSelector
#define method_getImplementation m32rt_method_getImplementation
#define method_getName m32rt_method_getName
#define method_getTypeEncoding m32rt_method_getTypeEncoding
#define method_setImplementation m32rt_method_setImplementation
#define object_getClass m32rt_object_getClass
#define object_getClassName m32rt_object_getClassName
#define object_isClass m32rt_object_isClass
#define objc_allocateClassPair m32rt_objc_allocateClassPair
#define objc_getClass m32rt_objc_getClass
#define objc_registerClassPair m32rt_objc_registerClassPair
#define objc_sync_enter m32rt_objc_sync_enter
#define objc_sync_exit m32rt_objc_sync_exit
#define protocol_getName m32rt_protocol_getName
#define sel_getName m32rt_sel_getName
#define sel_registerName m32rt_sel_registerName
/* the message senders as plain function pointers (cast at each use) */
#define M32_MSGSEND ((void *)m32rt_sym("objc_msgSend"))
#define M32_MSGSENDSUPER ((void *)m32rt_sym("objc_msgSendSuper"))

#endif
