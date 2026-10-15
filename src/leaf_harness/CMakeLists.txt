add_executable(leaf_harness leaf_harness.cpp)
target_link_libraries(leaf_harness PRIVATE citra_common citra_core video_core audio_core lodepng
                      Boost::boost ${PLATFORM_LIBRARIES} Threads::Threads)
