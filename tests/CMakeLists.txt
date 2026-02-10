include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

function(chm_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE concurrent_hashmap GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

chm_add_test(test_basic test_basic.cpp)
chm_add_test(test_spinlock test_spinlock.cpp)
chm_add_test(test_epoch test_epoch.cpp)
chm_add_test(test_resize test_resize.cpp)
chm_add_test(test_get_or_set test_get_or_set.cpp)
chm_add_test(test_concurrent test_concurrent.cpp)
chm_add_test(test_stress test_stress.cpp)
