# Third-party notices

ImView is distributed as a single executable that statically links the
components below. Their licence texts must ship with every binary release; the
paths below are relative to the repository root and point at the copies inside
the pinned EUI-NEO submodule, so the texts can never drift from the code that
was built.

## Components linked into `imview.exe`

| Component | Version | Licence | Licence text |
|---|---|---|---|
| [EUI-NEO](https://github.com/sudoevolve/EUI-NEO) | 0.6.2 (`3rd/EUI-NEO` submodule) | Apache-2.0 | `3rd/EUI-NEO/LICENSE` |
| [GLFW](https://www.glfw.org/) | 3.4.0 | zlib/libpng | `3rd/EUI-NEO/3rd/glfw/LICENSE.md` |
| [FreeType](https://freetype.org/) | 2.13.3 | FreeType Project Licence (FTL) | `3rd/EUI-NEO/3rd/freetype/LICENSE.TXT` |
| [libpng](http://www.libpng.org/) | 1.6.43 | libpng licence | `3rd/EUI-NEO/3rd/libpng-1.6.43/LICENSE` |
| [zlib](https://zlib.net/) | 1.3.1 | zlib licence | `3rd/EUI-NEO/3rd/zlib-1.3.1/LICENSE` |
| [md4c](https://github.com/mity/md4c) | bundled with EUI-NEO | MIT | `3rd/EUI-NEO/3rd/md4c/LICENSE.md` |
| [glad](https://glad.dav1d.de/) | 0.1.34 (generated) | generated output of glad; `khrplatform.h` is Khronos MIT-style | `3rd/EUI-NEO/3rd/glad/src/glad.c`, `3rd/EUI-NEO/3rd/glad/include/KHR/khrplatform.h` |
| [stb_image](https://github.com/nothings/stb) | 2.30 | public domain / MIT (dual) | header of `3rd/EUI-NEO/3rd/stb_image.h` |
| [miniaudio](https://miniaud.io/) | 0.11.21 | public domain / MIT-0 (dual) | header of `3rd/EUI-NEO/3rd/miniaudio.h` |
| [FFmpeg](https://ffmpeg.org/) | 9.0.2 (`3rd/ffmpeg` submodule) | LGPL-2.1-or-later | `3rd/ffmpeg/COPYING.LGPLv2.1` |

EUI-NEO is used unmodified, so no "changes" statement is required beyond the
configuration flags listed in `CMakeLists.txt`. Its `NOTICE` file, if present,
ships with it.

ImView itself, including everything in `src/`, is released into the public
domain under the [Unlicense](../LICENSE); it is not covered by the licences
above.

FFmpeg is linked statically with only the libraries and components listed in
`tools/build_ffmpeg.sh` (video, audio and text-subtitle demuxers/decoders,
including the sidecar subtitle demuxers, and libswresample/libswscale). Its
complete corresponding source is the pinned
`3rd/ffmpeg` submodule; the same script reproduces the object files and static
libraries used to link `imview.exe`, which is the relinking path required by the
LGPL when distributing a statically linked binary.

## Not redistributed

- **Fonts.** ImView does not ship any font file: the UI uses the system font
  (Segoe UI, with Microsoft YaHei and Segoe UI Emoji as glyph fallbacks). The
  fonts that live in `3rd/EUI-NEO/assets/` are therefore not part of a release.
- **EUI-NEO demo assets and its application icon** are not copied next to
  `imview.exe` either.
- **tray, yyjson and the optional modules** are disabled at configure time and
  are not compiled in.
