# nv-codec-headers (NVENC API header)

`nvEncodeAPI.h` from FFmpeg's nv-codec-headers, tag `n13.0.19.1`
(https://github.com/FFmpeg/nv-codec-headers), unchanged. It is NVIDIA's Video Codec SDK 13.0
encoder API header, MIT licensed (see `LICENSE`, the notice from the header itself).

The mod loads `nvEncodeAPI.dll` (the 32-bit one in SysWOW64, part of every NVIDIA driver) at run
time and links nothing from NVIDIA. API 13.0 needs driver R570 or newer; with an older driver the
video export uses Media Foundation's encoder instead (see `src/nvenc.cpp`).
