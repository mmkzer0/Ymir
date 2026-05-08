add_executable(ymir-headless-tests
    src/smoke_tests.cpp
    src/protocol_tests.cpp
)
add_executable(ymir::ymir-headless-tests ALIAS ymir-headless-tests)
set_target_properties(ymir-headless-tests PROPERTIES
                      VERSION ${Ymir_VERSION}
                      SOVERSION ${Ymir_VERSION_MAJOR})
target_compile_features(ymir-headless-tests PUBLIC cxx_std_20)

find_package(Catch2 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(ymir-headless-tests PRIVATE
    Catch2::Catch2WithMain
    ymir::ymir-headless-proto
    ymir::ymir-headless-framing
)
# Note: nlohmann_json is provided transitively by ymir-headless-framing.
# The direct find_package above is kept for the test files that use
# nlohmann::json directly (e.g., json_rpc_adapter tests).

cmrk_copy_runtime_dlls(ymir-headless-tests)

include(CheckIPOSupported)
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

if (IPO_SUPPORTED AND Ymir_ENABLE_IPO)
    message(STATUS "Enabling IPO / LTO for ymir-headless-tests")
    set_property(TARGET ymir-headless-tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if (MSVC)
    vs_set_filters(TARGET ymir-headless-tests)
    set_target_properties(ymir-headless-tests PROPERTIES FOLDER "Ymir-tests")
endif ()

include(Catch)
catch_discover_tests(ymir-headless-tests)
