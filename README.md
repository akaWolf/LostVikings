# LostVikings

## Linux build

0. run `git submodule update --init`
1. install `SDL2`
2. run `make`
3. place `DATA.DAT` in the root
4. run `vikings`

## Windows build (cross-compile via mingw-w64)

Prerequisites — mingw-w64 toolchain + SDL2 for mingw + a mingw pkg-config wrapper:

- Arch (x86_64): `pacman -S mingw-w64-gcc mingw-w64-sdl2 mingw-w64-pkg-config`
- Arch / Arch ARM (aarch64): same packages are AUR-only — build via `yay -S mingw-w64-gcc mingw-w64-sdl2 mingw-w64-pkg-config` (gcc-mingw compiles for several hours on ARM).
- Debian/Ubuntu: `apt install g++-mingw-w64-x86-64 mingw-w64-tools` and supply SDL2 dev libs from libsdl.org's mingw devel tarball into the mingw sysroot.

Then:

```
make clean
make WIN=1 -j$(nproc)
```

Output: `vikings.exe` (statically linked — no DLL dependencies). Copy `vikings.exe` + `DATA.DAT` to the Windows host and run.

Linux-only debug instrumentation (`backtrace()` stack dumps via `<execinfo.h>`, HW watchpoints via `perf_event_open` + `<sys/mman.h>` ring buffer, `dladdr` symbol resolution) is compiled out under `#ifdef __linux__` on the Windows build. Gameplay is unaffected; only platform-native debugging is unavailable.

Override the triplet (e.g. for i686 Windows) via:

```
make WIN=1 MINGW_TRIPLET=i686-w64-mingw32
```
