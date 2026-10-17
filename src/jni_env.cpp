#include "jni_env.h"
#include "log.h"

#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

JavaVM *g_vm = nullptr;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

typedef jint (*EvalStringFn)(JNIEnv *, jclass, jstring);

EvalStringFn g_evalString = nullptr;
jclass g_bridgeClass = nullptr;

jobject g_appLoader = nullptr;

const char *kBridgeClass = "com/cocos/lib/CocosJavascriptJavaBridge";
const char *kEvalSymbol = "Java_com_cocos_lib_CocosJavascriptJavaBridge_evalString";

jclass g_helperClass = nullptr;
jmethodID g_flushTasks = nullptr;
bool g_flushBroken = false;

// ---------------------------------------------------------------------------
// se::ScriptEngine singleton
//
// evalString() begins with
//      0x39bc00  bl    0x3bcc00
//    0x3bcc00  adrp  x8, #0x1a04000
//    0x3bcc04  ldr   x0, [x8, #0x1b8]     <- the singleton
// Calling evalString while it is null crashes at evalString+56 (fault 0xa4b8).
//
// The loadable segment holding that address has p_vaddr != p_offset
// (0x19358b8 vs 0x19348b8), and dladdr()'s dli_fbase is the mapping start of the
// p_offset==0 segment, so "base + vaddr" overshoots by 0x1000.  Rather than
// hard-coding that correction we resolve the *runtime* address of the file offset
// through /proc/self/maps, which is exact by construction.
// ---------------------------------------------------------------------------
uintptr_t g_engineSlotAddr = 0;
bool      g_engineReady = false;
bool      g_engineProbeBroken = false;

// file offset of the singleton inside libcocos.so
const unsigned long kEngineSlotFileOff = 0x1a031b8;

bool resolveEngineSlot() {
    if (g_engineSlotAddr) return true;

    unsigned long base = 0;
    unsigned long slot = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long start, end, off;
        char perms[8], path[256];
        path[0] = 0;
        int n = sscanf(line, "%lx-%lx %7s %lx %*s %*s %255s", &start, &end, perms, &off, path);
        if (n < 4) continue;
        if (!strstr(path, "libcocos.so")) continue;

        if (off == 0) base = start;
        if (off <= kEngineSlotFileOff && kEngineSlotFileOff < off + (end - start)) {
            slot = start + (kEngineSlotFileOff - off);
            break;
        }
    }
    fclose(f);

    if (!base || !slot) {
        static int logged = 0;
        if (logged++ == 0) LOGW("engine slot: libcocos.so mapping not found yet");
        return false;
    }

    g_engineSlotAddr = slot;
    LOGI("engine slot @ %p (libcocos base %p)", (void *) slot, (void *) base);
    return true;
}

bool engineExists() {
    if (!resolveEngineSlot()) return false;
    void *engine = nullptr;
    memcpy(&engine, (void *) g_engineSlotAddr, sizeof(void *));
    return engine != nullptr;
}

} // namespace

void MW_SetJavaVM(JavaVM *vm) {
    if (vm && !g_vm) {
        g_vm = vm;
        LOGI("JavaVM cached (%p)", vm);
    }
}

JavaVM *MW_GetJavaVM() {
    if (g_vm) return g_vm;
    typedef jint (*GetVMsFn)(JavaVM **, jsize, jsize *);
    static GetVMsFn getVMs = nullptr;
    if (!getVMs) getVMs = (GetVMsFn) dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (!getVMs) {
        static bool warned = false;
        if (!warned) { warned = true; LOGE("dlsym(JNI_GetCreatedJavaVMs) failed"); }
        return nullptr;
    }
    JavaVM *vm = nullptr;
    jsize n = 0;
    if (getVMs(&vm, 1, &n) == JNI_OK && n > 0) MW_SetJavaVM(vm);
    return g_vm;
}

