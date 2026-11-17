#include "Config.h"
#include "SocketHostDisplayScript.h"

#include <OuterSSH/OuterSSH.h>

#include <windows.h>
#include <commctrl.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT WM_OUTERSSH_EVENTS = WM_APP + 1;
constexpr INT_PTR ID_PORT = 1;
constexpr INT_PTR ID_HOME = 2;

// Request tags hold the kind in the low byte and the navigation number above it,
// so results that arrive after the user navigated elsewhere are ignored.
enum RequestKind : uint64_t {
    REQUEST_PROXY_START = 1,
    REQUEST_PROXY_DESTROY,
    REQUEST_POOL_DESTROY,
    REQUEST_HOMEPAGE,
};

struct App {
    HWND window = nullptr;
    HWND port_box = nullptr;
    HWND home_button = nullptr;
    HFONT font = nullptr;
    OuterSSHEventQueue *events = nullptr;
    PTP_WAIT events_wait = nullptr;
    OuterSSHSessionPool *pool = nullptr;
    OuterSSHHTTPProxyServer *proxy = nullptr;
    uint16_t proxy_port = 0;
    std::wstring proxy_username;
    std::wstring proxy_password;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    uint64_t navigation = 0;
    bool closing = false;
};

App app;

std::wstring widen(const char *text, size_t length)
{
    if (length == 0) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, static_cast<int>(length), result.data(), count);
    return result;
}

std::wstring widen(const std::string &text)
{
    return widen(text.data(), text.size());
}

std::string narrow(const std::wstring &text)
{
    if (text.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

void log(const std::string &message)
{
    std::string line = "outerssh-client: " + message + "\n";
    OutputDebugStringA(line.c_str());
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

std::string error_text(const OuterSSHError &error)
{
    if (error.message_length > 0) return std::string(error.message, error.message_length);
    return "Error " + std::to_string(error.domain) + "." + std::to_string(error.code);
}

std::wstring escape_html(const std::wstring &text)
{
    std::wstring result;
    for (wchar_t c : text) {
        if (c == L'<') result += L"&lt;";
        else if (c == L'>') result += L"&gt;";
        else if (c == L'&') result += L"&amp;";
        else result += c;
    }
    return result;
}

void show_message(const std::string &text)
{
    log(text);
    if (!app.web) return;
    std::wstring html = L"<!doctype html><meta charset=\"utf-8\"><title>Message</title>"
        L"<body style=\"font: 15px system-ui; margin: 2em\"><p>" + escape_html(widen(text)) + L"</p>";
    app.web->NavigateToString(html.c_str());
}

void fatal(const std::string &text)
{
    log(text);
    MessageBoxW(app.window, widen(text).c_str(), L"OuterSSH starter", MB_OK | MB_ICONERROR);
    PostMessageW(app.window, WM_CLOSE, 0, 0);
}

void load(const std::string &url)
{
    log("Loading " + url);
    app.web->Navigate(widen(url).c_str());
}

uint64_t tag(RequestKind kind)
{
    return (app.navigation << 8) | kind;
}

bool starts_with(const std::string &text, const char *prefix)
{
    size_t length = std::strlen(prefix);
    if (text.size() < length) return false;
    for (size_t i = 0; i < length; i++)
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
    return true;
}

bool percent_decode(const std::string &text, std::string &result)
{
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '%') {
            result += text[i];
            continue;
        }
        if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) return false;
        result += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
        i += 2;
    }
    return true;
}

