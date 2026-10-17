#include "hooks.h"
#include "cheats.h"
#include "menu.h"
#include "jni_env.h"
#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include "dobby.h"

// ==========================================================================
// shared input state (written by UI thread touch hooks, read by GL thread)
// ==========================================================================
static std::atomic<float> g_touchX{-1.0f};
static std::atomic<float> g_touchY{-1.0f};
static std::atomic<int>   g_touchDown{0};

static std::atomic<int>   g_menuValid{0};
static std::atomic<float> g_menuX{0}, g_menuY{0}, g_menuW{0}, g_menuH{0};

void Input_SetMenuRect(float x, float y, float w, float h) {
    g_menuX.store(x); g_menuY.store(y); g_menuW.store(w); g_menuH.store(h);
    g_menuValid.store((w > 1.0f && h > 1.0f) ? 1 : 0);
}

void Input_GetMenuRect(float *x, float *y, float *w, float *h) {
    if (x) *x = g_menuX.load();
    if (y) *y = g_menuY.load();
    if (w) *w = g_menuW.load();
    if (h) *h = g_menuH.load();
}

static inline bool shouldSwallow(float x, float y) {
    if (!g_cheat.menuVisible.load()) return false;
    if (!g_menuValid.load()) return false;
    float mx = g_menuX.load(), my = g_menuY.load();
    float mw = g_menuW.load(), mh = g_menuH.load();
    return (x >= mx && x <= mx + mw && y >= my && y <= my + mh);
}

namespace {

pid_t gettid_() { return (pid_t) syscall(SYS_gettid); }

void readThreadName(char *out, size_t n) {
    out[0] = 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", (int) gettid_());
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) { out[0] = 0; return; }
    out[r] = 0;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
}

// libhwui composites the Android UI in the same process through the same
// eglSwapBuffers* entry points.  Drawing our menu there broke everything, so
// those threads are filtered out.
bool looksLikeUiCompositor(const char *n) {
    return strstr(n, "RenderThread") || strstr(n, "hwui") ||
           strstr(n, "Skia") || strstr(n, "GPU");
}

// ======================================================================
// ImGui / GLES
// ======================================================================
bool g_imguiReady = false;
int  g_imguiAttempts = 0;

const char *kFonts[] = {
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "/system/fonts/NotoSansCJKsc-Regular.otf",
    "/system/fonts/NotoSansSC-Regular.otf",
    "/system/fonts/DroidSansFallbackFull.ttf",
    "/system/fonts/DroidSansFallback.ttf",
};

void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding    = 10.0f;
    s.FrameRounding     = 6.0f;
    s.GrabRounding      = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize  = 1.0f;
    s.FramePadding      = ImVec2(14, 10);
    s.ItemSpacing       = ImVec2(10, 12);
    s.ScrollbarSize     = 26.0f;
    s.GrabMinSize       = 22.0f;
    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.06f, 0.07f, 0.09f, 0.94f);
    c[ImGuiCol_TitleBg]       = ImVec4(0.10f, 0.75f, 0.60f, 0.85f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.75f, 0.60f, 1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.10f, 0.90f, 0.70f, 1.00f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.10f, 0.85f, 0.68f, 1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.16f, 0.20f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.55f, 0.46f, 1.00f);
}

void tryInitImGui() {
    if (g_imguiReady || g_imguiAttempts >= 3) return;
    ++g_imguiAttempts;

    if (g_imguiAttempts == 1) {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        applyStyle();

        ImFont *font = nullptr;
        for (const char *p : kFonts) {
            if (access(p, R_OK) != 0) continue;
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            cfg.PixelSnapH  = true;
            font = io.Fonts->AddFontFromFileTTF(p, 28.0f, &cfg,
                                                io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            if (font) { LOGI("font: %s", p); break; }
        }
        if (!font) {
            io.Fonts->AddFontDefault();
            LOGW("no CJK font, using default atlas");
        }
    }

    const char *glv  = (const char *) glGetString(GL_VERSION);
    const char *glsl = (glv && strstr(glv, "OpenGL ES 3")) ? "#version 300 es" : "#version 100";
    LOGI("attempt#%d GL_VERSION=%s -> %s", g_imguiAttempts, glv ? glv : "?", glsl);

    if (ImGui_ImplOpenGL3_Init(glsl)) {
        // the GL3 backend creates the font texture on its first NewFrame()
        ImGui_ImplOpenGL3_NewFrame();
        g_imguiReady = true;
        LOGI("ImGui ready");
    } else {
        LOGE("ImGui_ImplOpenGL3_Init FAILED (attempt %d)", g_imguiAttempts);
    }
}

// ---- EGL swap entry points ------------------------------------------------
// libcocos.so does NOT import eglSwapBuffers; it resolves everything through
// eglGetProcAddress() and prefers eglSwapBuffersWithDamageKHR, so all three
// entry points have to be intercepted.
typedef EGLBoolean (*EglSwapFn)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*EglSwapDamageFn)(EGLDisplay, EGLSurface, EGLint *, EGLint);

