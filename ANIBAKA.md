# AniBaka compatibility branch

Based on upstream commit `5a50cb2fc92d8d53cbfb78f46d705c92fccce5c9`
(`media_kit_video` 2.0.1). The Dart controls exports match the published 2.0.1
package used by AniBaka, which omits the unfinished Cupertino controls.

The Windows plugin supports an optional AniBaka video bridge. See
[`media_kit_video/windows/anibaka/README.md`](media_kit_video/windows/anibaka/README.md)
for its CMake opt-in and host backend requirements. The original renderer is
used by default. The fork does not distribute NVIDIA SDKs or runtime DLLs.

AniBaka consumes the `media_kit_video` subdirectory as a pub Git dependency
pinned to a commit. Keep the upstream fork relationship when syncing updates.
