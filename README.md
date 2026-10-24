# omp-overlay-wine

This DLL makes the launcher overlay work under Wine. The launcher shares
overlay frames with the game. When `omp-client.dll` calls
`USER32!PrintWindow`, the DLL copies a frame into the game's bitmap. If no frame
is available, it calls the original function.

## Build

Run the **Build Wine overlay DLL** workflow from the Actions tab to build a
32-bit Windows DLL with MSVC. Download `omp-wine.dll` from that run's
**Artifacts** section. Check **Publish this build as a GitHub Release** if you
want to keep it as a release asset.

To build it locally, use an x86 Native Tools Command Prompt for Visual Studio:

```bat
cl /nologo /LD /MT /O2 /W3 /TC ompwine.c /link /OUT:omp-wine.dll gdi32.lib
```
