# Startup runtime stand-ins (experimental)

This page describes the experimental desktop stand-ins under `src/startup_stubs/` and the tools
under `tools/startup/`. They let a console executable start on desktop Windows under a debugger,
so that its startup path can be studied one missing piece at a time.

This is not a port and not an emulator. No executable is playable. Every stand-in is opt-in, and
most answers are shortcuts that are logged as such. The open problems are tracked in the issues
whose titles start with `startup-runtime:`.

## What is measured

With the switches below, an x64 console executable on an AMD RDNA4 GPU (Windows 11, release
build of this branch):

- presents frames at about 29 fps through the D3D12 stand-in, and its translated UI draws reach the
  output window;
- reads the virtual gamepad and answers the first-run user prompt for one local user;
- streams data through the DirectStorage stand-in (about 20 000 requests in the first two minutes,
  zlib-inflated on the CPU);
- plays its intro video through the GPU-view mirror (colours are still wrong, see the issues);
- then stops in its texture-streaming path: either at a soft assert (an `int3` the title steps
  over when no debugger is attached), or, past that, on a black screen.

## Build

The stand-ins are part of the normal build. From a developer prompt:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release --no-tests=error -R '^startup_stubs$' -V
```

The build produces `xmem.dll`, `d3d12_xs.dll`, `AcpHal.dll`, `XCRDAPI.dll`, `chad.dll`, `xapu.dll`,
`xg_xs.dll` and `GameInput.dll`. Without the switches, every export that has no implementation
traps: it logs `x2w-trap <dll>!<export>` with its argument registers and ends the process with a
code that names the export (see `src/startup_stubs/trap.h`).

## Run

Prepare a folder with the executable and its package DLLs yourself. Never commit or upload it.
Then:

```powershell
.\tools\startup\run.ps1 -Exe <folder>\<title>.exe -DataDir <package root> -LogDir out\startup\run1 `
    -Seconds 180 -Render -FakeUser -GpuMirror -ContinueAsserts 1000 -InputScript "4@70"
```

`run.ps1` copies the stand-ins next to the executable and starts `tools/startup/debug_launch.py`.
The debugger runs the process in a kill-on-close job with a memory cap, **offline**: file opens of
socket, NetBIOS and DNS devices are refused (`--offline 0` turns this off; do not). It writes:

| File | Content |
|---|---|
| `<log>.out` | why the run stopped: exception, registers, stack, likely return addresses, raw stack |
| `<log>/events.txt` | module loads and every exception with its time |
| `<log>/stderr.txt` | the stand-ins' logs (`x2w-d3d12`, `x2w-dstorage`, `x2w-xmem`, `x2w-runtime`, ...) |
| `<log>/files.txt` | every file the process opened |

`tools/startup/live_view.py` follows a run in a second console. `X2W_TRAP_DUMP="exe+RVA:len,reg+off:len"`
adds memory dumps to the stop report.

## Switches

| Variable | Effect |
|---|---|
| `X2W_XMEM_PERMISSIVE=1` | accepts the reservation, mapping and allocation shapes a startup run uses |
| `X2W_XMEM_GPU_MIRROR=1` | pairs a no-access graphics reservation with the read-write reservation of the same size made just before it, and maps every physical page into both (same section) |
| `X2W_D3D12_PERMISSIVE=1` | wraps the device: recording command lists, console present, view tracking |
| `X2W_D3D12_RENDER=1` | draws through translated pipelines from `X2W_SHADER_CACHE` |
| `X2W_SHADER_CACHE=<dir>` | translation cache: the runtime writes `req_<key>.{a,b,root}` for untranslated pipelines; `tools/startup/gfx2hlsl.py --pending <dir>` translates them |
| `X2W_DRAW_NULL_IDENTITY=1` | null descriptors read as identity values instead of zero |
| `X2W_D3D12_DUMP_SURFACE=<bmp>` | writes the last presented surface |
| `X2W_DRAW_DUMP_FRAME=<n>`, `X2W_DRAW_DUMP_DIR` | Full-resolution RGB snapshots of single-sample RGBA8/BGRA8 UNORM or sRGB targets after translated draws. A later present writes completed queue-fenced copies. |
| `X2W_AUDIO_PERMISSIVE=1` | audio HAL / APU stand-ins accept their calls (no output) |
| `X2W_RUNTIME_PROXY=1` | proxies the gaming runtime and logs every call |
| `X2W_FAKE_USER=1` | answers the user, gamertag and save APIs for one local user |
| `X2W_STORAGE_PATH`, `X2W_STORAGE_MB`, `X2W_TITLE_ID` | persistent local storage and the manifest values the runtime would return |
| `X2W_INPUT_SCRIPT=mask@sec,...` | timed virtual gamepad presses (0x4 = A); keyboard and XInput pad 0 also work |
| `X2W_XG_VERBOSE=1` | logs every texture layout query with its mip sizes |
| `X2W_TRAP_CONTINUE=<exports>` | lets listed trap exports return 0 instead of ending the process |

## Findings

These are measured on one executable; treat them as hypotheses for others.

- **User API, second interface.** The user provider hands out a second interface for the
  gamertag (method 3: handle, component, size, buffer, used). It must be answered separately from
  the main user object, whose method 3 is the handle duplicate. Without a gamertag, the first-run
  prompt offers no player and never advances.
- **GPU-view reservations.** The title reserves the same large range twice in a row: read-write,
  then no-access with the graphics flag. It maps physical pages only into the first, yet places GPU
  resources (for example the video frame planes) at the same offset in the second. On the console
  the second is a GPU view of the first. Mapping each page's section into both views makes those
  resources readable; without it, a YUV video shows a solid green screen (all-zero planes).
- **DirectStorage ordering.** Requests must run after `Submit`, in order with status and fence
  entries. Titles fill destination page-array entries after `EnqueueRequest`; an entry that is 0
  at enqueue is read again when the request runs.
- **Soft asserts.** Retail code keeps `int3` instructions followed by a continuation. Without a
  debugger the title's handler steps over them; under the startup debugger use
  `-ContinueAsserts N`. One of them fires about ten times a second in the texture-streaming path
  (a chunked read whose length ends inside a compressed chunk); its cause is open.
- **Struct-returning methods.** When both register arguments of a wrapped method look like objects,
  the one on the calling thread's stack is the return buffer.
- **Inline samplers.** Pixel shaders build sampler descriptors from constants (`s_movk_i32`,
  `s_bfm_b32`, 64-bit moves); the translator folds those into static samplers.

## Shortcuts that are known to be wrong

- BCPack requests are completed with zero-filled destinations (blank textures).
- XG layouts are the desktop copyable-footprint layout, not the console tiling.
- Dynamically indexed sampler-heap loads become a default sampler.
- Stream pipelines (console subobject streams) have no shader translation.
- Audio produces no output.
