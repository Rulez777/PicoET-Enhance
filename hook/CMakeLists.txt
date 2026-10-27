cmake_minimum_required(VERSION 3.22)
project(picoet_hook C CXX ASM)

# ---- ShadowHook 2.0.1 (vendored source, static-in-object form) --------------
# Vendored upstream source (see third_party/shadowhook-2.0.1/VENDOR.txt). The
# Local integration patches are documented in VENDOR.txt: no linker monitor
# and no ELF-gap allocation (islands come from owned anonymous mappings).
#
# Built as an OBJECT library (not an archive) on purpose: ShadowHook's errno
# module is initialised in its own `__attribute__((constructor))` and our
# constructor calls shadowhook_init(). Listing the ShadowHook objects before
# ours makes sh_errno_ctor enter .init_array first, so the TLS errno key exists
# by the time we init. (Verified offline from .init_array order.)
set(SH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/shadowhook-2.0.1/cpp)
file(GLOB SH_SRC
    ${SH_DIR}/*.c
    ${SH_DIR}/arch/arm64/*.c
    ${SH_DIR}/arch/arm64/*.S
    ${SH_DIR}/common/*.c
    ${SH_DIR}/third_party/*/*.c)
list(FILTER SH_SRC EXCLUDE REGEX ".*/sh_jni\\.c$")   # no Java API; defines JNI_OnLoad

add_library(shadowhook_obj OBJECT ${SH_SRC})
set_target_properties(shadowhook_obj PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(shadowhook_obj PRIVATE
    PICOET_SHADOWHOOK_NO_LINKER_MONITOR PICOET_SHADOWHOOK_NO_ELF_GAP)
target_compile_options(shadowhook_obj PRIVATE -Os -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -w)
target_include_directories(shadowhook_obj PRIVATE
    ${SH_DIR} ${SH_DIR}/arch/arm64 ${SH_DIR}/common
    ${SH_DIR}/third_party/xdl ${SH_DIR}/third_party/bsd ${SH_DIR}/third_party/lss
    PUBLIC ${SH_DIR}/include)

# ---- our payload object (placed after ShadowHook for ctor ordering) ---------
# Compiled as C++17: the high-rate path reuses the verified source-dt and
# effective-observation-time code and needs exceptions. -fno-omit-frame-pointer is
# required because merge_proxy reads its caller via __builtin_return_address;
# -ffp-contract=off keeps the reconstructed stock arithmetic exact.
set_source_files_properties(src/picoet_hook.c PROPERTIES LANGUAGE CXX)
add_library(picoet_obj OBJECT
    src/picoet_hook.c
    src/expression/high_rate.cpp
    src/expression/integration.cpp
    src/expression/mouth.cpp
    src/expression/numerical.cpp
    src/expression/state.cpp)
set_target_properties(picoet_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(picoet_obj PRIVATE
    -std=c++17 -O2 -fPIC -fexceptions -fvisibility=hidden -Wall -Wextra
    -fno-omit-frame-pointer -ffp-contract=off)
target_include_directories(picoet_obj PRIVATE ${SH_DIR}/include)

add_library(picoet_hook SHARED
    $<TARGET_OBJECTS:shadowhook_obj>
    $<TARGET_OBJECTS:picoet_obj>)
set_target_properties(picoet_hook PROPERTIES OUTPUT_NAME picoet_hook PREFIX "lib")
target_link_options(picoet_hook PRIVATE
    "-Wl,-soname,libpicoet_hook.so"
    "-Wl,--gc-sections"
    "-Wl,-z,max-page-size=16384"
    "-static-libstdc++"
    "-Wl,--exclude-libs,ALL")
target_link_libraries(picoet_hook PRIVATE log dl m)

# ---- injector + benign control target --------------------------------------
add_executable(picoet-inject src/picoet-inject.c)
target_compile_options(picoet-inject PRIVATE -O2 -fPIE -Wall -Wextra)
target_link_libraries(picoet-inject PRIVATE dl)

add_executable(picoet-target src/picoet-target.c)
target_compile_options(picoet-target PRIVATE -O2 -fPIE -Wall -Wextra)
target_link_libraries(picoet-target PRIVATE dl)


add_executable(tongue-seg-check src/tongue-seg-check.cpp)
target_compile_options(tongue-seg-check PRIVATE -O2 -std=c++17 -ffp-contract=off)
target_link_options(tongue-seg-check PRIVATE -static-libstdc++)

add_executable(tongue-train-check src/tongue-train-check.cpp)
target_compile_options(tongue-train-check PRIVATE -O2 -std=c++17 -ffp-contract=off)
target_link_options(tongue-train-check PRIVATE -static-libstdc++)

add_executable(tongue-train-accept-check src/tongue-train-accept-check.cpp)
target_compile_options(tongue-train-accept-check PRIVATE -O2 -std=c++17 -ffp-contract=off)
target_link_options(tongue-train-accept-check PRIVATE -static-libstdc++)
