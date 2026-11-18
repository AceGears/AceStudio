# End-to-end CLI test, run by CTest: cmake -DCLI=... -DWORKDIR=... -P cli_smoke.cmake
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
# A data folder of our own: nothing from the real user.
set(ENV{ACESTUDIO_DATA_DIR} "${WORKDIR}/data")

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

# In-betweens: two keys and one in-between; with --loop-back and 3 per pair,
# each key gets 3 after it (8 frames). --loop-back is a switch of its own:
# export's --loop N (above) must keep taking a number.
run(0 --json inbetween "${T}/water.png" "${T}/sand.png" --out "${WORKDIR}/ib")
string(JSON ib_frames GET "${LAST_OUT}" frames)
if(NOT ib_frames EQUAL 3 OR NOT EXISTS "${WORKDIR}/ib/inbetween.png" OR NOT EXISTS "${WORKDIR}/ib/inbetween.gif")
    message(FATAL_ERROR "inbetween wrote ${ib_frames} frames: ${LAST_OUT}")
endif()
run(2 inbetween "${T}/water.png" "${T}/sand.png" --out "${WORKDIR}/ib")          # refuses to overwrite
run(0 --json inbetween "${T}/water.png" "${T}/sand.png" --out "${WORKDIR}/ib" --n 3 --loop-back --name loop)
string(JSON ib_frames GET "${LAST_OUT}" frames)
if(NOT ib_frames EQUAL 8)
    message(FATAL_ERROR "inbetween --loop-back --n 3 wrote ${ib_frames} frames: ${LAST_OUT}")
endif()
run(1 inbetween "${T}/water.png" --out "${WORKDIR}/ib1")                         # one key
run(1 inbetween "${T}/water.png" "${T}/sand.png" --out "${WORKDIR}/ib2" --n 9)   # too many
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

# ---- Rigs (schema 4): auto-rig a sprite, show the rig, bake a pose
file(COPY "${TEXTURES}/../../data/anim/rogue64.png" DESTINATION "${WORKDIR}/rigs")
file(WRITE "${WORKDIR}/rigs/make.lua" [=[
local s = ace.load_png("rogue64.png")
local d = ace.new_document(s.width + 8, s.height + 8)
d:set_cel(0, 0, s, 4, 4)
d:add_frame()
d:add_frame()
d:save("hero.acestudio")
]=])
run(0 run "${WORKDIR}/rigs/make.lua")
set(RP "${WORKDIR}/rigs/hero.acestudio")
set(RR "${WORKDIR}/rigs/rigged.acestudio")
run(0 --json rig show "${RP}")
string(JSON rig_type TYPE "${LAST_OUT}" rig)
if(NOT rig_type STREQUAL "NULL")
    message(FATAL_ERROR "rig show of an unrigged project: ${LAST_OUT}")
endif()
run(1 rig auto "${RP}" --out "${RR}")                         # no --layer
run(1 rig auto "${RP}" --layer nope --out "${RR}")            # no such layer
run(1 rig frobnicate "${RP}")
run(1 rig auto "${RP}" --layer 1 --as tentacles --out "${RR}")
run(0 --json rig auto "${RP}" --layer 1 --out "${RR}")
string(JSON rig_bones GET "${LAST_OUT}" bones)
if(rig_bones LESS 5)
    message(FATAL_ERROR "rig auto made ${rig_bones} bones: ${LAST_OUT}")
endif()
run(2 rig auto "${RP}" --layer 1 --out "${RR}")              # refuses to overwrite
run(1 rig auto "${RR}" --layer 1 --out "${WORKDIR}/rigs/again.acestudio")   # already rigged
# Arms the analysis missed (the rogue's raised daggers), marked as boxes
run(1 rig auto "${RP}" --layer 1 --arms "1,2,3" --out "${WORKDIR}/rigs/arms_bad.acestudio")
run(1 rig auto "${RP}" --layer 1 --arms "0,0,2,2" --out "${WORKDIR}/rigs/arms_bad.acestudio")        # no pixels there
run(1 rig auto "${RP}" --layer 1 --arms "15,10,14,22 20,12,4,4" --out "${WORKDIR}/rigs/arms_bad.acestudio")   # overlap (spaces: CMake splits at ';')
run(0 --json rig auto "${RP}" --layer 1 --arms "15,10,14,22 42,20,16,22" --out "${WORKDIR}/rigs/arms.acestudio")
string(JSON arm_bones GET "${LAST_OUT}" bones)
math(EXPR want_arm_bones "${rig_bones} + 2")
if(NOT arm_bones EQUAL want_arm_bones)
    message(FATAL_ERROR "rig auto --arms made ${arm_bones} bones (without: ${rig_bones}): ${LAST_OUT}")
