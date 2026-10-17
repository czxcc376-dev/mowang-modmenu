// libmwmenu.so -- Cocos Creator 3.7.3 (V8) mod menu for
// com.niaowustudio.imnodemonlord.green
//
// Loaded as a DT_NEEDED dependency of lib/arm64-v8a/libcocos.so (or via any
// injector).  JNI_OnLoad() is NOT called for DT_NEEDED libraries, so all work
// starts from a constructor.
#include "hooks.h"
#include "jni_env.h"
#include "log.h"

#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) reserved;
    LOGI("JNI_OnLoad");
    MW_SetJavaVM(vm);
    return JNI_VERSION_1_6;
}

namespace {

void *wait_dlopen(const char *name, int tries) {
    for (int i = 0; i < tries; ++i) {
        void *h = dlopen(name, RTLD_NOLOAD | RTLD_NOW);
        if (h) return h;
        usleep(100 * 1000);
    }
    return nullptr;
}

void *worker(void *) {
    JavaVM *vm = nullptr;
    for (int i = 0; i < 600; ++i) {
        vm = MW_GetJavaVM();
        if (vm) break;
        usleep(50 * 1000);
    }
    LOGI("worker start, vm=%p", vm);

    if (!wait_dlopen("libcocos.so", 600)) {
        LOGE("libcocos.so never appeared, aborting");
        return nullptr;
    }
    LOGI("libcocos.so present");

    if (!wait_dlopen("libEGL.so", 600)) {
        LOGE("libEGL.so never appeared, hooks will be partial");
    } else {
        LOGI("libEGL.so present");
    }

    int n = Hooks_Install();
    LOGI("Hooks_Install -> %d", n);

    if (MW_ResolveEngine()) LOGI("JS bridge resolved");
    else LOGW("JS bridge not resolved yet (will retry on first use)");

    return nullptr;
}

} // namespace

__attribute__((constructor)) static void mwmenu_ctor() {
    LOGI("libmwmenu.so loaded (ctor)");
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) {
        pthread_detach(t);
    } else {
        LOGE("pthread_create failed");
    }
}
