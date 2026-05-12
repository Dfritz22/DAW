add_executable(test_vm
    test_timeline_view.cpp
    test_timeline_zoom.cpp
)

target_link_libraries(test_vm
    PRIVATE
        daw::vm
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(test_vm PRIVATE /W4 /WX /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(test_vm)