endif()
run(0 --json info "${RR}")
string(JSON rig_schema GET "${LAST_OUT}" schemaVersion)
if(NOT rig_schema EQUAL 4)
    message(FATAL_ERROR "a rigged project saved as schema ${rig_schema}")
endif()
run(0 --json rig show "${RR}")
string(JSON rig_chains LENGTH "${LAST_OUT}" rig chains)
if(NOT rig_chains EQUAL 2)
    message(FATAL_ERROR "rig show: ${rig_chains} IK chains: ${LAST_OUT}")
endif()
run(1 rig pose "${RR}" --turn "nobone=10" --out "${WORKDIR}/rigs/bad.acestudio")
run(1 rig pose "${RP}" --out "${WORKDIR}/rigs/bad.acestudio")     # no rig
run(0 rig pose "${RR}" --out "${WORKDIR}/rigs/rest.acestudio")
run(0 rig pose "${RR}" --turn "leg0 thigh=35,body=8" --reach "leg1 IK=40,70" --out "${WORKDIR}/rigs/posed.acestudio")
run(0 render "${RR}" --out "${WORKDIR}/rigs/rigged.png")
run(0 render "${WORKDIR}/rigs/rest.acestudio" --out "${WORKDIR}/rigs/rest.png")
run(0 render "${WORKDIR}/rigs/posed.acestudio" --out "${WORKDIR}/rigs/posed.png")
file(SHA256 "${WORKDIR}/rigs/rigged.png" h_rigged)
file(SHA256 "${WORKDIR}/rigs/rest.png" h_rest)
file(SHA256 "${WORKDIR}/rigs/posed.png" h_posed)
if(NOT h_rigged STREQUAL h_rest OR h_rigged STREQUAL h_posed)
    message(FATAL_ERROR "rig pose: the rest pose must render as rigged, a turned pose differently")
endif()
# Keys (rig version 2): key frames 0 and 2, bake, frame 2 matches a plain pose
set(RK "${WORKDIR}/rigs/keys")
run(1 rig bake "${RR}" --out "${RK}_none.acestudio")                        # no keys yet
run(1 rig key "${RR}" --out "${RK}_bad.acestudio")                          # no --frame
run(1 rig key "${RR}" --frame 2 --curve wobbly --out "${RK}_bad.acestudio")
run(1 rig key "${RR}" --frame 9 --turn "body=8" --out "${RK}_bad.acestudio")   # no frame 9
run(1 rig key "${RR}" --frame 2 --curve "bezier:1,2" --out "${RK}_bad.acestudio")
run(0 rig key "${RR}" --frame 0 --out "${RK}0.acestudio")
run(0 --json rig key "${RK}0.acestudio" --frame 2 --turn "leg0 thigh=40,body=10" --curve ease-in-out --out "${RK}2.acestudio")
string(JSON key_count GET "${LAST_OUT}" keys)
if(NOT key_count EQUAL 2)
    message(FATAL_ERROR "rig key: ${key_count} keys: ${LAST_OUT}")
endif()
run(1 rig unkey "${RK}2.acestudio" --frame 1 --out "${RK}_bad.acestudio")    # no key there
run(0 rig unkey "${RK}2.acestudio" --frame 0 --out "${RK}_one.acestudio")
run(0 rig rest "${RK}2.acestudio" --frame 1 --out "${RK}_rest1.acestudio")
run(1 rig rest "${RK}2.acestudio" --frame 7 --out "${RK}_bad.acestudio")
run(0 rig bake "${RK}2.acestudio" --out "${RK}_baked.acestudio")
run(0 rig pose "${RR}" --frame 2 --turn "leg0 thigh=40,body=10" --out "${RK}_posed2.acestudio")
foreach(f 0 1 2)
    run(0 render "${RK}_baked.acestudio" --frame ${f} --out "${RK}_baked${f}.png")
    file(SHA256 "${RK}_baked${f}.png" h_baked${f})
