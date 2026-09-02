cmake_minimum_required(VERSION 3.20)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

set(TINYIMG_WASM ON CACHE BOOL "Building the wasm32 target" FORCE)

# find the cross compiler
find_program(WASM_CLANG
    NAMES clang
    REQUIRED
)
find_program(WASM_CLANGXX
    NAMES clang++
    REQUIRED
)
find_program(WASM_LD
    NAMES wasm-ld
    REQUIRED
)
find_program(WASM_AR
    NAMES llvm-ar
    REQUIRED
)
find_program(WASM_RANLIB
    NAMES llvm-ranlib
    REQUIRED
)
find_program(WASM_STRIP
    NAMES llvm-strip
    REQUIRED
)

set(CMAKE_C_COMPILER "${WASM_CLANG}")
set(CMAKE_C_COMPILER_TARGET wasm32)

set(CMAKE_CXX_COMPILER "${WASM_CLANGXX}")
set(CMAKE_CXX_COMPILER_TARGET wasm32)

set(CMAKE_AR "${WASM_AR}")
set(CMAKE_RANLIB "${WASM_RANLIB}")
set(CMAKE_STRIP "${WASM_STRIP}")

set(CMAKE_LINKER "${WASM_LD}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