EglSwapFn       orig_swap     = nullptr;   // eglSwapBuffers
EglSwapDamageFn orig_swap_khr = nullptr;   // eglSwapBuffersWithDamageKHR
EglSwapDamageFn orig_swap_ext = nullptr;   // eglSwapBuffersWithDamageEXT

std::atomic<unsigned long long> g_frames{0};
std::atomic<int>                g_probeLogs{0};
std::atomic<unsigned>           g_skipLogs{0};
std::atomic<pid_t>              g_renderTid{-1};

void renderFrame(EGLDisplay dpy, EGLSurface surf) {
    const pid_t tid = gettid_();
    char tname[32];
    readThreadName(tname, sizeof(tname));

    if (g_probeLogs.fetch_add(1) < 40) {
        LOGI("swap probe: tid=%d '%s' main=%d claimed=%d",
             (int) tid, tname, (int) (tid == getpid()), (int) g_renderTid.load());
    }
    if (looksLikeUiCompositor(tname)) return;   // Android UI, not the game

    pid_t cur = g_renderTid.load();
    if (cur < 0) {
        g_renderTid.store(tid);
        cur = tid;
        LOGI("claimed render thread: tid=%d '%s'", (int) tid, tname);
    }
    if (tid != cur) {
        if ((g_skipLogs.fetch_add(1) % 300) == 0)
            LOGW("skip swap from tid=%d '%s'", (int) tid, tname);
        return;
    }

    tryInitImGui();
    const unsigned long long f = g_frames.fetch_add(1) + 1;

    int w = 0, h = 0;
    if (eglQuerySurface(dpy, surf, EGL_WIDTH, &w) != EGL_TRUE) w = 0;
    if (eglQuerySurface(dpy, surf, EGL_HEIGHT, &h) != EGL_TRUE) h = 0;
    if (w <= 0 || h <= 0) MW_GetScreenSize(&w, &h);
    if (w <= 0 || h <= 0) { w = 1080; h = 1920; }

    if (!g_imguiReady) {
        if (f % 30 == 1) {
            glViewport(0, 0, w, h);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_DEPTH_TEST);
            glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 90, 90);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            LOGW("liveness marker drawn (ImGui not ready)");
        }
        if (f % 60 == 0) LOGW("swap#%llu no-imgui surf=%p %dx%d", f, surf, w, h);
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float) w, (float) h);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.AddMousePosEvent(g_touchX.load(), g_touchY.load());
    io.AddMouseButtonEvent(0, g_touchDown.load() != 0);

    // CocosTouchHandler.onTouchEvent() only posts a Runnable; handleAction*() run
    // when this queue is drained.  Drain it ourselves so touches arrive.
    MW_FlushCocosTasks();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    Menu_Draw();
    Cheats_Tick();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (f == 1 || f % 60 == 0)
        LOGI("swap#%llu surf=%p %dx%d menu=%d touch=%.0f,%.0f",
             f, surf, w, h, (int) g_cheat.menuVisible.load(),
             g_touchX.load(), g_touchY.load());
}

EGLBoolean hooked_swap(EGLDisplay d, EGLSurface s) {
    renderFrame(d, s);
    return orig_swap ? orig_swap(d, s) : EGL_TRUE;
}

EGLBoolean hooked_swap_khr(EGLDisplay d, EGLSurface s, EGLint *rects, EGLint n) {
    renderFrame(d, s);
    return orig_swap_khr ? orig_swap_khr(d, s, rects, n) : EGL_TRUE;
}

EGLBoolean hooked_swap_ext(EGLDisplay d, EGLSurface s, EGLint *rects, EGLint n) {
    renderFrame(d, s);
    return orig_swap_ext ? orig_swap_ext(d, s, rects, n) : EGL_TRUE;
}

// ------------------------------------------------- Cocos JNI touch handlers
// Java declarations (com/cocos/lib/CocosTouchHandler):
//   native void handleActionDown(int windowId, int pointerId, float x, float y)
//   native void handleActionUp  (int windowId, int pointerId, float x, float y)
//   native void handleActionMove(int windowId, int[] ids, float[] xs, float[] ys)
//   native void handleActionCancel(int windowId, int[] ids, float[] xs, float[] ys)
// They return void and are always invoked on the game thread, because
// onTouchEvent() posts them through CocosHelper.runOnGameThreadAtForeground().
typedef void (*TouchFn)(JNIEnv *, jclass, jint, jint, jfloat, jfloat);
typedef void (*TouchMultiFn)(JNIEnv *, jclass, jint, jintArray, jfloatArray, jfloatArray);

