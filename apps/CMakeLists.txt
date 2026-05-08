if (Ymir_ENABLE_SANDBOX)
	add_subdirectory(ymir-sandbox)
endif ()
add_subdirectory(ymir-sdl3)
if (Ymir_ENABLE_YMDASM)
	add_subdirectory(ymdasm)
endif ()
if (Ymir_ENABLE_YMIR_HEADLESS OR Ymir_ENABLE_YMIR_DBG OR Ymir_ENABLE_TESTS)
	add_library(ymir-headless-proto INTERFACE)
	add_library(ymir::ymir-headless-proto ALIAS ymir-headless-proto)
	target_include_directories(ymir-headless-proto
		INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ymir-headless/include>"
	)
	target_compile_features(ymir-headless-proto INTERFACE cxx_std_20)

	find_package(nlohmann_json CONFIG REQUIRED)

	# Header-only protocol/framing targets are shared by the debug apps and
	# their tests without coupling tests to ../../ source include paths.
	add_library(ymir-headless-framing INTERFACE)
	add_library(ymir::ymir-headless-framing ALIAS ymir-headless-framing)
	target_include_directories(ymir-headless-framing
		INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ymir-headless/src>"
	)
	target_link_libraries(ymir-headless-framing INTERFACE
		nlohmann_json::nlohmann_json
	)

	add_library(ymir-dbg-framing INTERFACE)
	add_library(ymir::ymir-dbg-framing ALIAS ymir-dbg-framing)
	target_include_directories(ymir-dbg-framing
		INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ymir-dbg/src>"
	)
	target_link_libraries(ymir-dbg-framing INTERFACE
		nlohmann_json::nlohmann_json
	)
endif ()
if (Ymir_ENABLE_YMIR_HEADLESS)
	add_subdirectory(ymir-headless)
endif ()
if (Ymir_ENABLE_YMIR_DBG)
	add_subdirectory(ymir-dbg)
endif ()
