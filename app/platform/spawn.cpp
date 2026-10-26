// Starting programs and desktop notifications, for the Automations. No shell is used anywhere: a value from a decoded message can be any text,
// so every argument is passed to the program as one argument.
#include "dect2/common/thread_name.h"
#include "platform.h"
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
#endif

namespace plat {

namespace {
std::string oneLine(const std::string& s, size_t n) {
    std::string o;
    for (unsigned char c : s) { if (o.size() >= n) break; o += (c < 0x20 || c == 0x7F) ? ' ' : (char)c; }
    return o;
}
}  // namespace

#ifdef _WIN32
namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
// one argument as the C runtime reads it back (backslashes before a quote are doubled)
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    for (size_t i = 0; i < a.size(); i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { bs++; i++; }
        if (i == a.size()) { o.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; }
        else { o.append(bs, L'\\'); o += a[i]; }
    }
    return o + L"\"";
}
}  // namespace

bool spawnProgram(const std::string& program, const std::vector<std::string>& args, std::string& err) {
    std::wstring cmd = quoteArg(wide(program));
    for (const std::string& a : args) cmd += L" " + quoteArg(wide(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { err = "cannot start " + program; return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool notify(const std::string& title, const std::string& body) {
    SetEnvironmentVariableW(L"SAURCOM_NOTE_T", wide(oneLine(title, 120)).c_str());
    SetEnvironmentVariableW(L"SAURCOM_NOTE_B", wide(oneLine(body, 240)).c_str());
    const char* script =
        "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null;"
        "$t = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02);"
        "$x = $t.GetElementsByTagName('text');"
        "$x[0].AppendChild($t.CreateTextNode($env:SAURCOM_NOTE_T)) > $null;"
        "$x[1].AppendChild($t.CreateTextNode($env:SAURCOM_NOTE_B)) > $null;"
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('Eye of SaurCom').Show([Windows.UI.Notifications.ToastNotification]::new($t))";
    std::string err;
    return spawnProgram("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command", script}, err);
}
#else
bool spawnProgram(const std::string& program, const std::vector<std::string>& args, std::string& err) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    const int rc = posix_spawnp(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) { err = "cannot start " + program; return false; }
    dect2::namedThread("saurcom-reap", [pid] { int st = 0; waitpid(pid, &st, 0); }).detach();   // reaped when it ends, nobody waits for it
    return true;
}

bool notify(const std::string& title, const std::string& body) {
    std::string err;
#ifdef __APPLE__
    auto q = [](const std::string& s) {   // inside an AppleScript string
        std::string o;
        for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
        return o;
    };
    const std::string script = "display notification \"" + q(oneLine(body, 240)) + "\" with title \"" + q(oneLine(title, 120)) + "\"";
    return spawnProgram("/usr/bin/osascript", {"-e", script}, err);
#else
    return spawnProgram("notify-send", {"--app-name=Eye of SaurCom", "--", oneLine(title, 120), oneLine(body, 240)}, err);
#endif
}
#endif

}  // namespace plat