// Converts http+unix://, http+pipe:// and http://localhost homepages into URLs the
// OuterSSH proxy understands. Extend this to support more homepage schemes.
std::optional<std::string> convert_homepage(const std::string &url)
{
    auto split = [&url](size_t start, std::string &authority, std::string &rest) {
        size_t end = url.find_first_of("/?#", start);
        if (end == std::string::npos) end = url.size();
        authority = url.substr(start, end - start);
        rest = url.substr(end);
        if (rest.empty() || rest[0] != '/') rest.insert(0, "/");
    };
    std::string authority, rest;
    char host[OUTERSSH_PROXY_SOCKET_HOST_CAPACITY];
    size_t host_length;
    bool unix_socket = starts_with(url, "http+unix://");
    if (unix_socket || starts_with(url, "http+pipe://")) {
        split(12, authority, rest);
        std::string path;
        if (!percent_decode(authority, path) || !(unix_socket
                ? outerssh_proxy_encode_unix_socket_host(path.data(), path.size(), host, sizeof(host), &host_length)
                : outerssh_proxy_encode_named_pipe_host(path.data(), path.size(), host, sizeof(host), &host_length)))
            return std::nullopt;
        return "http://" + std::string(host, host_length) + rest;
    }
    if (starts_with(url, "http://")) {
        split(7, authority, rest);
        if (authority.empty() || authority.find('@') != std::string::npos) return std::nullopt;
        size_t name_end = authority[0] == '[' ? authority.find(']') : authority.find(':');
        if (authority[0] == '[') {
            if (name_end == std::string::npos) return std::nullopt;
            name_end++;
        }
        if (name_end == std::string::npos) name_end = authority.size();
        std::string name = authority.substr(0, name_end);
        std::string port = authority.substr(name_end);
        if (!port.empty() && (port[0] != ':' || port.size() < 2 || port.size() > 6 ||
                              port.find_first_not_of("0123456789", 1) != std::string::npos))
            return std::nullopt;
        if (!outerssh_proxy_encode_tcp_host(name.data(), name.size(), host, sizeof(host), &host_length))
            return std::nullopt;
        return "http://" + std::string(host, host_length) + port + rest;
    }
    return std::nullopt;
}

void go_home()
{
    if (!app.web) return;
    app.navigation++;
    if (!outerssh_session_pool_get_homepage(app.pool, tag(REQUEST_HOMEPAGE)))
        show_message("Could not request the server's homepage.");
}

void load_port()
{
    if (!app.web) return;
    wchar_t text[16] = {};
    GetWindowTextW(app.port_box, text, 16);
    if (text[0] == 0) {
        go_home();
        return;
    }
    app.navigation++;
    wchar_t *end = nullptr;
    long port = std::wcstol(text, &end, 10);
    if (*end != 0 || port < 1 || port > 65535) {
        show_message("Enter a port from 1 to 65535.");
        return;
    }
    load("http://localhost" OUTERSSH_PROXY_TCP_HOST_SUFFIX ":" + std::to_string(port) + "/");
}

void homepage_received(const OuterSSHEvent &event)
{
    const auto &result = event.homepage_received;
    if (result.error.domain != OUTERSSH_ERROR_NONE) {
        show_message(error_text(result.error));
        return;
    }
    if (!result.found) {
        show_message("This server has no homepage. Type a port to open one of its web servers.");
        return;
    }
    std::string url(result.url, result.url_length);
    if (auto converted = convert_homepage(url)) load(*converted);
    else show_message("This starter doesn't support the homepage " + url + ".");
}

void layout()
{
    RECT client;
    GetClientRect(app.window, &client);
    UINT dpi = GetDpiForWindow(app.window);
    int margin = MulDiv(6, dpi, 96);
    int height = MulDiv(26, dpi, 96);
    int port_width = MulDiv(120, dpi, 96);
    MoveWindow(app.port_box, margin, margin, port_width, height, TRUE);
    MoveWindow(app.home_button, 2 * margin + port_width, margin, MulDiv(72, dpi, 96), height, TRUE);
    if (app.controller) {
        client.top = height + 2 * margin;
        app.controller->put_Bounds(client);
    }
}

