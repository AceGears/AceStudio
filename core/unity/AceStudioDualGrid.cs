// AceStudioDualGrid.cs: written by AceStudio next to exported tilesets. You
// may use, change and ship this file with your game. It needs no packages.
//
// Draws an AceStudio Wang16 tileset on the dual grid, as AceStudio does. Add
// it to a Tilemap (under a Grid) and set its Wang Set. Paint that Tilemap with
// the set's terrain and ground tiles; a child Tilemap, offset by half a cell,
// shows the corner tiles and updates as you paint. Use one per terrain level.

using UnityEngine;
using UnityEngine.Tilemaps;

namespace AceStudio
{
    [ExecuteAlways]
    [RequireComponent(typeof(Tilemap))]
    public class AceStudioDualGrid : MonoBehaviour
    {
        public AceStudioWangSet wangSet;
        [Tooltip("Hide the painted cells and show only the corner tiles.")]
        public bool hideCells = true;

        const string DisplayName = "AceStudio Dual Grid";
        Tilemap m_cells;
        Tilemap m_display;

        // The Tilemap that shows the corner tiles.
        public Tilemap Display
        {
            get { EnsureDisplay(); return m_display; }
        }

        bool m_needsRebuild;

        // Tiles may not change during Awake / OnEnable / OnValidate, so the
        // first build waits until right after (the editor) or Update (play).
        void OnEnable()
        {
            Tilemap.tilemapTileChanged += OnTilesChanged;
            RequestRebuild();
        }

        void OnDisable()
        {
            Tilemap.tilemapTileChanged -= OnTilesChanged;
        }

        void OnValidate()
        {
            RequestRebuild();
        }

        void Update()
        {
            if (m_needsRebuild) Rebuild();
        }

        void RequestRebuild()
        {
            m_needsRebuild = true;
#if UNITY_EDITOR
            UnityEditor.EditorApplication.delayCall += () =>
            {
                if (this != null && isActiveAndEnabled && m_needsRebuild) Rebuild();
            };
#endif
        }

        void EnsureDisplay()
        {
            if (m_cells == null) m_cells = GetComponent<Tilemap>();
            if (m_display != null) return;
            Transform t = transform.Find(DisplayName);
            GameObject go = t != null ? t.gameObject : new GameObject(DisplayName, typeof(Tilemap), typeof(TilemapRenderer));
            go.transform.SetParent(transform, false);
            m_display = go.GetComponent<Tilemap>();
            var own = GetComponent<TilemapRenderer>();
            var shown = go.GetComponent<TilemapRenderer>();
            if (own != null && shown != null)
            {
                shown.sortingLayerID = own.sortingLayerID;
                shown.sortingOrder = own.sortingOrder;
            }
        }

        // Rebuilds every corner tile from the painted cells.
        public void Rebuild()
        {
            m_needsRebuild = false;
            EnsureDisplay();
            Vector3 cell = m_cells.layoutGrid != null ? m_cells.layoutGrid.cellSize : Vector3.one;
            m_display.transform.localPosition = new Vector3(-0.5f * cell.x, -0.5f * cell.y, 0f);
            var own = GetComponent<TilemapRenderer>();
            if (own != null) own.enabled = !hideCells || wangSet == null;
            m_display.ClearAllTiles();
            if (wangSet == null) return;
            m_cells.CompressBounds();
            BoundsInt b = m_cells.cellBounds;
            for (int y = b.yMin; y <= b.yMax; ++y)
                for (int x = b.xMin; x <= b.xMax; ++x)
                    UpdateCorner(new Vector3Int(x, y, 0));
        }

        void OnTilesChanged(Tilemap tilemap, Tilemap.SyncTile[] changes)
        {
            if (tilemap != m_cells || wangSet == null || m_needsRebuild) return;   // the rebuild covers it
            EnsureDisplay();
            foreach (var c in changes)
                for (int dy = 0; dy <= 1; ++dy)
                    for (int dx = 0; dx <= 1; ++dx)
                        UpdateCorner(c.position + new Vector3Int(dx, dy, 0));
        }

        // Corner tile (x, y) touches cells (x-1, y) top-left, (x, y) top-right,
        // (x, y-1) bottom-right and (x-1, y-1) bottom-left.
        void UpdateCorner(Vector3Int p)
        {
            TileBase tl = m_cells.GetTile(p + Vector3Int.left), tr = m_cells.GetTile(p);
            TileBase br = m_cells.GetTile(p + Vector3Int.down), bl = m_cells.GetTile(p + new Vector3Int(-1, -1, 0));
            if (tl == null && tr == null && br == null && bl == null)
            {
                m_display.SetTile(p, null);
                return;
            }
            int mask = (tl == wangSet.terrain ? 1 : 0) | (tr == wangSet.terrain ? 2 : 0) |
                       (br == wangSet.terrain ? 4 : 0) | (bl == wangSet.terrain ? 8 : 0);
            m_display.SetTile(p, mask < wangSet.tiles.Length ? wangSet.tiles[mask] : null);
        }
    }
}
