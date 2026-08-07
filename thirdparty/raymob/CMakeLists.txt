cmake_minimum_required(VERSION 3.22.1)

set(RAYMOB_SOURCES
    helper.c
    sensor.c
    vibrator.c
    display.c
    soft_keyboard.c
    callback.c
    admob.c
)

add_library(raymoblib STATIC ${RAYMOB_SOURCES})

target_compile_options(raymoblib PRIVATE
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-O2>
)

target_compile_definitions(raymoblib PRIVATE
    RAYMOB_IMPLEMENTATION
)

if(ANDROID)
    target_include_directories(raymoblib PUBLIC
        ${ANDROID_NDK}/sources/android/native_app_glue/
    )
    
    target_compile_definitions(raymoblib PRIVATE
        PLATFORM_ANDROID
    )
    
    target_sources(raymoblib PRIVATE
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
    )
endif()

target_include_directories(raymoblib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(raymoblib PUBLIC raylib)
