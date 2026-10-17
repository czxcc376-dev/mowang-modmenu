#include "menu.h"
#include "cheats.h"
#include "hooks.h"
#include "log.h"

#include <imgui.h>
#include <cstdio>

namespace {

struct Style {
    ImVec4 accent = ImVec4(0.10f, 0.75f, 0.60f, 1.00f);
    ImVec4 warn   = ImVec4(1.00f, 0.45f, 0.30f, 1.00f);
};
Style st;

void section(const char *title) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(st.accent, "%s", title);
    ImGui::Spacing();
}

bool toggle(const char *label, std::atomic<bool> &v, const char *hint) {
    bool b = v.load();
    if (ImGui::Checkbox(label, &b)) {
        v.store(b);
        return true;
    }
    if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
    return false;
}

// Diagnostic: verifies the JS module registry / singleton path at runtime.
void selfTest() {
    Cheats_RawJS(
        "__MW.log('[MW] self-test: System=' + (typeof System) + ' cclegacy=' + (typeof cclegacy)"
        " + ' cc=' + (typeof cc) + ' __MW=' + (typeof __MW));"
        "__MW.mod('CharaDataManager').then(function(m){"
        "  __MW.log('[MW] CharaDataManager module ok, keys=' + Object.keys(m).join(','));"
        "  var M = m.CharaDataManager ? m.CharaDataManager.Instance : null;"
        "  __MW.log('[MW] instance=' + !!M);"
        "}).catch(function(e){ __MW.log('[MW] import FAILED: ' + e); });"
        "__MW.mod('GameManager').then(function(m){ __MW.log('[MW] GameManager keys=' + Object.keys(m).join(',')); })"
        ".catch(function(e){ __MW.log('[MW] GameManager import FAILED: ' + e); });");
}

} // namespace

void Menu_Draw() {
    const ImGuiWindowFlags base =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;

    // ------------------------------------------------- floating launcher
    if (!g_cheat.menuVisible.load()) {
        ImGui::SetNextWindowPos(ImVec2(24, 180), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("##mw_launcher", nullptr,
                         base | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings)) {
            if (ImGui::Button("MW MOD", ImVec2(96, 44))) g_cheat.menuVisible.store(true);
        }
        ImVec2 p = ImGui::GetWindowPos();
        ImVec2 s = ImGui::GetWindowSize();
        Input_SetMenuRect(p.x, p.y, s.x, s.y);
        ImGui::End();
        return;
    }

    // ------------------------------------------------------------- panel
    ImGui::SetNextWindowSize(ImVec2(470, 700), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.90f);
    bool open = true;
    if (ImGui::Begin("MoWang Mod v2  (Cocos 3.7.3 / V8)", &open,
                     base | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(st.accent, "com.niaowustudio.imnodemonlord.green");
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) g_cheat.menuVisible.store(false);

        section("登录 / 授权 (Login)");
        toggle("签名校验绕过  Bypass Signature", g_cheat.bypassSign,
               "Redirects TapSDK CommonUtils.getMD5 so the server sees the original\n"
               "certificate digest (921ee83d...). Default: ON.");
        ImGui::TextDisabled("上报摘要: 921ee83de604256b2e27e624d6cfd635");

        section("战斗 (Battle)");
        if (toggle("上帝模式  God Mode", g_cheat.god, "CharaDataManager.setGodMode(true) + setInvulnerable(true)"))
            Cheats_ApplyGod(g_cheat.god.load());
        if (toggle("锁血  Lock HP", g_cheat.lockHp, "每 60 帧 _hp = getMaxHp(i)"))
            Cheats_ApplyLockHp(g_cheat.lockHp.load());
        if (toggle("一击必杀  One Hit", g_cheat.oneHit, "attackDamage = 999999999"))
            Cheats_ApplyOneHit(g_cheat.oneHit.load());
        if (toggle("满暴击  Full Crit", g_cheat.critMax, "critRate=1000/1000, critDamage=x10"))
            Cheats_ApplyCritMax(g_cheat.critMax.load());
        if (toggle("自动攻击/瞄准  Auto Attack", g_cheat.autoAttack, "GameManager._isAutoAttack / _isAutoAim"))
            Cheats_ApplyAutoAttack(g_cheat.autoAttack.load());

        section("数值 (Stats)");
        if (toggle("伤害倍率  Damage x M", g_cheat.dmgMulOn, "attackDamage = maxHp * M"))
            Cheats_ApplyDamageMul(g_cheat.dmgMulOn.load(), g_cheat.dmgMul.load());
        int dm = g_cheat.dmgMul.load();
        if (ImGui::SliderInt("伤害倍率 M", &dm, 1, 200)) {
            g_cheat.dmgMul.store(dm);
            if (g_cheat.dmgMulOn.load()) Cheats_ApplyDamageMul(true, dm);
        }
        if (toggle("移速倍率  Speed x M", g_cheat.speedMulOn, "moveSpeed = base * M"))
            Cheats_ApplySpeedMul(g_cheat.speedMulOn.load(), g_cheat.speedMul.load());
        int sm = g_cheat.speedMul.load();
        if (ImGui::SliderInt("移速倍率 M", &sm, 1, 10)) {
            g_cheat.speedMul.store(sm);
            if (g_cheat.speedMulOn.load()) Cheats_ApplySpeedMul(true, sm);
        }

        section("经济 (Money)");
        if (ImGui::Button("金币 +1,000"))     Cheats_AddMoney(1000);
        ImGui::SameLine();
        if (ImGui::Button("金币 +100,000"))   Cheats_AddMoney(100000);
        ImGui::SameLine();
        if (ImGui::Button("金币 +1,000,000")) Cheats_AddMoney(1000000);

        section("工具 (Tools)");
        if (ImGui::Button("自检 Self Test"))  selfTest();
        ImGui::SameLine();
        if (ImGui::Button("打印状态 Log"))    Cheats_DumpState();
        ImGui::SameLine();
        if (ImGui::Button("还原 Reset"))      Cheats_Reset();

        ImGui::Spacing();
        ImGui::TextDisabled("logcat -s MWMENU");
    }
    ImVec2 p = ImGui::GetWindowPos();
    ImVec2 s = ImGui::GetWindowSize();
    Input_SetMenuRect(p.x, p.y, s.x, s.y);
    ImGui::End();

    if (!open) g_cheat.menuVisible.store(false);
}
