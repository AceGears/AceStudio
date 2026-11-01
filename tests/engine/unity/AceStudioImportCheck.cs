// AceStudio engine-export check for Unity (tests/engine/README.md).
//
// Put this file in Assets/Editor/ of a throwaway Unity project and the
// engine_fixture output in Assets/AceStudio/. After the editor has imported
// everything it writes <project>/acestudio_unity_report.json: every sprite
// Unity made from AceStudio's .meta files (name, rect, border, pivot, pixels
// per unit), the import settings, and every AnimationClip's sprite keys.
// It also paints the fixture's level map (map.json) with AceStudio's Rule
// Tiles (needs com.unity.2d.tilemap.extras) and records the sprite Unity
// chose for every cell.
// tests/engine/compare_unity.py compares the report with AceStudio's files.
// Menu: AceStudio > Write Import Report (to run it again).

using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEngine;
using UnityEngine.Tilemaps;

[InitializeOnLoad]
public static class AceStudioImportCheck
{
    const string Folder = "Assets/AceStudio";

    static AceStudioImportCheck()
    {
        // After the domain reload that follows the import.
        EditorApplication.delayCall += () =>
        {
            if (AssetDatabase.IsValidFolder(Folder)) Write();
        };
    }

    static string F(float v) => v.ToString("0.#####", CultureInfo.InvariantCulture);
    static string Q(string s) => "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";

    [MenuItem("AceStudio/Write Import Report")]
    public static void Write()
    {
        var sb = new StringBuilder();
        sb.Append("{\"unity\":").Append(Q(Application.unityVersion)).Append(",\"textures\":[");
        bool firstTex = true;
        foreach (var guid in AssetDatabase.FindAssets("t:Texture2D", new[] { Folder }).OrderBy(g => AssetDatabase.GUIDToAssetPath(g)))
        {
            string path = AssetDatabase.GUIDToAssetPath(guid);
            var importer = AssetImporter.GetAtPath(path) as TextureImporter;
            var tex = AssetDatabase.LoadAssetAtPath<Texture2D>(path);
            if (!firstTex) sb.Append(',');
            firstTex = false;
            sb.Append("{\"path\":").Append(Q(path)).Append(",\"guid\":").Append(Q(guid));
            if (tex != null) sb.Append(",\"width\":").Append(tex.width).Append(",\"height\":").Append(tex.height);
            if (importer != null)
            {
                sb.Append(",\"textureType\":").Append(Q(importer.textureType.ToString()))
                  .Append(",\"spriteImportMode\":").Append(Q(importer.spriteImportMode.ToString()))
                  .Append(",\"filterMode\":").Append(Q(importer.filterMode.ToString()))
                  .Append(",\"compression\":").Append(Q(importer.textureCompression.ToString()))
                  .Append(",\"mipmaps\":").Append(importer.mipmapEnabled ? "true" : "false")
                  .Append(",\"alphaIsTransparency\":").Append(importer.alphaIsTransparency ? "true" : "false");
            }
            sb.Append(",\"sprites\":[");
            var sprites = AssetDatabase.LoadAllAssetRepresentationsAtPath(path).OfType<Sprite>().OrderBy(s => s.name).ToArray();
            for (int i = 0; i < sprites.Length; ++i)
            {
                var s = sprites[i];
                if (i > 0) sb.Append(',');
                var r = s.rect;
                var b = s.border;   // x = left, y = bottom, z = right, w = top
                sb.Append("{\"name\":").Append(Q(s.name))
                  .Append(",\"rect\":[").Append(F(r.x)).Append(',').Append(F(r.y)).Append(',').Append(F(r.width)).Append(',').Append(F(r.height)).Append(']')
                  .Append(",\"border\":[").Append(F(b.x)).Append(',').Append(F(b.y)).Append(',').Append(F(b.z)).Append(',').Append(F(b.w)).Append(']')
                  .Append(",\"pivot\":[").Append(F(s.pivot.x / r.width)).Append(',').Append(F(s.pivot.y / r.height)).Append(']')
                  .Append(",\"ppu\":").Append(F(s.pixelsPerUnit)).Append('}');
            }
            sb.Append("]}");
        }
        sb.Append("],\"clips\":[");
        bool firstClip = true;
        foreach (var guid in AssetDatabase.FindAssets("t:AnimationClip", new[] { Folder }).OrderBy(g => AssetDatabase.GUIDToAssetPath(g)))
        {
            string path = AssetDatabase.GUIDToAssetPath(guid);
            var clip = AssetDatabase.LoadAssetAtPath<AnimationClip>(path);
            if (clip == null) continue;
            if (!firstClip) sb.Append(',');
            firstClip = false;
            var settings = AnimationUtility.GetAnimationClipSettings(clip);
            sb.Append("{\"path\":").Append(Q(path)).Append(",\"length\":").Append(F(clip.length))
              .Append(",\"frameRate\":").Append(F(clip.frameRate)).Append(",\"loop\":").Append(settings.loopTime ? "true" : "false")
              .Append(",\"bindings\":[");
            var bindings = AnimationUtility.GetObjectReferenceCurveBindings(clip);
            for (int i = 0; i < bindings.Length; ++i)
            {
                var bnd = bindings[i];
                if (i > 0) sb.Append(',');
                sb.Append("{\"type\":").Append(Q(bnd.type.Name)).Append(",\"property\":").Append(Q(bnd.propertyName)).Append(",\"keys\":[");
                var keys = AnimationUtility.GetObjectReferenceCurve(clip, bnd);
                for (int k = 0; k < keys.Length; ++k)
                {
                    if (k > 0) sb.Append(',');
                    var sprite = keys[k].value as Sprite;
                    sb.Append("{\"time\":").Append(F(keys[k].time)).Append(",\"sprite\":").Append(sprite ? Q(sprite.name) : "null")
                      .Append(",\"texture\":").Append(sprite && sprite.texture ? Q(AssetDatabase.GetAssetPath(sprite.texture)) : "null").Append('}');
                }
                sb.Append("]}");
            }
            sb.Append("]}");
        }
        sb.Append("],\"paint\":");
        Paint(sb);
        sb.Append("}");
        string report = Path.Combine(Directory.GetParent(Application.dataPath).FullName, "acestudio_unity_report.json");
        File.WriteAllText(report, sb.ToString());
        Debug.Log("AceStudio import report: " + report);
    }

