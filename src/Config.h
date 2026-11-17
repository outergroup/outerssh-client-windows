#pragma once

#include <cstdint>

// Edit these values to point the starter at your server, then rebuild.
namespace config {

inline constexpr char ssh_host[] = "example.com";
inline constexpr uint16_t ssh_port = 22;
inline constexpr char ssh_username[] = "you";

// Private key file in OpenSSH format. Leave it empty to log in with ssh_password.
// The default Windows CNG build supports RSA and ECDSA keys, not Ed25519.
inline constexpr wchar_t private_key_path[] = L"C:\\Users\\you\\.ssh\\id_ecdsa";
inline constexpr char private_key_passphrase[] = "";
inline constexpr char ssh_password[] = "";

// The server's host key as OpenSSH prints it, for example "SHA256:abc...".
// When empty, the starter trusts the server's key and logs its fingerprint.
inline constexpr char trusted_host_key_fingerprint[] = "";

}
