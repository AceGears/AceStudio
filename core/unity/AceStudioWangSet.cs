// AceStudioWangSet.cs: written by AceStudio next to exported tilesets. You
// may use, change and ship this file with your game. It needs no packages.
//
// One level of an AceStudio Wang16 (corner / dual grid) tileset, used by
// AceStudioDualGrid. Paint `terrain` where the terrain is and `ground` around
// it; the dual grid draws the 16 corner tiles between the cells.

using UnityEngine;
using UnityEngine.Tilemaps;

namespace AceStudio
{
    [CreateAssetMenu(menuName = "AceStudio/Wang Set", fileName = "Wang Set")]
    public class AceStudioWangSet : ScriptableObject
    {
        [Tooltip("Paint this where the terrain is.")]
        public TileBase terrain;
        [Tooltip("Paint this where the terrain is not (cells left empty draw nothing).")]
        public TileBase ground;
        [Tooltip("The 16 corner tiles by corner mask: top-left 1, top-right 2, bottom-right 4, bottom-left 8.")]
        public TileBase[] tiles = new TileBase[16];
    }
}
