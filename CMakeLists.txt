cmake_minimum_required(VERSION 3.20)

project(tinyimg VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)


# Sources
file(GLOB_RECURSE C_SOURCES ${PROJECT_SOURCE_DIR}/src/*.c)
add_executable(tinyimg ${C_SOURCES})
target_include_directories(tinyimg PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Executable
set_target_properties(tinyimg PROPERTIES
	C_STANDARD 11
	C_STANDARD_REQUIRED YES
	RUNTIME_OUTPUT_DIRECTORY
	${PROJECT_SOURCE_DIR}/bin=
)
target_compile_options(tinyimg PRIVATE
	-Oz # size optimization
	-flto # link-time optimization
    -ffunction-sections # Remove unused functions and data
    -fdata-sections # Remove unused functions and data
    -msimd128 # Enable SIMD128 instructions
)

target_link_options(tinyimg PRIVATE
    -Wl,--no-entry # no main function
    -Wl,--export-memory # export memory for wasm
    -Wl,--initial-memory=131072 # initial memory size = 128KB
    -Wl,--max-memory=67108864 # maximum memory size = 64MB
    -Wl,--gc-sections # garbage collection of unused sections

    -flto # link-time optimization
	-nostdlib # no standard library
)

set_target_properties(tinyimg PROPERTIES
    SUFFIX ".wasm"
	RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin
)

# Documentation
option(DOCS_TINYIMG "Build API documentation with Doxygen" ON)
if (DOCS_TINYIMG)
    find_package(Doxygen)

    if (DOXYGEN_FOUND)
        set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
        set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile.in)

        configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM)
    else()
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not found, skipping documentation generation"
            VERBATIM)
    endif()
endif()
