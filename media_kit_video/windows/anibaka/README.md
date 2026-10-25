# Windows video enhancement

This fork owns AniBaka's Windows renderer and DLSS GPU backend. Set
`MEDIA_KIT_VIDEO_ENABLE_DLSS=ON` before adding Flutter plugins. Use
`media_kit_libs_windows_video` from the same revision and set
`MEDIA_KIT_LIBS_ENABLE_ANIMEJANAI=ON` to bundle the pinned AnimeJaNai libmpv.
Both options default to OFF. Other platforms and the upstream Dart API are
unchanged. The host provides no native source replacements or GPU target.

AnimeJaNai, Anime4K and DLSS use D3D11/ANGLE. Completed texture leases survive
resize and disposal; published enhanced textures are immutable. Teardown
releases GPU output and mpv rendering resources before replying to Flutter.
ANGLE resolves `glFinish`/`glFlush` through its EGL resolver, so desktop GL
symbol resolution cannot override frame synchronization in Debug or Release. The native
interop path resolves desktop GL through WGL. Every mpv render API call runs
with its creation context current, including update and frame-info queries.

`VideoStateObserver` owns a separate mpv client and receives typed property
events. Rendering shares immutable media paths and DLSS configuration snapshots.
Frame pacing waits until mpv's target timestamp and wakes on configuration or
disposal. GPU transfers use reusable D3D11 fence events instead of polling.

The `com.anibaka/video_enhancement` channel has one contract:

| Method | Data |
| --- | --- |
| `Video.Properties` | `{handle: int}` returns the current string property map, for initialization and explicit captures. |
| `Video.Property` | Native sends `[handle: int, name: String, value: String]` only on change; playback clock ticks are excluded. |
| `Video.RenderFrame` | `{handle: int}` publishes the completed frame and replies after Flutter requests its synchronized texture. |
| `Dlss.Configure` | `{enabled: false}` releases enhancement resources. When enabled, runtime/cache paths and every effect field are required. |
| `Dlss.Status` | Native sends the changed status; no polling query is needed. |

The developer target `baka_video_pipeline_probe` is excluded from application
builds and bundles. Its source is in `tool/video_pipeline_probe/`.

The renderer originates from media-kit under MIT. The GPU backend in
`dlss_realtime/` retains AniBaka's GPLv3 license; vendor code retains its MIT
notices. Runtime and SDK licenses are installed with the Windows bundle.
