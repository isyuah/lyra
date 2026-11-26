# Lyra SDK third-party components

License and attribution files for bundled dependencies are installed alongside this notice. This notice does not grant a license for Lyra itself.

- Both SDKs include LLVM, Clang, and LLD (Apache-2.0 with LLVM exceptions); BDWGC, libuv, jansson, SQLite, llhttp, tomlc17, curl, c-ares, nghttp2, zlib, and OpenSSL. Linux distributions' `copyright` files for shipped objects and libraries are in `host/`.
- The Windows SDK additionally includes LLVM-MinGW target libraries and headers, mingw-w64, and the Microsoft Visual C++ redistributable DLLs. LLVM and LLVM-MinGW license text is in `llvm-mingw/`; mingw-w64 notices are in `mingw-w64/`. The Microsoft DLLs are distributed subject to the applicable [Visual Studio redistribution terms](https://learn.microsoft.com/visualstudio/releases/2022/redistribution).
- The Linux SDK additionally includes the target linker inputs from glibc and GCC. Their distribution license and GCC Runtime Library Exception are recorded in the associated `host/` notices.
