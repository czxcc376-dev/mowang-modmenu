#pragma once

// Touch input sourced from the game's own JS layer (see touchbridge.cpp).
// Returns true once the JS side has been asked to start mirroring touches.
bool TouchBridge_Install();

// Reads the latest touch sample.  Values are only updated when the file changed,
// so callers can poll every frame and keep the previous values otherwise.
void TouchBridge_Poll(float *x, float *y, int *down);
