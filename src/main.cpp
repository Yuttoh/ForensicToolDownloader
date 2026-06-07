#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int ID_SEARCH = 1001;
constexpr int ID_DOWNLOAD_ALL = 1002;
constexpr int ID_TABS = 1003;
constexpr int ID_DOWNLOAD_TAB = 1004;
constexpr int ID_ASK_EXTRACT = 1005;
constexpr int ID_ASK_DELETE = 1006;
constexpr int ID_GRID_MODE = 1007;
constexpr int ID_THEME_TOGGLE = 1008;

constexpr int ID_TOOL_BUTTON_BASE = 2000;
constexpr int ID_SCRIPT_BUTTON_BASE = 3000;
constexpr int ID_SHORTCUT_BUTTON_BASE = 4000;
constexpr int ID_SHORTCUT_COPY_BASE = 5000;
constexpr int ID_COMMAND_RANGE = 1000;

constexpr UINT WM_APP_TOOL_CHANGED = WM_APP + 1;
constexpr UINT WM_APP_DOWNLOAD_FINISHED = WM_APP + 2;
constexpr UINT WM_APP_EXTRACTION_FINISHED = WM_APP + 3;
constexpr UINT WM_APP_DOWNLOAD_ALL_FINISHED = WM_APP + 4;

constexpr int kHeaderHeight = 92;
constexpr int kTabsTop = kHeaderHeight;
constexpr int kTabsHeight = 30;
constexpr int kContentTop = 130;
constexpr int kOuterMargin = 16;
constexpr int kRowHeight = 54;
constexpr int kRowGap = 6;
constexpr int kFooterHeight = 48;
constexpr int kGridCardHeight = 106;
constexpr int kMaxParallelDownloads = 4;

struct ToolItem {
    std::wstring name;
    std::wstring folder;
    std::wstring url;
    std::wstring filename;
    std::wstring description;
    bool archive = false;

    bool busy = false;
    bool downloaded = false;
    bool failed = false;
    int progress = 0;
    std::wstring status = L"Pronto";
};

struct ScriptItem {
    std::wstring name;
    std::wstring description;
    std::wstring command;
    bool elevated = false;
};

struct ShortcutItem {
    std::wstring name;
    std::wstring command;
    std::wstring description;
};

struct ToolRow {
    int index = -1;
    HWND row = nullptr;
    HWND name = nullptr;
    HWND status = nullptr;
    HWND progress = nullptr;
    HWND button = nullptr;
};

struct ScriptRow {
    int index = -1;
    HWND row = nullptr;
    HWND button = nullptr;
};

struct ShortcutRow {
    int index = -1;
    HWND row = nullptr;
    HWND button = nullptr;
    HWND copyButton = nullptr;
};

struct ToolSnapshot {
    std::wstring name;
    std::wstring folder;
    std::wstring status;
    std::wstring description;
    bool busy = false;
    bool downloaded = false;
    bool failed = false;
    int progress = 0;
};

HWND g_mainWindow = nullptr;
HWND g_searchEdit = nullptr;
HWND g_downloadAllButton = nullptr;
HWND g_downloadTabButton = nullptr;
HWND g_askExtractCheck = nullptr;
HWND g_askDeleteCheck = nullptr;
HWND g_gridModeCheck = nullptr;
HWND g_tabs = nullptr;
HWND g_contentPanel = nullptr;
HWND g_tooltip = nullptr;
HWND g_themeToggleButton = nullptr;

HFONT g_font = nullptr;
HFONT g_smallFont = nullptr;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_panelBrush = nullptr;
HBRUSH g_rowBrush = nullptr;
HBRUSH g_borderBrush = nullptr;

struct ThemeColors {
    COLORREF window;
    COLORREF panel;
    COLORREF row;
    COLORREF border;
    COLORREF text;
    COLORREF mutedText;
    COLORREF editBackground;
    COLORREF editText;
};

bool g_darkMode = false;
ThemeColors g_theme{};

std::vector<ToolItem> g_tools;
std::vector<ScriptItem> g_scripts;
std::vector<ShortcutItem> g_shortcuts;
std::vector<std::wstring> g_categories;
std::vector<ToolRow> g_toolRows;
std::vector<ScriptRow> g_scriptRows;
std::vector<ShortcutRow> g_shortcutRows;
std::map<HWND, std::wstring> g_tooltipTexts;
std::mutex g_toolsMutex;
std::atomic_bool g_downloadAllRunning{false};

int g_activeTab = 0;
int g_scrollPos = 0;
int g_contentHeight = 0;

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

int ClampInt(int value, int low, int high) {
    return std::min(std::max(value, low), high);
}

template <typename T, size_t N>
DWORD ArrayCount(T (&)[N]) {
    return static_cast<DWORD>(N);
}

bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

bool StartsWithInsensitive(const std::wstring& value, const std::wstring& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    return ToLower(value.substr(0, prefix.size())) == ToLower(prefix);
}

bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }
    return ToLower(value.substr(value.size() - suffix.size())) == ToLower(suffix);
}

bool IsZipFile(const std::wstring& filename) {
    return EndsWithInsensitive(filename, L".zip");
}

std::wstring WindowText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) {
        GetWindowTextW(hwnd, &text[0], len + 1);
    }
    text.resize(static_cast<size_t>(len));
    return text;
}

bool IsChecked(HWND hwnd) {
    return hwnd && SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

std::wstring ExeDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD len = 0;
    while (true) {
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) {
            return L".";
        }
        if (len < buffer.size() - 1) {
            break;
        }
        buffer.resize(buffer.size() * 2);
    }

    std::wstring path(buffer.data(), len);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring ReplaceAll(std::wstring value, const std::wstring& from, const std::wstring& to) {
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::wstring::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::wstring WidenAscii(const std::string& value) {
    std::wstring result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        result.push_back(static_cast<wchar_t>(ch));
    }
    return result;
}

std::wstring ExpandEnvironment(const std::wstring& value) {
    DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed == 0) {
        return value;
    }

    std::wstring expanded(static_cast<size_t>(needed), L'\0');
    DWORD written = ExpandEnvironmentStringsW(value.c_str(), &expanded[0], needed);
    if (written == 0 || written > needed) {
        return value;
    }
    expanded.resize(wcslen(expanded.c_str()));
    return expanded;
}

std::wstring SanitizeName(std::wstring name) {
    const std::wstring invalid = L"<>:\"/\\|?*";
    for (wchar_t& ch : name) {
        if (invalid.find(ch) != std::wstring::npos || ch < 32) {
            ch = L'_';
        }
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) {
        name.pop_back();
    }
    return name.empty() ? L"Tool" : name;
}

std::wstring ToolFolderPath(const ToolItem& tool) {
    return JoinPath(ExeDirectory(), tool.folder);
}

std::wstring ToolFilePath(const ToolItem& tool) {
    return JoinPath(ToolFolderPath(tool), tool.filename);
}

std::wstring ToolExtractPath(const ToolItem& tool) {
    return JoinPath(ToolFolderPath(tool), SanitizeName(tool.name));
}

bool EnsureDirectory(const std::wstring& path) {
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

std::wstring LastErrorText(DWORD error = GetLastError()) {
    wchar_t* raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    std::wstring text = len > 0 && raw ? std::wstring(raw, len) : L"Erro desconhecido";
    if (raw) {
        LocalFree(raw);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L'.')) {
        text.pop_back();
    }
    return text;
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring FormatBytes(unsigned long long bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }

    std::wstringstream ss;
    if (unit == 0) {
        ss << static_cast<unsigned long long>(value) << L" " << units[unit];
    } else {
        ss.setf(std::ios::fixed);
        ss.precision(value >= 10.0 ? 1 : 2);
        ss << value << L" " << units[unit];
    }
    return ss.str();
}

std::wstring EscapePowerShellSingleQuoted(std::wstring value) {
    return ReplaceAll(value, L"'", L"''");
}

bool CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_mainWindow)) {
        return false;
    }

    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }

    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    return true;
}

void NotifyToolChanged(int index) {
    if (g_mainWindow) {
        PostMessageW(g_mainWindow, WM_APP_TOOL_CHANGED, static_cast<WPARAM>(index), 0);
    }
}

ToolSnapshot SnapshotTool(int index) {
    std::lock_guard<std::mutex> lock(g_toolsMutex);
    const ToolItem& tool = g_tools.at(static_cast<size_t>(index));
    ToolSnapshot snapshot;
    snapshot.name = tool.name;
    snapshot.folder = tool.folder;
    snapshot.status = tool.status;
    snapshot.description = tool.description;
    snapshot.busy = tool.busy;
    snapshot.downloaded = tool.downloaded;
    snapshot.failed = tool.failed;
    snapshot.progress = tool.progress;
    return snapshot;
}

void SetToolState(int index, const std::wstring& status, int progress, bool busy, bool downloaded, bool failed) {
    {
        std::lock_guard<std::mutex> lock(g_toolsMutex);
        ToolItem& tool = g_tools.at(static_cast<size_t>(index));
        tool.status = status;
        tool.progress = ClampInt(progress, 0, 100);
        tool.busy = busy;
        tool.downloaded = downloaded;
        tool.failed = failed;
    }
    NotifyToolChanged(index);
}

void UpdateToolProgress(int index, const std::wstring& status, int progress) {
    {
        std::lock_guard<std::mutex> lock(g_toolsMutex);
        ToolItem& tool = g_tools.at(static_cast<size_t>(index));
        tool.status = status;
        if (progress >= 0) {
            tool.progress = ClampInt(progress, 0, 100);
        }
    }
    NotifyToolChanged(index);
}

