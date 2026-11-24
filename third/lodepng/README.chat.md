# LodePNG 20261102

Unmodified files from [lvandeve/lodepng at 7b2893e35891c0c85bee5595caafaf3c2044b409](https://github.com/lvandeve/lodepng/commit/7b2893e35891c0c85bee5595caafaf3c2044b409).
The original zlib license is in `LICENSE` and in both source headers.

```text
32cdd7a395b5476a3fe4615305a99066dc4d37aaea6988643cca8b4eccc352cc  lodepng.cpp
a49b98b45ea603a05e2a77bd6b0320e7535a9c6e18aef8ce904692a3e02b87ab  lodepng.h
bdafa318b8e637d600ca1b60545e894881977a16a16b74809c95e805701621e3  LICENSE
```

Chat builds only the core, in-memory decoder. No utilities (including the one
described in [CVE-2026-112020](https://www.cve.org/CVERecord?id=CVE-2026-112020)),
encoder, disk helpers or metadata decompression are included. DEFLATE, Adler-32
and CRC validation remain enabled. The existing upstream `LODEPNG_MAX_ALLOC`
caps each allocation at 1 MiB; it does not cap the entire decoder at 1 MiB.
