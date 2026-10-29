# Applies a consistent, strict warning set to a target.
function(acestudio_set_warnings target)
    if(MSVC)
        set(warnings /W4 /permissive- /utf-8)
        if(ACESTUDIO_WARNINGS_AS_ERRORS)
            list(APPEND warnings /WX)
        endif()
    else()
        set(warnings -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                     -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast
                     -Woverloaded-virtual)
        if(ACESTUDIO_WARNINGS_AS_ERRORS)
            list(APPEND warnings -Werror)
        endif()
    endif()
    target_compile_options(${target} PRIVATE ${warnings})
endfunction()
