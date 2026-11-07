# Third-party notices

ZERODROID uses or builds against the following third-party projects. Each
project remains subject to its own license.

- devkitPro, devkitA64, libnx and deko3d
- AMD FidelityFX Super Resolution 1 (FSR 1) — EASU/RCAS shader port
- SDL2, SDL2_ttf and SDL2_image
- FFmpeg/libavcodec and libavutil
- Opus
- mbedTLS
- libpeer
- libsrtp
- usrsctp
- nlohmann/json
- Project Nayuki QR Code generator

The dependency build script downloads source from upstream projects instead of
committing generated static libraries. Consult the corresponding upstream
repositories and source headers for complete copyright and license terms.

## AMD FidelityFX Super Resolution 1

`switch/shaders/video_fsr_fsh.glsl` and `switch/shaders/fsr_rcas_fsh.glsl` are
a GLSL port of EASU and RCAS from AMD FidelityFX FSR 1.0.2
(`ffx-fsr/ffx_fsr1.h`, https://github.com/GPUOpen-Effects/FidelityFX-FSR),
adapted to the Switch NV12 zero-copy path.

```
Copyright (c) 2021 Advanced Micro Devices, Inc.
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
