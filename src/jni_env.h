#pragma once
#include <jni.h>
#include <string>

void    MW_SetJavaVM(JavaVM *vm);
JavaVM *MW_GetJavaVM();
// Attach current thread if needed and return a usable JNIEnv (nullptr on failure).
JNIEnv *MW_GetEnv();

// Screen size from the Activity's DisplayMetrics (used when eglQuerySurface
// does not give us a usable size).
bool MW_GetScreenSize(int *w, int *h);

// Resolve com/cocos/lib/CocosJavascriptJavaBridge + its exported JNI evalString().
// Safe to call repeatedly.
bool MW_ResolveEngine();

// Execute JavaScript inside the running game. MUST be called from the Cocos
// script thread (the GL thread) -- we call it from the eglSwapBuffers hook.
bool MW_RunJS(const char *code);
inline bool MW_RunJS(const std::string &code) { return MW_RunJS(code.c_str()); }
