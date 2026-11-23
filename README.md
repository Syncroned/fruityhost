# fruityhost

An experimental standalone host for Image-Line **Fruity** plug-ins — the
Delphi/VCL `*_x64.dll` effects and generators that ship with FL Studio —
outside of FL Studio.

Fruity plug-ins aren't VSTs. They export

```
TFruityPlug* __stdcall CreatePlugInstance(TFruityPlugHost* host, intptr_t tag)
```

and expect a Delphi object with a specific field/vtable layout as `host`.
This project impersonates that object in C++: matching field layout for the
SDK-documented `TFruityPlugHost`, a dispatcher vtable for host/service calls,
and enough services (resources, licence attest, file mirroring, the FHD_71
plug-in-manager factory that Patcher uses for nested plug-ins) to make most
FL Studio 2025 effects construct, process, save state, and open their VCL
editor forms inside a plain Win32 window.

## Build

Windows x64, MSVC, CMake ≥ 3.28, static CRT:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Run

```
fruityhost.exe                  # file dialog to pick a *_x64.dll
fruityhost.exe <path\to.dll>    # skip the dialog
```

The dialog starts in the newest `FL Studio*\Plugins\Fruity` folder it can
find. The plug-in's editor opens in a window with a Presets menu and `.fst`
drag & drop. Audio is fed silence at 48 kHz / 512 frames — this is a
UI/host-protocol testbed, not a mixer.

`MP_FRUITY_DEBUGLOG=<path>` enables a full call/file/registry trace of the
host protocol — the primary reverse-engineering tool.

## What's interesting

- **Delphi ABI in C++.** `fruitynode.cpp` impersonates Delphi classes:
  user virtuals sit at vtable[0..] (TObject's own methods live at negative
  VMT offsets), so a C++ object with matching layout passes `Supports()`
  checks and serves methods directly.
- **Service objects.** Undocumented host dispatcher calls return interface
  objects, not scalars — plug-ins call further methods on the results, so
  every unknown service gets a generic object, never a bare value.
- **FHD_71 plug-in factory.** Patcher is a recursive host: after it
  `LoadLibrary`s a sub-plugin module it asks this service to instantiate it
  (op `0x40F`), receiving the created object's first 0x400 bytes.
- **VCL compatibility fixes.** IAT patches (opt out with
  `MP_FRUITY_NOCOMPAT=1`) make `DestroyWindow`/`PostMessage` on dead HWNDs
  and a `VirtualFree` on a shared-singleton heap region succeed, matching
  what FL's process provides implicitly.
- **Writable FL mirror.** Plug-ins resolve and write resources relative to
  `GetProgPath`, but `Program Files` is read-only — the host hands out an
  NTFS-junction mirror under `%LOCALAPPDATA%` instead.
- **`.fst` presets.** `fstfile.cpp` parses FL's `FLhd`/`FLdt` preset files;
  plug-ins that take files (Patcher) get them via `FPD_LoadFile`, others get
  the state chunk via `SaveRestoreState`.

## Layout

| file | role |
| --- | --- |
| `main.cpp` | file picker + host stub + message loop |
| `fruitynode.*` | the Fruity host: Delphi impersonation, services, mirror, editor |
| `presetui.*` | Presets menu + `.fst` drag & drop on editor windows |
| `fstfile.*` | `.fst` preset parser |
| `node.*`, `rt.h`, `mp_engine.h` | minimal node/host plumbing the host builds against |

Protocol notes inline in `fruitynode.cpp` were verified empirically against
FL Studio 2025 and instrumented probes; expect gaps.

Extracted from the Phonic audio engine as a standalone experiment.
