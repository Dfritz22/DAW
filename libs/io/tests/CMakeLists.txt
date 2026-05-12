add_executable(test_io
    test_wav.cpp
)

target_link_libraries(test_io
    PRIVATE
        daw::io
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(test_io PRIVATE /W4 /WX /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(test_io)
