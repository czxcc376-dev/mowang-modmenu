#include "cheats.h"
#include "jni_env.h"
#include "log.h"

#include <string>

CheatState g_cheat;

namespace {

// ---------------------------------------------------------------- bootstrap
// Game: Cocos Creator 3.7.3, JS bundle  assets/assets/main/index.js
// Modules are SystemJS chunk registrations: chunks:///_virtual/<Name>.ts
// Managers are singletons: <Class>.Instance (from Singleton.ts static getter).
const char *kBootstrap = R"JS(
(function(){
  var g = (typeof globalThis !== 'undefined') ? globalThis : this;
  if (g.__MW && g.__MW.v >= 2) { return; }
  var MW = { v: 2, cache: {} };
  MW.mod = function(name){
    if (MW.cache[name]) { return Promise.resolve(MW.cache[name]); }
    return System.import('chunks:///_virtual/' + name + '.ts').then(function(m){ MW.cache[name] = m; return m; });
  };
  MW.inst = function(name, cls){
    return MW.mod(name).then(function(m){ var C = m[cls || name]; return C ? C.Instance : null; });
  };
  MW.set = function(o, v){
    try { if (o && typeof o.setValue === 'function') { o.setValue(v); return true; } } catch(e){}
    return false;
  };
  MW.get = function(o, d){
    try { if (o && typeof o.getValue === 'function') { return o.getValue(); } } catch(e){}
    return d;
  };
  MW.chara = function(fn){
    return MW.inst('CharaDataManager').then(function(M){
      if (!M) { return false; }
      var n = 1;
      try { if (typeof M.getPlayerNum === 'function') { n = M.getPlayerNum(); } } catch(e){}
      if (!n || n < 1) { n = 1; }
      for (var i = 0; i <= n && i < 4; i++) {
        try { var d = M.getCharaData(i); if (d) { fn(M, d, i); } } catch(e){}
      }
      return true;
    });
  };
  MW.log = function(){ try { console.log.apply(console, arguments); } catch(e){} };
  g.__MW = MW;
  MW.log('[MW] bootstrap v2 ok');
})();
)JS";

std::string num(long long v) { return std::to_string(v); }

} // namespace

bool Cheats_RawJS(const char *js) {
    if (!js || !*js) return false;
    g_cheat.wantJS.store(true);          // engine is certainly up if we got here
    if (!g_cheat.bootstrapped.load()) {
        if (!MW_RunJS(kBootstrap)) return false;
        g_cheat.bootstrapped.store(true);
        LOGI("JS bootstrap injected (lazy)");
    }
    return MW_RunJS(js);
}

// ------------------------------------------------------------------ actions
void Cheats_ApplyGod(bool on) {
    std::string js =
        "Promise.all([__MW.inst('CharaDataManager')]).then(function(a){"
        "var M=a[0]; if(!M){return;} try{M.setGodMode(" + std::string(on ? "true" : "false") + ");}catch(e){}"
        "try{M.setInvulnerable(" + std::string(on ? "true" : "false") + ");}catch(e){}"
        "});";
    Cheats_RawJS(js.c_str());
}

void Cheats_ApplyLockHp(bool on) {
    if (!on) return;  // nothing to undo; letting HP regenerate is enough
    Cheats_RawJS("__MW.chara(function(M,d,i){ try{ if(typeof d.isDead==='function' && d.isDead()) return;"
                 " d.forceSetHp(M.getMaxHp(i)); }catch(e){} });");
}

void Cheats_ApplyOneHit(bool on) {
    if (on) {
        Cheats_RawJS("__MW.chara(function(M,d,i){ __MW.set(d._attackDamage, 999999999); });");
    } else {
        Cheats_Reset();
    }
}

void Cheats_ApplyCritMax(bool on) {
    if (on) {
        Cheats_RawJS("__MW.chara(function(M,d,i){ __MW.set(d._critRate_int1000, 1000);"
                     " __MW.set(d._critDamageRate_int1000, 10000); });");
    }
}

void Cheats_ApplyAutoAttack(bool on) {
    std::string js =
        "Promise.all([__MW.inst('GameManager')]).then(function(a){"
        "var G=a[0]; if(!G){return;} try{G._isAutoAttack=" + std::string(on ? "true" : "false") + ";}catch(e){}"
        "try{G._isAutoAim=" + std::string(on ? "true" : "false") + ";}catch(e){}"
        "});";
    Cheats_RawJS(js.c_str());
}

