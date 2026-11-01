add_executable(acestudio-cli main.cpp CliCommon.h AiCommands.cpp AssetCommands.cpp ScriptCommand.cpp)
target_link_libraries(acestudio-cli PRIVATE AceStudio::core)
acestudio_set_warnings(acestudio-cli)
if(WIN32)
    target_compile_definitions(acestudio-cli PRIVATE UNICODE _UNICODE)
endif()

acestudio_add_win_resource(acestudio-cli "AceStudio command line")
