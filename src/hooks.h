#pragma once

// Install shadowhook hooks (eglSwapBuffers + Cocos JNI touch handlers).
// Returns number of successfully installed hooks.
int Hooks_Install();

// ---- input plumbing (touch hooks -> ImGui, thread safe) -------------------
void Input_SetMenuRect(float x, float y, float w, float h);
void Input_GetMenuRect(float *x, float *y, float *w, float *h);
