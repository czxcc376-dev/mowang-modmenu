#pragma once
#include <jni.h>
#include <string>

void    MW_SetJavaVM(JavaVM *vm);
JavaVM *MW_GetJavaVM();
// Attach current thread if needed and return a usable JNIEnv (nullptr on failure).
JNIEnv *MW_GetEnv();

// Finds a class using the *application* ClassLoader.
// FindClass() from an attached native thread uses the system loader, which cannot
// see the game's classes -- this goes through ActivityThread.currentApplication()
// instead, and caches the loader.
jclass MW_FindAppClass(JNIEnv *env, const char *name);

// Screen size from the Activity's DisplayMetrics.
bool MW_GetScreenSize(int *w, int *h);

// Resolve com/cocos/lib/CocosJavascriptJavaBridge + its exported JNI evalString().
bool MW_ResolveEngine();

// Execute JavaScript inside the running game (GL/script thread only).
bool MW_RunJS(const char *code);
inline bool MW_RunJS(const std::string &code) { return MW_RunJS(code.c_str()); }

// Drains CocosHelper.sForegroundTaskQOnGameThread.
// CocosTouchHandler.onTouchEvent() does not call handleAction*() directly: it
// posts a Runnable through CocosHelper.runOnGameThreadAtForeground(), and only
// libcocos' own native per-frame callback flushes that queue.  Flushing it from
// our swap hook makes the touch events actually reach the JNI entry points.
void MW_FlushCocosTasks();
