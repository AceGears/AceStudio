# Loads AceStudio's exported resources in Godot and reports what Godot sees:
#   godot --headless --path <project> --script res://check.gd -- <result.json>
# The fixture (engine_fixture) must have been copied to res://fx first and the
# project imported (godot --headless --path <project> --import).
extends SceneTree

func _report_sprite_frames(path: String) -> Dictionary:
	var sf: SpriteFrames = load(path)
	var out := {}
	for anim in sf.get_animation_names():
		var frames := []
		for i in sf.get_frame_count(anim):
			var tex: AtlasTexture = sf.get_frame_texture(anim, i)
			frames.append({
				"duration": sf.get_frame_duration(anim, i),
				"region": [tex.region.position.x, tex.region.position.y, tex.region.size.x, tex.region.size.y],
				"atlas": tex.atlas.resource_path,
				"atlas_size": [tex.atlas.get_width(), tex.atlas.get_height()],
			})
		out[String(anim)] = {"speed": sf.get_animation_speed(anim), "loop": sf.get_animation_loop(anim), "frames": frames}
	return out

func _report_stylebox(path: String) -> Dictionary:
	var sb: StyleBoxTexture = load(path)
	return {
		"margins": [sb.texture_margin_left, sb.texture_margin_top, sb.texture_margin_right, sb.texture_margin_bottom],
		"region": [sb.region_rect.position.x, sb.region_rect.position.y, sb.region_rect.size.x, sb.region_rect.size.y],
		"texture_size": [sb.texture.get_width(), sb.texture.get_height()],
	}

func _report_tileset(path: String, map: Dictionary) -> Dictionary:
	var ts: TileSet = load(path)
	var out := {"terrain_sets": ts.get_terrain_sets_count(), "sources": ts.get_source_count(), "modes": [], "terrains": [], "tiles": 0}
	for s in ts.get_terrain_sets_count():
		out.modes.append(ts.get_terrain_set_mode(s))
		var names := []
		for t in ts.get_terrains_count(s):
			names.append(ts.get_terrain_name(s, t))
		out.terrains.append(names)
	for i in ts.get_source_count():
		var src: TileSetAtlasSource = ts.get_source(ts.get_source_id(i))
		out.tiles += src.get_tiles_count()
	# Paint the fixture's level map with Godot's terrain tools, one layer per
	# level, and read back which tile Godot chose for every cell.
	var w: int = map.w
	var h: int = map.h
	var levels: Array = map.levels
	var placed := []
	for level in [1, 2]:
		var layer := TileMapLayer.new()
		layer.tile_set = ts
		get_root().add_child(layer)
		var cells: Array[Vector2i] = []
		var everything: Array[Vector2i] = []
		# Godot re-evaluates cells around painted ones and clears those at the
		# edge of the painted area (next to empty cells), as in the editor, so
		# the base goes 2 cells beyond the map.
		for y in range(-2, h + 2):
			for x in range(-2, w + 2):
				everything.append(Vector2i(x, y))
		for y in h:
			for x in w:
				if int(levels[y * w + x]) >= level:
					cells.append(Vector2i(x, y))
		if level == 1:
			layer.set_cells_terrain_connect(everything, 0, 0)    # base everywhere
			layer.set_cells_terrain_connect(cells, 0, 1)
		else:
			layer.set_cells_terrain_connect(cells, 1, 0)
		var grid := []
		for y in h:
			for x in w:
				var c := Vector2i(x, y)
				var src := layer.get_cell_source_id(c)
				var ac := layer.get_cell_atlas_coords(c)
				grid.append([src, ac.x, ac.y])
		placed.append(grid)
	out.placed = placed
	return out

func _init() -> void:
	var args := OS.get_cmdline_user_args()
	var result := {"godot": Engine.get_version_info().string}
	var map_text := FileAccess.get_file_as_string("res://fx/map.json")
	var map: Dictionary = JSON.parse_string(map_text)
	result.sprite_frames = _report_sprite_frames("res://fx/hero.tres")
	result.stylebox_panel = _report_stylebox("res://fx/ui_panel.tres")
	result.stylebox_button = _report_stylebox("res://fx/ui_button_normal.tres")
	for scheme in ["blob47", "wang16", "cardinal16"]:
		result[scheme] = _report_tileset("res://fx/%s.tres" % scheme, map)
	var f := FileAccess.open(args[0], FileAccess.WRITE)
	f.store_string(JSON.stringify(result, "  "))
	f.close()
	quit()
