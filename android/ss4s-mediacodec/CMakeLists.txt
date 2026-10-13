# Android MediaCodec video module for SS4S. Included from the top-level CMakeLists.txt when
# building for Android, after third_party/ss4s has defined ss4s_add_module().
ss4s_add_module(mediacodec FOR_VIDEO WEIGHT 100 DISPLAY_NAME "Android MediaCodec"
        SOURCES mediacodec.c)
if (NOT SS4S_MODULE_TARGET)
    return()
endif ()
target_include_directories(${SS4S_MODULE_TARGET} SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(${SS4S_MODULE_TARGET} PRIVATE mediandk android log ${SDL2_LIBRARIES})
