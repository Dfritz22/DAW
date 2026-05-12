add_executable(test_base
    test_result.cpp
)

target_link_libraries(test_base
    PRIVATE
        daw::base
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(test_base PRIVATE /W4 /WX /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(test_base)
