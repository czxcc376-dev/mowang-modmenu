#pragma once

// Runs fn(arg) with a SIGSEGV/SIGBUS guard installed on the calling thread.
// Returns false if the call faulted; the process survives either way.
//
// Needed because libcocos' ScriptEngine::evalString() dereferences the engine
// singleton unconditionally -- calling it before the engine exists is fatal.
bool JSGuard_Call(void (*fn)(void *), void *arg);
