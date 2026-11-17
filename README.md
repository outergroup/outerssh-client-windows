# outerssh-client-windows

A small Win32 starter for building an SSH client on [OuterSSH](https://github.com/outergroup/OuterSSH).
It opens one window with a WebView2 browser, a port box and a Home button.
OuterSSH connects to your server and runs a local HTTP proxy, and the web view
uses that proxy, so pages load from the server's loopback ports and Unix sockets.

- On launch, and when you press Home, it loads the server's configured homepage
  (`~/.config/outerssh/homepage`). Without one, it opens
  [Outer Shell](https://outershell.org) if it's running for your user.
- Type a port and press Enter to load `http://localhost:<port>/` on the server.

## Configuration

Edit `src/Config.h` and rebuild: host, port, username, a private key file (or a
password), and optionally the server's host-key fingerprint. When the
fingerprint is empty, the starter trusts the server and logs the fingerprint so
you can paste it in.

Messages go to the debugger output and to stderr, for example
`outerssh-client.exe 2> log.txt`.

## Building

Requirements: Visual Studio 2022 or its Build Tools (C++ and CMake), the
[Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
(included in Windows 11), and [vcpkg](https://vcpkg.io) for zlib.

```bat
git submodule update --init
vcpkg install zlib:arm64-windows-static
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=arm64-windows-static
cmake --build build
build\outerssh-client.exe
```

Run these from a Developer Command Prompt for your architecture, and use
`x64-windows-static` on x64. CMake downloads the WebView2 SDK from NuGet.

### Server helpers

OuterSSH uploads a small helper to the server when a page uses a Unix socket,
such as Outer Shell. Build the Linux helpers into `helpers/` (git-ignored) with
the script in a recent OuterSSH checkout (update the `OuterSSH` submodule if it
doesn't have the script yet; it needs Docker):

```sh
OuterSSH/Scripts/build_outerssh_linux_helpers.sh helpers
```

The build copies `helpers/` next to `outerssh-client.exe`. Ship that folder with
the app.

### Ed25519 keys

By default OuterSSH uses Windows CNG, which supports RSA and ECDSA keys but not
Ed25519 keys or encrypted OpenSSH key files. To use those, build OuterSSH with
OpenSSL instead: `vcpkg install openssl:arm64-windows-static` and add
`-DOUTERSSH_WINDOWS_CRYPTO=OFF` to the configure command.

## Limitations

- Opens Outer Shell only on Linux servers. The starter bundles only the Linux
  helpers; macOS and Windows servers need their own helper builds for Outer
  Shell's launchd socket or named pipe. Root-owned sockets need `sudo`, which
  the starter declines.
- Supports `http+unix` and `http://localhost` homepages only.
- No saved settings, tabs or bookmarks: edit the code.
