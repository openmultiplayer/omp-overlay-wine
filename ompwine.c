/* Under Wine, redirects omp-client.dll's USER32!PrintWindow import to copy
 * shared frames from Local\omp_overlay_frame_<pid> into the game's DIB.
 * Calls the original PrintWindow if no frame is ready.
 *
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define FRAME_MAGIC 0x46504D4Fu /* "OMPF" */
#define FRAME_HEADER 32
#define FRAME_MAX_W 3840
#define FRAME_MAX_H 2160
#define TARGET_DLL "omp-client.dll"

typedef BOOL(WINAPI *PrintWindowFn)(HWND, HDC, UINT);

static PrintWindowFn real_print_window;
static const volatile DWORD *frame;
static SIZE_T frame_size; /* size of the mapped view */

static const volatile DWORD *open_frames(void)
{
    char name[64];
    HANDLE mapping;
    const volatile DWORD *view;
    MEMORY_BASIC_INFORMATION info;

    if (frame)
        return frame;
    _snprintf(name, sizeof name, "Local\\omp_overlay_frame_%lu", GetCurrentProcessId());
    name[sizeof name - 1] = 0;
    mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!mapping)
        return NULL;
    view = (const volatile DWORD *)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping); /* the view keeps the mapping alive */
    if (!view)
        return NULL;
    if (!VirtualQuery((const void *)view, &info, sizeof info)) {
        UnmapViewOfFile((const void *)view);
        return NULL;
    }
    frame_size = info.RegionSize;
    frame = view;
    return frame;
}

/* Copy the newest frame into the DIB selected into hdc. 0: nothing to copy. */
static int copy_frame(HDC hdc)
{
    const volatile DWORD *f = open_frames();
    DIBSECTION ds;
    HGDIOBJ bmp;
    int attempt;

    if (!f || frame_size < FRAME_HEADER || f[0] != FRAME_MAGIC || f[6] != GetCurrentProcessId())
        return 0;

    bmp = GetCurrentObject(hdc, OBJ_BITMAP);
    if (!bmp || GetObjectA(bmp, sizeof ds, &ds) != (int)sizeof ds || !ds.dsBm.bmBits ||
        ds.dsBm.bmBitsPixel != 32)
        return 0;

    for (attempt = 0; attempt < 3; attempt++) {
        DWORD seq, w, h, stride, dw, dh, cw, ch, y;
        unsigned char *dst;
        const unsigned char *src;

        seq = f[5];
        MemoryBarrier();
        if (seq & 1) {
            Sleep(0);
            continue; /* the launcher is writing a frame */
        }

        /* Never trust the sizes in the block: check them against the view */
        w = f[2];
        h = f[3];
        stride = f[4];
        if (w == 0 || h == 0 || w > FRAME_MAX_W || h > FRAME_MAX_H || stride < w * 4 ||
            (SIZE_T)stride * h > frame_size - FRAME_HEADER)
            return 0;

        dw = (DWORD)ds.dsBm.bmWidth;
        dh = (DWORD)ds.dsBm.bmHeight;
        cw = w < dw ? w : dw;
        ch = h < dh ? h : dh;
        dst = (unsigned char *)ds.dsBm.bmBits;
        src = (const unsigned char *)f + FRAME_HEADER;
        /* omp-client's DIB is top-down, like the frames */
        for (y = 0; y < ch; y++)
            memcpy(dst + (size_t)y * ds.dsBm.bmWidthBytes, src + (size_t)y * stride, (size_t)cw * 4);

        MemoryBarrier();
        if (f[5] == seq) {
            GdiFlush();
            return 1;
        }
        /* a new frame was written during the copy: copy again */
    }
    return 1; /* keep the DIB as it is, the next capture replaces it */
}

static BOOL WINAPI hooked_print_window(HWND hwnd, HDC hdc, UINT flags)
{
    if (copy_frame(hdc))
        return TRUE;
    return real_print_window(hwnd, hdc, flags);
}

/* Point USER32!PrintWindow in the module's import table at ours. */
static int patch_import(HMODULE module)
{
    unsigned char *base = (unsigned char *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;

    if (!dir.VirtualAddress)
        return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *slots;
        if (_stricmp((const char *)base + d->Name, "USER32.dll") != 0)
            continue;
        names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            IMAGE_IMPORT_BY_NAME *by_name;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)by_name->Name, "PrintWindow") != 0)
                continue;
            if ((void *)slots->u1.Function == (void *)hooked_print_window)
                return 1; /* already ours */
            if (!VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, PAGE_READWRITE, &old))
                return 0;
            real_print_window = (PrintWindowFn)slots->u1.Function;
            slots->u1.Function = (ULONG_PTR)hooked_print_window;
            VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, old, &old);
            return 1;
        }
    }
    return 0;
}

static int is_wine(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
}

/* The launcher injects this once omp-client has connected to it, so
 * omp-client.dll is normally loaded already; wait for it all the same. */
static DWORD WINAPI watch(void *unused)
{
    int i;
    (void)unused;
    if (!is_wine())
        return 0;
    for (i = 0; i < 1200; i++) { /* two minutes */
        HMODULE omp = GetModuleHandleA(TARGET_DLL);
        if (omp) {
            if (patch_import(omp))
                OutputDebugStringA("[ompwine] serving overlay frames to " TARGET_DLL "\n");
            else
                OutputDebugStringA("[ompwine] " TARGET_DLL " imports no PrintWindow\n");
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE thread;
        DisableThreadLibraryCalls(inst);
        thread = CreateThread(NULL, 0, watch, NULL, 0, NULL);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
