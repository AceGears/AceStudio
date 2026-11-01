// AceStudioAutoTile.cs: written by AceStudio next to exported tilesets. You
// may use, change and ship this file with your game. It needs no packages.
//
// One terrain level of an AceStudio Blob47 or Cardinal16 tileset. Paint it on
// a Tilemap: every cell picks its sprite from the cells around it that hold the
// same tile, exactly as AceStudio places them. Paint each level on its own
// Tilemap, lowest first.

using UnityEngine;
using UnityEngine.Tilemaps;

namespace AceStudio
{
    [CreateAssetMenu(menuName = "AceStudio/Autotile", fileName = "Autotile")]
    public class AceStudioAutoTile : TileBase
    {
        public enum Scheme { Cardinal16 = 0, Blob47 = 1 }

        public Scheme scheme = Scheme.Blob47;
        [Tooltip("The tileset's sprites, by AceStudio tile index (Blob47: 47, Cardinal16: 16).")]
        public Sprite[] sprites = new Sprite[0];
        public Tile.ColliderType colliderType = Tile.ColliderType.Sprite;
        public Color color = Color.white;

        // Blob47: N=1 NE=2 E=4 SE=8 S=16 SW=32 W=64 NW=128; a corner counts
        // only when both of its sides do. Tile i has the i-th smallest
        // reduced mask, like AceStudio's tileset order.
        static int[] s_blobIndex;

        static int Reduce(int m)
        {
            bool n = (m & 1) != 0, e = (m & 4) != 0, s = (m & 16) != 0, w = (m & 64) != 0;
            if (!(n && e)) m &= ~2;
            if (!(s && e)) m &= ~8;
            if (!(s && w)) m &= ~32;
            if (!(n && w)) m &= ~128;
            return m;
        }

        static int BlobIndex(int mask)
        {
            if (s_blobIndex == null)
            {
                var index = new int[256];
                var used = new bool[256];
                for (int m = 0; m < 256; ++m) used[Reduce(m)] = true;
                int next = 0;
                for (int m = 0; m < 256; ++m) index[m] = used[m] ? next++ : -1;
                s_blobIndex = index;
            }
            return s_blobIndex[Reduce(mask)];
        }

        bool Same(ITilemap tilemap, Vector3Int p) => tilemap.GetTile(p) == this;

        // The AceStudio tile index for the cell at p (Unity's y points up).
        public int TileIndex(ITilemap tilemap, Vector3Int p)
        {
            bool n = Same(tilemap, p + Vector3Int.up), e = Same(tilemap, p + Vector3Int.right);
            bool s = Same(tilemap, p + Vector3Int.down), w = Same(tilemap, p + Vector3Int.left);
            if (scheme == Scheme.Cardinal16) return (n ? 1 : 0) | (e ? 2 : 0) | (s ? 4 : 0) | (w ? 8 : 0);
            int m = (n ? 1 : 0) | (e ? 4 : 0) | (s ? 16 : 0) | (w ? 64 : 0);
            if (Same(tilemap, p + new Vector3Int(1, 1, 0))) m |= 2;
            if (Same(tilemap, p + new Vector3Int(1, -1, 0))) m |= 8;
            if (Same(tilemap, p + new Vector3Int(-1, -1, 0))) m |= 32;
            if (Same(tilemap, p + new Vector3Int(-1, 1, 0))) m |= 128;
            return BlobIndex(m);
        }

        public override void RefreshTile(Vector3Int position, ITilemap tilemap)
        {
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                    tilemap.RefreshTile(position + new Vector3Int(x, y, 0));
        }

        public override void GetTileData(Vector3Int position, ITilemap tilemap, ref TileData tileData)
        {
            int i = TileIndex(tilemap, position);
            tileData.sprite = i >= 0 && i < sprites.Length ? sprites[i] : null;
            tileData.color = color;
            tileData.transform = Matrix4x4.identity;
            tileData.flags = TileFlags.LockTransform;
            tileData.colliderType = colliderType;
        }
    }
}
