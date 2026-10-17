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

// Provide the JNI symbols the game's libcocos.so expects at runtime.
// (None needed -- we only *call* into it.)

} // namespace

void MW_SetJavaVM(JavaVM *vm) {
    if (vm && !g_vm) {
        g_vm = vm;
        LOGI("JavaVM cached (%p)", vm);
    }
}

JavaVM *MW_GetJavaVM() {
    if (g_vm) return g_vm;
    JavaVM *vm = nullptr;
    jsize n = 0;
    if (JNI_GetCreatedJavaVMs(&vm, 1, &n) == JNI_OK && n > 0) {
        MW_SetJavaVM(vm);
    }
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
