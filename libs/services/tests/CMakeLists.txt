add_executable(test_services
    test_transport_fsm.cpp
)

target_link_libraries(test_services
    PRIVATE
        daw::services
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(test_services PRIVATE /W4 /WX /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(test_services)
