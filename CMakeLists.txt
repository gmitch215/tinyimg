cmake_minimum_required(VERSION 3.20)

set(PROJECT_VERSION "1.0.0")
project(tinyimg
	LANGUAGES C
	VERSION ${PROJECT_VERSION}
	DESCRIPTION "Lightweight and fast image processing library for Cloudflare Workers"
	HOMEPAGE_URL "https://github.com/gmitch215/tinyimg"
)
set(PROJECT_DOCUMENTATION "https://tinyimg.gmitch215.dev")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TINYIMG_LTO "Enable link-time optimization" ON)
option(TINYIMG_SIMD "Enable wasm SIMD128" ON)
option(TINYIMG_TESTS "Build the ctest suite (native builds only)" ON)
option(TINYIMG_COVERAGE "Instrument the library and the ctest suite for coverage" OFF)
option(TINYIMG_WASM_OPT "Run wasm-opt over the linked module" ON)
option(DOCS_TINYIMG "Build API documentation with Doxygen" ON)

# usage: -DSANITIZE_TINYIMG=address,undefined or -DSANITIZE_TINYIMG=thread
set(SANITIZE_TINYIMG "" CACHE STRING "Comma-separated sanitizers to enable (address,undefined,thread)")

# CONFIGURE_DEPENDS so adding a source re-globs on the next build; without it an existing build
# directory keeps the old list and the new file shows up as an undefined symbol at link time
file(GLOB_RECURSE C_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.c)

# TINYIMG_WASM comes from cmake/wasm32.cmake; a build with no toolchain file is the native one
if (TINYIMG_WASM)
	add_executable(tinyimg ${C_SOURCES})

	# -ffreestanding is load-bearing, not decorative: it tells the optimizer no C library exists,
	# which stops loop-idiom recognition turning an inlined tiny_strlen back into a call to strlen.
	# Under LTO that produced an undefined symbol at link time and nowhere else
	target_compile_options(tinyimg PRIVATE
		-Oz
		-ffreestanding
		-ffunction-sections
		-fdata-sections
		-fno-builtin-malloc
		-mbulk-memory
		-mnontrapping-fptoint
		-Wall
		-Wextra
		-Wstrict-prototypes
		-Wmissing-prototypes
		-Wshadow
		-Wconversion
		-Wsign-conversion
	)

	# the 64 KiB wasm-ld default cannot hold a recursive fill, and initial memory must exceed the
	# stack or the first call traps
	target_link_options(tinyimg PRIVATE
		-Wl,--no-entry
		-Wl,--export-memory
		-Wl,--gc-sections
		-Wl,--strip-all
		-Wl,-z,stack-size=262144
		-Wl,--initial-memory=1048576
		-Wl,--max-memory=67108864
		-nostdlib
	)

	if (TINYIMG_SIMD)
		target_compile_options(tinyimg PRIVATE -msimd128)
	endif()

	if (TINYIMG_LTO)
		target_compile_options(tinyimg PRIVATE -flto)
		target_link_options(tinyimg PRIVATE -flto)
	endif()

	# overridable so an ablation relink cannot leave a crippled module sitting in bin/, where
	# build:ts, both vitest lanes and the release artifact all read from
	set(TINYIMG_BIN_DIR "${PROJECT_SOURCE_DIR}/bin" CACHE PATH "Directory the linked wasm is written to")

	set_target_properties(tinyimg PROPERTIES
		SUFFIX ".wasm"
		RUNTIME_OUTPUT_DIRECTORY ${TINYIMG_BIN_DIR}
	)

	if (TINYIMG_WASM_OPT)
		# homebrew leaves binaryen unlinked when wabt is installed (both ship wasm2c), so the
		# keg is searched as well as PATH
		find_program(WASM_OPT
			NAMES wasm-opt
			HINTS
				ENV BINARYEN_ROOT
				/opt/homebrew/opt/binaryen/bin
				/usr/local/opt/binaryen/bin
				/usr/lib/binaryen/bin
		)
		if (WASM_OPT)
			# wasm-opt validates against its own feature set, not the module's, so the enables
			# have to mirror the compile flags or an auto-vectorized loop fails validation
			set(WASM_OPT_FEATURES --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext)
			if (TINYIMG_SIMD)
				list(APPEND WASM_OPT_FEATURES --enable-simd)
			endif()

			add_custom_command(TARGET tinyimg POST_BUILD
				COMMAND ${WASM_OPT} -Oz ${WASM_OPT_FEATURES}
					--strip-debug --strip-producers --strip-target-features
					$<TARGET_FILE:tinyimg> -o $<TARGET_FILE:tinyimg>
				COMMENT "Optimizing ${PROJECT_NAME}.wasm with wasm-opt"
				VERBATIM
			)
		else()
			message(STATUS "tinyimg: wasm-opt not found, shipping the wasm-ld output as-is")
		endif()
	endif()
else()
	if (SANITIZE_TINYIMG AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
		message(STATUS "tinyimg: enabling sanitizers (${SANITIZE_TINYIMG})")
		add_compile_options(-fsanitize=${SANITIZE_TINYIMG} -fno-omit-frame-pointer -g)
		add_link_options(-fsanitize=${SANITIZE_TINYIMG})
	endif()

	add_library(tinyimg STATIC ${C_SOURCES})

	if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
		target_compile_options(tinyimg PRIVATE
			-Wall
			-Wextra
			-Wstrict-prototypes
			-Wmissing-prototypes
			-Wshadow
			-Wconversion
			-Wsign-conversion
		)
		target_link_libraries(tinyimg PRIVATE m)

		# TINYIMG_LTO is deliberately not honoured here, only in the wasm target. An LTO archive
		# cannot be linked from an ordinary compile on this toolchain ("Unsupported stack probing
		# method"), which breaks every consumer of libtinyimg.a including the sanitizer lane and any
		# throwaway probe. Codegen parity with what ships is covered where it belongs: both vitest
		# lanes run the real bin/tinyimg.wasm, which is built with LTO. A performance figure has to
		# come from that module or from a purpose-built arm, never from this library.

		# the library is what coverage is meant to measure. instrumenting only the test executables
		# produced a report of zero lines, which the job then printed as a summary
		if (TINYIMG_COVERAGE)
			target_compile_options(tinyimg PRIVATE --coverage)
			target_link_options(tinyimg PUBLIC --coverage)
		endif()
	endif()

	set_target_properties(tinyimg PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
	)

	if (TINYIMG_TESTS)
		enable_testing()
		add_subdirectory(tests/c)
	endif()
endif()

target_include_directories(tinyimg PUBLIC
	$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:include>
)

# Documentation
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
