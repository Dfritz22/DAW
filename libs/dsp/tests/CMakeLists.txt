add_executable(test_dsp
    test_chain.cpp
    test_resample.cpp
    test_mix.cpp
    test_metronome.cpp
    test_input_monitor.cpp
    test_mix_add.cpp
    test_mix_diff.cpp
)

target_link_libraries(test_dsp
    PRIVATE
        daw::dsp
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(test_dsp PRIVATE /W4 /WX /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(test_dsp)
