# Building xbased

xbased builds with clang and CMake on Windows (MSYS2), Linux and macOS. The third-party
sources are fetched at pinned versions by `tools/fetch_deps.sh` into a `deps`
folder beside the repository:

```
<work>/
  xbased/      this repository
  deps/        third-party sources (tools/fetch_deps.sh)
  build/       build trees
```

The executable is built from the `hakux` target and named `xbased`.

## Windows (x64)

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 CLANG64** shell.
2. Install the toolchain:

   ```bash
   pacman -S --needed git mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-lld \
     mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-python
   ```

3. Get the source, fetch the dependencies and build:

   ```bash
   git clone https://github.com/crazyriddler/xbased
   cd xbased
   tools/fetch_deps.sh
   cmake -S native -B ../build/win -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   cmake --build ../build/win --target hakux
   ```

`../build/win/xbased.exe` runs from the build folder (its fonts and mods are copied
beside it). It imports only system DLLs, so it runs on any Windows 10/11 x64 with a
Vulkan 1.1 driver.

To make the release folder and zip in `../dist` (PowerShell):

```powershell
tools\release\package_windows.ps1
```

## Linux (x86-64)

Ubuntu/Debian packages (other distributions: the same libraries' development packages):

```bash
sudo apt install git clang lld cmake ninja-build pkg-config python3 zlib1g-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgl-dev libdecor-0-dev \
  libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev \
  libdbus-1-dev libibus-1.0-dev
```

Then:

```bash
git clone https://github.com/crazyriddler/xbased
cd xbased
tools/fetch_deps.sh
cmake -S native -B ../build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build ../build/linux --target hakux
```

`../build/linux/xbased` runs from the build folder. Settings, saves and states go to
`~/.local/share/xbased` (or beside the executable when a `portable.txt` is there).
SDL3 is linked statically; at run time the system needs glibc 2.35 or newer and a
Vulkan driver (Mesa or the GPU vendor's).

To make the AppImage in `../dist`:

```bash
tools/release/package_linux.sh ../build/linux
```

The release AppImage is built on Ubuntu 22.04, so it runs on that and newer
distributions.

## macOS (x86-64; Apple silicon through Rosetta 2)

The Mac build is an x86-64 app: Intel Macs run it natively, Apple silicon
under Rosetta 2. The guest CPU is the same x86-32 to x86-64 translator as on
Windows and Linux, and the Xbox's 4 GiB sit at host address 0, which macOS
allows an x86-64 process but not an arm64 one. Rendering is the same Vulkan
renderer on [MoltenVK](https://github.com/KhronosGroup/MoltenVK), which
`tools/fetch_deps.sh` fetches (its release's universal `libMoltenVK.dylib`)
and the build puts in the app's `Contents/Frameworks`.

1. Install Xcode (or its command line tools: `xcode-select --install`),
   CMake and Ninja ([Homebrew](https://brew.sh): `brew install cmake ninja`).
   On Apple silicon, Rosetta 2 too:

   ```bash
   softwareupdate --install-rosetta --agree-to-license
   ```

2. Get the source, fetch the dependencies and build:

   ```bash
   git clone https://github.com/crazyriddler/xbased
   cd xbased
   tools/fetch_deps.sh
   cmake -S native -B ../build/mac -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build ../build/mac --target hakux
   ```

   The build is x86-64 whatever the Mac (`CMAKE_OSX_ARCHITECTURES` is set
   to `x86_64`; macOS 11 or newer).

`../build/mac/xbased.app` runs from the build folder (`open ../build/mac/xbased.app`,
or its `Contents/MacOS/xbased` with a disc image to start a game). Its fonts,
mods and cheats are in `Contents/Resources`; settings, saves and states go to
`~/Library/Application Support/xbased`. The app is signed ad hoc.

To make the release zip in `../dist`:

```bash
tools/release/package_macos.sh ../build/mac
```

A copy downloaded from the web is quarantined by Gatekeeper: open it the
first time with Control-click › Open, or clear the attribute with
`xattr -dr com.apple.quarantine xbased.app`.

At run time the app loads `Contents/Frameworks/libMoltenVK.dylib`; the
`XBASED_VULKAN_LIBRARY` environment variable names another Vulkan library
instead (the Vulkan SDK's `/usr/local/lib/libvulkan.1.dylib`, for its
validation layers). MoltenVK's own `MVK_CONFIG_*` variables apply too; the
app turns fast math off (`MVK_CONFIG_FAST_MATH_ENABLED=0`) unless one is set.

## Options

| CMake option | Default | Meaning |
| --- | --- | --- |
| `NC_DEPS` | `../deps` | where the third-party sources are |
| `HAKUX_LTO` | `OFF` | link-time optimization |
| `NC_MOLTENVK` | found | macOS: the `libMoltenVK.dylib` (with an x86_64 slice) to bundle; by default `NC_DEPS/MoltenVK`'s, a MoltenVK checkout's package beside the repository, or the Vulkan SDK's |
