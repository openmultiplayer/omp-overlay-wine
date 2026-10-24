# omp-overlay-wine

This DLL makes the launcher overlay work under Wine. The launcher shares
overlay frames with the game. When `omp-client.dll` calls
`USER32!PrintWindow`, the DLL copies a frame into the game's bitmap. If no frame
is available, it calls the original function.