endforeach()
run(0 render "${RK}_posed2.acestudio" --frame 2 --out "${RK}_posed2.png")
file(SHA256 "${RK}_posed2.png" h_posed2)
if(NOT h_baked2 STREQUAL h_posed2 OR NOT h_baked0 STREQUAL h_rigged OR h_baked1 STREQUAL h_baked0 OR h_baked1 STREQUAL h_baked2)
    message(FATAL_ERROR "rig bake: frame 2 must match the keyed pose, frame 0 stay at rest, frame 1 lie between")
endif()

# Motion keys (RigMotion.h): a preset keyed onto the rig, then baked
run(1 rig motion "${RR}" preset:walk)                                                   # no --out
run(1 rig motion "${RP}" preset:walk --out "${RK}_m_bad.acestudio")                     # no rig
run(2 rig motion "${RR}" preset:moonwalk --out "${RK}_m_bad.acestudio")                 # no such preset
run(4 rig motion "${RR}" "${WORKDIR}/rigs/moves.fbx" --out "${RK}_m_bad.acestudio")     # FBX: convert first
run(1 rig motion "${RR}" preset:walk --first 0 --out "${RK}_m_bad.acestudio")           # covers the rest frame
run(1 rig motion "${RR}" preset:walk --bones "nobone=LThigh" --out "${RK}_m_bad.acestudio")
run(1 rig motion "${RR}" preset:walk --bones "body=Elbow" --out "${RK}_m_bad.acestudio")
run(1 rig motion "${RR}" preset:walk --animation 0 --out "${RK}_m_bad.acestudio")       # a preset holds one motion
run(0 --json rig motion "${RR}" preset:walk --fps 8 --out "${RK}_walk.acestudio")
string(JSON mk_keys GET "${LAST_OUT}" keys)
string(JSON mk_first GET "${LAST_OUT}" first)
string(JSON mk_added GET "${LAST_OUT}" framesAdded)
string(JSON mk_thigh GET "${LAST_OUT}" matched "leg0 thigh")
if(NOT mk_keys EQUAL 8 OR NOT mk_first EQUAL 1 OR NOT mk_added EQUAL 6 OR mk_thigh STREQUAL "")
    message(FATAL_ERROR "rig motion walk: ${LAST_OUT}")
endif()
run(2 rig motion "${RR}" preset:walk --fps 8 --out "${RK}_walk.acestudio")              # refuses to overwrite
run(0 --json rig motion "${RR}" preset:jump --fps 8 --view right --bake --out "${RK}_jump.acestudio")
string(JSON mk_baked GET "${LAST_OUT}" baked)
string(JSON mk_last GET "${LAST_OUT}" last)
if(NOT mk_baked OR NOT mk_last EQUAL 8)
    message(FATAL_ERROR "rig motion jump: ${LAST_OUT}")
endif()
foreach(f 0 4)
    run(0 render "${RK}_jump.acestudio" --frame ${f} --out "${RK}_jump${f}.png")
    file(SHA256 "${RK}_jump${f}.png" h_jump${f})
endforeach()
if(h_jump0 STREQUAL h_jump4 OR NOT h_jump0 STREQUAL h_rigged)
    message(FATAL_ERROR "rig motion --bake: the rest frame must stay as rigged and a jump frame differ from it")
endif()

# ---- Vector shapes (schema 5): add, show, rasterise, clear
file(MAKE_DIRECTORY "${WORKDIR}/vec")
file(WRITE "${WORKDIR}/vec/make.lua" [=[
local d = ace.new_document(32, 24)
d:save("blank.acestudio")
]=])
run(0 run "${WORKDIR}/vec/make.lua")
set(VB "${WORKDIR}/vec/blank.acestudio")
set(VD "${WORKDIR}/vec/v")
run(1 vector add "${VB}" --layer 1 --out "${VD}_bad.acestudio")                        # no shape
run(1 vector add "${VB}" --layer 1 --rect 1,2,3 --out "${VD}_bad.acestudio")
run(1 vector add "${VB}" --layer 1 --path "1,1" --out "${VD}_bad.acestudio")           # one anchor
run(1 vector add "${VB}" --layer 1 --rect 1,1,5,5 --width 0 --out "${VD}_bad.acestudio")
run(1 vector add "${VB}" --layer 1 --rect 1,1,5,5 --fill plaid --out "${VD}_bad.acestudio")
run(1 vector frobnicate "${VB}")
run(0 vector add "${VB}" --layer 1 --rect 2,2,12,10 --fill solid --fill-color "#ff0000" --out "${VD}1.acestudio")
run(0 --json info "${VD}1.acestudio")
string(JSON vec_schema GET "${LAST_OUT}" schemaVersion)
if(NOT vec_schema EQUAL 5)
    message(FATAL_ERROR "a project with vector shapes saved as schema ${vec_schema}")
