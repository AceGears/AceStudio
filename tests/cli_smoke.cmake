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

# Asset commands (Phase 7).
set(T "${TEXTURES}")
run(0 --json tileset --base "${T}/water.png" --terrain "${T}/sand.png,${T}/grass.png" --out "${WORKDIR}/isle.png"
    --engines godot,unity,tiled --names water,sand,grass --phases 2)
string(JSON nfiles LENGTH "${LAST_OUT}" files)
string(JSON seams0 GET "${LAST_OUT}" seamCheck 0 seams)
if(nfiles LESS 12 OR NOT seams0 EQUAL 0)
    message(FATAL_ERROR "tileset wrote ${nfiles} files / ${seams0} seams: ${LAST_OUT}")
endif()
foreach(f isle.tres isle_level1.tsx isle_level2.png.meta isle_terrain.json isle_base.png)
    if(NOT EXISTS "${WORKDIR}/${f}")
        message(FATAL_ERROR "tileset did not write ${f}")
    endif()
endforeach()
run(2 tileset --base "${T}/water.png" --terrain "${T}/sand.png" --out "${WORKDIR}/isle.png")   # refuses to overwrite
run(1 tileset --base "${T}/water.png" --out "${WORKDIR}/x.png")                                 # no terrain
run(1 tileset --base "${T}/water.png" --terrain "${T}/sand.png" --out "${WORKDIR}/x.png" --scheme hex)
run(1 tileset --base "${T}/water.png" --terrain "${T}/sand.png" --out "${WORKDIR}/x.png" --engines unreal)
run(0 --json seams "${WORKDIR}/isle_level1.png" --scheme blob47 --phases 2 --base "${WORKDIR}/isle_base.png")
run(0 tileset-template --scheme wang16 --tile 12 --out "${WORKDIR}/tmpl.png")
run(3 seams "${WORKDIR}/tmpl.png" --scheme wang16)                                               # grid lines are seams
run(0 --json atlas "${T}/water.png" "${T}/sand.png" "${T}/grass.png" --out "${WORKDIR}/atl.png" --engines unity --padding 2)
string(JSON sprites GET "${LAST_OUT}" sprites)
if(NOT sprites EQUAL 3 OR NOT EXISTS "${WORKDIR}/atl.png.meta")
    message(FATAL_ERROR "atlas: ${LAST_OUT}")
endif()
run(0 --json slice "${WORKDIR}/atl.png" --out "${WORKDIR}/sliced")
string(JSON nslices LENGTH "${LAST_OUT}" sprites)
if(NOT nslices EQUAL 3)
    message(FATAL_ERROR "slice found ${nslices} sprites: ${LAST_OUT}")
endif()
run(0 --json slice "${WORKDIR}/tmpl.png" --out "${WORKDIR}/grid" --grid 12x12)
string(JSON ncells LENGTH "${LAST_OUT}" sprites)
if(NOT ncells EQUAL 16)
    message(FATAL_ERROR "grid slice found ${ncells} cells: ${LAST_OUT}")
endif()
run(0 uikit --out "${WORKDIR}/ui.png" --engines godot,unity --scale 2)
if(NOT EXISTS "${WORKDIR}/ui_panel.tres" OR NOT EXISTS "${WORKDIR}/ui.png.meta")
    message(FATAL_ERROR "uikit did not write Godot / Unity files")
endif()
run(1 uikit --out "${WORKDIR}/ui2.png" --colors "#fff")
run(0 export "${P}" --format sheet --out "${WORKDIR}/eng" --engines godot,unity)
if(NOT EXISTS "${WORKDIR}/eng.tres" OR NOT EXISTS "${WORKDIR}/eng.png.meta" OR NOT EXISTS "${WORKDIR}/eng_default.anim")
    message(FATAL_ERROR "export --engines did not write the engine files")
endif()


# ---- Lua scripts (Phase 7)
file(WRITE "${WORKDIR}/scripts/make.lua" [=[
local img = ace.image(4, 4, arg[1])
img:save("made.png")
local d = ace.document()
if d then
  d:add_frame(90)
  d:add_tag("lua", 0, d.frames - 1)
end
print("made", arg[1])
]=])
file(WRITE "${WORKDIR}/scripts/escape.lua" [=[ace.image(1, 1):save("../escaped.png")]=])
file(WRITE "${WORKDIR}/scripts/io.lua" [=[io.open("x", "w")]=])
file(WRITE "${WORKDIR}/scripts/loop.lua" [=[while true do pcall(function() while true do end end) end]=])
run(0 --json run "${WORKDIR}/scripts/make.lua" "#ff0000")
string(JSON out GET "${LAST_OUT}" output)
if(NOT out STREQUAL "made\t#ff0000\n" OR NOT EXISTS "${WORKDIR}/scripts/made.png")
    message(FATAL_ERROR "run make.lua: ${LAST_OUT}")
endif()
run(6 run "${WORKDIR}/scripts/escape.lua")
if(EXISTS "${WORKDIR}/escaped.png")
    message(FATAL_ERROR "a script wrote outside its folder")
endif()
run(0 run "${WORKDIR}/scripts/escape.lua" --allow "${WORKDIR}")   # the parent folder is allowed now
run(6 run "${WORKDIR}/scripts/io.lua")
run(6 run "${WORKDIR}/scripts/loop.lua" --timeout 1)
run(1 run "${WORKDIR}/scripts/make.lua" --save "${WORKDIR}/x.acestudio")   # --save needs --doc
run(0 --json run "${WORKDIR}/scripts/make.lua" "#00ff00" --doc "${P}" --save "${WORKDIR}/lua_out.acestudio")
string(JSON changed GET "${LAST_OUT}" documentChanged)
run(0 --json info "${WORKDIR}/lua_out.acestudio")
string(JSON frames GET "${LAST_OUT}" frames)
if(NOT changed OR NOT frames GREATER 1)
    message(FATAL_ERROR "run --doc --save did not keep the script's frame: ${LAST_OUT}")
endif()

message(STATUS "cli_smoke passed")