void update_font()
{
    UINT dpi = GetDpiForWindow(app.window);
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    SendMessageW(app.port_box, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(app.home_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (app.font) DeleteObject(app.font);
    app.font = font;
}

void configure_webview()
{
    ComPtr<ICoreWebView2_10> web;
    EventRegistrationToken token;
    if (FAILED(app.web.As(&web))) {
        fatal("This WebView2 Runtime is too old. Update the Microsoft Edge WebView2 Runtime.");
        return;
    }
    web->add_BasicAuthenticationRequested(Callback<ICoreWebView2BasicAuthenticationRequestedEventHandler>(
        [](ICoreWebView2 *, ICoreWebView2BasicAuthenticationRequestedEventArgs *args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            std::wstring proxy = L"http://127.0.0.1:" + std::to_wstring(app.proxy_port);
            bool from_proxy = uri && std::wstring(uri).rfind(proxy, 0) == 0 &&
                (uri[proxy.size()] == 0 || uri[proxy.size()] == L'/');
            CoTaskMemFree(uri);
            ComPtr<ICoreWebView2BasicAuthenticationResponse> response;
            if (from_proxy && SUCCEEDED(args->get_Response(&response))) {
                response->put_UserName(app.proxy_username.c_str());
                response->put_Password(app.proxy_password.c_str());
            }
            return S_OK;
        }).Get(), &token);
    web->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
        [](ICoreWebView2 *sender, IUnknown *) -> HRESULT {
            LPWSTR title = nullptr;
            if (SUCCEEDED(sender->get_DocumentTitle(&title))) {
                SetWindowTextW(app.window, (std::wstring(title) + L" - OuterSSH starter").c_str());
                CoTaskMemFree(title);
            }
            return S_OK;
        }).Get(), &token);
}

// Pages served from a Unix socket see their address as a long encoded hostname. OuterSSH's
// script shows the socket path in its place wherever the page displays it. The first page
// loads once the script is installed, so it applies there too.
void add_socket_host_display_script()
{
    HRESULT result = app.web->AddScriptToExecuteOnDocumentCreated(widen(socket_host_display_script).c_str(),
        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [](HRESULT result, LPCWSTR) -> HRESULT {
                if (FAILED(result)) log("Could not install the socket host display script.");
                go_home();
                return S_OK;
            }).Get());
    if (FAILED(result)) {
        log("Could not install the socket host display script.");
        go_home();
    }
}

void create_webview()
{
    // Route every request, including *.localhost names, through the OuterSSH proxy.
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    std::wstring arguments = L"--proxy-server=http://127.0.0.1:" + std::to_wstring(app.proxy_port) +
        L" --proxy-bypass-list=<-loopback>";
    options->put_AdditionalBrowserArguments(arguments.c_str());
    HRESULT result = CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                if (FAILED(result) || !environment) {
                    fatal("Could not start WebView2. Install the Microsoft Edge WebView2 Runtime.");
                    return S_OK;
                }
                return environment->CreateCoreWebView2Controller(app.window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                fatal("Could not create the web view.");
                                return S_OK;
                            }
                            if (app.closing) {
                                controller->Close();
                                return S_OK;
                            }
                            app.controller = controller;
                            controller->get_CoreWebView2(&app.web);
                            configure_webview();
                            layout();
                            add_socket_host_display_script();
                            return S_OK;
                        }).Get());
            }).Get());
    if (FAILED(result)) fatal("Could not start WebView2. Install the Microsoft Edge WebView2 Runtime.");
}

// OuterSSH uploads a helper to the server, which checks the server's socket
// allowlist. The build downloads the helpers and copies them next to the
// executable. Windows helpers are tar archives.
void provide_helper(const OuterSSHEvent &event)
{
    const auto &question = event.helper_binary_needed;
    std::wstring platform = widen(question.platform, question.platform_length);
    const wchar_t *extension = platform.rfind(L"windows-", 0) == 0 ? L".tar.gz" : L".gz";
    wchar_t executable[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    std::wstring path(executable, length);
    path = path.substr(0, path.find_last_of(L'\\') + 1) + L"helpers\\outerssh-helper-" + platform + extension;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size;
    if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size)) {
        outerssh_session_pool_provide_helper_binary(event.pool, question.question, file, 0,
            static_cast<uint64_t>(size.QuadPart));
        return;
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    log("Missing " + narrow(path) + "; this server's sockets need it.");
    outerssh_session_pool_decline_helper_binary(event.pool, question.question, OuterSSHError{});
}

