cmake_minimum_required(VERSION 3.18)
project(mwmenu C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

# target ABI -> shadowhook arch dir name
if(ANDROID_ABI STREQUAL "arm64-v8a")
  set(MW_ARCH "arm64")
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
  set(MW_ARCH "arm")
else()
  message(FATAL_ERROR "unsupported ANDROID_ABI: ${ANDROID_ABI}")
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ------------------------------------------------------------------ imgui
set(IMGUI_DIR ${TP}/imgui)
if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
  message(FATAL_ERROR "third_party/imgui missing -- run tools/fetch_deps.sh")
endif()
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PRIVATE IMGUI_IMPL_OPENGL_ES3)

# -------------------------------------------------------------- shadowhook
set(SH_DIR ${TP}/shadowhook/shadowhook/src/main/cpp)
if(NOT EXISTS ${SH_DIR}/shadowhook.c)
  message(FATAL_ERROR "third_party/shadowhook missing -- run tools/fetch_deps.sh")
endif()
file(GLOB SH_SRC
  ${SH_DIR}/*.c
  ${SH_DIR}/arch/${MW_ARCH}/*.c
  ${SH_DIR}/arch/${MW_ARCH}/*.S
  ${SH_DIR}/common/*.c
  ${SH_DIR}/third_party/*/*.c)
# sh_jni.c defines JNI_OnLoad (conflicts); nothing/ is the optional shared-mode proxy lib
list(FILTER SH_SRC EXCLUDE REGEX "sh_jni\\.c$")
list(FILTER SH_SRC EXCLUDE REGEX "/nothing/")
add_library(shadowhook STATIC ${SH_SRC})
target_compile_features(shadowhook PRIVATE c_std_11)
target_include_directories(shadowhook PUBLIC ${SH_DIR}/include)
target_include_directories(shadowhook PRIVATE
  ${SH_DIR} ${SH_DIR}/arch/${MW_ARCH} ${SH_DIR}/common
  ${SH_DIR}/third_party/xdl ${SH_DIR}/third_party/bsd ${SH_DIR}/third_party/lss)
target_link_libraries(shadowhook PUBLIC log)

# ------------------------------------------------------------------ module
add_library(mwmenu SHARED
  src/main.cpp
  src/hooks.cpp
  src/menu.cpp
  src/cheats.cpp
  src/jni_env.cpp)

target_include_directories(mwmenu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mwmenu PRIVATE imgui shadowhook android log EGL GLESv3)

set_target_properties(mwmenu PROPERTIES
  OUTPUT_NAME "mwmenu"
  PREFIX "lib"
  LINK_FLAGS "-Wl,-z,max-page-size=16384 -Wl,--gc-sections")
