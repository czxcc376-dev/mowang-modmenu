#include "hooks.h"
#include "cheats.h"
#include "menu.h"
#include "jni_env.h"
#include "log.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <shadowhook.h>

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
// ImGui / GLES renderer
// ==========================================================================
namespace {

bool      g_imguiReady = false;
std::atomic<int> g_menuOpen{1};

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
    s.WindowRounding   = 10.0f;
    s.FrameRounding    = 6.0f;
    s.GrabRounding     = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.FramePadding     = ImVec2(14, 10);
    s.ItemSpacing      = ImVec2(10, 12);
    s.ScrollbarSize    = 26.0f;
    s.GrabMinSize      = 22.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]   = ImVec4(0.06f, 0.07f, 0.09f, 0.94f);
    c[ImGuiCol_TitleBg]    = ImVec4(0.10f, 0.75f, 0.60f, 0.85f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.75f, 0.60f, 1.00f);
    c[ImGuiCol_CheckMark]  = ImVec4(0.10f, 0.90f, 0.70f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.10f, 0.85f, 0.68f, 1.00f);
    c[ImGuiCol_Button]     = ImVec4(0.16f, 0.20f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.55f, 0.46f, 1.00f);
}

void initImGui() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;

    applyStyle();

    ImFont *font = nullptr;
    for (const char *p : kFonts) {
        if (access(p, R_OK) != 0) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        cfg.PixelSnapH  = true;
        font = io.Fonts->AddFontFromFileTTF(p, 30.0f, &cfg,
                                            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (font) { LOGI("font loaded: %s", p); break; }
    }
    if (!font) {
        io.Fonts->AddFontDefault();
        LOGW("no CJK font found, falling back to default");
    }

    const char *glv = (const char *) glGetString(GL_VERSION);
    const char *glsl = (glv && strstr(glv, "OpenGL ES 3")) ? "#version 300 es" : "#version 100";
    LOGI("GL_VERSION=%s -> %s", glv ? glv : "?", glsl);

    if (!ImGui_ImplOpenGL3_Init(glsl)) {
        LOGE("ImGui_ImplOpenGL3_Init failed");
        return;
    }
    g_imguiReady = true;
    LOGI("ImGui ready");
}

// ---------------------------------------------------------- eglSwapBuffers
typedef EGLBoolean (*EglSwapBuffersFn)(EGLDisplay, EGLSurface);
EglSwapBuffersFn orig_swap = nullptr;

EGLBoolean hooked_swap(EGLDisplay dpy, EGLSurface surf) {
    static std::atomic<int> inited{0};
    if (inited.load() == 0) {
        if (inited.exchange(1) == 0) initImGui();
    }

    if (g_imguiReady && !orig_swap) {
        // nothing
    }

    if (g_imguiReady) {
        int w = 0, h = 0;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        if (w > 0 && h > 0) {
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
        }
    }

    return orig_swap ? orig_swap(dpy, surf) : EGL_TRUE;
}

// ------------------------------------------------- Cocos JNI touch handlers
typedef jboolean (*TouchDownFn)(JNIEnv *, jclass, jint, jfloat, jfloat, jint);
typedef jboolean (*TouchMoveFn)(JNIEnv *, jclass, jint, jintArray, jfloatArray, jfloatArray);

TouchDownFn orig_down = nullptr;
TouchDownFn orig_up = nullptr;
TouchMoveFn orig_move = nullptr;
TouchDownFn orig_cancel = nullptr;

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
        jsize n = env->GetArrayLength(xs);
        jsize n2 = env->GetArrayLength(ys);
        if (n > 0 && n2 > 0) {
            float fx = 0, fy = 0;
            env->GetFloatArrayRegion(xs, 0, 1, &fx);
            env->GetFloatArrayRegion(ys, 0, 1, &fy);
            g_touchX.store(fx); g_touchY.store(fy); g_touchDown.store(1);
        }
    }
    if (shouldSwallow(g_touchX.load(), g_touchY.load())) return JNI_TRUE;
    return orig_move ? orig_move(env, c, id, ids, xs, ys) : JNI_TRUE;
}

} // namespace

// ==========================================================================
int Hooks_Install() {
    int n = 0;

    int rc = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    if (rc != 0) {
        LOGE("shadowhook_init failed: %d (%s)", rc, shadowhook_to_errmsg(rc));
    } else {
        LOGI("shadowhook %s ready", shadowhook_get_version());
    }

    void *stub;

    stub = shadowhook_hook_sym_name("libEGL.so", "eglSwapBuffers",
                                    (void *) hooked_swap, (void **) &orig_swap);
    if (stub) { n++; LOGI("hooked eglSwapBuffers -> orig=%p", (void *) orig_swap); }
    else LOGE("hook eglSwapBuffers failed: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    stub = shadowhook_hook_sym_name("libcocos.so",
                                    "Java_com_cocos_lib_CocosTouchHandler_handleActionDown",
                                    (void *) hooked_down, (void **) &orig_down);
    if (stub) { n++; LOGI("hooked handleActionDown"); }
    else LOGE("hook handleActionDown failed: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    stub = shadowhook_hook_sym_name("libcocos.so",
                                    "Java_com_cocos_lib_CocosTouchHandler_handleActionUp",
                                    (void *) hooked_up, (void **) &orig_up);
    if (stub) { n++; LOGI("hooked handleActionUp"); }
    else LOGE("hook handleActionUp failed: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    stub = shadowhook_hook_sym_name("libcocos.so",
                                    "Java_com_cocos_lib_CocosTouchHandler_handleActionCancel",
                                    (void *) hooked_cancel, (void **) &orig_cancel);
    if (stub) { n++; LOGI("hooked handleActionCancel"); }
    else LOGE("hook handleActionCancel failed: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    stub = shadowhook_hook_sym_name("libcocos.so",
                                    "Java_com_cocos_lib_CocosTouchHandler_handleActionMove",
                                    (void *) hooked_move, (void **) &orig_move);
    if (stub) { n++; LOGI("hooked handleActionMove"); }
    else LOGE("hook handleActionMove failed: %s", shadowhook_to_errmsg(shadowhook_get_errno()));

    LOGI("hooks installed: %d/5", n);
    return n;
}
