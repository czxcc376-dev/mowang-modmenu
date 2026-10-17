// Touch bridge over the game's own JS input system.
//
// The Cocos -> JNI path (CocosTouchHandler.onTouchEvent -> CocosHelper queue ->
// handleAction*()) never delivers anything in this build: the JNI entry points
// are hooked successfully but are never invoked.  The game itself receives
// touches perfectly well through its JS layer (cc.input / Node.on(TOUCH_*)), which
// is where InputController.ts listens.  So instead of intercepting events we let
// the game tell us: a small JS snippet mirrors the touch coordinates into a file
// under the app's private dir and libmwmenu polls it once per frame.
#include "touchbridge.h"
#include "cheats.h"
#include "jni_env.h"
#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

namespace {

std::string g_touchPath;
bool        g_installed = false;
std::atomic<int> g_logs{0};

float g_x = -1.f, g_y = -1.f;
int   g_down = 0;

// A file whose mtime does not change means nothing new happened.
timespec g_lastMtime = {0, 0};

} // namespace

// The file lives in the app's private files dir, which both sides can reach:
// C++ via getFilesDir(), JS via jsb.fileUtils.
static bool resolveTouchPath() {
    if (!g_touchPath.empty()) return true;

    JNIEnv *env = MW_GetEnv();
    if (!env) return false;

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
    jmethodID mFiles = clsCtx ? env->GetMethodID(clsCtx, "getFilesDir",
                                                 "()Ljava/io/File;") : nullptr;
    if (clsCtx) env->DeleteLocalRef(clsCtx);
    jobject file = mFiles ? env->CallObjectMethod(app, mFiles) : nullptr;
    env->DeleteLocalRef(app);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!file) return false;

    jclass clsFile = env->FindClass("java/io/File");
    jmethodID mPath = clsFile ? env->GetMethodID(clsFile, "getAbsolutePath",
                                                 "()Ljava/lang/String;") : nullptr;
    if (clsFile) env->DeleteLocalRef(clsFile);
    jstring jp = mPath ? (jstring) env->CallObjectMethod(file, mPath) : nullptr;
    env->DeleteLocalRef(file);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!jp) return false;

    const char *p = env->GetStringUTFChars(jp, nullptr);
    if (p) {
        g_touchPath = std::string(p) + "/mw_touch.txt";
        env->ReleaseStringUTFChars(jp, p);
        LOGI("touch file: %s", g_touchPath.c_str());
    }
    env->DeleteLocalRef(jp);
    return !g_touchPath.empty();
}

bool TouchBridge_Install() {
    if (g_installed) return true;
    if (!resolveTouchPath()) return false;
    if (!g_cheat.bootstrapped.load()) return false;   // need __MW first

    char js[512];
    snprintf(js, sizeof(js),
             "__MW.installTouch(%s);", ("'" + g_touchPath + "'").c_str());
    if (!Cheats_RawJS(js)) return false;

    g_installed = true;
    LOGI("touch bridge requested");
    return true;
}

void TouchBridge_Poll(float *x, float *y, int *down) {
    if (!g_installed) return;

    struct stat st{};
    if (stat(g_touchPath.c_str(), &st) != 0) return;
    if (st.st_mtim.tv_sec == g_lastMtime.tv_sec &&
        st.st_mtim.tv_nsec == g_lastMtime.tv_nsec) {
        return;                                   // unchanged
    }
    g_lastMtime = st.st_mtim;

    FILE *f = fopen(g_touchPath.c_str(), "r");
    if (!f) return;
    char buf[128] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return;

    float fx = -1.f, fy = -1.f;
    int d = 0;
    if (sscanf(buf, "%f,%f,%d", &fx, &fy, &d) == 3) {
        g_x = fx; g_y = fy; g_down = d;
        if (g_logs.fetch_add(1) < 10)
            LOGI("touch %.0f,%.0f down=%d", fx, fy, d);
    }
}