void Cheats_ApplyDamageMul(bool on, int mul) {
    if (!on) return;
    std::string js = "__MW.chara(function(M,d,i){ var base=M.getMaxHp(i); __MW.set(d._attackDamage, base*(" + num(mul) + ")); });";
    Cheats_RawJS(js.c_str());
}

void Cheats_ApplySpeedMul(bool on, int mul) {
    if (!on) return;
    std::string js = "__MW.chara(function(M,d,i){"
                     " if(!d.__mwBaseSpeed){ d.__mwBaseSpeed = __MW.get(d._moveSpeed, 0); }"
                     " __MW.set(d._moveSpeed, (d.__mwBaseSpeed||0)*(" + num(mul) + ")); });";
    Cheats_RawJS(js.c_str());
}

void Cheats_AddMoney(long long delta) {
    std::string d = num(delta);
    std::string js =
        "__MW.inst('CharaDataManager').then(function(M){ if(!M){return;} var ok=false;"
        " try{ if(typeof M.changeMoney==='function'){ M.changeMoney(" + d + "); ok=true; } }catch(e){}"
        " if(!ok){ try{ if(typeof M.addGainMoney==='function'){ M.addGainMoney(" + d + "); ok=true; } }catch(e){} }"
        " if(!ok){ try{ if(typeof M.setCurDayMoney==='function'){ M.setCurDayMoney(__MW.get(M.getCurDayMoney&&M.getCurDayMoney(),0)+" + d + "); ok=true; } }catch(e){} }"
        " __MW.log('[MW] money delta=" + d + " ok='+ok); });";
    Cheats_RawJS(js.c_str());
}

void Cheats_DumpState() {
    Cheats_RawJS(
        "__MW.chara(function(M,d,i){ __MW.log('[MW] chara#'+i+' hp='+__MW.get(d._hp)+'/'+M.getMaxHp(i)"
        "+' shield='+__MW.get(d._shield)+' mp='+__MW.get(d._mp)"
        "+' atk='+__MW.get(d._attackDamage)+' spd='+__MW.get(d._moveSpeed)"
        "+' crit='+__MW.get(d._critRate_int1000)+' dead='+(typeof d.isDead==='function'?d.isDead():'?')); });"
        "__MW.inst('CharaDataManager').then(function(M){ if(!M){return;} try{ __MW.log('[MW] god='+M.getGodMode()"
        "+' invul='+M.getIsInvulnerable()+' money='+__MW.get(M.getCurMoney&&M.getCurMoney(),'?')); }catch(e){} });");
}

void Cheats_Reset() {
    Cheats_RawJS("__MW.chara(function(M,d,i){ try{ M.resetCommonBattleData && 0; }catch(e){} });");
    // restore stats from the game's own calculators
    Cheats_RawJS("__MW.chara(function(M,d,i){ try{ if(typeof M._calAttackDamage==='function'){ M._calAttackDamage(i); } }catch(e){} });");
}

// ------------------------------------------------------------------- ticker
void Cheats_Tick() {
    int f = g_cheat.frames.fetch_add(1) + 1;

    // Bootstrap lazily and late.  evalString() dereferences the V8 engine
    // singleton without a null check, so a frame-1 injection crashes the game
    // before the engine exists.  Waiting for the menu to be interacted with
    // guarantees the engine is well past initialisation.
    if (!g_cheat.bootstrapped.load() && g_cheat.wantJS.load()) {
        if (MW_RunJS(kBootstrap)) {
            g_cheat.bootstrapped.store(true);
            LOGI("JS bootstrap injected");
        }
    }

    if (f % 240 == 0) {
        if (g_cheat.god.load())        Cheats_ApplyGod(true);
        if (g_cheat.autoAttack.load()) Cheats_ApplyAutoAttack(true);
    }
    if (f % 60 == 0) {
        if (g_cheat.lockHp.load())     Cheats_ApplyLockHp(true);
        if (g_cheat.oneHit.load())     Cheats_ApplyOneHit(true);
        if (g_cheat.critMax.load())    Cheats_ApplyCritMax(true);
    }
    if (f % 90 == 0) {
        if (g_cheat.dmgMulOn.load())   Cheats_ApplyDamageMul(true, g_cheat.dmgMul.load());
        if (g_cheat.speedMulOn.load()) Cheats_ApplySpeedMul(true, g_cheat.speedMul.load());
    }
}
