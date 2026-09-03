cmake_minimum_required(VERSION 3.20)

include(CTest)

set(TINYIMG_FIXTURE_DIR ${PROJECT_SOURCE_DIR}/tests/fixtures)

function(add_tinyimg_test folder name)
	set(TEST_TARGET "tinyimg-test-${folder}-${name}")

	add_executable("${TEST_TARGET}" "${folder}/${name}.c" "test.h")
	target_link_libraries("${TEST_TARGET}" PRIVATE tinyimg)
	target_include_directories("${TEST_TARGET}" PRIVATE
		"${PROJECT_SOURCE_DIR}/include"
		"${CMAKE_CURRENT_SOURCE_DIR}"
	)
	target_compile_definitions("${TEST_TARGET}" PRIVATE
		TINYIMG_FIXTURE_DIR="${TINYIMG_FIXTURE_DIR}"
	)

	if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
		target_compile_options("${TEST_TARGET}" PRIVATE -Wall -Wextra)
		# libm for the fabs/log10 in test.h
		target_link_libraries("${TEST_TARGET}" PRIVATE m)

		if (TINYIMG_COVERAGE)
			target_compile_options("${TEST_TARGET}" PRIVATE --coverage)
			target_link_options("${TEST_TARGET}" PRIVATE --coverage)
		endif()
	endif()

	string(REPLACE "_" "/" TEST_LABEL ${name})
	add_test(NAME ${folder}/${TEST_LABEL} COMMAND "${TEST_TARGET}")

	set_target_properties("${TEST_TARGET}" PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests/${folder}
	)
endfunction()

file(GLOB_RECURSE TINYIMG_TESTS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*/*.c")

foreach(TEST_FILE ${TINYIMG_TESTS})
	get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
	get_filename_component(TEST_FOLDER ${TEST_FILE} DIRECTORY)

	add_tinyimg_test(${TEST_FOLDER} ${TEST_NAME})
endforeach()

list(LENGTH TINYIMG_TESTS TINYIMG_TEST_COUNT)
message(STATUS "tinyimg: registered ${TINYIMG_TEST_COUNT} ctest executable(s)")
