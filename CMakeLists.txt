cmake_minimum_required(VERSION 3.18)
project(mwmenu C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

# ------------------------------------------------------------------ imgui
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
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

# ------------------------------------------------------------------- dobby
set(DOBBY_DIR ${TP}/Dobby)
if(NOT EXISTS ${DOBBY_DIR}/CMakeLists.txt)
  message(FATAL_ERROR "third_party/Dobby missing -- run tools/fetch_deps.sh")
endif()
set(DOBBY_DEBUG            OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE    OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST       OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver  ON  CACHE BOOL "" FORCE)
add_subdirectory(${DOBBY_DIR} ${CMAKE_BINARY_DIR}/dobby_build)
# we link dobby_static; the shared target pulls non-PIC relocations
set_target_properties(dobby PROPERTIES EXCLUDE_FROM_ALL TRUE)

# ------------------------------------------------------------------ module
add_library(mwmenu SHARED
  src/main.cpp
  src/hooks.cpp
  src/menu.cpp
  src/cheats.cpp
  src/jni_env.cpp
  src/signspoof.cpp           # TapSDK signature bypass
  src/touchbridge.cpp)        # touch input via the game JS layer

target_include_directories(mwmenu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mwmenu PRIVATE imgui dobby_static android log EGL GLESv3)

set_target_properties(mwmenu PROPERTIES
  OUTPUT_NAME "mwmenu"
  PREFIX "lib"
  LINK_FLAGS "-Wl,-z,max-page-size=16384 -Wl,--gc-sections")