bool ReserveTool(int index) {
    {
        std::lock_guard<std::mutex> lock(g_toolsMutex);
        ToolItem& tool = g_tools.at(static_cast<size_t>(index));
        if (tool.busy) {
            return false;
        }
        tool.busy = true;
        tool.downloaded = false;
        tool.failed = false;
        tool.progress = 0;
        tool.status = L"Preparando...";
    }
    NotifyToolChanged(index);
    return true;
}

void AddTooltip(HWND control, const std::wstring& text) {
    if (!g_tooltip || !control || text.empty()) {
        return;
    }

    g_tooltipTexts[control] = text;

    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = g_mainWindow;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<LPWSTR>(g_tooltipTexts[control].c_str());
    SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void ApplyFont(HWND hwnd, HFONT font = nullptr) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
}

ThemeColors LightTheme() {
    ThemeColors theme;
    theme.window = RGB(240, 243, 248);
    theme.panel = RGB(255, 255, 255);
    theme.row = RGB(249, 250, 253);
    theme.border = RGB(221, 226, 233);
    theme.text = RGB(28, 33, 43);
    theme.mutedText = RGB(104, 112, 126);
    theme.editBackground = theme.panel;
    theme.editText = RGB(28, 33, 43);
    return theme;
}

ThemeColors DarkTheme() {
    ThemeColors theme;
    theme.window = RGB(23, 25, 30);
    theme.panel = RGB(32, 35, 42);
    theme.row = RGB(40, 44, 52);
    theme.border = RGB(60, 65, 76);
    theme.text = RGB(229, 233, 240);
    theme.mutedText = RGB(157, 165, 179);
    theme.editBackground = theme.panel;
    theme.editText = RGB(229, 233, 240);
    return theme;
}

