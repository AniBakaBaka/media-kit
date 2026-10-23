# AniBaka Windows video output

Based on the Windows sources in `media_kit_video` **2.0.1**, distributed under
the included MIT license. Upstream: https://github.com/media-kit/media-kit.

This fork selects these sources when the host sets
`MEDIA_KIT_VIDEO_ENABLE_DLSS=ON` before adding Flutter plugins. The package
builds its own `baka_dlss_gpu` target from `dlss_realtime/`, including its
public `video_bridge.h` header, fixed NGX SDK downloads and license notices.
The migrated GPU backend retains AniBaka's GPLv3 license in that directory;
the original renderer and vendor components retain their existing MIT licenses.
No host-owned renderer sources or post-registration source overrides are needed.
The option defaults to OFF and builds the original upstream Windows sources.
Other platforms and the upstream Dart API are unchanged. Reconcile this
directory against upstream when upgrading the plugin.

For AnimeJaNai, also use `media_kit_libs_windows_video` from the same fork
revision and set `MEDIA_KIT_LIBS_ENABLE_ANIMEJANAI=ON` before adding Flutter
plugins. That package owns the verified x64 libmpv archive, import library,
headers, extraction tool and corresponding runtime notices. Inference libraries
and models continue to be installed on demand by the application's Dart layer.

AnimeJaNai, Anime4K and DLSS share the D3D11/ANGLE presentation path.
`VideoStateObserver` uses a separate mpv client and typed property events;
Flutter reads `AnimeJaNai.Properties` snapshots without synchronous UI-thread
mpv getters. Timing changes use the observer's asynchronous property API.
Resizing retains the registered Flutter texture, and completed frame leases
keep published resources alive across output changes. Disposal responds only
after GPU output and mpv render resources have been released with a valid GL
context. The developer target `baka_video_pipeline_probe` is excluded from
normal application builds and bundles; its source is in `tool/video_pipeline_probe`.

AniBaka changes:

- `Dlss.Configure` / `Dlss.Status` methods control all current and future outputs.
- Each hardware video output can use `baka_dlss_gpu` for NR, 2x SR and 2x FG.
  Shared GPU textures cross between D3D11 and D3D12; video pixels are not read
  back to the CPU. Flutter frame leases retain the resources across resize.
  D3D11 creates the interchange textures and exports NT handles for D3D12 to
  import. NGX writes to private D3D12 UAVs, then copies to those shared textures;
  D3D11 does not need to import native D3D12 UAV resources.
  All copies and GPU waits execute on the video worker. Flutter acquires a
  completed, immutable frame and never waits for enhancement.
  The bridge waits on D3D11 fence events with a 2 second timeout, avoiding
  polling sleeps. ANGLE producer completion retains its separate query below.
  D3D12 alternates the two input color textures instead of copying history.
  Embedded playback allocates no NV12 upload buffers, and submission accounting
  drains completed entries even when standalone frame counters are never read.
  Each published output has its own BGRA texture. Flutter's release_callback
  runs after opening the handle, before sampling, so shared_ptr counts cannot
  determine when a texture is safe to overwrite. Published textures are never
  rewritten; ANGLE/Direct3D retain their own resources while using them.
  Frame leases retain only a completed texture, never an NGX instance. NGX
  teardown runs outside the texture mutex. While DLSS is enabled, ANGLE flushes
  its GL commands, then queries completion on its actual D3D11 immediate context
  with a 2 second timeout. This does not require EGL_KHR_fence_sync, which the
  bundled ANGLE backend does not advertise. A local 64x64 / 128x64 synthetic
  completion probe passed all eight frames, including resize (2026-09-27).
  This probe does not load NGX or validate enhanced video playback.
  A subsequent synthetic pixel probe (2026-09-27) reproduced EGL_BAD_PARAMETER
  when importing the RGBA enhanced texture. Presentation now converts RGBA to
  BGRA in a D3D11 shader, matching the ordinary Flutter/ANGLE surface format.
  Three-frame NR-only and NR+SR+FG runs produced non-black output whose pixels
  matched after Flutter-style EGL import on RTX 4070 Laptop. These are synthetic
  checks, not video quality, interpolation cadence, or audio-sync validation.
  The texture descriptor lives with the registered texture; release_callback
  releases only its frame lease because Flutter reads descriptor dimensions
  after invoking that callback.
- mpv keeps demux, HTTP headers, decoding, audio, tracks and player controls.
  Output uses `NEXT_FRAME_INFO.target_time` from mpv's audio clock and skips late
  generated frames. It does not enable mpv's advanced-control mode.
- Render notifications are coalesced. Seek/redraw/source/size changes reset
  enhancement history; unsupported inputs or NGX failures fall back to mpv.
  Repeated frames reuse completed output instead of resetting temporal history.
  A missing target timestamp alone does not reset consecutive video samples.
- The player toggle retains one pipeline per video; the laboratory master
  switch releases it. Reopening an ANGLE surface rebinds its shared input.
  Intensity changes recreate only NR. Initialized FG can be bypassed/resumed;
  SR output size changes still rebuild. Presentation sharpness updates a small
  constant buffer with bounded overshoot and does not recreate NGX features.
- Embedded SR consumes the original input by default. NR is an independent,
  optional pre-pass; the standalone player retains its previous NR behavior.
  Split comparison shows linear input scaling on the left and NVIDIA output
  on the right. During generated midpoints the OFF half holds the previous
  original image; the ON half advances to the generated image. Real outputs
  advance both halves and replace the reference for the next midpoint.
  A local GDI atlas supplies gray RTX DLSS 5 OFF and green RTX DLSS 5 ON
  panels, input/output dimensions, and a digit strip for live submission FPS.
  Distinct successful outputs are counted over one-second windows; duplicate
  repainting is excluded. OFF counts real outputs, ON includes generated
  outputs; these are update rates, not independent OFF/ON benchmark rates.
  Status distinguishes FG suppression, late output, timing and missing references.
  Immutable frame sequence IDs count unique Flutter reads of generated output.
  The shader composes digits from scalar constants;
  static text/icons are rasterized only on first comparison use.
- The renderer must already run on NVIDIA. On hybrid systems the Windows
  Graphics preference may need to be set to High performance for `baka.exe`.

Build validation does not verify visual quality, A/V synchronization, or actual
frame generation on a GPU. Video-derived motion and flat depth remain experimental.

The regression probe in `tool/dlss_texture_lifetime_probe` imports an output
through EGL, releases the producer lease, publishes 30 later frames, and checks
all original pixels again, including after pipeline disposal. The former pool
overwrote the imported frame; immutable publication passes the same scenario.
Previous throughput results measured that faulty pool and do not establish
correct playback or describe this revised allocation path's performance.
