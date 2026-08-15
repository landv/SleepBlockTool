//=============================================================================
// SleepBlockTool.cpp - 休眠阻止查看工具
//-----------------------------------------------------------------------------
// 功能：
//   1. 以管理员权限运行（自动提权）。
//   2. 执行 `powercfg /requests` 查看当前阻止系统休眠/睡眠的请求。
//   3. 从输出中解析出 [PROCESS] 列表，界面只显示进程名。
//   4. 一键结束选中的阻止休眠进程（wmic）。
//   5. 操作结果以带时间戳的日志形式追加显示，可一键清空。
//
// 编译：
//   在 Visual Studio Developer PowerShell 中运行 `.\build.ps1`
//   （详见 README.md）。
//
// 依赖：
//   - Windows SDK（windows.h / shellapi.h）
//   - SleepBlockTool.rc：图标 + 内嵌 app.manifest（管理员权限/DPI）
//=============================================================================
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <cstdio>

// 按钮控件ID 2000以上，避免和rc图标1001冲突
#define ID_BTN_QUERY        2001
#define ID_BTN_KILL         2002
#define ID_EDIT_OUTPUT      2003
#define ID_LIST_PROC        2004
#define ID_BTN_CLEAR        2005
#define WM_CMD_DONE (WM_USER + 100)
#define IDI_MAIN_ICON       1001   // rc: 1001 ICON "icon.ico"

HWND hWndMain;
HWND hEditOutput;
HWND hListProc;
HICON g_hIcon = nullptr;
HFONT g_hFont = nullptr; //全局美化字体
HFONT g_hBtnFont = nullptr; //按钮专用字体（更大一号）

//-----------------------------------------------------------------------------
// RequireAdmin - 检查并以管理员权限重新启动
//   powercfg /requests 与结束进程都需要管理员权限。
//   若当前进程已提权则直接返回；否则用 "runas" 触发 UAC 重新启动自身，
//   并结束当前未提权的进程实例。
//-----------------------------------------------------------------------------
BOOL RequireAdmin()
{
    BOOL bIsAdmin = FALSE;
    HANDLE hToken = NULL;
    // 读取当前进程令牌，判断是否已提权（TokenElevation）
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION te;
        DWORD cb = sizeof(te);
        if (GetTokenInformation(hToken, TokenElevation, &te, cb, &cb))
        {
            bIsAdmin = te.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    if (bIsAdmin) return TRUE; // 已经是管理员，无需再提权

    // 通过 ShellExecuteEx 的 runas 动词触发 UAC 重新启动自己
    WCHAR szExe[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, szExe, MAX_PATH);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = szExe;
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
    ExitProcess(0); // 当前（未提权）实例退出
    return FALSE;
}

// 从 powercfg /requests 输出中解析出的一个阻止休眠的进程
struct BlockProc {
    std::wstring fullLine; // powercfg 输出的原始行
    std::wstring path;     // 进程完整路径（如 \Device\...\Code.exe）
    DWORD pid;             // 进程ID（当前未填充，预留字段）
};
std::vector<BlockProc> g_procList; // 全局进程列表，供列表框与结束操作使用

// 传给后台线程的参数（new 分配，由 CmdThread 负责释放）
struct ThreadParam {
    std::wstring cmd;   // 要执行的命令
    std::wstring desc;  // 日志中显示的操作说明
};

// 后台命令执行完成后的结果（new 分配，由主线程 WM_CMD_DONE 释放）
struct CmdResult {
    std::wstring desc;   // 操作说明
    std::wstring output; // 命令输出
};

//-----------------------------------------------------------------------------
// ExecCmd - 执行一条命令并捕获其标准输出/错误输出
//   相比 _popen：
//   - 使用 CREATE_NO_WINDOW + SW_HIDE，彻底避免执行时弹出黑色控制台窗口。
//   - 通过匿名管道重定向 stdout/stderr，父进程用 ReadFile 阻塞读取到命令结束。
//   返回：命令输出转换成的宽字符串（按 GBK/936 转码，兼容中文系统）。
//-----------------------------------------------------------------------------
std::wstring ExecCmd(const std::wstring& cmdW)
{
    std::wstring cmdLine = L"cmd.exe /c " + cmdW; // 经 cmd.exe 执行，支持重定向等语法

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return L"创建管道失败";
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0); //读端仅父进程使用

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    std::string ansiText;
    BOOL ok = CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWritePipe); //父进程关闭写端

    if (ok)
    {
        CloseHandle(pi.hThread);
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf), &read, NULL) && read > 0)
        {
            ansiText.append(buf, read);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(hReadPipe);

    if (ansiText.empty())
        return L"";

    int wcharCount = MultiByteToWideChar(936, 0, ansiText.c_str(), (int)ansiText.size(), nullptr, 0);
    if (wcharCount <= 0)
        return L"";
    std::wstring wResult(wcharCount, 0);
    MultiByteToWideChar(936, 0, ansiText.c_str(), (int)ansiText.size(), &wResult[0], wcharCount);
    return wResult;
}

