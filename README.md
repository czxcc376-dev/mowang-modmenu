# MoWang Mod Menu — 《我不是魔王》Cocos 3.7.3 (V8) 外挂菜单

C++ / ImGui 原生菜单，打包成单个 `.so`（`libmwmenu.so`），由 GitHub Actions 云端交叉编译。

| 项 | 值 |
|---|---|
| 目标 | `com.niaowustudio.imnodemonlord.green` (`com.cocos.game.AppActivity`) |
| 引擎 | Cocos Creator **3.7.3**，脚本引擎 **V8**，游戏逻辑为**明文 JS**（`assets/assets/main/index.js`，15.2 MB / 708 模块） |
| 主库 | `lib/<abi>/libcocos.so`（未剥离导出符号） |
| 注入方式 | 把 `libmwmenu.so` 放进 APK 的 `lib/<abi>/`，并给 `libcocos.so` 加 `DT_NEEDED libmwmenu.so`（`patchelf --add-needed`），重打包+重签名 |
| UI | ImGui（`imgui_impl_opengl3`）直接画在游戏 GL 帧上 |
| Hook | Dobby（jmpews/Dobby，静态链接）内联挂钩 |

## 工作原理

```
libcocos.so 启动
   └─ dlopen(libmwmenu.so)          ← DT_NEEDED，无需 root / 注入器
        ├─ constructor → 工作线程
        │     ├─ JNI_GetCreatedJavaVMs()  取 JavaVM
        │     ├─ dlopen(RTLD_NOLOAD) libcocos.so / libEGL.so
        │     ├─ shadowhook_init(UNIQUE)
        │     └─ 挂钩 5 个函数
        │           libEGL!eglSwapBuffers                    → 每帧渲染 ImGui
        │           libcocos!…CocosTouchHandler_handleActionDown/Up/Cancel/Move → 触摸输入
        └─ 菜单按钮 → CocosJavascriptJavaBridge.evalString(js)
                          └─ ScriptEngine::evalString  ← 在 GL/脚本线程执行
```

* **渲染**：`eglSwapBuffers` 钩子在游戏 swap 之前把 ImGui 画进同一个 framebuffer。
* **输入**：Cocos 的触摸 JNI 函数被内联 hook，坐标写入原子变量，GL 线程消费；触点落在菜单矩形内时**吞掉事件**，不传给游戏。
* **作弊**：通过 `evalString` 在脚本线程注入 JS，直接操作运行时对象。
  `se::ScriptEngine` 本身符号隐藏，所以走导出的 JNI `Java_com_cocos_lib_CocosJavascriptJavaBridge_evalString`（vaddr `0x39bbd8`，arm64）。

## Hook 目标（arm64 vaddr，来自 libcocos.so 动态符号表）

| 符号 | vaddr | Java 签名 |
|---|---|---|
| `Java_com_cocos_lib_CocosTouchHandler_handleActionDown` | `0x250544` | `(int pointerId, float x, float y, int extra)` |
| `Java_com_cocos_lib_CocosTouchHandler_handleActionUp` | `0x250690` | 同上 |
| `Java_com_cocos_lib_CocosTouchHandler_handleActionCancel` | `0x250994` | 同上 |
| `Java_com_cocos_lib_CocosTouchHandler_handleActionMove` | `0x2507e0` | `(int pointerId, int[] ids, float[] xs, float[] ys)` |
| `Java_com_cocos_lib_CocosJavascriptJavaBridge_evalString` | `0x39bbd8` | `(JNIEnv*, jclass, jstring)` |

## 作弊项 ↔ 游戏内数据

单例：`System.import('chunks:///_virtual/<模块>.ts') → <类>.Instance`（`Singleton` 静态 getter）。

