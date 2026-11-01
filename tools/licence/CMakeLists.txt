# acestudio-licence: AceGears' key tool (never shipped to customers).
add_executable(acestudio-licence main.cpp)
target_link_libraries(acestudio-licence PRIVATE AceStudio::core)
acestudio_set_warnings(acestudio-licence)
if(WIN32)
    target_link_libraries(acestudio-licence PRIVATE bcrypt)
endif()