void handle_event(const OuterSSHEvent &event)
{
    uint64_t kind = event.tag & 0xff;
    bool current = (event.tag >> 8) == app.navigation;
    switch (event.type) {
    case OUTERSSH_EVENT_PROXY_STARTED: {
        const auto &started = event.proxy_started;
        if (started.error.domain != OUTERSSH_ERROR_NONE) {
            fatal("The HTTP proxy could not start: " + error_text(started.error));
            break;
        }
        app.proxy_port = started.listen_port;
        app.proxy_username = widen(started.username, started.username_length);
        app.proxy_password = widen(started.password, started.password_length);
        create_webview();
        break;
    }
    case OUTERSSH_EVENT_HOST_KEY_VALIDATION: {
        const auto &question = event.host_key_validation;
        std::string fingerprint(question.fingerprint, question.fingerprint_length);
        bool trusted = config::trusted_host_key_fingerprint[0] == 0;
        if (trusted)
            log("Trusting the server's host key " + fingerprint +
                ". To pin it, paste it into trusted_host_key_fingerprint in src/Config.h.");
        else
            log("The server's host key " + fingerprint + " doesn't match trusted_host_key_fingerprint in src/Config.h.");
        outerssh_session_pool_answer_host_key(event.pool, question.question, trusted);
        break;
    }
    case OUTERSSH_EVENT_SUDO_PASSWORD_NEEDED:
        outerssh_session_pool_decline_sudo_password(event.pool, event.sudo_password_needed.question);
        break;
    case OUTERSSH_EVENT_HELPER_BINARY_NEEDED:
        provide_helper(event);
        break;
    case OUTERSSH_EVENT_CONNECTION_FAILED: {
        const auto &failure = event.connection_failed;
        std::string destination = failure.socket_path_length > 0
            ? std::string(failure.socket_path, failure.socket_path_length)
            : std::string(failure.host, failure.host_length) + ":" + std::to_string(failure.port);
        if (!app.closing) show_message("Could not connect to " + destination + ": " + error_text(failure.error));
        break;
    }
    case OUTERSSH_EVENT_HOMEPAGE_RECEIVED:
        if (current && !app.closing) homepage_received(event);
        break;
    case OUTERSSH_EVENT_DESTROYED:
        if (kind == REQUEST_PROXY_DESTROY) app.proxy = nullptr;
        if (kind == REQUEST_POOL_DESTROY) app.pool = nullptr;
        break;
    default:
        break;
    }
}

void drain_events()
{
    OuterSSHEvent events[16];
    size_t count;
    while ((count = outerssh_event_queue_poll(app.events, events, 16)) > 0)
        for (size_t i = 0; i < count; i++) handle_event(events[i]);
    if (app.closing && !app.proxy && !app.pool) DestroyWindow(app.window);
    else SetThreadpoolWait(app.events_wait, outerssh_event_queue_wait_handle(app.events), nullptr);
}

void CALLBACK events_signaled(PTP_CALLBACK_INSTANCE, void *window, PTP_WAIT, TP_WAIT_RESULT)
{
    PostMessageW(static_cast<HWND>(window), WM_OUTERSSH_EVENTS, 0, 0);
}

bool start_events()
{
    app.events = outerssh_event_queue_create();
    HANDLE handle = app.events ? outerssh_event_queue_wait_handle(app.events) : nullptr;
    app.events_wait = handle ? CreateThreadpoolWait(events_signaled, app.window, nullptr) : nullptr;
    if (!app.events_wait) return false;
    SetThreadpoolWait(app.events_wait, handle, nullptr);
    return true;
}