| 菜单项 | 游戏接口 |
|---|---|
| 上帝模式 | `CharaDataManager.setGodMode(true)` + `setInvulnerable(true)` |
| 锁血 | `CharaData.forceSetHp(CharaDataManager.getMaxHp(i))` |
| 一击必杀 / 伤害倍率 | `CharaData._attackDamage.setValue(maxHp * M)` |
| 满暴击 | `CharaData._critRate_int1000 = 1000`, `_critDamageRate_int1000 = 10000` |
| 移速倍率 | `CharaData._moveSpeed.setValue(base * M)` |
| 自动攻击/瞄准 | `GameManager._isAutoAttack / _isAutoAim` |
| 加金币 | `CharaDataManager.changeMoney / addGainMoney / setCurDayMoney` |
| 打印状态 | `_hp/_shield/_mp/_attackDamage/_moveSpeed/_critRate_int1000/getCurMoney` |

## 云端构建

```bash
# 本地无需 NDK；推送到 GitHub 后自动构建
git push
```

`.github/workflows/build.yml`：
* matrix 编译 `arm64-v8a` / `armeabi-v7a`，NDK **r27d**，`ANDROID_PLATFORM=android-24`
* `tools/fetch_deps.sh` 拉取 pinned 依赖（imgui v1.91.0，ShadowHook `593f491`）
* 产物 `libmwmenu-<abi>/libmwmenu.so`
* 可选 job `repackage APK`：`workflow_dispatch` 时填 `apk_url`，自动注入 + `zipalign -p 4` + `apksigner` 签名，产出可直接安装的 `mowang-modded.apk`

本地手工构建：

```bash
export ANDROID_NDK=$HOME/android-ndk-r27d
bash tools/fetch_deps.sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DANDROID_ARM_MODE=arm \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 手工注入（无 CI）

```bash
python3 tools/mkinject.py 我不是魔王_0.9.0.2.apk \
  --so build/libmwmenu.so --abi arm64-v8a --out patched.apk
zipalign -p -f 4 patched.apk aligned.apk
apksigner sign --ks ks.jks --out mowang-modded.apk aligned.apk
adb install -r mowang-modded.apk
```

> 若 `libcocos.so` 是 `extractNativeLibs=false`（lib 未压缩），`zipalign -p 4` 必须执行，否则链接器读不到新加的 `.so`。

## 日志

```bash
adb logcat -s MWMENU
```
启动阶段应看到：`libmwmenu.so loaded (ctor)` → `shadowhook ready` → `hooks installed: 5/5` → `ImGui ready`。

## 目录

```
src/main.cpp      构造器 + 工作线程
src/hooks.cpp     eglSwapBuffers / 触摸 JNI 挂钩 + ImGui 渲染
src/menu.cpp      ImGui 菜单 UI（C++）
src/cheats.cpp    作弊实现（JS 注入）
src/jni_env.cpp   JavaVM / JNIEnv / evalString 桥
src/log.h
tools/fetch_deps.sh
tools/mkinject.py
.github/workflows/build.yml
```

## 备注 / 已知限制

* 目前只挂 `handleAction*` 的 4 个入口，多点触控只取数组首元素（单指拖动够用）。
* `ImGui_ImplOpenGL3` 以 `IMGUI_IMPL_OPENGL_ES3` 编译；若设备回落到 GLES2 上下文，菜单不渲染（游戏本体在 3.x 设备上默认 ES3）。
* 中文字体依次尝试 `NotoSansCJK-Regular.ttc` / `NotoSansCJKsc*` / `DroidSansFallbackFull.ttf`，缺失时回落 ImGui 默认字体。
* 依赖 Dobby（Apache-2.0）、Dear ImGui（MIT）。

> 注：`libcocos.so` **不导入** `eglSwapBuffers`，它经 `eglGetProcAddress` 取指针且优先用 `eglSwapBuffersWithDamageKHR`，所以三个 swap 入口都要钩。
> 另：ShadowHook 在本机（Android 11 / OPPO PDVM00）`shadowhook_init` 报 `Init linker mod failed`(12)，钩子全废，故改用 Dobby。
