# outerssh-client-windows

A small Win32 starter for building an SSH client on [OuterSSH](https://github.com/outergroup/OuterSSH).
It opens one window with a WebView2 browser, a port box and a Home button.
OuterSSH connects to your server and runs a local HTTP proxy, and the web view
uses that proxy, so pages load from the server's loopback ports and Unix sockets.

- On launch, and when you press Home, it loads the server's configured homepage
  (see [Server-defined homepage](https://github.com/outergroup/OuterSSH/blob/main/OuterSSH/README.md#server-defined-homepage)).
  Installing [Outer Shell](https://outershell.org) on a server makes it that
  server's homepage, on Linux, macOS, and Windows.
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

OuterSSH uploads a small helper to the server when it first needs one, such as
to open a Unix socket like Outer Shell's. Configuring the build downloads signed
helpers for Linux, macOS, and Windows servers from outerssh.org into `helpers/`
(git-ignored), checking each against `helpers.sha256`, and the build copies
`helpers/` next to `outerssh-client.exe`. Ship that folder with the app.

To use helpers you build yourself (see `OuterSSH/Release/README.md`), put them in
`helpers/` and their hashes in `helpers.sha256`; the build only downloads files
whose hash doesn't match.

### Ed25519 keys

By default OuterSSH uses Windows CNG, which supports RSA and ECDSA keys but not
Ed25519 keys or encrypted OpenSSH key files. To use those, build OuterSSH with
OpenSSL instead: `vcpkg install openssl:arm64-windows-static` and add
`-DOUTERSSH_WINDOWS_CRYPTO=OFF` to the configure command.

## Limitations

- Root-owned sockets need `sudo`, which the starter declines.
- Supports `http+unix`, `http+pipe` and `http://localhost` homepages only.
- No saved settings, tabs or bookmarks: edit the code.