void ApplyTheme(bool dark) {
    g_darkMode = dark;
    g_theme = dark ? DarkTheme() : LightTheme();

    HBRUSH oldWindowBrush = g_windowBrush;
    HBRUSH oldPanelBrush = g_panelBrush;
    HBRUSH oldRowBrush = g_rowBrush;
    HBRUSH oldBorderBrush = g_borderBrush;

    g_windowBrush = CreateSolidBrush(g_theme.window);
    g_panelBrush = CreateSolidBrush(g_theme.panel);
    g_rowBrush = CreateSolidBrush(g_theme.row);
    g_borderBrush = CreateSolidBrush(g_theme.border);

    if (oldWindowBrush) DeleteObject(oldWindowBrush);
    if (oldPanelBrush) DeleteObject(oldPanelBrush);
    if (oldRowBrush) DeleteObject(oldRowBrush);
    if (oldBorderBrush) DeleteObject(oldBorderBrush);

    if (g_themeToggleButton) {
        SetWindowTextW(g_themeToggleButton, dark ? L"Modo claro" : L"Modo escuro");
    }

    if (g_mainWindow) {
        RedrawWindow(g_mainWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

ToolItem MakeTool(const wchar_t* name,
                  const wchar_t* folder,
                  const wchar_t* url,
                  const wchar_t* filename,
                  const wchar_t* description,
                  bool archive) {
    ToolItem tool;
    tool.name = name;
    tool.folder = folder;
    tool.url = url;
    tool.filename = filename;
    tool.description = description;
    tool.archive = archive;
    return tool;
}

ScriptItem MakeScript(const wchar_t* name, const wchar_t* description, const wchar_t* command, bool elevated = false) {
    ScriptItem script;
    script.name = name;
    script.description = description;
    script.command = command;
    script.elevated = elevated;
    return script;
}

ShortcutItem MakeShortcut(const wchar_t* name,
                          const wchar_t* command,
                          const wchar_t* description) {
    ShortcutItem shortcut;
    shortcut.name = name;
    shortcut.command = command;
    shortcut.description = description;
    return shortcut;
}

void InitializeTools() {
    g_categories = {L"Others", L"Spokwn", L"Orbdiff", L"Nirsoft", L"Scripts", L"Atalhos Windows"};

    g_tools = {
        MakeTool(L"Hayabusa", L"Others",
                 L"https://github.com/Yamato-Security/hayabusa/releases/download/v1.7.0/hayabusa-1.7.0-windows.zip",
                 L"hayabusa-1.7.0.zip",
                 L"Hayabusa para gerar logs do Event Viewer e gerar logs em vários tipos de arquivo", true),
        MakeTool(L"AltDetector", L"Others",
                 L"https://github.com/Smoothzada/Minecraft-Alt-Detector/releases/download/Compiled/AltDetector.exe",
                 L"AltDetector.exe",
                 L"Scanner de Alts do tal do Smoothzada", false),
        MakeTool(L"Hollows Hunter", L"Others",
                 L"https://github.com/hasherezade/hollows_hunter/releases/download/v0.4.1.1/hollows_hunter64.exe",
                 L"hollows_hunter64.exe",
                 L"Ferramenta para encontrar processos suspeitos e tecnicas de process hollowing.", false),
        MakeTool(L"Volatility Workbench", L"Others",
                 L"https://www.osforensics.com/downloads/VolatilityWorkbench.zip",
                 L"VolatilityWorkbench.zip",
                 L"Interface grafica para analise de memoria com Volatility.", true),
        MakeTool(L"Everything", L"Others",
                 L"https://www.voidtools.com/Everything-1.4.1.1032.x86-Setup.exe",
                 L"Everything-1.4.1.1032.x86-Setup.exe",
                 L"Indexador e buscador rapido de arquivos no Windows.", false),
        MakeTool(L"System Informer", L"Others",
                 L"https://github.com/winsiderss/si-builds/releases/download/4.0.26144.416/systeminformer-build-canary-setup.exe",
                 L"systeminformer-build-canary-setup.exe",
                 L"Monitor avancado de processos, servicos, rede e recursos do sistema.", false),
        MakeTool(L"SmoothTool", L"Others",
                 L"https://github.com/Smoothzada/SmoothTool/releases/download/SmoothTool-v1.5.3/SmoothTool.exe",
                 L"SmoothTool.exe",
                 L"Ferramenta utilitaria distribuida pelo repositorio SmoothTool.", false),
        MakeTool(L"RedLotus Task Sentinel", L"Others",
                 L"https://github.com/ItzIceHere/RedLotus-Task-Sentinel/releases/download/RL/RedLotusTaskSentinel.exe",
                 L"RedLotusTaskSentinel.exe",
                 L"Analise e inspecao de tarefas relacionadas ao projeto RedLotus.", false),
        MakeTool(L"RedLotus Mod Analyzer", L"Others",
                 L"https://github.com/ItzIceHere/RedLotus-Mod-Analyzer/releases/download/RL/RedLotusModAnalyzer.exe",
                 L"RedLotusModAnalyzer.exe",
                 L"Analisador de modulos do projeto RedLotus.", false),
        MakeTool(L"AccessData FTK Imager 4.7.1", L"Others",
                 L"https://www.mediafire.com/file/wvw1ho34otn2neu/AccessData_FTK_Imager_4.7.1.exe/file",
                 L"AccessData_FTK_Imager_4.7.1.exe",
                 L"Ferramenta de imagem e coleta forense FTK Imager.", false),
        MakeTool(L"Recuva", L"Others",
                 L"https://sourceforge.net/projects/recuva/files/latest/download",
                 L"rcsetup154.exe",
                 L"Utilitario para recuperacao de arquivos apagados.", false),
        MakeTool(L"CachedProgramsList", L"Others",
                 L"https://github.com/ponei/CachedProgramsList/releases/download/1.1/CachedProgramsList.exe",
                 L"CachedProgramsList.exe",
                 L"Lista programas em cache para auxiliar triagem de execucao.", false),
        MakeTool(L"FishMan", L"Others",
                 L"https://github.com/kahzgbb/fishman/releases/download/Release/FishMan.exe",
                 L"FishMan.exe",
                 L"Ferramenta FishMan distribuida pelo repositorio kahzgbb/fishman.", false),
        MakeTool(L"Technical Utilities", L"Others",
                 L"https://github.com/txvch/Screenshare-Collector/releases/download/tech/Technical.Utilities.exe",
                 L"Technical.Utilities.exe",
                 L"Utilitario tecnico do Screenshare Collector.", false),
        MakeTool(L"Magnet RAM Capture", L"Others",
                 L"https://go.magnetforensics.com/e/52162/mail-utm-campaign-UTMC-0000044/llr4bg/1682303419/h/1kJdgbng_zGm1nNYV9J6rjlcB83Qnp75ccBILwRXGZw",
                 L"MRCv120.exe",
                 L"Ferramenta Magnet Forensics para captura de memoria RAM.", false),
        MakeTool(L"Magnet Process Capture", L"Others",
                 L"https://go.magnetforensics.com/e/52162/MagnetProcessCapture/kpt99v/1612057320/h/AYroh2VZeIDWQwxfA0dfD4ygbhEoOtQZbhrwlCC5v8c",
                 L"MagnetProcessCaptureV13.zip",
                 L"Ferramenta Magnet Forensics para captura de memoria de processos.", true),

        MakeTool(L"JournalTrace", L"Spokwn",
                 L"https://github.com/spokwn/JournalTrace/releases/download/1.2/JournalTrace.exe",
                 L"JournalTrace.exe",
                 L"Ferramenta para inspecao de rastros relacionados ao NTFS USN Journal.", false),
        MakeTool(L"Espouken", L"Spokwn",
                 L"https://github.com/spokwn/Tool/releases/download/v1.1.3/espouken.exe",
                 L"espouken.exe",
                 L"Ferramenta do repositorio spokwn/Tool.", false),
        MakeTool(L"KernelLiveDumpTool", L"Spokwn",
                 L"https://github.com/spokwn/KernelLiveDumpTool/releases/download/v1.1/KernelLiveDumpTool.exe",
                 L"KernelLiveDumpTool.exe",
                 L"Coleta ou analise relacionada a dumps de kernel ao vivo.", false),
        MakeTool(L"ActivitiesCache Parser", L"Spokwn",
                 L"https://github.com/spokwn/ActivitiesCache-execution/releases/download/v0.6.5/ActivitiesCacheParser.exe",
                 L"ActivitiesCacheParser.exe",
                 L"Parser para artefatos ActivitiesCache e execucoes recentes.", false),

        MakeTool(L"Fileless", L"Orbdiff",
                 L"https://github.com/Orbdiff/Fileless/releases/download/v1.3/fileless.exe",
                 L"fileless.exe",
                 L"Ferramenta para analise de indicios de execucao fileless.", false),
        MakeTool(L"DPS Analyzer", L"Orbdiff",
                 L"https://github.com/Orbdiff/DPS-Analyzer/releases/download/v1.1/dpsanalyzer.exe",
                 L"dpsanalyzer.exe",
                 L"Analisador de artefatos DPS.", false),
        MakeTool(L"UserAssistView", L"Orbdiff",
                 L"https://github.com/Orbdiff/UserAssistView/releases/download/v1.0/UserAssistView.exe",
                 L"UserAssistView.exe",
                 L"Visualizador para artefatos UserAssist do Windows.", false),
        MakeTool(L"InjGen", L"Orbdiff",
                 L"https://github.com/Orbdiff/InjGen/releases/download/fork/InjGen.exe",
                 L"InjGen.exe",
                 L"Utilitario relacionado a testes e analise de injecao.", false),
        MakeTool(L"AmcacheParser", L"Orbdiff",
                 L"https://github.com/Orbdiff/AmcacheParser/releases/download/v1.0/AmcacheParser.exe",
                 L"AmcacheParser.exe",
                 L"Parser para artefatos Amcache do Windows.", false),
        MakeTool(L"USBDetector", L"Orbdiff",
                 L"https://github.com/Orbdiff/USBDetector/releases/download/v1.1/USBDetector.exe",
                 L"USBDetector.exe",
                 L"Ferramenta para detectar historico e evidencias de dispositivos USB.", false),
        MakeTool(L"JARParser", L"Orbdiff",
                 L"https://github.com/Orbdiff/JARParser/releases/download/v1.2/JARParser.exe",
                 L"JARParser.exe",
                 L"Parser para arquivos JAR.", false),
        MakeTool(L"PFTrace", L"Orbdiff",
                 L"https://github.com/Orbdiff/PFTrace/releases/download/v1.0.1/PFTrace.exe",
                 L"PFTrace.exe",
                 L"Ferramenta para rastrear e analisar Prefetch.", false),
        MakeTool(L"BAMReveal", L"Orbdiff",
                 L"https://github.com/Orbdiff/BAMReveal/releases/download/v1.3/BAMReveal.exe",
                 L"BAMReveal.exe",
                 L"Visualizador de artefatos BAM do Windows.", false),
        MakeTool(L"StringsParser", L"Orbdiff",
                 L"https://github.com/Orbdiff/StringsParser/releases/download/v0.1.1/StringsParser.exe",
                 L"StringsParser.exe",
                 L"Parser para extracao e analise de strings.", false),
        MakeTool(L"MFTParser", L"Orbdiff",
                 L"https://github.com/Orbdiff/MFTParser/releases/download/v0.1/mftparser.exe",
                 L"mftparser.exe",
                 L"Parser para artefatos da Master File Table.", false),

        MakeTool(L"BrowserDownloadsView", L"Nirsoft",
                 L"https://www.nirsoft.net/utils/browserdownloadsview-x64.zip",
                 L"browserdownloadsview-x64.zip",
                 L"Visualizador NirSoft para historico de downloads de navegadores.", true),
        MakeTool(L"DriverView", L"Nirsoft",
                 L"https://www.nirsoft.net/utils/driverview-x64.zip",
                 L"driverview-x64.zip",
                 L"Visualizador NirSoft para informacoes de unidades e dispositivos.", true),
        MakeTool(L"JumpListsView", L"Nirsoft",
                 L"https://www.nirsoft.net/utils/jumplistsview.zip",
                 L"jumplistsview.zip",
                 L"Visualizador NirSoft para artefatos Jump List.", true),
        MakeTool(L"LastActivityView", L"Nirsoft",
                 L"https://www.nirsoft.net/utils/lastactivityview.zip",
                 L"lastactivityview.zip",
                 L"Visualizador NirSoft para atividades recentes do Windows.", true),
        MakeTool(L"PreviousFilesRecovery", L"Nirsoft",
                 L"https://www.nirsoft.net/utils/previousfilesrecovery-x64.zip",
                 L"previousfilesrecovery-x64.zip",
                 L"Ferramenta NirSoft para recuperar versoes anteriores de arquivos.", true),
        MakeTool(L"TaskSchedulerView", L"Nirsoft",
                 L"https://www.nirsoft.net/utils/taskschedulerview-x64.zip",
                 L"taskschedulerview-x64.zip",
                 L"Visualizador NirSoft para tarefas agendadas.", true),
        MakeTool(L"WinPrefetchView", L"Nirsoft",
                 L"https://www.nirsoft.net/utils/winprefetchview-x64.zip",
                 L"winprefetchview-x64.zip",
                 L"Visualizador NirSoft para arquivos Prefetch.", true),
    };
}

void InitializeScripts() {
    g_scripts = {
        MakeScript(L"Habilitar servicos",
                   L"Executa o Service-Enabler.ps1 com politica de execucao liberada apenas no processo atual.",
                   L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force; Invoke-Expression (Invoke-RestMethod 'https://raw.githubusercontent.com/praiselily/lilith-ps/refs/heads/main/Service-Enabler.ps1')\"",
                   true),
        MakeScript(L"Kernel Scanner",
                   L"Executa o Nacio Kernel Scanner como administrador e salva o CSV em Desktop\\ForensicToolReports.",
                   L"powershell.exe -NoProfile -ExecutionPolicy Bypass -NoExit -Command \"$outDir=Join-Path $env:USERPROFILE 'Desktop\\ForensicToolReports'; New-Item -ItemType Directory -Force -Path $outDir | Out-Null; Set-Location $outDir; Invoke-Expression (Invoke-RestMethod 'https://raw.githubusercontent.com/Nacio78923/Kernel-Scanner/refs/heads/main/Nacio%20Kernel%20Scanner.ps1')\"",
                   true),
        MakeScript(L"Eventvwr Logs Scanner",
                   L"Executa o Eventvwr Logs Scanner como administrador e exporta eventos recentes para CSV em Desktop\\ForensicToolReports.",
                   L"powershell.exe -NoProfile -ExecutionPolicy Bypass -NoExit -Command \"$outDir=Join-Path $env:USERPROFILE 'Desktop\\ForensicToolReports'; New-Item -ItemType Directory -Force -Path $outDir | Out-Null; Set-Location $outDir; Invoke-Expression (Invoke-RestMethod 'https://raw.githubusercontent.com/Nacio78923/Eventvwr-Logs-Scanner/refs/heads/main/EventLogExporter.ps1')\"",
                   true),
        MakeScript(L"Aquisição de HWID",
                   L"Exporta o HWID do usuário para as Downloads.",
                   L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force; Invoke-Expression (Invoke-RestMethod 'https://gist.githubusercontent.com/Smoothzada/ad9b391a3bba387028f2710394a61e18/raw/4f91e10608944047d924beec6cb83d0e2e7abd8a/Hwid-Acquisition.ps1')\"",
                   true),
    };
}

void InitializeShortcuts() {
    g_shortcuts = {
        MakeShortcut(L"Control Folders", L"control folders",
                     L"Ativa ou desativa os itens ocultos das pastas."),
        MakeShortcut(L"C:\\$Recycle.Bin", L"%SystemDrive%\\$Recycle.Bin",
                     L"Mostra se a lixeira foi modificada."),
        MakeShortcut(L"Prefetch", L"%SystemRoot%\\Prefetch",
                     L"Mostra os arquivos abertos recentemente, incluindo executaveis."),
        MakeShortcut(L"Temp", L"%TEMP%",
                     L"Mostra os arquivos temporarios do usuario."),
        MakeShortcut(L"Recent", L"shell:recent",
                     L"Mostra os arquivos abertos recentemente."),
        MakeShortcut(L"History", L"%USERPROFILE%\\AppData\\Local\\Microsoft\\Windows\\History",
                     L"Mostra os arquivos executados durante o dia."),
        MakeShortcut(L"CLR UsageLogs", L"%USERPROFILE%\\AppData\\Local\\Microsoft\\CLR_v4.0\\UsageLogs",
                     L"Logs dos aplicativos usados pelo CLR v4.0."),
        MakeShortcut(L"WER ReportArchive", L"%ProgramData%\\Microsoft\\Windows\\WER\\ReportArchive",
                     L"Mostra os appcrash arquivados pelo Windows Error Reporting."),
        MakeShortcut(L"Programas instalados", L"appwiz.cpl",
                     L"Mostra os programas instalados."),
        MakeShortcut(L"Usuarios do computador", L"netplwiz",
                     L"Mostra os usuarios do computador."),
        MakeShortcut(L"Icones do computador", L"explorer.exe shell:::{05d7b0f4-2121-4eff-bf6b-ed3f69b894d9}",
                     L"Mostra os icones do computador."),
        MakeShortcut(L"Event Viewer", L"eventvwr.msc",
                     L"Mostra os eventos do Windows."),
        MakeShortcut(L"Services", L"services.msc",
                     L"Mostra a lista de processos e servicos em execucao."),
        MakeShortcut(L"Group Policy", L"gpedit.msc",
                     L"Mostra a lista de politicas dos aplicativos."),
        MakeShortcut(L"System Information", L"msinfo32",
                     L"Ajuda a verificar informacoes de sistema e sinais de maquina virtual."),
        MakeShortcut(L"PowerShell PSReadLine", L"%APPDATA%\\Microsoft\\Windows\\PowerShell\\PSReadLine",
                     L"Historico do PowerShell salvo pelo PSReadLine."),
        MakeShortcut(L"Indexed Locations", L"%USERPROFILE%\\Searches\\Indexed Locations.search-ms",
                     L"Abre a busca salva Indexed Locations."),
    };
}

void LoadScripts() {
    const std::wstring iniPath = JoinPath(ExeDirectory(), L"scripts.ini");
    std::vector<wchar_t> sections(32768);
    const DWORD chars = GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), iniPath.c_str());
    if (chars == 0) {
        return;
    }

    const std::wstring appDir = ExeDirectory();
    for (wchar_t* section = sections.data(); *section; section += wcslen(section) + 1) {
        wchar_t name[256]{};
        wchar_t description[1024]{};
        wchar_t command[2048]{};

        GetPrivateProfileStringW(section, L"Name", L"", name, 256, iniPath.c_str());
        GetPrivateProfileStringW(section, L"Description", L"", description, 1024, iniPath.c_str());
        GetPrivateProfileStringW(section, L"Command", L"", command, 2048, iniPath.c_str());

        if (name[0] == L'\0' || command[0] == L'\0') {
            continue;
        }

        ScriptItem script;
        script.name = name;
        script.description = description;
        script.command = ReplaceAll(command, L"{APP_DIR}", appDir);
        script.command = ReplaceAll(script.command, L"{TOOLS_DIR}", appDir);
        script.elevated = GetPrivateProfileIntW(section, L"Elevated", 0, iniPath.c_str()) != 0;
        g_scripts.push_back(script);
    }
}

std::wstring QueryHeaderString(HINTERNET request, DWORD query) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return L"";
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, &value[0], &size, WINHTTP_NO_HEADER_INDEX)) {
        return L"";
    }
    value.resize(wcslen(value.c_str()));
    return value;
}

