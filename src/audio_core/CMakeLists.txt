add_library(audio_core STATIC
    audio_types.h
    codec.cpp
    codec.h
    audio_interface.cpp
    audio_interface.h
    audio_hle.h
    audio_hle.cpp
    interpolate.cpp
    interpolate.h
    null_sink.h
    sink.h
    sink_details.cpp
    sink_details.h
    time_stretch.cpp
    time_stretch.h

    $<$<BOOL:${SDL2_FOUND}>:sdl2_sink.cpp sdl2_sink.h>
    $<$<BOOL:${ENABLE_CUBEB}>:cubeb_sink.cpp cubeb_sink.h>
)

create_target_directory_groups(audio_core)

target_link_libraries(audio_core PUBLIC common core)
target_link_libraries(audio_core PRIVATE SoundTouch)

if(SDL2_FOUND)
    target_link_libraries(audio_core PRIVATE SDL2)
    target_compile_definitions(audio_core PRIVATE HAVE_SDL2)
endif()

if(ENABLE_CUBEB)
    target_link_libraries(audio_core PRIVATE cubeb)
    add_definitions(-DHAVE_CUBEB=1)
endif()