void stop_events()
{
    if (app.events_wait) {
        SetThreadpoolWait(app.events_wait, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(app.events_wait, TRUE);
        CloseThreadpoolWait(app.events_wait);
        app.events_wait = nullptr;
    }
    outerssh_event_queue_destroy(app.events);
    app.events = nullptr;
}

void start_ssh()
{
    if (!start_events()) {
        fatal("Could not create the OuterSSH event queue.");
        return;
    }
    std::string key;
    if (config::private_key_path[0] != 0) {
        std::ifstream file(config::private_key_path, std::ios::binary);
        key.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (key.empty()) {
            fatal("Could not read the private key file named in src/Config.h.");
            return;
        }
    }
    OuterSSHSessionPoolConfiguration configuration = {};
    configuration.hostname = config::ssh_host;
    configuration.hostname_length = std::strlen(config::ssh_host);
    configuration.username = config::ssh_username;
    configuration.username_length = std::strlen(config::ssh_username);
    configuration.port = config::ssh_port;
    auto &authentication = configuration.authentication;
    if (!key.empty()) {
        authentication.kind = OUTERSSH_SESSION_AUTHENTICATION_PRIVATE_KEY;
        authentication.private_key = key.data();
        authentication.private_key_length = key.size();
        if (config::private_key_passphrase[0] != 0) {
            authentication.passphrase = config::private_key_passphrase;
            authentication.passphrase_length = std::strlen(config::private_key_passphrase);
        }
    } else {
        authentication.kind = OUTERSSH_SESSION_AUTHENTICATION_PASSWORD;
        authentication.password = config::ssh_password;
        authentication.password_length = std::strlen(config::ssh_password);
    }
    if (config::trusted_host_key_fingerprint[0] != 0) {
        configuration.trusted_host_key_fingerprint = config::trusted_host_key_fingerprint;
        configuration.trusted_host_key_fingerprint_length = std::strlen(config::trusted_host_key_fingerprint);
    }
    OuterSSHError error = {};
    app.pool = outerssh_session_pool_create(app.events, &configuration, &error);
    SecureZeroMemory(key.data(), key.size());
    if (!app.pool) {
        fatal("Could not create the SSH session pool: " + error_text(error));
        return;
    }
    OuterSSHHTTPProxyServerConfiguration proxy_configuration = {};
    proxy_configuration.authentication = OUTERSSH_PROXY_AUTHENTICATION_PASSWORD;
    proxy_configuration.rewrite_loopback_addresses = true;
    app.proxy = outerssh_http_proxy_server_create(app.pool, &proxy_configuration, &error);
    if (!app.proxy) {
        fatal("Could not create the HTTP proxy: " + error_text(error));
        return;
    }
    if (!outerssh_http_proxy_server_start(app.proxy, 0, REQUEST_PROXY_START))
        fatal("Could not start the HTTP proxy.");
}

void close_app()
{
    if (app.closing) return;
    app.closing = true;
    ShowWindow(app.window, SW_HIDE);
    if (app.controller) app.controller->Close();
    app.controller = nullptr;
    app.web = nullptr;
    if (app.proxy && !outerssh_http_proxy_server_destroy(app.proxy, REQUEST_PROXY_DESTROY)) app.proxy = nullptr;
    if (app.pool && !outerssh_session_pool_destroy(app.pool, REQUEST_POOL_DESTROY)) app.pool = nullptr;
    if (!app.proxy && !app.pool) DestroyWindow(app.window);
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_OUTERSSH_EVENTS:
        drain_events();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_HOME && HIWORD(wparam) == BN_CLICKED) {
            SetWindowTextW(app.port_box, L"");
            go_home();
            return 0;
        }
        break;
    case WM_SIZE:
        layout();
        return 0;
    case WM_DPICHANGED: {
        const RECT *suggested = reinterpret_cast<const RECT *>(lparam);
        update_font();
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        close_app();
        return 0;
    case WM_DESTROY:
        stop_events();
        if (app.font) DeleteObject(app.font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"OuterSSHStarter";
    RegisterClassW(&window_class);
    app.window = CreateWindowExW(0, window_class.lpszClassName, L"OuterSSH starter", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1100, GetDpiForSystem(), 96), MulDiv(800, GetDpiForSystem(), 96),
        nullptr, nullptr, instance, nullptr);
    if (!app.window) return 1;
    app.port_box = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 0, 0, app.window, reinterpret_cast<HMENU>(ID_PORT), instance, nullptr);
    app.home_button = CreateWindowExW(0, L"BUTTON", L"Home", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, app.window, reinterpret_cast<HMENU>(ID_HOME), instance, nullptr);
    SendMessageW(app.port_box, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Port"));
    SendMessageW(app.port_box, EM_SETLIMITTEXT, 5, 0);
    update_font();
    layout();
    ShowWindow(app.window, show);
    start_ssh();

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && message.hwnd == app.port_box) {
            load_port();
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return 0;
}
