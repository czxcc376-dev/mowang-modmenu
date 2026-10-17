#include "jni_env.h"
#include "log.h"

#include <dlfcn.h>
#include <pthread.h>
#include <cstring>

namespace {

JavaVM *g_vm = nullptr;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

typedef jint (*EvalStringFn)(JNIEnv *, jclass, jstring);

EvalStringFn g_evalString = nullptr;
jclass g_bridgeClass = nullptr;   // global ref, com/cocos/lib/CocosJavascriptJavaBridge

const char *kBridgeClass = "com/cocos/lib/CocosJavascriptJavaBridge";
const char *kEvalSymbol = "Java_com_cocos_lib_CocosJavascriptJavaBridge_evalString";

} // namespace

void MW_SetJavaVM(JavaVM *vm) {
    if (vm && !g_vm) {
        g_vm = vm;
        LOGI("JavaVM cached (%p)", vm);
    }
}

// JNI_GetCreatedJavaVMs lives in libnativehelper/libart (not an NDK public stub),
// so resolve it at runtime instead of linking against it.
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
    if (!ok) LOGW("MW_GetScreenSize failed");
    return ok;
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
            jclass local = env->FindClass(kBridgeClass);
            if (local) {
                g_bridgeClass = (jclass) env->NewGlobalRef(local);
                env->DeleteLocalRef(local);
                LOGI("bridge class cached");
            } else {
                env->ExceptionClear();
                LOGW("FindClass(%s) not ready yet", kBridgeClass);
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
    JNIEnv *env = MW_GetEnv();
    if (!env || !g_evalString || !g_bridgeClass) return false;

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