endif()
run(0 vector add "${VD}1.acestudio" --layer 1 --path "14,2 30,2 22,20" --closed --stroke dashed --fill linear
    --fill-color "#00ff00" --fill-color2 "#0000ff" --axis 14,2,30,2 --out "${VD}2.acestudio")
run(0 --json vector show "${VD}2.acestudio")
string(JSON vec_shapes LENGTH "${LAST_OUT}" cels 0 shapes)
if(NOT vec_shapes EQUAL 2)
    message(FATAL_ERROR "vector show: ${vec_shapes} shapes: ${LAST_OUT}")
endif()
run(0 vector rasterize "${VD}2.acestudio" --layer 1 --out "${VD}3.acestudio")
run(0 vector clear "${VD}2.acestudio" --layer 1 --out "${VD}4.acestudio")
run(1 vector rasterize "${VD}4.acestudio" --layer 1 --out "${VD}5.acestudio")          # no shapes left
run(0 --json info "${VD}3.acestudio")
string(JSON vec_schema GET "${LAST_OUT}" schemaVersion)
if(vec_schema EQUAL 5)
    message(FATAL_ERROR "a rasterised project still needs schema 5")
endif()
foreach(n 2 3 4)
    run(0 render "${VD}${n}.acestudio" --out "${VD}${n}.png")
    file(SHA256 "${VD}${n}.png" h_vec${n})
endforeach()
if(NOT h_vec2 STREQUAL h_vec3 OR h_vec2 STREQUAL h_vec4)
    message(FATAL_ERROR "vector: rasterising must keep the pixels, clearing must remove them")
endif()

# ---- Palette lock: a smooth gradient leaves the palette, conform and lock bring it back
run(0 vector add "${VB}" --layer 1 --rect 0,0,31,23 --stroke none --fill linear --fill-color "#000000"
    --fill-color2 "#ffffff" --axis 0,0,31,0 --smooth --out "${VD}_grad.acestudio")
run(1 palette check "${VD}_grad.acestudio")
run(0 --json palette show "${VD}_grad.acestudio")
string(JSON pal_locked GET "${LAST_OUT}" locked)
string(JSON pal_off GET "${LAST_OUT}" offPalettePixels)
if(pal_locked OR pal_off EQUAL 0)
    message(FATAL_ERROR "palette show: a smooth gradient must be off the palette and unlocked: ${LAST_OUT}")
endif()
run(1 palette lock "${VD}_grad.acestudio")                                       # no --out
run(1 palette frobnicate "${VD}_grad.acestudio")
run(0 palette lock "${VD}_grad.acestudio" --out "${VD}_locked.acestudio")
run(0 --json info "${VD}_locked.acestudio")
string(JSON pal_schema GET "${LAST_OUT}" schemaVersion)
if(NOT pal_schema EQUAL 5)
    message(FATAL_ERROR "palette lock must not raise the schema (vectors need 5): ${pal_schema}")
endif()
run(1 palette check "${VD}_locked.acestudio")                                    # locking keeps the pixels
run(0 palette conform "${VD}_locked.acestudio" --out "${VD}_conformed.acestudio")
run(0 palette check "${VD}_conformed.acestudio")
run(0 --json palette show "${VD}_conformed.acestudio")
string(JSON pal_locked GET "${LAST_OUT}" locked)
if(NOT pal_locked)
    message(FATAL_ERROR "palette lock was not saved: ${LAST_OUT}")
endif()
run(0 vector add "${VD}_conformed.acestudio" --layer 1 --ellipse 2,2,20,20 --stroke solid --stroke-color "#123456"
    --fill radial --fill-color "#ff00ff" --fill-color2 "#00ffff" --axis 11,11,20,11 --smooth --out "${VD}_locked2.acestudio")
run(0 palette check "${VD}_locked2.acestudio")                                   # new pixels snapped
run(0 palette unlock "${VD}_locked2.acestudio" --out "${VD}_unlocked.acestudio")

# ---- Motion library: built-in presets, export, save, retarget a preset, remove
set(MD "${WORKDIR}/motions")
run(0 --json motion list)
string(JSON n_presets LENGTH "${LAST_OUT}" presets)
if(NOT n_presets EQUAL 6)
    message(FATAL_ERROR "motion list: expected the 6 built-in presets: ${LAST_OUT}")