//-----------------------------------------------------------------------------
// CmdThread - 后台工作线程
//   命令在后台线程执行，避免阻塞 UI 消息循环（否则窗口会“未响应”）。
//   完成后通过 PostMessage 把结果交回主线程处理（线程安全的跨线程通信）。
//-----------------------------------------------------------------------------
DWORD WINAPI CmdThread(LPVOID lpParam)
{
    ThreadParam* p = (ThreadParam*)lpParam;
    std::wstring res = ExecCmd(p->cmd);           // 同步执行命令（可能耗时）
    CmdResult* r = new CmdResult{ p->desc, res }; // 构造结果，交由主线程显示
    delete p;
    PostMessageW(hWndMain, WM_CMD_DONE, 0, (LPARAM)r); // 通知主线程
    return 0;
}

//当前时间字符串
std::wstring GetTimeStr()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, L"[%04d-%02d-%02d %02d:%02d:%02d]",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

//-----------------------------------------------------------------------------
// AppendLogLine - 向日志框末尾追加一行文本
//   EDIT 控件要求 \r\n 换行，这里把输出中的 \n 统一转换为 \r\n，
//   并通过“选中末尾 + 替换选中”实现追加，同时自动滚动到底部。
//-----------------------------------------------------------------------------
void AppendLogLine(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size() + 2);
    for (wchar_t c : text) // 统一换行符为 \r\n
    {
        if (c == L'\n')
        {
            if (out.empty() || out.back() != L'\r')
                out += L'\r';
            out += L'\n';
        }
        else
            out += c;
    }
    out += L"\r\n";
    SendMessageW(hEditOutput, EM_SETSEL, -1, -1);   // 光标移到文本末尾
    SendMessageW(hEditOutput, EM_REPLACESEL, FALSE, (LPARAM)out.c_str()); // 追加
}

