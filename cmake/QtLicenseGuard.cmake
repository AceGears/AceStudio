# AceStudio ships under Qt's LGPLv3 with dynamic linking. These Qt modules
# are GPLv3-only (or commercial-only) in the open-source distribution and
# must never be linked. See docs/research/stack.md and LICENSE_AUDIT.md.
set(ACESTUDIO_QT_FORBIDDEN_MODULES
    Graphs Charts DataVisualization VirtualKeyboard Quick3D Quick3DPhysics
    Lottie QuickTimeline CanvasPainter HttpServer NetworkAuth
    InsightTracker VncServer)

function(acestudio_check_qt_licenses target)
    get_target_property(libs ${target} LINK_LIBRARIES)
    foreach(lib IN LISTS libs)
        foreach(bad IN LISTS ACESTUDIO_QT_FORBIDDEN_MODULES)
            if(lib MATCHES "^Qt[0-9]*::${bad}$")
                message(FATAL_ERROR
                    "${target} links ${lib}, which is GPL-only/commercial-only in open-source Qt. "
                    "It is incompatible with AceStudio's LGPL distribution model.")
            endif()
        endforeach()
    endforeach()
    if(TARGET Qt6::Core)
        get_target_property(qt_core_type Qt6::Core TYPE)
        if(qt_core_type STREQUAL "STATIC_LIBRARY")
            message(FATAL_ERROR "Static Qt detected. LGPL distribution requires dynamically linked Qt.")
        endif()
    endif()
endfunction()
