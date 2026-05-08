add_executable(ymir-dbg-tests
    src/smoke_tests.cpp
)
add_executable(ymir::ymir-dbg-tests ALIAS ymir-dbg-tests)
set_target_properties(ymir-dbg-tests PROPERTIES
                      VERSION ${Ymir_VERSION}
                      SOVERSION ${Ymir_VERSION_MAJOR})
target_compile_features(ymir-dbg-tests PUBLIC cxx_std_20)

find_package(Catch2 CONFIG REQUIRED)

target_link_libraries(ymir-dbg-tests PRIVATE Catch2::Catch2WithMain)

cmrk_copy_runtime_dlls(ymir-dbg-tests)

if (IPO_SUPPORTED AND Ymir_ENABLE_IPO)
    message(STATUS "Enabling IPO / LTO for ymir-dbg-tests")
    set_property(TARGET ymir-dbg-tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if (MSVC)
    vs_set_filters(TARGET ymir-dbg-tests)
    set_target_properties(ymir-dbg-tests PROPERTIES FOLDER "Ymir-tests")
endif ()

include(Catch)
catch_discover_tests(ymir-dbg-tests)