endif()
run(1 motion frobnicate)
run(1 motion export walk)                                                       # no --out
run(2 motion export moonwalk --out "${MD}/x.bvh")                              # no such preset
run(0 motion export walk --out "${WORKDIR}/walk.bvh")
run(2 motion export walk --out "${WORKDIR}/walk.bvh")                          # refuses to overwrite
run(1 motion save "${WORKDIR}/walk.bvh")                                        # no --name
run(1 motion save "${WORKDIR}/walk.bvh" --name walk)                            # a built-in name
run(1 motion save "${WORKDIR}/walk.bvh" --name "../out")                        # not a preset name
run(0 motion save "${WORKDIR}/walk.bvh" --name "my walk" --library "${MD}")
run(1 motion save "${WORKDIR}/walk.bvh" --name "my walk" --library "${MD}")     # already saved
run(0 motion save "${WORKDIR}/walk.bvh" --name "my walk" --library "${MD}" --force)
run(0 --json motion list --library "${MD}")
string(JSON n_presets LENGTH "${LAST_OUT}" presets)
if(NOT n_presets EQUAL 7 OR NOT EXISTS "${MD}/my walk.bvh")
    message(FATAL_ERROR "motion save: the saved preset is not listed: ${LAST_OUT}")
endif()
run(0 --json retarget "${WORKDIR}/rigs/rogue64.png" preset:jump --out "${WORKDIR}/presets" --fps 10)
string(JSON n_frames GET "${LAST_OUT}" frames)
if(n_frames LESS 8 OR NOT EXISTS "${WORKDIR}/presets/rogue64_jump.gif")
    message(FATAL_ERROR "retarget preset:jump: expected about 10 frames (1 s at 10 fps) and a GIF: ${LAST_OUT}")
endif()
run(0 retarget "${WORKDIR}/rigs/rogue64.png" "preset:my walk" --library "${MD}" --out "${WORKDIR}/presets")
run(2 retarget "${WORKDIR}/rigs/rogue64.png" preset:moonwalk --out "${WORKDIR}/presets")
run(1 retarget "${WORKDIR}/rigs/rogue64.png" preset:walk --animation 0 --out "${WORKDIR}/presets")
run(1 motion remove walk --library "${MD}")                                      # built in
run(0 motion remove "my walk" --library "${MD}")
run(2 motion remove "my walk" --library "${MD}")

# ---- Direction sets (DirectionSet.h): a walk in eight directions from two drawn views
set(DV "${WORKDIR}/views")
file(COPY "${TEXTURES}/../../data/anim/rogue64.png" "${TEXTURES}/../../data/anim/knight32.png" DESTINATION "${DV}")
file(RENAME "${DV}/rogue64.png" "${DV}/front.png")
file(RENAME "${DV}/knight32.png" "${DV}/e.png")
file(WRITE "${DV}/readme.png" "not a view and not a PNG either")        # skipped by its name
run(1 directions "${DV}" preset:walk)                                         # no --out
run(1 directions "${DV}" preset:walk --turn 30 --out "${WORKDIR}/dirs_bad.acestudio")
run(1 directions "${DV}" preset:walk --dirs s,up --out "${WORKDIR}/dirs_bad.acestudio")
run(1 directions "${DV}" preset:walk --turn-clip s --out "${WORKDIR}/dirs_bad.acestudio")
run(2 directions "${WORKDIR}/no_views" preset:walk --out "${WORKDIR}/dirs_bad.acestudio")
run(1 directions "${WORKDIR}/rigs" preset:walk --out "${WORKDIR}/dirs_bad.acestudio")  # no file named by a view
run(0 --json directions "${DV}" preset:walk --fps 8 --turn-clip s,w --out "${WORKDIR}/dirs.acestudio")
string(JSON n_dirs LENGTH "${LAST_OUT}" directions)
string(JSON n_frames GET "${LAST_OUT}" frames)
string(JSON w_view GET "${LAST_OUT}" directions 6 view)
string(JSON w_mirror GET "${LAST_OUT}" directions 6 mirrored)
string(JSON w_approx GET "${LAST_OUT}" directions 6 approximateView)
string(JSON n_approx GET "${LAST_OUT}" directions 4 approximateView)
string(JSON turn_frames GET "${LAST_OUT}" turnClip frames)
string(JSON skipped GET "${LAST_OUT}" skipped 0)
if(NOT n_dirs EQUAL 8 OR NOT n_frames EQUAL 66 OR NOT w_view STREQUAL "e" OR NOT w_mirror OR w_approx OR NOT n_approx
   OR NOT turn_frames EQUAL 2 OR NOT skipped STREQUAL "readme.png")
    message(FATAL_ERROR "directions: expected 8 walks of 8 frames and a 2-frame turn, west from the mirrored east view, north flagged: ${LAST_OUT}")
