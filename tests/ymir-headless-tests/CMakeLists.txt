add_executable(ymir-headless-tests
    src/smoke_tests.cpp
)
add_executable(ymir::ymir-headless-tests ALIAS ymir-headless-tests)
set_target_properties(ymir-headless-tests PROPERTIES
                      VERSION ${Ymir_VERSION}
                      SOVERSION ${Ymir_VERSION_MAJOR})
target_compile_features(ymir-headless-tests PUBLIC cxx_std_20)

find_package(Catch2 CONFIG REQUIRED)

target_link_libraries(ymir-headless-tests PRIVATE Catch2::Catch2WithMain)

cmrk_copy_runtime_dlls(ymir-headless-tests)

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
