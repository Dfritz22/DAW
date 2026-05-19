add_executable(test_engine
    test_clip_reader.cpp
    test_clip_render.cpp
    test_mix_pipeline.cpp
    test_mix_snapshot.cpp
    test_timeline_layout.cpp
)

target_link_libraries(test_engine
    PRIVATE
        daw::engine
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(test_engine PRIVATE /W4 /WX /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(test_engine)