endif()
run(0 --json info "${WORKDIR}/dirs.acestudio")
string(JSON n_tags LENGTH "${LAST_OUT}" tags)
if(NOT n_tags EQUAL 9)
    message(FATAL_ERROR "directions: expected a tag per direction and one for the turn: ${LAST_OUT}")
endif()
run(2 directions "${DV}" preset:walk --out "${WORKDIR}/dirs.acestudio")              # refuses to overwrite
run(0 --json directions "${DV}" preset:jump --dirs 4 --no-mirror --name hop --out "${WORKDIR}/dirs4.acestudio")
string(JSON w_mirror GET "${LAST_OUT}" directions 3 mirrored)
string(JSON w_approx GET "${LAST_OUT}" directions 3 approximateView)
string(JSON w_tag GET "${LAST_OUT}" directions 3 tag)
if(w_mirror OR NOT w_approx OR NOT w_tag STREQUAL "hop_w")
    message(FATAL_ERROR "directions --no-mirror: west must use drawn art, flagged: ${LAST_OUT}")
endif()
run(1 run "${WORKDIR}/scripts/make.lua" --save "${WORKDIR}/x.acestudio")   # --save needs --doc
run(0 --json run "${WORKDIR}/scripts/make.lua" "#00ff00" --doc "${P}" --save "${WORKDIR}/lua_out.acestudio")
string(JSON changed GET "${LAST_OUT}" documentChanged)
run(0 --json info "${WORKDIR}/lua_out.acestudio")
string(JSON frames GET "${LAST_OUT}" frames)
if(NOT changed OR NOT frames GREATER 1)
    message(FATAL_ERROR "run --doc --save did not keep the script's frame: ${LAST_OUT}")
endif()

# ---- connect: merges into each client's config and keeps other entries.
set(CH "${WORKDIR}/connect")
file(MAKE_DIRECTORY "${CH}/home/.codex" "${CH}/proj/.vscode")
set(ENV{ACESTUDIO_CONNECT_HOME} "${CH}/home")
file(WRITE "${CH}/proj/.vscode/mcp.json" "{\"servers\": {\"other\": {\"command\": \"x\"}}, \"inputs\": []}")
file(WRITE "${CH}/home/.codex/config.toml" "[model]\nname = 'x'\n\n[mcp_servers.acestudio]\ncommand = 'old'\n\n[mcp_servers.acestudio.env]\nA = '1'\n\n[other]\nk = 1\n")
set(FAKE_MCP "${CH}/acestudio-mcp")
file(WRITE "${FAKE_MCP}" "")
run(1 connect notepad)
run(2 connect cursor --project "${CH}/proj" --mcp "${CH}/missing")
foreach(c claude cursor vscode opencode codex)
    run(0 connect ${c} --project "${CH}/proj" --mcp "${FAKE_MCP}")
endforeach()
run(0 connect codex --global --mcp "${FAKE_MCP}")
run(0 connect claude-desktop --mcp "${FAKE_MCP}")
run(0 --json connect claude --global --mcp "${FAKE_MCP}")
string(JSON written GET "${LAST_OUT}" written)
if(written)
    message(FATAL_ERROR "connect claude --global must not edit ~/.claude.json")
endif()
file(READ "${CH}/proj/.vscode/mcp.json" vs)
string(JSON other GET "${vs}" servers other command)
string(JSON ace GET "${vs}" servers acestudio type)
string(JSON inputs TYPE "${vs}" inputs)
if(NOT other STREQUAL "x" OR NOT ace STREQUAL "stdio" OR NOT inputs STREQUAL "ARRAY")
    message(FATAL_ERROR "connect vscode lost or misplaced entries: ${vs}")