JNIEnv *MW_GetEnv() {
    JavaVM *vm = MW_GetJavaVM();
    if (!vm) return nullptr;
    JNIEnv *env = nullptr;
    jint r = vm->GetEnv((void **) &env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("AttachCurrentThread failed");
            return nullptr;
        }
    } else if (r != JNI_OK) {
        return nullptr;
    }
    return env;
}

static bool cacheAppLoader(JNIEnv *env) {
    if (g_appLoader) return true;

    jclass clsAT = env->FindClass("android/app/ActivityThread");
    if (!clsAT) { env->ExceptionClear(); return false; }
    jmethodID mCur = env->GetStaticMethodID(clsAT, "currentApplication",
                                            "()Landroid/app/Application;");
    if (!mCur) { env->ExceptionClear(); env->DeleteLocalRef(clsAT); return false; }

    jobject app = env->CallStaticObjectMethod(clsAT, mCur);
    env->DeleteLocalRef(clsAT);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!app) return false;

    jclass clsCtx = env->FindClass("android/content/Context");
    jmethodID mLoader = clsCtx
        ? env->GetMethodID(clsCtx, "getClassLoader", "()Ljava/lang/ClassLoader;")
        : nullptr;
    if (clsCtx) env->DeleteLocalRef(clsCtx);

    jobject loader = mLoader ? env->CallObjectMethod(app, mLoader) : nullptr;
    env->DeleteLocalRef(app);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!loader) return false;

    g_appLoader = env->NewGlobalRef(loader);
    env->DeleteLocalRef(loader);
    if (g_appLoader) LOGI("app ClassLoader cached");
    return g_appLoader != nullptr;
}

jclass MW_FindAppClass(JNIEnv *env, const char *name) {
    if (!env) return nullptr;
    if (!g_appLoader && !cacheAppLoader(env)) return nullptr;

    jclass clsLoader = env->FindClass("java/lang/ClassLoader");
    if (!clsLoader) { env->ExceptionClear(); return nullptr; }
    jmethodID mLoad = env->GetMethodID(clsLoader, "loadClass",
                                       "(Ljava/lang/String;)Ljava/lang/Class;");
    env->DeleteLocalRef(clsLoader);
    if (!mLoad) { env->ExceptionClear(); return nullptr; }

    char dotted[256];
    size_t n = strlen(name);
    if (n >= sizeof(dotted)) return nullptr;
    for (size_t i = 0; i <= n; ++i) dotted[i] = (name[i] == '/') ? '.' : name[i];

    jstring jn = env->NewStringUTF(dotted);
    jobject c = env->CallObjectMethod(g_appLoader, mLoad, jn);
    env->DeleteLocalRef(jn);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return (jclass) c;
}