bool FetchUrlToMemory(const std::wstring& url, std::string& body, DWORD maxBytes, std::wstring& error) {
    body.clear();

    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);

    wchar_t host[512]{};
    wchar_t path[4096]{};
    wchar_t extra[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ArrayCount(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ArrayCount(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ArrayCount(extra);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = L"URL invalida: " + LastErrorText();
        return false;
    }

    std::wstring requestPath = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) {
        requestPath += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"ForensicToolDownloader/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        error = L"WinHttpOpen falhou: " + LastErrorText();
        return false;
    }

    WinHttpSetTimeouts(session, 30000, 30000, 30000, 30000);

    HINTERNET connect = WinHttpConnect(session,
                                       std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(),
                                       parts.nPort,
                                       0);
    if (!connect) {
        error = L"WinHttpConnect falhou: " + LastErrorText();
        WinHttpCloseHandle(session);
        return false;
    }

    LPCWSTR acceptTypes[] = {L"*/*", nullptr};
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect,
                                           L"GET",
                                           requestPath.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           acceptTypes,
                                           flags);
    if (!request) {
        error = L"WinHttpOpenRequest falhou: " + LastErrorText();
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    const wchar_t* headers = L"Accept: text/html,*/*\r\n";
    bool ok = WinHttpSendRequest(request,
                                 headers,
                                 static_cast<DWORD>(-1L),
                                 WINHTTP_NO_REQUEST_DATA,
                                 0,
                                 0,
                                 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        error = L"Requisicao falhou: " + LastErrorText();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode >= 400) {
        std::wstringstream ss;
        ss << L"HTTP " << statusCode;
        error = ss.str();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::vector<char> buffer(32 * 1024);
    while (true) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead)) {
            error = L"Leitura da pagina falhou: " + LastErrorText();
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        if (bytesRead == 0) {
            break;
        }
        if (body.size() + bytesRead > maxBytes) {
            error = L"Resposta grande demais para resolver link";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        body.append(buffer.data(), bytesRead);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return true;
}

bool ResolveMediaFireUrl(const std::wstring& url, int toolIndex, std::wstring& resolvedUrl, std::wstring& error) {
    resolvedUrl = url;
    const std::wstring lower = ToLower(url);
    if (lower.find(L"mediafire.com/file/") == std::wstring::npos) {
        return true;
    }

    UpdateToolProgress(toolIndex, L"Resolvendo link MediaFire...", 0);

    std::string html;
    if (!FetchUrlToMemory(url, html, 2 * 1024 * 1024, error)) {
        return false;
    }

    size_t start = 0;
    while ((start = html.find("https://download", start)) != std::string::npos) {
        const size_t end = html.find_first_of("\"'<> \r\n\t", start);
        if (end == std::string::npos) {
            break;
        }

        std::string candidate = html.substr(start, end - start);
        if (candidate.find("mediafire.com") != std::string::npos) {
            std::wstring wideCandidate = WidenAscii(candidate);
            wideCandidate = ReplaceAll(wideCandidate, L"&amp;", L"&");
            resolvedUrl = wideCandidate;
            return true;
        }
        start = end;
    }

    error = L"Nao foi possivel encontrar o link direto do MediaFire";
    return false;
}

bool DownloadFileWinHttp(const std::wstring& url, const std::wstring& destination, int toolIndex, std::wstring& error) {
    std::wstring effectiveUrl;
    if (!ResolveMediaFireUrl(url, toolIndex, effectiveUrl, error)) {
        return false;
    }

    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);

    wchar_t host[512]{};
    wchar_t path[4096]{};
    wchar_t extra[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ArrayCount(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ArrayCount(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ArrayCount(extra);

    if (!WinHttpCrackUrl(effectiveUrl.c_str(), 0, 0, &parts)) {
        error = L"URL invalida: " + LastErrorText();
        return false;
    }

    std::wstring requestPath = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) {
        requestPath += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"ForensicToolDownloader/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session) {
        error = L"WinHttpOpen falhou: " + LastErrorText();
        return false;
    }

    WinHttpSetTimeouts(session, 30000, 30000, 30000, 30000);

    HINTERNET connect = WinHttpConnect(session,
                                       std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(),
                                       parts.nPort,
                                       0);
    if (!connect) {
        error = L"WinHttpConnect falhou: " + LastErrorText();
        WinHttpCloseHandle(session);
        return false;
    }

    LPCWSTR acceptTypes[] = {L"*/*", nullptr};
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect,
                                           L"GET",
                                           requestPath.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           acceptTypes,
                                           flags);
    if (!request) {
        error = L"WinHttpOpenRequest falhou: " + LastErrorText();
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    const wchar_t* headers = L"Accept: */*\r\n";
    if (!WinHttpSendRequest(request,
                            headers,
                            static_cast<DWORD>(-1L),
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0)) {
        error = L"Envio da requisicao falhou: " + LastErrorText();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        error = L"Resposta HTTP falhou: " + LastErrorText();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode >= 400) {
        std::wstringstream ss;
        ss << L"HTTP " << statusCode;
        error = ss.str();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    const std::wstring lengthText = QueryHeaderString(request, WINHTTP_QUERY_CONTENT_LENGTH);
    unsigned long long totalBytes = 0;
    if (!lengthText.empty()) {
        totalBytes = _wcstoui64(lengthText.c_str(), nullptr, 10);
    }

    HANDLE file = CreateFileW(destination.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Nao foi possivel criar o arquivo: " + LastErrorText();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    unsigned long long downloaded = 0;
    int lastPercent = -1;
    unsigned long long lastStatusBytes = 0;

    while (true) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead)) {
            error = L"Leitura do download falhou: " + LastErrorText();
            CloseHandle(file);
            DeleteFileW(destination.c_str());
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        if (bytesRead == 0) {
            break;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(file, buffer.data(), bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead) {
            error = L"Gravacao do arquivo falhou: " + LastErrorText();
            CloseHandle(file);
            DeleteFileW(destination.c_str());
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }

        downloaded += bytesRead;
        if (totalBytes > 0) {
            const int percent = static_cast<int>((downloaded * 100ULL) / totalBytes);
            if (percent != lastPercent) {
                lastPercent = percent;
                std::wstringstream ss;
                ss << L"Baixando... " << percent << L"%";
                UpdateToolProgress(toolIndex, ss.str(), percent);
            }
        } else if (downloaded - lastStatusBytes >= 512ULL * 1024ULL) {
            lastStatusBytes = downloaded;
            UpdateToolProgress(toolIndex, L"Baixando... " + FormatBytes(downloaded), -1);
        }
    }

    CloseHandle(file);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    UpdateToolProgress(toolIndex, L"Baixando... 100%", 100);
    return true;
}

void DownloadWorkerReserved(int index) {
    ToolItem tool;
    {
        std::lock_guard<std::mutex> lock(g_toolsMutex);
        tool = g_tools.at(static_cast<size_t>(index));
    }

    if (!EnsureDirectory(ToolFolderPath(tool))) {
        SetToolState(index, L"Falha ao criar pasta", 0, false, false, true);
        PostMessageW(g_mainWindow, WM_APP_DOWNLOAD_FINISHED, static_cast<WPARAM>(index), 0);
        return;
    }

    UpdateToolProgress(index, L"Conectando...", 0);

    std::wstring error;
    const bool ok = DownloadFileWinHttp(tool.url, ToolFilePath(tool), index, error);
    if (ok) {
        SetToolState(index, L"Baixado", 100, false, true, false);
        PostMessageW(g_mainWindow, WM_APP_DOWNLOAD_FINISHED, static_cast<WPARAM>(index), 1);
    } else {
        SetToolState(index, L"Falha: " + error, 0, false, false, true);
        PostMessageW(g_mainWindow, WM_APP_DOWNLOAD_FINISHED, static_cast<WPARAM>(index), 0);
    }
}

void StartDownload(int index) {
    if (!ReserveTool(index)) {
        return;
    }
    std::thread([index]() {
        DownloadWorkerReserved(index);
    }).detach();
}

std::vector<int> AllToolIndexes() {
    std::vector<int> indexes;
    std::lock_guard<std::mutex> lock(g_toolsMutex);
    for (size_t i = 0; i < g_tools.size(); ++i) {
        indexes.push_back(static_cast<int>(i));
    }
    return indexes;
}

std::vector<int> ToolIndexesForCategory(const std::wstring& category) {
    std::vector<int> indexes;
    std::lock_guard<std::mutex> lock(g_toolsMutex);
    for (size_t i = 0; i < g_tools.size(); ++i) {
        if (g_tools[i].folder == category) {
            indexes.push_back(static_cast<int>(i));
        }
    }
    return indexes;
}

void DownloadIndexesWorker(std::vector<int> indexes) {
    std::atomic_size_t nextIndex{0};
    const size_t workerCount = std::min(indexes.size(), static_cast<size_t>(kMaxParallelDownloads));
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&indexes, &nextIndex]() {
            while (true) {
                const size_t current = nextIndex.fetch_add(1);
                if (current >= indexes.size()) {
                    return;
                }

                const int index = indexes[current];
                if (!ReserveTool(index)) {
                    continue;
                }
                DownloadWorkerReserved(index);
            }
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    g_downloadAllRunning = false;
    PostMessageW(g_mainWindow, WM_APP_DOWNLOAD_ALL_FINISHED, 0, 0);
}

void StartDownloadIndexes(std::vector<int> indexes) {
    if (indexes.empty()) {
        MessageBoxW(g_mainWindow, L"Nenhuma ferramenta encontrada para baixar nesta aba.", L"Downloads", MB_ICONINFORMATION);
        return;
    }

    bool expected = false;
    if (!g_downloadAllRunning.compare_exchange_strong(expected, true)) {
        return;
    }
    EnableWindow(g_downloadAllButton, FALSE);
    if (g_downloadTabButton) {
        EnableWindow(g_downloadTabButton, FALSE);
    }
    std::thread([indexes]() {
        DownloadIndexesWorker(indexes);
    }).detach();
}

void StartDownloadAll() {
    StartDownloadIndexes(AllToolIndexes());
}

void StartDownloadCurrentTab();

void ExtractionWorker(int index, bool deleteArchive) {
    ToolItem tool;
    {
        std::lock_guard<std::mutex> lock(g_toolsMutex);
        tool = g_tools.at(static_cast<size_t>(index));
    }

    SetToolState(index, L"Extraindo...", 100, true, true, false);
    EnsureDirectory(ToolExtractPath(tool));

    const std::wstring source = ToolFilePath(tool);
    const std::wstring destination = ToolExtractPath(tool);
    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '";
    command += EscapePowerShellSingleQuoted(source);
    command += L"' -DestinationPath '";
    command += EscapePowerShellSingleQuoted(destination);
    command += L"' -Force\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    BOOL created = CreateProcessW(nullptr,
                                  &mutableCommand[0],
                                  nullptr,
                                  nullptr,
                                  FALSE,
                                  CREATE_NO_WINDOW,
                                  nullptr,
                                  ExeDirectory().c_str(),
                                  &startup,
                                  &process);
    if (!created) {
        SetToolState(index, L"Falha ao extrair: " + LastErrorText(), 100, false, true, true);
        PostMessageW(g_mainWindow, WM_APP_EXTRACTION_FINISHED, static_cast<WPARAM>(index), 0);
        return;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (exitCode == 0) {
        if (deleteArchive) {
            DeleteFileW(source.c_str());
        }
        SetToolState(index, L"Extraido", 100, false, true, false);
        PostMessageW(g_mainWindow, WM_APP_EXTRACTION_FINISHED, static_cast<WPARAM>(index), 1);
    } else {
        SetToolState(index, L"Falha ao extrair", 100, false, true, true);
        PostMessageW(g_mainWindow, WM_APP_EXTRACTION_FINISHED, static_cast<WPARAM>(index), 0);
    }
}

void StartExtraction(int index, bool deleteArchive) {
    std::thread([index, deleteArchive]() {
        ExtractionWorker(index, deleteArchive);
    }).detach();
}

void RunScript(int index) {
    if (index < 0 || index >= static_cast<int>(g_scripts.size())) {
        return;
    }

    const ScriptItem& script = g_scripts[static_cast<size_t>(index)];
    std::wstring parameters = L"/C " + script.command;
    HINSTANCE result = ShellExecuteW(g_mainWindow,
                                     script.elevated ? L"runas" : L"open",
                                     L"cmd.exe",
                                     parameters.c_str(),
                                     ExeDirectory().c_str(),
                                     SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(g_mainWindow, (L"Nao foi possivel executar o script:\n" + LastErrorText()).c_str(), L"Scripts", MB_ICONERROR);
    }
}

void RunShortcut(int index) {
    if (index < 0 || index >= static_cast<int>(g_shortcuts.size())) {
        return;
    }

    const ShortcutItem& shortcut = g_shortcuts[static_cast<size_t>(index)];
    std::wstring command = Trim(ExpandEnvironment(shortcut.command));
    std::wstring file = command;
    std::wstring parameters;

    if (StartsWithInsensitive(command, L"control ")) {
        file = L"control.exe";
        parameters = Trim(command.substr(8));
    } else if (StartsWithInsensitive(command, L"explorer.exe ")) {
        file = L"explorer.exe";
        parameters = Trim(command.substr(13));
    } else if (StartsWithInsensitive(command, L"shell:")) {
        file = L"explorer.exe";
        parameters = command;
    }

    HINSTANCE result = ShellExecuteW(g_mainWindow,
                                     L"open",
                                     file.c_str(),
                                     parameters.empty() ? nullptr : parameters.c_str(),
                                     nullptr,
                                     SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(g_mainWindow, (L"Nao foi possivel abrir o atalho:\n" + command).c_str(), L"Atalhos Windows", MB_ICONERROR);
    }
}

void CopyShortcutCommand(int index) {
    if (index < 0 || index >= static_cast<int>(g_shortcuts.size())) {
        return;
    }

    const std::wstring command = g_shortcuts[static_cast<size_t>(index)].command;
    if (!CopyTextToClipboard(command)) {
        MessageBoxW(g_mainWindow, L"Nao foi possivel copiar o comando.", L"Atalhos Windows", MB_ICONERROR);
    }
}

void RenderContent();
void LayoutMainWindow();

bool HandleCommand(WPARAM wParam) {
    const int id = LOWORD(wParam);
    const int code = HIWORD(wParam);

    if (id == ID_SEARCH && code == EN_CHANGE) {
        RenderContent();
        LayoutMainWindow();
        return true;
    }
    if (id == ID_GRID_MODE && code == BN_CLICKED) {
        RenderContent();
        return true;
    }
    if (id == ID_THEME_TOGGLE && code == BN_CLICKED) {
        ApplyTheme(!g_darkMode);
        return true;
    }
    if (id == ID_DOWNLOAD_ALL && code == BN_CLICKED) {
        StartDownloadAll();
        return true;
    }
    if (id == ID_DOWNLOAD_TAB && code == BN_CLICKED) {
        StartDownloadCurrentTab();
        return true;
    }
    if (id >= ID_TOOL_BUTTON_BASE && id < ID_TOOL_BUTTON_BASE + ID_COMMAND_RANGE && code == BN_CLICKED) {
        StartDownload(id - ID_TOOL_BUTTON_BASE);
        return true;
    }
    if (id >= ID_SCRIPT_BUTTON_BASE && id < ID_SCRIPT_BUTTON_BASE + ID_COMMAND_RANGE && code == BN_CLICKED) {
        RunScript(id - ID_SCRIPT_BUTTON_BASE);
        return true;
    }
    if (id >= ID_SHORTCUT_BUTTON_BASE && id < ID_SHORTCUT_BUTTON_BASE + ID_COMMAND_RANGE && code == BN_CLICKED) {
        RunShortcut(id - ID_SHORTCUT_BUTTON_BASE);
        return true;
    }
    if (id >= ID_SHORTCUT_COPY_BASE && id < ID_SHORTCUT_COPY_BASE + ID_COMMAND_RANGE && code == BN_CLICKED) {
        CopyShortcutCommand(id - ID_SHORTCUT_COPY_BASE);
        return true;
    }

    return false;
}

void ClearRows() {
    for (const ToolRow& row : g_toolRows) {
        if (row.row) {
            DestroyWindow(row.row);
        }
    }
    g_toolRows.clear();

    for (const ScriptRow& row : g_scriptRows) {
        if (row.row) {
            DestroyWindow(row.row);
        }
    }
    g_scriptRows.clear();

    for (const ShortcutRow& row : g_shortcutRows) {
        if (row.row) {
            DestroyWindow(row.row);
        }
    }
    g_shortcutRows.clear();
}

std::wstring ActiveCategory() {
    if (g_activeTab < 0 || g_activeTab >= static_cast<int>(g_categories.size())) {
        return L"";
    }
    return g_categories[static_cast<size_t>(g_activeTab)];
}

bool IsScriptsTab() {
    return ActiveCategory() == L"Scripts";
}

bool IsShortcutsTab() {
    return ActiveCategory() == L"Atalhos Windows";
}

bool IsDownloadTab() {
    return !IsScriptsTab() && !IsShortcutsTab();
}

bool IsSearchActive() {
    return !WindowText(g_searchEdit).empty();
}

void StartDownloadCurrentTab() {
    if (!IsDownloadTab()) {
        return;
    }
    StartDownloadIndexes(ToolIndexesForCategory(ActiveCategory()));
}

std::vector<int> VisibleToolIndexes() {
    std::vector<int> indexes;
    const std::wstring filter = WindowText(g_searchEdit);
    const bool searchAll = !filter.empty();

    std::lock_guard<std::mutex> lock(g_toolsMutex);
    for (size_t i = 0; i < g_tools.size(); ++i) {
        const ToolItem& tool = g_tools[i];
        const bool categoryMatches = searchAll || tool.folder == g_categories[static_cast<size_t>(g_activeTab)];
        if (!categoryMatches) {
            continue;
        }

        const std::wstring searchable = tool.name + L" " + tool.folder + L" " + tool.description + L" " + tool.filename;
        if (ContainsInsensitive(searchable, filter)) {
            indexes.push_back(static_cast<int>(i));
        }
    }
    return indexes;
}

std::vector<int> VisibleScriptIndexes() {
    std::vector<int> indexes;
    const std::wstring filter = WindowText(g_searchEdit);
    for (size_t i = 0; i < g_scripts.size(); ++i) {
        const ScriptItem& script = g_scripts[i];
        const std::wstring searchable = script.name + L" " + script.description + L" " + script.command;
        if (ContainsInsensitive(searchable, filter)) {
            indexes.push_back(static_cast<int>(i));
        }
    }
    return indexes;
}

std::vector<int> VisibleShortcutIndexes() {
    std::vector<int> indexes;
    const std::wstring filter = WindowText(g_searchEdit);
    for (size_t i = 0; i < g_shortcuts.size(); ++i) {
        const ShortcutItem& shortcut = g_shortcuts[i];
        const std::wstring searchable = shortcut.name + L" " + shortcut.description + L" " + shortcut.command;
        if (ContainsInsensitive(searchable, filter)) {
            indexes.push_back(static_cast<int>(i));
        }
    }
    return indexes;
}

void UpdateToolRow(const ToolRow& row) {
    if (!row.row) {
        return;
    }

    ToolSnapshot snapshot = SnapshotTool(row.index);
    SetWindowTextW(row.status, snapshot.status.c_str());
    SendMessageW(row.progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(row.progress, PBM_SETPOS, snapshot.progress, 0);
    SendMessageW(row.progress, PBM_SETSTATE, snapshot.failed ? PBST_ERROR : PBST_NORMAL, 0);

    EnableWindow(row.button, !snapshot.busy);
    SetWindowTextW(row.button, snapshot.busy ? L"Aguarde" : (snapshot.downloaded ? L"Baixar de novo" : L"Baixar"));
}

void UpdateToolRowByIndex(int index) {
    for (const ToolRow& row : g_toolRows) {
        if (row.index == index) {
            UpdateToolRow(row);
            break;
        }
    }
}

void LayoutRows();

HWND CreateLabel(HWND parent, const std::wstring& text, DWORD style = SS_LEFT | SS_ENDELLIPSIS) {
    HWND label = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                 parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    ApplyFont(label);
    return label;
}

void AppendToolRow(int toolIndex, bool showFolder) {
    ToolSnapshot snapshot = SnapshotTool(toolIndex);
    ToolRow rowInfo;
    rowInfo.index = toolIndex;
    rowInfo.row = CreateWindowExW(0, L"DownloaderRow", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, kRowHeight,
                                   g_contentPanel, nullptr, GetModuleHandleW(nullptr), nullptr);

    std::wstring title = showFolder ? snapshot.name + L" (" + snapshot.folder + L")" : snapshot.name;
    rowInfo.name = CreateLabel(rowInfo.row, title);
    rowInfo.status = CreateLabel(rowInfo.row, snapshot.status, SS_LEFT | SS_ENDELLIPSIS);
    ApplyFont(rowInfo.status, g_smallFont);
    rowInfo.progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                       0, 0, 10, 10, rowInfo.row, nullptr, GetModuleHandleW(nullptr), nullptr);
    rowInfo.button = CreateWindowExW(0, L"BUTTON", L"Baixar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, rowInfo.row,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TOOL_BUTTON_BASE + toolIndex)),
                                     GetModuleHandleW(nullptr), nullptr);
    ApplyFont(rowInfo.button);

    AddTooltip(rowInfo.row, snapshot.description);
    AddTooltip(rowInfo.name, snapshot.description);
    AddTooltip(rowInfo.button, L"Baixar " + snapshot.name);

    g_toolRows.push_back(rowInfo);
    UpdateToolRow(g_toolRows.back());
}

void AppendScriptRow(int scriptIndex) {
    const ScriptItem& script = g_scripts[static_cast<size_t>(scriptIndex)];
    ScriptRow rowInfo;
    rowInfo.index = scriptIndex;
    rowInfo.row = CreateWindowExW(0, L"DownloaderRow", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, kRowHeight,
                                   g_contentPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
    rowInfo.button = CreateWindowExW(0, L"BUTTON", script.name.c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, rowInfo.row,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SCRIPT_BUTTON_BASE + scriptIndex)),
                                     GetModuleHandleW(nullptr), nullptr);
    ApplyFont(rowInfo.button);
    AddTooltip(rowInfo.button, script.description.empty() ? script.command : script.description);
    g_scriptRows.push_back(rowInfo);
}

void AppendShortcutRow(int shortcutIndex) {
    const ShortcutItem& shortcut = g_shortcuts[static_cast<size_t>(shortcutIndex)];
    ShortcutRow rowInfo;
    rowInfo.index = shortcutIndex;
    rowInfo.row = CreateWindowExW(0, L"DownloaderRow", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, kRowHeight,
                                   g_contentPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
    rowInfo.button = CreateWindowExW(0, L"BUTTON", shortcut.name.c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     0, 0, 10, 10, rowInfo.row,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SHORTCUT_BUTTON_BASE + shortcutIndex)),
                                     GetModuleHandleW(nullptr), nullptr);
    rowInfo.copyButton = CreateWindowExW(0, L"BUTTON", L"Copiar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         0, 0, 10, 10, rowInfo.row,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SHORTCUT_COPY_BASE + shortcutIndex)),
                                         GetModuleHandleW(nullptr), nullptr);
    ApplyFont(rowInfo.button);
    ApplyFont(rowInfo.copyButton);
    const std::wstring tip = shortcut.description + L"\nWin+R: " + shortcut.command;
    AddTooltip(rowInfo.button, tip);
    AddTooltip(rowInfo.copyButton, L"Copiar comando Win+R: " + shortcut.command);
    g_shortcutRows.push_back(rowInfo);
}

void RenderContent() {
    ClearRows();
    g_scrollPos = 0;

    if (IsSearchActive()) {
        // A busca abrange ferramentas (todas as categorias), scripts e atalhos do Windows ao mesmo tempo.
        const std::vector<int> toolIndexes = VisibleToolIndexes();
        const std::vector<int> scriptIndexes = VisibleScriptIndexes();
        const std::vector<int> shortcutIndexes = VisibleShortcutIndexes();

        if (toolIndexes.empty() && scriptIndexes.empty() && shortcutIndexes.empty()) {
            HWND row = CreateWindowExW(0, L"DownloaderRow", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, kRowHeight,
                                       g_contentPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
            ToolRow emptyRow;
            emptyRow.index = -1;
            emptyRow.row = row;
            emptyRow.name = CreateLabel(row, L"Nenhum item encontrado.");
            g_toolRows.push_back(emptyRow);
        } else {
            for (int toolIndex : toolIndexes) {
                AppendToolRow(toolIndex, true);
            }
            for (int scriptIndex : scriptIndexes) {
                AppendScriptRow(scriptIndex);
            }
            for (int shortcutIndex : shortcutIndexes) {
                AppendShortcutRow(shortcutIndex);
            }
        }
    } else if (IsScriptsTab()) {
        const std::vector<int> scriptIndexes = VisibleScriptIndexes();
        if (scriptIndexes.empty()) {
            HWND row = CreateWindowExW(0, L"DownloaderRow", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, kRowHeight,
                                       g_contentPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
            ScriptRow emptyRow;
            emptyRow.index = -1;
            emptyRow.row = row;
            emptyRow.button = CreateWindowExW(0, L"BUTTON", L"Nenhum script configurado", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                              0, 0, 10, 10, row, nullptr, GetModuleHandleW(nullptr), nullptr);
            ApplyFont(emptyRow.button);
            AddTooltip(emptyRow.button, L"Crie um arquivo scripts.ini ao lado do executavel para adicionar botoes.");
            g_scriptRows.push_back(emptyRow);
        } else {
            for (int scriptIndex : scriptIndexes) {
                AppendScriptRow(scriptIndex);
            }
        }
    } else if (IsShortcutsTab()) {
        const std::vector<int> shortcutIndexes = VisibleShortcutIndexes();
        if (shortcutIndexes.empty()) {
            HWND row = CreateWindowExW(0, L"DownloaderRow", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, kRowHeight,
                                       g_contentPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
            ShortcutRow emptyRow;
            emptyRow.index = -1;
            emptyRow.row = row;
            emptyRow.button = CreateWindowExW(0, L"BUTTON", L"Nenhum atalho encontrado", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                              0, 0, 10, 10, row, nullptr, GetModuleHandleW(nullptr), nullptr);
            ApplyFont(emptyRow.button);
            g_shortcutRows.push_back(emptyRow);
        } else {
            for (int shortcutIndex : shortcutIndexes) {
                AppendShortcutRow(shortcutIndex);
            }
        }
    } else {
        const std::vector<int> toolIndexes = VisibleToolIndexes();
        if (toolIndexes.empty()) {
            HWND row = CreateWindowExW(0, L"DownloaderRow", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, kRowHeight,
                                       g_contentPanel, nullptr, GetModuleHandleW(nullptr), nullptr);
            ToolRow emptyRow;
            emptyRow.index = -1;
            emptyRow.row = row;
            emptyRow.name = CreateLabel(row, L"Nenhum item encontrado.");
            g_toolRows.push_back(emptyRow);
        } else {
            for (int toolIndex : toolIndexes) {
                AppendToolRow(toolIndex, false);
            }
        }
    }

    LayoutRows();
}

void UpdateScrollInfo(int totalHeight, int panelHeight) {
    g_contentHeight = totalHeight;
    const int maxScroll = std::max(0, totalHeight - panelHeight);
    g_scrollPos = ClampInt(g_scrollPos, 0, maxScroll);

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(0, totalHeight - 1);
    info.nPage = static_cast<UINT>(std::max(1, panelHeight));
    info.nPos = g_scrollPos;
    SetScrollInfo(g_contentPanel, SB_VERT, &info, TRUE);
}

void PositionToolRow(const ToolRow& row, int x, int y, int rowWidth) {
    MoveWindow(row.row, x, y, rowWidth, kRowHeight, TRUE);

    if (row.index < 0) {
        if (row.name) {
            MoveWindow(row.name, 12, 17, rowWidth - 24, 22, TRUE);
        }
        return;
    }

    const int buttonWidth = 118;
    const int labelWidth = ClampInt(rowWidth / 3, 170, 300);
    const int progressX = labelWidth + 24;
    const int progressWidth = std::max(110, rowWidth - progressX - buttonWidth - 28);

    MoveWindow(row.name, 12, 8, labelWidth, 20, TRUE);
    MoveWindow(row.status, 12, 30, labelWidth, 18, TRUE);
    MoveWindow(row.progress, progressX, 20, progressWidth, 14, TRUE);
    MoveWindow(row.button, rowWidth - buttonWidth - 12, 11, buttonWidth, 30, TRUE);
}

void PositionScriptRow(const ScriptRow& row, int x, int y, int rowWidth) {
    MoveWindow(row.row, x, y, rowWidth, kRowHeight, TRUE);
    MoveWindow(row.button, 10, 10, std::max(120, rowWidth - 20), 32, TRUE);
}

void PositionShortcutRow(const ShortcutRow& row, int x, int y, int rowWidth) {
    MoveWindow(row.row, x, y, rowWidth, kRowHeight, TRUE);
    const int copyWidth = 82;
    MoveWindow(row.button, 10, 10, std::max(120, rowWidth - copyWidth - 26), 32, TRUE);
    if (row.copyButton) {
        MoveWindow(row.copyButton, rowWidth - copyWidth - 10, 10, copyWidth, 32, TRUE);
    }
}

void LayoutRows() {
    if (!g_contentPanel) {
        return;
    }

    RECT panelRect{};
    GetClientRect(g_contentPanel, &panelRect);
    const int panelWidth = panelRect.right - panelRect.left;
    const int panelHeight = panelRect.bottom - panelRect.top;
    const int rowWidth = std::max(260, panelWidth - 18);

    if (IsSearchActive()) {
        const int count = static_cast<int>(g_toolRows.size() + g_scriptRows.size() + g_shortcutRows.size());
        const int totalHeight = 8 + count * kRowHeight + std::max(0, count - 1) * kRowGap + 8;
        UpdateScrollInfo(totalHeight, panelHeight);

        int y = 8 - g_scrollPos;
        for (const ToolRow& row : g_toolRows) {
            PositionToolRow(row, 4, y, rowWidth);
            y += kRowHeight + kRowGap;
        }
        for (const ScriptRow& row : g_scriptRows) {
            PositionScriptRow(row, 4, y, rowWidth);
            y += kRowHeight + kRowGap;
        }
        for (const ShortcutRow& row : g_shortcutRows) {
            PositionShortcutRow(row, 4, y, rowWidth);
            y += kRowHeight + kRowGap;
        }
        return;
    }

    const int count = IsScriptsTab() ? static_cast<int>(g_scriptRows.size())
                      : IsShortcutsTab() ? static_cast<int>(g_shortcutRows.size())
                                         : static_cast<int>(g_toolRows.size());
    const bool gridMode = IsDownloadTab() && IsChecked(g_gridModeCheck) && count > 0;
    const int gridColumns = gridMode ? std::max(1, rowWidth / 230) : 1;
    const int gridRows = gridMode ? ((count + gridColumns - 1) / gridColumns) : count;
    const int totalHeight = gridMode
                                ? 8 + gridRows * kGridCardHeight + std::max(0, gridRows - 1) * kRowGap + 8
                                : 8 + count * kRowHeight + std::max(0, count - 1) * kRowGap + 8;
    UpdateScrollInfo(totalHeight, panelHeight);

    int y = 8 - g_scrollPos;

    if (IsScriptsTab()) {
        for (const ScriptRow& row : g_scriptRows) {
            PositionScriptRow(row, 4, y, rowWidth);
            y += kRowHeight + kRowGap;
        }
        return;
    }

    if (IsShortcutsTab()) {
        for (const ShortcutRow& row : g_shortcutRows) {
            PositionShortcutRow(row, 4, y, rowWidth);
            y += kRowHeight + kRowGap;
        }
        return;
    }

    if (gridMode) {
        const int cardGap = kRowGap;
        const int cardWidth = std::max(190, (rowWidth - (gridColumns - 1) * cardGap) / gridColumns);

        for (size_t i = 0; i < g_toolRows.size(); ++i) {
            const ToolRow& row = g_toolRows[i];
            const int col = static_cast<int>(i % gridColumns);
            const int rowNumber = static_cast<int>(i / gridColumns);
            const int x = 4 + col * (cardWidth + cardGap);
            const int cardY = 8 - g_scrollPos + rowNumber * (kGridCardHeight + kRowGap);

            MoveWindow(row.row, x, cardY, cardWidth, kGridCardHeight, TRUE);

            if (row.index < 0) {
                if (row.name) {
                    MoveWindow(row.name, 12, 40, cardWidth - 24, 22, TRUE);
                }
                continue;
            }

            MoveWindow(row.name, 12, 10, cardWidth - 24, 20, TRUE);
            MoveWindow(row.status, 12, 32, cardWidth - 24, 18, TRUE);
            MoveWindow(row.progress, 12, 58, cardWidth - 24, 13, TRUE);
            MoveWindow(row.button, 12, 76, cardWidth - 24, 24, TRUE);
        }
        return;
    }

    for (const ToolRow& row : g_toolRows) {
        PositionToolRow(row, 4, y, rowWidth);
        y += kRowHeight + kRowGap;
    }
}

void UpdateBatchButtons() {
    const bool running = g_downloadAllRunning.load();
    EnableWindow(g_downloadAllButton, !running);

    if (!g_downloadTabButton) {
        return;
    }

    const bool showTabButton = IsDownloadTab() && !IsSearchActive();
    ShowWindow(g_downloadTabButton, showTabButton ? SW_SHOW : SW_HIDE);
    EnableWindow(g_downloadTabButton, showTabButton && !running);

    if (showTabButton) {
        SetWindowTextW(g_downloadTabButton, L"Baixar esta aba");
    }
}

void LayoutMainWindow() {
    RECT rect{};
    GetClientRect(g_mainWindow, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const bool showTabDownloadButton = IsDownloadTab() && !IsSearchActive();
    const int footerHeight = showTabDownloadButton ? kFooterHeight : 0;

    const int searchWidth = ClampInt(width / 3, 300, 460);
    MoveWindow(g_askExtractCheck, 16, 8, 178, 20, TRUE);
    MoveWindow(g_askDeleteCheck, 16, 33, 178, 20, TRUE);
    MoveWindow(g_gridModeCheck, 16, 58, 150, 20, TRUE);
    MoveWindow(g_searchEdit, std::max(210, (width - searchWidth) / 2), 32, searchWidth, 28, TRUE);
    MoveWindow(g_themeToggleButton, std::max(12, width - 134), 4, 122, 24, TRUE);
    MoveWindow(g_downloadAllButton, std::max(12, width - 190), 31, 174, 30, TRUE);
    MoveWindow(g_tabs, kOuterMargin, kTabsTop, std::max(280, width - kOuterMargin * 2), kTabsHeight, TRUE);
    MoveWindow(g_contentPanel, kOuterMargin, kContentTop, std::max(280, width - kOuterMargin * 2),
               std::max(180, height - kContentTop - kOuterMargin - footerHeight), TRUE);
    MoveWindow(g_downloadTabButton, std::max(12, width - 178), std::max(kContentTop + 180, height - 40), 160, 30, TRUE);

    UpdateBatchButtons();
    LayoutRows();
}

void SetMinimumWindowSize(HWND, LPARAM lParam) {
    auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
    minMax->ptMinTrackSize.x = 760;
    minMax->ptMinTrackSize.y = 520;
}

LRESULT CALLBACK RowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        return HandleCommand(wParam) ? 0 : DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, g_rowBrush);
        FrameRect(dc, &rect, g_borderBrush);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_theme.text);
        return reinterpret_cast<LRESULT>(g_rowBrush);
    }
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void ScrollContent(int delta) {
    RECT rect{};
    GetClientRect(g_contentPanel, &rect);
    const int panelHeight = rect.bottom - rect.top;
    const int maxScroll = std::max(0, g_contentHeight - panelHeight);
    const int oldPos = g_scrollPos;
    g_scrollPos = ClampInt(g_scrollPos + delta, 0, maxScroll);
    if (oldPos != g_scrollPos) {
        LayoutRows();
    }
}

LRESULT CALLBACK ContentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        return HandleCommand(wParam) ? 0 : DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, g_panelBrush);
        return 1;
    }
    case WM_SIZE:
        LayoutRows();
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &info);
        int pos = info.nPos;
        switch (LOWORD(wParam)) {
        case SB_LINEUP:
            pos -= 32;
            break;
        case SB_LINEDOWN:
            pos += 32;
            break;
        case SB_PAGEUP:
            pos -= static_cast<int>(info.nPage);
            break;
        case SB_PAGEDOWN:
            pos += static_cast<int>(info.nPage);
            break;
        case SB_THUMBTRACK:
            pos = info.nTrackPos;
            break;
        default:
            break;
        }
        ScrollContent(pos - g_scrollPos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int wheel = GET_WHEEL_DELTA_WPARAM(wParam);
        ScrollContent(wheel > 0 ? -72 : 72);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void CreateMainControls(HWND hwnd) {
    g_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, 420);

    g_searchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   0, 0, 10, 10, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH)),
                                   GetModuleHandleW(nullptr), nullptr);
    ApplyFont(g_searchEdit);
    SendMessageW(g_searchEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Pesquisar ferramentas, scripts e atalhos..."));

    g_askExtractCheck = CreateWindowExW(0, L"BUTTON", L"Perguntar extrair ZIP",
                                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        0, 0, 10, 10, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ASK_EXTRACT)),
                                        GetModuleHandleW(nullptr), nullptr);
    ApplyFont(g_askExtractCheck, g_smallFont);
    SendMessageW(g_askExtractCheck, BM_SETCHECK, BST_CHECKED, 0);
    AddTooltip(g_askExtractCheck, L"Quando marcado, pergunta se deseja extrair. Quando desmarcado, extrai automaticamente.");

    g_askDeleteCheck = CreateWindowExW(0, L"BUTTON", L"Perguntar apagar ZIP",
                                       WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       0, 0, 10, 10, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ASK_DELETE)),
                                       GetModuleHandleW(nullptr), nullptr);
    ApplyFont(g_askDeleteCheck, g_smallFont);
    SendMessageW(g_askDeleteCheck, BM_SETCHECK, BST_CHECKED, 0);
    AddTooltip(g_askDeleteCheck, L"Quando marcado, pergunta se deseja apagar. Quando desmarcado, apaga automaticamente depois da extracao.");

    g_gridModeCheck = CreateWindowExW(0, L"BUTTON", L"Modo quadrados",
                                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      0, 0, 10, 10, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_GRID_MODE)),
                                      GetModuleHandleW(nullptr), nullptr);
    ApplyFont(g_gridModeCheck, g_smallFont);
    AddTooltip(g_gridModeCheck, L"Alterna as abas de download entre lista e quadrados compactos.");

    g_themeToggleButton = CreateWindowExW(0, L"BUTTON", g_darkMode ? L"Modo claro" : L"Modo escuro",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          0, 0, 10, 10, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_THEME_TOGGLE)),
                                          GetModuleHandleW(nullptr), nullptr);
    ApplyFont(g_themeToggleButton, g_smallFont);
    AddTooltip(g_themeToggleButton, L"Alterna entre o modo claro e o modo escuro da interface.");

    g_downloadAllButton = CreateWindowExW(0, L"BUTTON", L"Download All Tools",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          0, 0, 10, 10, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DOWNLOAD_ALL)),
                                          GetModuleHandleW(nullptr), nullptr);
    ApplyFont(g_downloadAllButton);
    AddTooltip(g_downloadAllButton, L"Baixar todas as ferramentas em sequencia.");

    g_downloadTabButton = CreateWindowExW(0, L"BUTTON", L"Baixar esta aba",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          0, 0, 10, 10, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DOWNLOAD_TAB)),
                                          GetModuleHandleW(nullptr), nullptr);
    ApplyFont(g_downloadTabButton);
    AddTooltip(g_downloadTabButton, L"Baixar todas as ferramentas da aba atual.");

    g_tabs = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             0, 0, 10, 10, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TABS)),
                             GetModuleHandleW(nullptr), nullptr);
    ApplyFont(g_tabs);

    for (size_t i = 0; i < g_categories.size(); ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(g_categories[i].c_str());
        TabCtrl_InsertItem(g_tabs, static_cast<int>(i), &item);
    }

    g_contentPanel = CreateWindowExW(0, L"DownloaderContent", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                     0, 0, 10, 10, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_mainWindow = hwnd;
        ApplyTheme(g_darkMode);
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        InitializeTools();
        InitializeScripts();
        InitializeShortcuts();
        LoadScripts();
        CreateMainControls(hwnd);
        LayoutMainWindow();
        RenderContent();
        return 0;

    case WM_GETMINMAXINFO:
        SetMinimumWindowSize(hwnd, lParam);
        return 0;

    case WM_SIZE:
        LayoutMainWindow();
        return 0;

    case WM_MOUSEWHEEL:
        if (g_contentPanel) {
            SendMessageW(g_contentPanel, WM_MOUSEWHEEL, wParam, lParam);
        }
        return 0;

    case WM_COMMAND: {
        if (HandleCommand(wParam)) {
            return 0;
        }
        break;
    }

    case WM_NOTIFY: {
        const NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr && hdr->hwndFrom == g_tabs && hdr->code == TCN_SELCHANGE) {
            g_activeTab = TabCtrl_GetCurSel(g_tabs);
            LayoutMainWindow();
            RenderContent();
            return 0;
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_theme.text);
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, g_theme.editBackground);
        SetTextColor(dc, g_theme.editText);
        return reinterpret_cast<LRESULT>(g_panelBrush);
    }

    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, g_windowBrush);
        return 1;
    }

    case WM_APP_TOOL_CHANGED:
        UpdateToolRowByIndex(static_cast<int>(wParam));
        return 0;

    case WM_APP_DOWNLOAD_FINISHED: {
        const int index = static_cast<int>(wParam);
        const bool success = lParam != 0;
        if (!success) {
            return 0;
        }

        ToolItem tool;
        {
            std::lock_guard<std::mutex> lock(g_toolsMutex);
            tool = g_tools.at(static_cast<size_t>(index));
        }

        if (tool.archive && IsZipFile(tool.filename)) {
            bool shouldExtract = true;
            if (IsChecked(g_askExtractCheck)) {
                const std::wstring extractQuestion = L"O download de \"" + tool.name + L"\" terminou.\nDeseja extrair o arquivo compactado?";
                const int extract = MessageBoxW(hwnd, extractQuestion.c_str(), L"Extrair arquivo", MB_ICONQUESTION | MB_YESNO);
                shouldExtract = extract == IDYES;
            }

            if (shouldExtract) {
                bool deleteArchive = true;
                if (IsChecked(g_askDeleteCheck)) {
                    const std::wstring deleteQuestion = L"Deseja apagar o arquivo compactado depois da extracao?";
                    const int deleteAnswer = MessageBoxW(hwnd, deleteQuestion.c_str(), L"Apagar compactado", MB_ICONQUESTION | MB_YESNO);
                    deleteArchive = deleteAnswer == IDYES;
                }
                StartExtraction(index, deleteArchive);
            }
        }
        return 0;
    }

    case WM_APP_EXTRACTION_FINISHED:
        if (lParam == 0) {
            MessageBoxW(hwnd, L"Nao foi possivel extrair o arquivo. Verifique se o PowerShell esta disponivel e se o ZIP e valido.",
                        L"Extracao", MB_ICONERROR);
        }
        return 0;

    case WM_APP_DOWNLOAD_ALL_FINISHED:
        UpdateBatchButtons();
        return 0;

    case WM_DESTROY:
        ClearRows();
        if (g_font) {
            DeleteObject(g_font);
        }
        if (g_smallFont) {
            DeleteObject(g_smallFont);
        }
        if (g_windowBrush) {
            DeleteObject(g_windowBrush);
        }
        if (g_panelBrush) {
            DeleteObject(g_panelBrush);
        }
        if (g_rowBrush) {
            DeleteObject(g_rowBrush);
        }
        if (g_borderBrush) {
            DeleteObject(g_borderBrush);
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterClasses(HINSTANCE instance) {
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    mainClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    mainClass.hbrBackground = nullptr;
    mainClass.lpszClassName = L"ForensicToolDownloaderWindow";
    if (!RegisterClassExW(&mainClass)) {
        return false;
    }

    WNDCLASSEXW contentClass{};
    contentClass.cbSize = sizeof(contentClass);
    contentClass.lpfnWndProc = ContentProc;
    contentClass.hInstance = instance;
    contentClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    contentClass.hbrBackground = nullptr;
    contentClass.lpszClassName = L"DownloaderContent";
    if (!RegisterClassExW(&contentClass)) {
        return false;
    }

    WNDCLASSEXW rowClass{};
    rowClass.cbSize = sizeof(rowClass);
    rowClass.lpfnWndProc = RowProc;
    rowClass.hInstance = instance;
    rowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    rowClass.hbrBackground = nullptr;
    rowClass.lpszClassName = L"DownloaderRow";
    return RegisterClassExW(&rowClass) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    if (!RegisterClasses(instance)) {
        MessageBoxW(nullptr, L"Nao foi possivel registrar a janela.", L"Forensic Tool Downloader", MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(0,
                                L"ForensicToolDownloaderWindow",
                                L"Forensic Tool Downloader",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                980,
                                720,
                                nullptr,
                                nullptr,
                                instance,
                                nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Nao foi possivel criar a janela.", L"Forensic Tool Downloader", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
