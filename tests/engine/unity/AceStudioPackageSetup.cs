// Adds com.unity.2d.tilemap.extras (for AceStudio's Rule Tiles) to the
// throwaway check project the first time it is opened, through the Package
// Manager API (tests/engine/README.md). Test projects only.

using System.IO;
using UnityEditor;
using UnityEditor.PackageManager;
using UnityEditor.PackageManager.Requests;
using UnityEngine;

[InitializeOnLoad]
public static class AceStudioPackageSetup
{
    const string Package = "com.unity.2d.tilemap.extras";
    static AddRequest s_request;

    static AceStudioPackageSetup()
    {
        if (File.ReadAllText("Packages/manifest.json").Contains(Package)) return;
        Debug.Log("AceStudio check: adding " + Package);
        s_request = Client.Add(Package);
        EditorApplication.update += Poll;
    }

    static void Poll()
    {
        if (s_request == null || !s_request.IsCompleted) return;
        EditorApplication.update -= Poll;
        if (s_request.Status == StatusCode.Success)
            Debug.Log("AceStudio check: added " + s_request.Result.name + "@" + s_request.Result.version);
        else
            Debug.LogError("AceStudio check: could not add " + Package + ": " + s_request.Error?.message);
    }
}