bool MW_GetScreenSize(int *ow, int *oh) {
    JNIEnv *env = MW_GetEnv();
    if (!env) return false;
    bool ok = false;

    jclass clsAT = env->FindClass("android/app/ActivityThread");
    jclass clsCtx = env->FindClass("android/content/Context");
    jclass clsRes = env->FindClass("android/content/res/Resources");
    jclass clsDm  = env->FindClass("android/util/DisplayMetrics");
    if (clsAT && clsCtx && clsRes && clsDm) {
        jmethodID mCur = env->GetStaticMethodID(clsAT, "currentApplication", "()Landroid/app/Application;");
        jmethodID mGetRes = env->GetMethodID(clsCtx, "getResources", "()Landroid/content/res/Resources;");
        jmethodID mGetDm = env->GetMethodID(clsRes, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
        jfieldID fW = env->GetFieldID(clsDm, "widthPixels", "I");
        jfieldID fH = env->GetFieldID(clsDm, "heightPixels", "I");
        if (mCur && mGetRes && mGetDm && fW && fH) {
            jobject app = env->CallStaticObjectMethod(clsAT, mCur);
            if (app) {
                jobject res = env->CallObjectMethod(app, mGetRes);
                if (res) {
                    jobject dm = env->CallObjectMethod(res, mGetDm);
                    if (dm) {
                        jint w = env->GetIntField(dm, fW);
                        jint h = env->GetIntField(dm, fH);
                        if (w > 0 && h > 0) { *ow = w; *oh = h; ok = true; }
                        env->DeleteLocalRef(dm);
                    }
                    env->DeleteLocalRef(res);
                }
                env->DeleteLocalRef(app);
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (clsAT) env->DeleteLocalRef(clsAT);
    if (clsCtx) env->DeleteLocalRef(clsCtx);
    if (clsRes) env->DeleteLocalRef(clsRes);
    if (clsDm) env->DeleteLocalRef(clsDm);
    return ok;
}

void MW_FlushCocosTasks() {
    if (g_flushBroken) return;
    JNIEnv *env = MW_GetEnv();
    if (!env) return;

    if (!g_flushTasks) {
        if (!g_helperClass) {
            jclass c = MW_FindAppClass(env, "com/cocos/lib/CocosHelper");
            if (!c) return;
            g_helperClass = (jclass) env->NewGlobalRef(c);
            env->DeleteLocalRef(c);
            if (!g_helperClass) return;
        }
        g_flushTasks = env->GetStaticMethodID(
            g_helperClass, "flushTasksOnGameThreadAtForeground", "()V");
        if (!g_flushTasks) {
            env->ExceptionClear();
            LOGW("CocosHelper.flushTasksOnGameThreadAtForeground not found");
            g_flushBroken = true;
            return;
        }
        LOGI("CocosHelper queue flush resolved");
    }

    env->CallStaticVoidMethod(g_helperClass, g_flushTasks);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        g_flushBroken = true;
        LOGW("flushTasksOnGameThreadAtForeground threw, disabled");
    }
}

bool MW_ResolveEngine() {
    pthread_mutex_lock(&g_lock);
    bool ok = true;

    if (!g_evalString) {
        void *cocos = dlopen("libcocos.so", RTLD_NOLOAD | RTLD_NOW);
        if (!cocos) cocos = dlopen("libcocos.so", RTLD_NOW);
        if (!cocos) {
            LOGE("libcocos.so not loadable: %s", dlerror());
            ok = false;
        } else {
            g_evalString = (EvalStringFn) dlsym(cocos, kEvalSymbol);
            if (!g_evalString) {
                LOGE("dlsym(%s) failed: %s", kEvalSymbol, dlerror());
                ok = false;
            } else {
                LOGI("evalString resolved at %p", (void *) g_evalString);
            }
        }
    }

    if (!g_bridgeClass) {
        JNIEnv *env = MW_GetEnv();
        if (env) {
            jclass c = MW_FindAppClass(env, kBridgeClass);
            if (c) {
                g_bridgeClass = (jclass) env->NewGlobalRef(c);
                env->DeleteLocalRef(c);
                LOGI("bridge class cached");
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool MW_RunJS(const char *code) {
    if (!code || !*code) return false;
    if (!g_evalString || !g_bridgeClass) {
        if (!MW_ResolveEngine()) return false;
    }
    if (!g_evalString) return false;

    // Never call into the engine before it exists -- evalString dereferences the
    // singleton unconditionally.
    if (!g_engineReady) {
        if (g_engineProbeBroken) return false;
        if (!engineExists()) {
            static int n = 0;
            if (n++ % 120 == 0) LOGW("script engine not up yet, skipping JS");
            return false;
        }
        g_engineReady = true;
        LOGI("script engine is up, JS injection enabled");
    }

    JNIEnv *env = MW_GetEnv();
    if (!env || !g_bridgeClass) return false;

    jstring js = env->NewStringUTF(code);
    if (!js) {
        env->ExceptionClear();
        return false;
    }
    g_evalString(env, g_bridgeClass, js);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(js);
    return true;
}