    // Paints map.json's levels like a user would: level 1 (or the base tile)
    // on one Tilemap, level 2 on another; map row y goes to Unity row -y.
    static void Paint(StringBuilder sb)
    {
        string mapPath = Path.Combine(Folder, "map.json");
        if (!File.Exists(mapPath)) { sb.Append("null"); return; }
        var map = JsonUtility.FromJson<LevelMapJson>(File.ReadAllText(mapPath));
        sb.Append('{');
        bool firstScheme = true;
        foreach (var scheme in new[] { "blob47", "cardinal16" })
        {
            if (!firstScheme) sb.Append(',');
            firstScheme = false;
            sb.Append(Q(scheme)).Append(':');
            var baseTile = AssetDatabase.LoadAssetAtPath<TileBase>(Folder + "/" + scheme + "_base.asset");
            var rule1 = AssetDatabase.LoadAssetAtPath<TileBase>(Folder + "/" + scheme + "_level1.asset");
            var rule2 = AssetDatabase.LoadAssetAtPath<TileBase>(Folder + "/" + scheme + "_level2.asset");
            if (baseTile == null || rule1 == null || rule2 == null)
            {
                sb.Append("{\"error\":").Append(Q("could not load the tiles (is com.unity.2d.tilemap.extras installed?)")).Append('}');
                continue;
            }
            var grid = new GameObject("AceStudioCheckGrid", typeof(Grid));
            try
            {
                var layers = new Tilemap[2];
                for (int l = 0; l < 2; ++l)
                {
                    var go = new GameObject("Level" + (l + 1), typeof(Tilemap));
                    go.transform.SetParent(grid.transform);
                    layers[l] = go.GetComponent<Tilemap>();
                }
                for (int y = 0; y < map.h; ++y)
                    for (int x = 0; x < map.w; ++x)
                    {
                        int level = map.levels[y * map.w + x];
                        var cell = new Vector3Int(x, -y, 0);
                        layers[0].SetTile(cell, level >= 1 ? rule1 : baseTile);
                        if (level >= 2) layers[1].SetTile(cell, rule2);
                    }
                sb.Append("{\"ruleType\":").Append(Q(rule1.GetType().FullName)).Append(",\"levels\":[");
                for (int l = 0; l < 2; ++l)
                {
                    layers[l].RefreshAllTiles();
                    if (l > 0) sb.Append(',');
                    sb.Append('[');
                    for (int y = 0; y < map.h; ++y)
                        for (int x = 0; x < map.w; ++x)
                        {
                            if (y > 0 || x > 0) sb.Append(',');
                            var sprite = layers[l].GetSprite(new Vector3Int(x, -y, 0));
                            sb.Append(sprite ? Q(sprite.name) : "null");
                        }
                    sb.Append(']');
                }
                sb.Append("]}");
            }
            finally
            {
                Object.DestroyImmediate(grid);
            }
        }
        sb.Append('}');
    }

    [System.Serializable]
    class LevelMapJson
    {
        public int w;
        public int h;
        public int[] levels;
    }
}