//-----------------------------------------------------------------------------
// ParsePowerReq - 解析 `powercfg /requests` 的输出
//   逐行扫描，凡是包含 "[PROCESS]" 的行即为阻止休眠的进程，
//   记录完整路径到 g_procList，并在列表框中仅显示进程名（exe 文件名）。
//-----------------------------------------------------------------------------
void ParsePowerReq(const std::wstring& out)
{
    g_procList.clear();
    SendMessageW(hListProc, LB_RESETCONTENT, 0, 0); // 清空列表框
    size_t pos = 0;
    while (pos < out.size()) // 按行遍历输出
    {
        size_t eol = out.find(L'\n', pos);
        std::wstring line = out.substr(pos, eol - pos);
        pos = (eol != std::wstring::npos) ? (eol + 1) : out.size();

        size_t procMark = line.find(L"[PROCESS]");
        if (procMark != std::wstring::npos)
        {
            BlockProc bp{};
            bp.fullLine = line;                   // 保存原始行
            bp.path = line.substr(procMark + 10); // "[PROCESS] " 之后的完整路径
            bp.pid = 0;
            g_procList.push_back(bp);

            // 列表只显示进程名，完整路径保留在数据结构中
            std::wstring exeName = bp.path;
            size_t bs = bp.path.rfind(L'\\');
            if (bs != std::wstring::npos)
                exeName = bp.path.substr(bs + 1); // 取最后一个 \ 之后的名字
            SendMessageW(hListProc, LB_ADDSTRING, 0, (LPARAM)exeName.c_str());
        }
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // 创建子控件：两个功能按钮 + 清空按钮 + 进程列表框 + 日志编辑框
        // （后续 WM_SIZE 会根据窗口大小统一重新排布）
        CreateWindowW(L"BUTTON", L"查看powercfg /requests", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 12, 250, 48, hWnd, (HMENU)ID_BTN_QUERY, NULL, NULL);

        CreateWindowW(L"BUTTON", L"结束选中阻止休眠进程", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            270, 12, 318, 48, hWnd, (HMENU)ID_BTN_KILL, NULL, NULL);

        CreateWindowW(L"BUTTON", L"清空", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            596, 12, 92, 48, hWnd, (HMENU)ID_BTN_CLEAR, NULL, NULL);

        hListProc = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL,
            12, 68, 576, 200, hWnd, (HMENU)ID_LIST_PROC, NULL, NULL);

        hEditOutput = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_WANTRETURN,
            12, 276, 576, 272, hWnd, (HMENU)ID_EDIT_OUTPUT, NULL, NULL);

        //按钮使用更大的专用字体，其余控件用全局字体
        SendMessageW(GetDlgItem(hWnd, ID_BTN_QUERY), WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);
        SendMessageW(GetDlgItem(hWnd, ID_BTN_KILL), WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);
        SendMessageW(GetDlgItem(hWnd, ID_BTN_CLEAR), WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);
        SendMessageW(hListProc, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(hEditOutput, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    }
    break;

    case WM_GETMINMAXINFO:
    {
        //限制窗口最小尺寸，防止控件被压扁
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 600;
        mmi->ptMinTrackSize.y = 560;
    }
    break;

    case WM_SIZE:
    {
        //控件随窗口大小自适应排列
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (w <= 0 || h <= 0) break;

        const int m = 12;    //外边距
        const int gap = 8;   //控件间距
        const int btnH = 48; //按钮高度
        int btnArea = w - m * 2 - gap * 2;

        //按钮按 44% / 40% / 16% 分配宽度
        int btn1W = btnArea * 44 / 100;
        int btn2W = btnArea * 40 / 100;
        int btn3W = btnArea - btn1W - btn2W;

        MoveWindow(GetDlgItem(hWnd, ID_BTN_QUERY), m, m, btn1W, btnH, TRUE);
        MoveWindow(GetDlgItem(hWnd, ID_BTN_KILL), m + btn1W + gap, m, btn2W, btnH, TRUE);
        MoveWindow(GetDlgItem(hWnd, ID_BTN_CLEAR), m + btn1W + gap + btn2W + gap, m, btn3W, btnH, TRUE);

        int listY = m + btnH + gap;
        int listH = (h - listY - gap) * 40 / 100;
        MoveWindow(hListProc, m, listY, w - m * 2, listH, TRUE);

        int editY = listY + listH + gap;
        int editH = h - editY - m;
        MoveWindow(hEditOutput, m, editY, w - m * 2, editH, TRUE);
    }
    break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        // 双缓冲绘制：先画到内存位图再一次性 BitBlt，避免界面闪烁
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        HDC hMemDC = CreateCompatibleDC(hdc);
        HBITMAP hMemBmp = CreateCompatibleBitmap(hdc, rcClient.right, rcClient.bottom);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hMemBmp);

        FillRect(hMemDC, &rcClient, (HBRUSH)(COLOR_WINDOW + 1));
        BitBlt(hdc, 0, 0, rcClient.right, rcClient.bottom, hMemDC, 0, 0, SRCCOPY);

        SelectObject(hMemDC, hOldBmp);
        DeleteObject(hMemBmp);
        DeleteDC(hMemDC);

        EndPaint(hWnd, &ps);
    }
    break;

    // 后台线程执行完命令后，主线程在此收到结果并刷新界面
    case WM_CMD_DONE:
    {
        CmdResult* r = (CmdResult*)lParam;
        AppendLogLine(GetTimeStr() + L" " + r->desc); // 时间戳 + 操作说明
        AppendLogLine(r->output);                     // 命令输出
        AppendLogLine(L"----------------------------------------");
        ParsePowerReq(r->output); // 重新解析进程列表
        delete r;
    }
    break;

    case WM_COMMAND:
    {
        //【查看】按钮：后台执行 powercfg /requests
        if (LOWORD(wParam) == ID_BTN_QUERY)
        {
            ThreadParam* p = new ThreadParam();
            p->cmd = L"powercfg /requests";
            p->desc = L"查看 powercfg /requests";
            CreateThread(NULL, 0, CmdThread, p, 0, NULL); // 交给后台线程执行
        }
        //【结束】按钮：结束列表中所选进程
        else if (LOWORD(wParam) == ID_BTN_KILL)
        {
            // 获取列表框中选中的进程索引
            int sel = (int)SendMessageW(hListProc, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR) // 没有选中任何项
            {
                MessageBoxW(hWnd, L"请在列表选中一个进程", L"提示", MB_OK);
                break;
            }
            if (sel >= (int)g_procList.size()) break; // 索引越界保护

            // 从保存的完整路径中提取 exe 文件名
            std::wstring path = g_procList[sel].path;
            size_t bs = path.rfind(L'\\');
            std::wstring exeName;
            if (bs != std::wstring::npos)
                exeName = path.substr(bs + 1);

            if (exeName.empty())
            {
                MessageBoxW(hWnd, L"无法解析exe名称", L"错误", MB_OK);
                break;
            }

            // 构造结束进程的命令，后台执行（结果会显示在日志区）
            wchar_t cmd[512];
            swprintf_s(cmd, L"wmic process where name=\"%s\" call terminate", exeName.c_str());

            ThreadParam* p = new ThreadParam();
            p->cmd = cmd;
            p->desc = L"结束进程 " + exeName;
            CreateThread(NULL, 0, CmdThread, p, 0, NULL);
        }
        //【清空】按钮：清空日志显示区
        else if (LOWORD(wParam) == ID_BTN_CLEAR)
        {
            SetWindowTextW(hEditOutput, L""); //清空日志
        }
    }
    break;

    case WM_DESTROY:
        DestroyIcon(g_hIcon);
        DeleteObject(g_hFont); //释放字体GDI对象
        DeleteObject(g_hBtnFont); //释放按钮字体GDI对象
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

