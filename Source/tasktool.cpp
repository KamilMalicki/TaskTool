/*
* tasktool - Console Task Manager for Windows
 *
 * Copyright 2026 KamilMalicki
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <iostream>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <vector>
#include <algorithm>
#include <io.h>
#include <iomanip>
#include <string>
#include <map>
#include <shellapi.h>
#include <conio.h>

using namespace std;

enum SortMode {
    BY_RAM,
    BY_CPU,
    BY_DISK,
    BY_PID
};

SortMode currentSort = BY_CPU;
wstring filterStr = L"%";
bool running = true;
bool showHelpScreen = false;
bool showInfoScreen = false;

struct ProcessInfo {
    DWORD pid;
    wstring name;
    DWORD threads;
    size_t memMB;
    double cpuUsage;
    double diskMBs;
};

struct PerfTracker {
    ULONGLONG lastP = 0;
    ULONGLONG lastS = 0;
    ULONGLONG lastIO = 0;
};
map<DWORD, PerfTracker> perfHistory;

HANDLE hOut;
HANDLE hIn;
int g_lastW = 0, g_lastH = 0;

void initConsole() {
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hIn = GetStdHandle(STD_INPUT_HANDLE);
    _setmode(_fileno(stdout), 0x00020000);
    constexpr CONSOLE_CURSOR_INFO ci = {
        100,
        FALSE
    };
    SetConsoleCursorInfo(hOut, &ci);
}

void gotoxy(int x, int y) {
    const COORD c = { static_cast<SHORT>(x), static_cast<SHORT>(y) };
    SetConsoleCursorPosition(hOut, c);
}

void setColor(const int text, const int bg = 0) {
    SetConsoleTextAttribute(hOut, text | (bg << 4));
}

void flushInput() {
    FlushConsoleInputBuffer(hIn);
}

void getConsoleSize(int &w, int &h) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
}

int getGorColor(const double val, const double warn, const double crit) {
    if (val < warn) return 10;
    if (val < crit) return 14;
    return 12;
}

wstring makeBar(double percent, const int width) {
    if (width < 1) return L"";
    if (percent > 100.0) percent = 100.0;
    int bars = static_cast<int>((percent / 100.0) * width);
    if (bars > width) bars = width;
    if (bars < 0) bars = 0;
    wstring s;
    for(int i=0; i<bars; i++) s += L"|";
    for(int i=bars; i<width; i++) s += L" ";
    return s;
}

wstring makeLine(int width, wchar_t c = L'─') {
    if (width < 1) return L"";
    return wstring(width, c);
}

wstring getUptime() {
    const ULONGLONG tick = GetTickCount64();
    ULONGLONG sec = tick / 1000;
    ULONGLONG min = sec / 60;
    ULONGLONG hour = min / 60;
    const ULONGLONG day = hour / 24;
    sec %= 60; min %= 60; hour %= 24;
    wchar_t buf[64];
    swprintf_s(buf, L"%lldd %02lldh %02lldm", day, hour, min);
    return wstring(buf);
}

bool getUserInput(wstring& outStr) {
    outStr.clear();
    flushInput();
    while (true) {
        if (_kbhit()) {
            const wint_t ch = _getwch();

            if (ch == 27) return false;
            if (ch == 13) return true;

            if (ch == 8) {
                if (!outStr.empty()) {
                    outStr.pop_back();
                    wcout << L"\b \b";
                }
            } else if (ch >= 32) {
                outStr += static_cast<wchar_t>(ch);
                wcout << static_cast<wchar_t>(ch);
            }
        }
    }
}

void drawDialogBox(const wstring &title, const wstring &prompt) {
    constexpr CONSOLE_CURSOR_INFO ci = { 100, TRUE }; SetConsoleCursorInfo(hOut, &ci);
    int scrW, scrH; getConsoleSize(scrW, scrH);
    int w = 50;
    if(w > scrW) w = scrW - 2;
    constexpr int h = 5;
    const int sx = (scrW - w) / 2;
    int sy = scrH / 2 - 2;
    if (sy < 0) sy = 0;

    setColor(15, 1);
    for(int i=0; i<h; i++) { gotoxy(sx, sy+i); wcout << wstring(w, L' '); }
    gotoxy(sx, sy); wcout << L"╔" << wstring(w-2, L'═') << L"╗";
    gotoxy(sx, sy+h-1); wcout << L"╚" << wstring(w-2, L'═') << L"╝";
    for(int i=1; i<h-1; i++) { gotoxy(sx, sy+i); wcout << L"║"; gotoxy(sx+w-1, sy+i); wcout << L"║"; }

    gotoxy(sx+2, sy+1); wcout << title;
    gotoxy(sx+2, sy+2); wcout << prompt;
}

void closeDialog() {
    constexpr CONSOLE_CURSOR_INFO ci = {
        100,
        FALSE
    }; SetConsoleCursorInfo(hOut, &ci);
    setColor(7, 0);
    system("cls");
    g_lastW = 0;
}

void handleFilter() {
    drawDialogBox(L"FILTER ENGINE (ESC to Cancel):", L"Query > ");
    wstring tmp;
    if (getUserInput(tmp))
        if(!tmp.empty())
            filterStr = tmp;
    closeDialog();
}

void handleKill() {
    drawDialogBox(L"KILL PROCESS (ESC to Cancel):", L"PID > ");
    if (wstring tmp; getUserInput(tmp) && !tmp.empty()) {
        try {
            const DWORD pid = stoi(tmp);
            if(HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) { TerminateProcess(h, 1); CloseHandle(h); }
        } catch (...) {}
    }
    closeDialog();
}

void handleNewTask() {
    drawDialogBox(L"RUN NEW TASK (ESC to Cancel):", L"Name > ");
    wstring tmp;
    if (getUserInput(tmp) && !tmp.empty()) ShellExecuteW(nullptr, L"open", tmp.c_str(), nullptr, nullptr, SW_SHOW);
    closeDialog();
}

bool checkFilter(wstring name, wstring f) {
    if (f == L"%") return true;
    ranges::transform(name, name.begin(), ::tolower);
    ranges::transform(f, f.begin(), ::tolower);
    if (f.find(L'%') == wstring::npos) return name.find(f) != wstring::npos;
    if (f.size() > 1 && f.front() == L'%' && f.back() == L'%') return name.find(f.substr(1, f.size()-2)) != wstring::npos;
    return name.find(f) != wstring::npos;
}

void GetMetrics(const DWORD pid, const HANDLE h, double &cpuOut, double &diskOut) {
    cpuOut = 0.0; diskOut = 0.0;
    FILETIME ct, et, kt, ut, si, sk, su;
    IO_COUNTERS io;
    if (GetProcessTimes(h, &ct, &et, &kt, &ut) && GetSystemTimes(&si, &sk, &su)) {
        ULARGE_INTEGER p, s;
        p.LowPart = kt.dwLowDateTime + ut.dwLowDateTime; p.HighPart = kt.dwHighDateTime + ut.dwHighDateTime;
        s.LowPart = sk.dwLowDateTime + su.dwLowDateTime; s.HighPart = sk.dwHighDateTime + su.dwHighDateTime;
        const bool ioOk = GetProcessIoCounters(h, &io);
        ULONGLONG currentIO = ioOk ? (io.ReadTransferCount + io.WriteTransferCount) : 0;
        if (perfHistory.contains(pid)) {
            const ULONGLONG sd = s.QuadPart - perfHistory[pid].lastS;
            const ULONGLONG pd = p.QuadPart - perfHistory[pid].lastP;
            if (sd > 0) cpuOut = 100.0 * pd / sd;
            const ULONGLONG ioDelta = currentIO - perfHistory[pid].lastIO;
            diskOut = (static_cast<double>(ioDelta) / 1024.0 / 1024.0) * 2.5;
            perfHistory[pid].lastP = p.QuadPart; perfHistory[pid].lastS = s.QuadPart; perfHistory[pid].lastIO = currentIO;
        } else perfHistory[pid] = {p.QuadPart, s.QuadPart, currentIO};
    }
}

void drawHelpScreen(const int scrW, const int scrH) {
    int sx = (scrW - 40) / 2; if (sx < 0) sx = 0;
    int sy = scrH / 2 - 4;    if (sy < 0) sy = 0;

    setColor(15, 1);
    gotoxy(sx, sy);   wcout << L"╔════════════ COMMAND LIST ════════════╗";
    gotoxy(sx, sy+1); wcout << L"║                                      ║";
    gotoxy(sx, sy+2); wcout << L"║  [F] Filter Processes                ║";
    gotoxy(sx, sy+3); wcout << L"║  [S] Sort (CPU / RAM / DISK)         ║";
    gotoxy(sx, sy+4); wcout << L"║  [K] Kill Process (by PID)           ║";
    gotoxy(sx, sy+5); wcout << L"║  [N] New Task (Run app)              ║";
    gotoxy(sx, sy+6); wcout << L"║  [ESC] Return / Exit                 ║";
    gotoxy(sx, sy+7); wcout << L"║                                      ║";
    gotoxy(sx, sy+8); wcout << L"╚══════════════════════════════════════╝";
}

void drawInfoScreen(const int scrW, const int scrH) {
    int sx = (scrW - 40) / 2; if (sx < 0) sx = 0;
    int sy = scrH / 2 - 4;    if (sy < 0) sy = 0;

    setColor(7, 0);
    gotoxy(sx, sy);   wcout << L"╔═════════════ INFO WINDOW ════════════╗";
    gotoxy(sx, sy+1); wcout << L"║                                      ║";
    gotoxy(sx, sy+2); wcout << L"║           TaskTool v1.0.0            ║";
    gotoxy(sx, sy+3); wcout << L"║       Writted by KamilMalicki        ║";
    gotoxy(sx, sy+4); wcout << L"║   Github:  github.com/KamilMalicki   ║";
    gotoxy(sx, sy+5); wcout << L"║                                      ║";
    gotoxy(sx, sy+6); wcout << L"║        [ESC] Return / Exit           ║";
    gotoxy(sx, sy+7); wcout << L"║                                      ║";
    gotoxy(sx, sy+8); wcout << L"╚══════════════════════════════════════╝";
}

void drawCompactMode(int scrW, int scrH, double cpu, double ram, double disk) {
    gotoxy(0, 0); setColor(15, 1);
    const wstring h = L" COMPACT MODE ";
    const wstring u = L"UPTIME: " + getUptime() + L" ";

    int pad = scrW - static_cast<int>(h.length()) - static_cast<int>(u.length());
    if(pad < 0) pad = 0;
    const wstring head = h + wstring(pad, L' ') + u;
    wcout << head.substr(0, scrW);

    constexpr int contentH = 3;
    int startY = (scrH / 2) - (contentH / 2);
    if(startY < 1) startY = 1;

    int barW = scrW - 10; if(barW < 5) barW = 5;

    gotoxy(0, startY); setColor(7, 0); wprintf(L" CPU: ");
    setColor(getGorColor(cpu, 50, 80), 0); wprintf(L"[%ls]", makeBar(cpu, barW).c_str());

    gotoxy(0, startY + 1); setColor(7, 0); wprintf(L" RAM: ");
    setColor(getGorColor(ram, 60, 85), 0); wprintf(L"[%ls]", makeBar(ram, barW).c_str());

    gotoxy(0, startY + 2); setColor(7, 0); wprintf(L" DSK: ");
    double dP = (disk/200.0)*100.0;
    setColor(getGorColor(dP, 25, 50), 0); wprintf(L"[%ls]", makeBar(dP, barW).c_str());

    gotoxy(0, scrH - 1);
    const wstring foot = L" [?] Full Help | [ESC] Exit ";
    int fPad = scrW - static_cast<int>(foot.length()); if(fPad < 0) fPad = 0;
    wstring fullFoot = foot + wstring(fPad, L' ');

    setColor(0, 7);
    wcout << fullFoot.substr(0, scrW);
    setColor(7, 0);
}

void drawHeader(const int vis, const int tot, const double totalCpu, const double totalDisk, const int scrW) {
    MEMORYSTATUSEX m; m.dwLength = sizeof(m); GlobalMemoryStatusEx(&m);
    const double ramP = static_cast<double>(m.ullTotalPhys - m.ullAvailPhys) * 100 / m.ullTotalPhys;
    const double swapUsed = static_cast<float>(m.ullTotalPageFile - m.ullAvailPageFile)/1e9;
    const double swapTot = static_cast<float>(m.ullTotalPageFile)/1e9;
    const double swapP = (swapUsed / swapTot) * 100.0;

    gotoxy(0, 0);
    setColor(7, 0);
    wstring info = L" TASKTOOL | VIEW: " + to_wstring(vis) + L"/" + to_wstring(tot) + L" | FILTER: " + filterStr;
    wstring sysInfo = L"UPTIME: " + getUptime() + L" ";

    int spaceLen = scrW - static_cast<int>(info.length()) - static_cast<int>(sysInfo.length());
    if(spaceLen < 0) spaceLen = 0;
    wstring fullLine = info + wstring(spaceLen, L' ') + sysInfo;
    if(fullLine.length() > scrW) fullLine = fullLine.substr(0, scrW);
    wcout << fullLine;
    setColor(7, 0);

    int barW = scrW - 16; if(barW < 5) barW = 5;

    gotoxy(0, 1); setColor(7, 0); wprintf(L" CPU: ");
    setColor(getGorColor(totalCpu, 50, 80), 0); wprintf(L"[%ls]", makeBar(totalCpu, barW).c_str());
    setColor(15, 0); wprintf(L" %5.1f%%", totalCpu);

    gotoxy(0, 2); setColor(7, 0); wprintf(L" RAM: ");
    setColor(getGorColor(ramP, 60, 85), 0); wprintf(L"[%ls]", makeBar(ramP, barW).c_str());
    setColor(15, 0); wprintf(L" %5.1f%%", ramP);

    gotoxy(0, 3); setColor(7, 0); wprintf(L" SWP: ");
    setColor(getGorColor(swapP, 50, 80), 0); wprintf(L"[%ls]", makeBar(swapP, barW).c_str());
    setColor(15, 0); wprintf(L" %5.1fGB", swapUsed);

    double diskPercent = (totalDisk / 200.0) * 100.0;
    gotoxy(0, 4); setColor(7, 0); wprintf(L" DSK: ");
    setColor(getGorColor(diskPercent, 25, 50), 0); wprintf(L"[%ls]", makeBar(diskPercent, barW).c_str());
    setColor(15, 0); wprintf(L" %5.1fMB", totalDisk);
}

int main() {
    initConsole();
    system("cls");

    while (running) {
        int scrW, scrH; getConsoleSize(scrW, scrH);

        if (scrW != g_lastW || scrH != g_lastH) {
            setColor(7, 0); system("cls");
            g_lastW = scrW; g_lastH = scrH;
        }

        vector<ProcessInfo> procs;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = {sizeof(pe)};
        int total = 0;
        double globalDiskSum = 0;
        double globalCpuSum = 0;
        double ramP = 0;

        if (Process32FirstW(snap, &pe)) {
            do {
                total++;
                if (checkFilter(pe.szExeFile, filterStr)) {
                    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                    size_t mem = 0; double cpu = 0; double disk = 0;
                    if (h) {
                        PROCESS_MEMORY_COUNTERS pmc;
                        if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) mem = pmc.WorkingSetSize / 1024 / 1024;
                        GetMetrics(pe.th32ProcessID, h, cpu, disk);
                        CloseHandle(h);
                    }
                    globalDiskSum += disk;
                    globalCpuSum += cpu;
                    procs.push_back({pe.th32ProcessID, pe.szExeFile, pe.cntThreads, mem, cpu, disk});
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);

        ranges::sort(procs, [](const ProcessInfo& a, const ProcessInfo& b) {
            if(currentSort == BY_RAM) return a.memMB > b.memMB;
            if(currentSort == BY_CPU) return a.cpuUsage > b.cpuUsage;
            if(currentSort == BY_DISK) return a.diskMBs > b.diskMBs;
            return a.pid < b.pid;
        });
        if(globalCpuSum > 100.0) globalCpuSum = 100.0;
        MEMORYSTATUSEX m; m.dwLength = sizeof(m); GlobalMemoryStatusEx(&m);
        ramP = static_cast<double>(m.ullTotalPhys - m.ullAvailPhys) * 100 / m.ullTotalPhys;

        bool isSmallWindow = (scrW < 70 || scrH < 18);

        if (showHelpScreen) drawHelpScreen(scrW, scrH);
        else if (showInfoScreen) drawInfoScreen(scrW, scrH);
        else if (isSmallWindow) {
            drawCompactMode(scrW, scrH, globalCpuSum, ramP, globalDiskSum);
            setColor(0, 0);
        } else {
            drawHeader(static_cast<int>(procs.size()), total, globalCpuSum, globalDiskSum, scrW);

            int tableStartY = 6;
            int reservedBottom = 3;
            int availableDataRows = scrH - (tableStartY + 3) - reservedBottom;
            if(availableDataRows < 0) availableDataRows = 0;

            constexpr int W_PID = 8;
            constexpr int W_CPU = 8;
            constexpr int W_RAM = 10;
            constexpr int W_DSK = 10;

            int usedWidth = W_PID + W_CPU + W_RAM + W_DSK + 6;
            int wName = scrW - usedWidth; if (wName < 5) wName = 5;

            gotoxy(0, tableStartY); setColor(8, 0);
            wprintf(L"┌%ls┬%ls┬%ls┬%ls┬%ls┐", makeLine(W_PID).c_str(), makeLine(W_CPU).c_str(), makeLine(W_RAM).c_str(), makeLine(W_DSK).c_str(), makeLine(wName).c_str());

            gotoxy(0, tableStartY+1); setColor(15, 0);
            wprintf(L"│ %-*ls │ %-*ls │ %-*ls │ %-*ls │ %-*ls │", W_PID-2, L"PID", W_CPU-2, L"CPU%", W_RAM-2, L"RAM", W_DSK-2, L"DSK", wName-2, L"NAME");

            gotoxy(0, tableStartY+2); setColor(8, 0);
            wprintf(L"├%ls┼%ls┼%ls┼%ls┼%ls┤", makeLine(W_PID).c_str(), makeLine(W_CPU).c_str(), makeLine(W_RAM).c_str(), makeLine(W_DSK).c_str(), makeLine(wName).c_str());

            for (int i = 0; i < availableDataRows; ++i) {
                gotoxy(0, tableStartY + 3 + i);
                if (i < procs.size()) {
                    int col = 7;
                    if(procs[i].pid == GetCurrentProcessId()) col = 11;
                    else if(procs[i].cpuUsage > 20.0 || procs[i].diskMBs > 5.0) col = 12;
                    else if(procs[i].cpuUsage > 5.0 || procs[i].diskMBs > 1.0) col = 14;

                    setColor(col, 0);
                    wstring n = procs[i].name;
                    if(n.length() > wName - 2) n = n.substr(0, wName - 2);

                    wprintf(L"│ %6d │ %5.1f%% │ %8zu │ %8.1f │ %-*ls ", procs[i].pid, procs[i].cpuUsage, procs[i].memMB, procs[i].diskMBs, wName-2, n.c_str());
                    setColor(8, 0); wprintf(L"│");
                } else {
                    setColor(7, 0);
                    wprintf(L"│%ls│%ls│%ls│%ls│%ls│", wstring(W_PID, L' ').c_str(), wstring(W_CPU, L' ').c_str(), wstring(W_RAM, L' ').c_str(), wstring(W_DSK, L' ').c_str(), wstring(wName, L' ').c_str());
                }
            }

            int footerY = tableStartY + 3 + availableDataRows;
            gotoxy(0, footerY); setColor(8, 0);
            wprintf(L"└%ls┴%ls┴%ls┴%ls┴%ls┘", makeLine(W_PID).c_str(), makeLine(W_CPU).c_str(), makeLine(W_RAM).c_str(), makeLine(W_DSK).c_str(), makeLine(wName).c_str());

            gotoxy(0, footerY + 1);
            wstring sName;
            if(currentSort==BY_RAM) sName=L"RAM"; else if(currentSort==BY_CPU) sName=L"CPU";
            else if(currentSort==BY_DISK) sName=L"DISK"; else sName=L"PID";
            wstring menuTxt = L" [F] Filter | [N] New | [K] Kill | [S] Sort: " + sName + L" | [?] Help | [ESC] Exit";
            int padding = scrW - static_cast<int>(menuTxt.length()); if (padding < 0) padding = 0;
            wstring fullMenu = menuTxt + wstring(padding, L' ');
            setColor(0, 7);
            wcout << fullMenu.substr(0, scrW);
            setColor(7, 0);

            for(int y = footerY + 2; y < scrH; y++) gotoxy(0, y); wcout << wstring(scrW, L' ');
        }

        if (_kbhit()) {
            int ch = _getch();
            if (ch == 0 || ch == 224) { _getch(); }
            else {
                ch = toupper(ch);
                if (showHelpScreen || showInfoScreen) {
                    if (ch == 27 || ch == '?' || ch == 'H' || ch == 'I') {
                        showHelpScreen = false;
                        showInfoScreen = false;
                        system("cls");
                        g_lastW = 0;
                    }
                } else {
                    if (ch == 'F') handleFilter();
                    else if (ch == 'I') {showInfoScreen = true; system("cls"); g_lastW = 0; }
                    else if (ch == 'K') handleKill();
                    else if (ch == 'N') handleNewTask();
                    else if (ch == 'S') { currentSort = static_cast<SortMode>((currentSort + 1) % 4); }
                    else if (ch == 27) running = false;
                    else if (ch == '?' || ch == 'H') { showHelpScreen = true; system("cls"); g_lastW = 0; }
                }
            }
            flushInput();
        }

        Sleep(200);
    }
    system("color");
    return 0;
}
