#include "hooks.h"
#include "cheats.h"
#include "menu.h"
#include "jni_env.h"
#include "log.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
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

// ==========================================================================
// ImGui / GLES
// ==========================================================================
namespace {

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

void renderFrame(EGLDisplay dpy, EGLSurface surf) {
    tryInitImGui();

    const unsigned long long f = g_frames.fetch_add(1) + 1;

    int w = 0, h = 0;
    if (eglQuerySurface(dpy, surf, EGL_WIDTH, &w) != EGL_TRUE) w = 0;
    if (eglQuerySurface(dpy, surf, EGL_HEIGHT, &h) != EGL_TRUE) h = 0;
    if (w <= 0 || h <= 0) MW_GetScreenSize(&w, &h);
    if (w <= 0 || h <= 0) { w = 1080; h = 1920; }

    if (!g_imguiReady) {
        // liveness marker: red square, bottom-left. Seeing it means the swap
        // hook fires and the problem is ImGui itself.
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
typedef jboolean (*TouchDownFn)(JNIEnv *, jclass, jint, jfloat, jfloat, jint);
typedef jboolean (*TouchMoveFn)(JNIEnv *, jclass, jint, jintArray, jfloatArray, jfloatArray);

TouchDownFn orig_down = nullptr;
TouchDownFn orig_up = nullptr;
TouchDownFn orig_cancel = nullptr;
TouchMoveFn orig_move = nullptr;

jboolean hooked_down(JNIEnv *env, jclass c, jint id, jfloat x, jfloat y, jint extra) {
    g_touchX.store(x); g_touchY.store(y); g_touchDown.store(1);
    if (shouldSwallow(x, y)) return JNI_TRUE;
    return orig_down ? orig_down(env, c, id, x, y, extra) : JNI_TRUE;
}

jboolean hooked_up(JNIEnv *env, jclass c, jint id, jfloat x, jfloat y, jint extra) {
    g_touchX.store(x); g_touchY.store(y); g_touchDown.store(0);
    if (shouldSwallow(x, y)) return JNI_TRUE;
    return orig_up ? orig_up(env, c, id, x, y, extra) : JNI_TRUE;
}

jboolean hooked_cancel(JNIEnv *env, jclass c, jint id, jfloat x, jfloat y, jint extra) {
    g_touchDown.store(0);
    if (shouldSwallow(x, y)) return JNI_TRUE;
    return orig_cancel ? orig_cancel(env, c, id, x, y, extra) : JNI_TRUE;
}

jboolean hooked_move(JNIEnv *env, jclass c, jint id, jintArray ids, jfloatArray xs, jfloatArray ys) {
    if (xs && ys) {
        if (env->GetArrayLength(xs) > 0 && env->GetArrayLength(ys) > 0) {
            float fx = 0, fy = 0;
            env->GetFloatArrayRegion(xs, 0, 1, &fx);
            env->GetFloatArrayRegion(ys, 0, 1, &fy);
            g_touchX.store(fx); g_touchY.store(fy); g_touchDown.store(1);
        }
    }
    if (shouldSwallow(g_touchX.load(), g_touchY.load())) return JNI_TRUE;
    return orig_move ? orig_move(env, c, id, ids, xs, ys) : JNI_TRUE;
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
    if (!logged) { logged = true; LOGI("hook engine: Dobby %s", DobbyGetVersion()); }

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
