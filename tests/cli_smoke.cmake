# End-to-end CLI test, run by CTest: cmake -DCLI=... -DWORKDIR=... -P cli_smoke.cmake
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

function(run expected_code)
    execute_process(COMMAND "${CLI}" ${ARGN}
        RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL expected_code)
        message(FATAL_ERROR "acestudio-cli ${ARGN}\n  expected exit ${expected_code}, got ${code}\n  stdout: ${out}\n  stderr: ${err}")
    endif()
    set(LAST_OUT "${out}" PARENT_SCOPE)
endfunction()

set(P "${WORKDIR}/hero.acestudio")

run(0 --json version)
string(JSON ok GET "${LAST_OUT}" ok)
if(NOT ok)
    message(FATAL_ERROR "version --json did not report ok: ${LAST_OUT}")
endif()

run(1)                                                    # no command
run(1 frobnicate)                                         # unknown command
run(1 new "${P}" --width 64)                              # missing --height
run(0 new "${P}" --width 64 --height 48 --frames 4)
run(2 new "${P}" --width 64 --height 48)                  # refuses to overwrite
run(0 --json info "${P}")
string(JSON frames GET "${LAST_OUT}" frames)
string(JSON width  GET "${LAST_OUT}" width)
if(NOT frames EQUAL 4 OR NOT width EQUAL 64)
    message(FATAL_ERROR "info reported wrong values: ${LAST_OUT}")
endif()
run(0 validate "${P}")

run(0 render "${P}" --out "${WORKDIR}/f0.png" --scale 2)
run(2 render "${P}" --out "${WORKDIR}/f0.png")            # refuses to overwrite
run(1 render "${P}" --out "${WORKDIR}/f9.png" --frame 9)  # missing frame
if(NOT EXISTS "${WORKDIR}/f0.png")
    message(FATAL_ERROR "render produced no file")
endif()

# Exports
run(0 export "${P}" --format sheet --out "${WORKDIR}/sheet" --layout grid --trim)
if(NOT EXISTS "${WORKDIR}/sheet.png" OR NOT EXISTS "${WORKDIR}/sheet.json")
    message(FATAL_ERROR "sheet export did not write PNG + JSON")
endif()
file(READ "${WORKDIR}/sheet.json" sheet_json)
string(JSON sheet_w GET "${sheet_json}" meta size w)
if(sheet_w LESS 1)
    message(FATAL_ERROR "sheet json has no size: ${sheet_json}")
endif()
run(2 export "${P}" --format sheet --out "${WORKDIR}/sheet")        # refuses to overwrite
run(0 export "${P}" --format gif  --out "${WORKDIR}/a.gif" --scale 2)
run(0 export "${P}" --format apng --out "${WORKDIR}/a.png" --loop 3)
run(0 export "${P}" --format webp --out "${WORKDIR}/a.webp")
run(0 --json export "${P}" --format png --out "${WORKDIR}/seq")
string(JSON nfiles LENGTH "${LAST_OUT}" files)
if(NOT nfiles EQUAL 4)
    message(FATAL_ERROR "png sequence wrote ${nfiles} files: ${LAST_OUT}")
endif()
run(1 export "${P}" --format bmp --out "${WORKDIR}/x.bmp")
run(2 export "${P}" --format gif --tag nosuchtag --out "${WORKDIR}/t.gif")

# A damaged project must be reported as corrupt (exit 3), not crash.
file(WRITE "${WORKDIR}/broken.acestudio" "this is not a project")
run(3 validate "${WORKDIR}/broken.acestudio")
run(2 info "${WORKDIR}/missing.acestudio")

message(STATUS "cli_smoke passed")