//-----------------------------------------------------------------------------
// CenterWindow - 把窗口移动到工作区（屏幕去掉任务栏后的区域）正中央
//-----------------------------------------------------------------------------
void CenterWindow(HWND hWnd)
{
    RECT rcWnd;
    GetWindowRect(hWnd, &rcWnd);
    int w = rcWnd.right - rcWnd.left; // 窗口宽度
    int h = rcWnd.bottom - rcWnd.top; // 窗口高度

    RECT rcScreen;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcScreen, 0); // 获取工作区

    // 计算居中坐标
    int x = rcScreen.left + (rcScreen.right - rcScreen.left - w)/2;
    int y = rcScreen.top + (rcScreen.bottom - rcScreen.top - h)/2;
    SetWindowPos(hWnd, NULL, x, y, 0,0, SWP_NOSIZE | SWP_NOZORDER); // 仅移动，不改变尺寸
}

//-----------------------------------------------------------------------------
// WinMain - 程序入口
//   流程：提权 → 创建字体/图标 → 注册窗口类 → 创建主窗口 → 居中 →
//         显示 → 进入消息循环，直到收到 WM_QUIT。
//-----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    RequireAdmin(); // 先确保以管理员权限运行

    //创建微软雅黑9号字体
    g_hFont = CreateFontW(
        18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY,DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    //按钮专用字体，更大一号便于查看
    g_hBtnFont = CreateFontW(
        26,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY,DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    g_hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_MAIN_ICON));

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SleepBlockToolClass";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = g_hIcon;
    wc.hIconSm = g_hIcon;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    //去掉WS_MAXIMIZEBOX禁止最大化，WS_THICKFRAME允许拖动边框调整大小
    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME ;
    hWndMain = CreateWindowExW(0, wc.lpszClassName, L"休眠阻止查看工具 By landv",
        dwStyle, CW_USEDEFAULT, CW_USEDEFAULT, 600, 560,
        NULL, NULL, hInst, NULL);

    CenterWindow(hWndMain); //屏幕居中

    ShowWindow(hWndMain, nCmdShow);
    UpdateWindow(hWndMain);

    MSG msg;
    // 标准消息循环：取消息 → 翻译（键盘消息）→ 分发到窗口过程
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(g_hFont); // 消息循环结束后释放字体
    return (int)msg.wParam;
}