endif()
file(READ "${CH}/proj/.mcp.json" cc)
string(JSON cmd GET "${cc}" mcpServers acestudio command)
file(READ "${CH}/proj/opencode.json" oc)
string(JSON octype GET "${oc}" mcp acestudio type)
file(READ "${CH}/home/AppData/Claude/claude_desktop_config.json" cd)
string(JSON cdcmd GET "${cd}" mcpServers acestudio command)
if(NOT octype STREQUAL "local" OR NOT cmd STREQUAL "${FAKE_MCP}" OR NOT cdcmd STREQUAL "${FAKE_MCP}")
    message(FATAL_ERROR "connect wrote unexpected configs:\n${cc}\n${oc}\n${cd}")
endif()
file(READ "${CH}/home/.codex/config.toml" toml)
string(FIND "${toml}" "command = 'old'" old)
string(FIND "${toml}" "[mcp_servers.acestudio.env]" env)
string(FIND "${toml}" "[other]" kept)
string(FIND "${toml}" "tool_timeout_sec = 900" timeout)
if(NOT old EQUAL -1 OR NOT env EQUAL -1 OR kept EQUAL -1 OR timeout EQUAL -1 OR NOT EXISTS "${CH}/home/.codex/config.toml.bak")
    message(FATAL_ERROR "connect codex did not replace its table cleanly:\n${toml}")
endif()
run(0 connect vscode --project "${CH}/proj" --remove)
file(READ "${CH}/proj/.vscode/mcp.json" vs)
string(JSON n LENGTH "${vs}" servers)
if(NOT n EQUAL 1)
    message(FATAL_ERROR "connect --remove left: ${vs}")
endif()
unset(ENV{ACESTUDIO_CONNECT_HOME})

# ---- animate (a sprite with transparency: the UI kit sheet) and map
run(0 --json animate "${WORKDIR}/ui.png" --out "${WORKDIR}/anim" --action idle,walk,jump)
string(JSON nclips LENGTH "${LAST_OUT}" clips)
if(NOT nclips EQUAL 3 OR NOT EXISTS "${WORKDIR}/anim/ui_walk.png" OR NOT EXISTS "${WORKDIR}/anim/ui_jump.json")
    message(FATAL_ERROR "animate did not write its strips: ${LAST_OUT}")
endif()
run(2 animate "${WORKDIR}/ui.png" --out "${WORKDIR}/anim" --action idle)     # refuses to overwrite
run(1 animate "${WORKDIR}/ui.png" --out "${WORKDIR}/anim" --action moonwalk)
file(MAKE_DIRECTORY "${WORKDIR}/level.assets")
file(COPY "${T}/grass.png" DESTINATION "${WORKDIR}/level.assets")
file(WRITE "${WORKDIR}/level.acemap" "{\"format\":\"acestudio.map\",\"version\":1,\"width\":2,\"height\":2,\"tileWidth\":16,\"tileHeight\":16,
 \"sheets\":[{\"name\":\"grass\",\"image\":\"level.assets/grass.png\",\"tileWidth\":16,\"tileHeight\":16}],
 \"layers\":[{\"name\":\"Ground\",\"cells\":[0,0,-1,0]}],\"collision\":\"0011\"}")
run(0 --json map info "${WORKDIR}/level.acemap")
string(JSON solid GET "${LAST_OUT}" solidCells)
string(JSON rects GET "${LAST_OUT}" collisionRects)
if(NOT solid EQUAL 2 OR NOT rects EQUAL 1)
    message(FATAL_ERROR "map info: ${LAST_OUT}")
endif()
run(0 map render "${WORKDIR}/level.acemap" --out "${WORKDIR}/level.png" --collision)
run(0 map export "${WORKDIR}/level.acemap" --out "${WORKDIR}/tiled/level.tmx")
if(NOT EXISTS "${WORKDIR}/tiled/level.tmx" OR NOT EXISTS "${WORKDIR}/tiled/level_tiles0.png")
    message(FATAL_ERROR "map export did not write the Tiled map")
endif()

# Personal use: no licence or trial. An expired trial record left in the data
# folder by an earlier version changes nothing, and the old command is gone.
string(TIMESTAMP now "%s" UTC)
math(EXPR old "(${now} - 20 * 86400) * 1000")
file(WRITE "${WORKDIR}/data/trial.json" "{\"started\":\"old\",\"startedMs\":${old}}")
run(0 new "${WORKDIR}/after-trial.acestudio" --width 8 --height 8)
run(0 render "${P}" --out "${WORKDIR}/after.png")
run(1 licence)

message(STATUS "cli_smoke passed")