TouchFn orig_down = nullptr;
TouchFn orig_up = nullptr;
TouchMultiFn orig_cancel = nullptr;
TouchMultiFn orig_move = nullptr;

static std::atomic<int> g_touchLogs{0};

void hooked_down(JNIEnv *env, jclass c, jint window, jint id, jfloat x, jfloat y) {
    g_touchX.store(x); g_touchY.store(y); g_touchDown.store(1);
    if (g_touchLogs.fetch_add(1) < 20)
        LOGI("touch down win=%d id=%d %.0f,%.0f", window, id, x, y);
    if (shouldSwallow(x, y)) return;
    if (orig_down) orig_down(env, c, window, id, x, y);
}

void hooked_up(JNIEnv *env, jclass c, jint window, jint id, jfloat x, jfloat y) {
    g_touchX.store(x); g_touchY.store(y); g_touchDown.store(0);
    if (shouldSwallow(x, y)) return;
    if (orig_up) orig_up(env, c, window, id, x, y);
}

// Move / Cancel carry arrays; the first pointer drives the menu.
static void handleMulti(JNIEnv *env, jclass c, jint window, jintArray ids,
                        jfloatArray xs, jfloatArray ys, bool release,
                        TouchMultiFn orig) {
    if (xs && ys && env->GetArrayLength(xs) > 0 && env->GetArrayLength(ys) > 0) {
        float fx = 0, fy = 0;
        env->GetFloatArrayRegion(xs, 0, 1, &fx);
        env->GetFloatArrayRegion(ys, 0, 1, &fy);
        g_touchX.store(fx); g_touchY.store(fy);
        g_touchDown.store(release ? 0 : 1);
    }
    if (shouldSwallow(g_touchX.load(), g_touchY.load())) return;
    if (orig) orig(env, c, window, ids, xs, ys);
}

void hooked_cancel(JNIEnv *env, jclass c, jint window, jintArray ids,
                   jfloatArray xs, jfloatArray ys) {
    handleMulti(env, c, window, ids, xs, ys, true, orig_cancel);
}

void hooked_move(JNIEnv *env, jclass c, jint window, jintArray ids,
                 jfloatArray xs, jfloatArray ys) {
    handleMulti(env, c, window, ids, xs, ys, false, orig_move);
}

// ---------------------------------------------------------------- installer
void *resolveSym(const char *lib, const char *sym) {
    void *h = dlopen(lib, RTLD_NOLOAD | RTLD_NOW);
    if (!h) h = dlopen(lib, RTLD_NOW);
    if (!h) {
        LOGW("dlopen(%s) failed: %s", lib, dlerror());
        return nullptr;
    }
    void *p = dlsym(h, sym);
    if (!p) LOGW("dlsym(%s!%s) failed: %s", lib, sym, dlerror());
    return p;
}

bool installHook(const char *lib, const char *sym, void *newFn, void **orig) {
    if (*orig) return true;                     // already installed
    void *addr = resolveSym(lib, sym);
    if (!addr) return false;
    int rc = DobbyHook(addr, newFn, orig);
    if (rc == 0 && *orig) {
        LOGI("hooked %s!%s @%p -> orig=%p", lib, sym, addr, *orig);
        return true;
    }
    LOGW("DobbyHook(%s!%s @%p) failed rc=%d", lib, sym, addr, rc);
    *orig = nullptr;
    return false;
}

} // namespace

int Hooks_Install() {
    static bool logged = false;
    if (!logged) { logged = true; LOGI("hook engine: Dobby (inline, no linker interception)"); }

    int n = 0;
    if (installHook("libEGL.so", "eglSwapBuffers",
                    (void *) hooked_swap, (void **) &orig_swap)) n++;
    if (installHook("libEGL.so", "eglSwapBuffersWithDamageKHR",
                    (void *) hooked_swap_khr, (void **) &orig_swap_khr)) n++;
    if (installHook("libEGL.so", "eglSwapBuffersWithDamageEXT",
                    (void *) hooked_swap_ext, (void **) &orig_swap_ext)) n++;

    if (installHook("libcocos.so", "Java_com_cocos_lib_CocosTouchHandler_handleActionDown",
                    (void *) hooked_down, (void **) &orig_down)) n++;
    if (installHook("libcocos.so", "Java_com_cocos_lib_CocosTouchHandler_handleActionUp",
                    (void *) hooked_up, (void **) &orig_up)) n++;
    if (installHook("libcocos.so", "Java_com_cocos_lib_CocosTouchHandler_handleActionCancel",
                    (void *) hooked_cancel, (void **) &orig_cancel)) n++;
    if (installHook("libcocos.so", "Java_com_cocos_lib_CocosTouchHandler_handleActionMove",
                    (void *) hooked_move, (void **) &orig_move)) n++;

    LOGI("hooks installed: %d/7", n);
    return n;
}
