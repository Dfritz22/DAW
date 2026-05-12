add_executable(test_core
    test_automation_curve.cpp
    test_timeline_math.cpp
    test_audio_clip.cpp
    test_project_data.cpp
    test_timeline_edit.cpp
    test_track_audibility.cpp
)

target_link_libraries(test_core
    PRIVATE
        daw::core
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(test_core PRIVATE /W4 /WX /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(test_core)
