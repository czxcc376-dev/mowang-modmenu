#pragma once
#include <atomic>

// ------------------------------------------------------------------ state
struct CheatState {
    std::atomic<bool> menuVisible{true};
    std::atomic<bool> god{false};         // CharaDataManager.setGodMode + setInvulnerable
    std::atomic<bool> lockHp{false};      // re-write _hp = maxHp
    std::atomic<bool> oneHit{false};      // attack damage = huge
    std::atomic<bool> critMax{false};     // crit rate = 100%
    std::atomic<bool> autoAttack{false};  // GameManager._isAutoAttack / _isAutoAim
    std::atomic<bool> dmgMulOn{false};
    std::atomic<int>  dmgMul{10};         // damage = maxHp * dmgMul
    std::atomic<bool> speedMulOn{false};
    std::atomic<int>  speedMul{3};        // move speed * speedMul
    std::atomic<int>  frames{0};
    std::atomic<bool> bootstrapped{false};
};

extern CheatState g_cheat;

// Runs a raw JS snippet with the __MW bootstrap prepended (GL/script thread only).
bool Cheats_RawJS(const char *js);

// Called once per frame from the eglSwapBuffers hook (GL/script thread).
void Cheats_Tick();

// Actions (safe to call from the GL thread while drawing the menu).
void Cheats_ApplyGod(bool on);
void Cheats_ApplyLockHp(bool on);
void Cheats_ApplyOneHit(bool on);
void Cheats_ApplyCritMax(bool on);
void Cheats_ApplyAutoAttack(bool on);
void Cheats_ApplyDamageMul(bool on, int mul);
void Cheats_ApplySpeedMul(bool on, int mul);
void Cheats_AddMoney(long long delta);
void Cheats_DumpState();
void Cheats_Reset();
