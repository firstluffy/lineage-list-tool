/*
 * 리니지 변신 파일 최적화 순서 검증 도구 — Win32, Windows 7 이상
 */

#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef RICHEDIT_VER
#define RICHEDIT_VER 0x0500
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <wchar.h>

#include <algorithm>
#include <string>
#include <vector>

#include "resource.h"

#pragma comment(lib, "comdlg32.lib")

static const WCHAR kAppTitle[] =
    L"리니지 변신 리스트 수정 도구 - By Light";

/* list.spz.e 등 대용량 리스트 (기존 16MB → 128MB) */
static const DWORD kMaxFileBytes = 128u * 1024u * 1024u;
static const DWORD kMaxEditChars = 64u * 1024u * 1024u;

enum {
  IDC_MAIN_EDIT = 1001,
  IDC_LINE_GUTTER = 1002,
};

enum {
  IDM_FILE_OPEN = 40001,
  IDM_FILE_SAVE,
  IDM_FILE_SAVEAS,
  IDM_FILE_CLEAR,
  IDM_FILE_EXIT,
  IDM_EDIT_INDENT,
  IDM_EDIT_SYMBOLS,
  IDM_EDIT_SORT_ENGLISH,
  IDM_EDIT_FIND,
  IDM_VERIFY_SEQUENCE,
  IDM_HELP_ABOUT,
};

enum {
  IDC_FIND_EDIT = 2101,
  IDC_FIND_BTN_ALL = 2102,
  IDC_FIND_BTN_UP = 2103,
  IDC_FIND_BTN_DOWN = 2104,
  IDC_FIND_BTN_CLEAR = 2105,
};

struct AppState {
  HWND hwndMain = nullptr;
  HWND hwndEdit = nullptr;
  HWND hwndGutter = nullptr;
  WNDPROC pfnOldEdit = nullptr;
  bool editorIsRichEdit = false;
  WCHAR filePath[MAX_PATH] = L"";
  HWND hwndFindDlg = nullptr;
  HWND hwndFindEdit = nullptr;
  WCHAR findBuf[256] = L"";
};

static AppState g_app;
static void SyncFindBufFromDialogIfOpen();
static void DoFindNext(HWND hwnd);
static void DoFindPrev(HWND hwnd);
static HACCEL g_hAccel = nullptr;
static HMODULE g_hMsfteditDll = nullptr;

static void LayoutChildren(HWND hwnd) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  const int gutterW = 44;
  if (g_app.hwndGutter)
    MoveWindow(g_app.hwndGutter, 0, 0, gutterW, rc.bottom, TRUE);
  if (g_app.hwndEdit)
    MoveWindow(g_app.hwndEdit, gutterW, 0, rc.right - gutterW, rc.bottom, TRUE);
}

static int GetEditLineHeight(HWND hEdit) {
  HDC hdc = GetDC(hEdit);
  if (!hdc)
    return 16;
  HFONT hf = (HFONT)SendMessageW(hEdit, WM_GETFONT, 0, 0);
  HFONT old = hf ? (HFONT)SelectObject(hdc, hf) : nullptr;
  TEXTMETRICW tm;
  GetTextMetricsW(hdc, &tm);
  if (old)
    SelectObject(hdc, old);
  ReleaseDC(hEdit, hdc);
  int h = tm.tmHeight + tm.tmExternalLeading;
  return h > 1 ? h : 16;
}

static void PaintGutter(HWND hwndGutter) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwndGutter, &ps);
  RECT rc;
  GetClientRect(hwndGutter, &rc);
  FillRect(hdc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));

  HWND hEdit = g_app.hwndEdit;
  if (!hEdit) {
    EndPaint(hwndGutter, &ps);
    return;
  }

  SetBkMode(hdc, TRANSPARENT);
  HFONT hf = (HFONT)SendMessageW(hEdit, WM_GETFONT, 0, 0);
  HFONT oldF = hf ? (HFONT)SelectObject(hdc, hf) : nullptr;
  SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));

  const int lh = GetEditLineHeight(hEdit);
  const int first = (int)SendMessageW(hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
  const int total = (int)SendMessageW(hEdit, EM_GETLINECOUNT, 0, 0);
  const int visible = (rc.bottom + lh - 1) / lh + 1;

  for (int i = 0; i < visible; ++i) {
    int line = first + i + 1;
    if (line > total && total > 0)
      break;
    WCHAR num[16];
    wsprintfW(num, L"%d", line);
    RECT lineRc = {0, i * lh, rc.right, (i + 1) * lh};
    DrawTextW(hdc, num, -1, &lineRc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  }

  if (oldF)
    SelectObject(hdc, oldF);
  EndPaint(hwndGutter, &ps);
}

static LRESULT CALLBACK GutterWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam) {
  switch (msg) {
  case WM_PAINT:
    PaintGutter(hwnd);
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_LBUTTONDOWN:
  case WM_SETFOCUS:
    if (g_app.hwndEdit)
      SetFocus(g_app.hwndEdit);
    return 0;
  default:
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

static void InvalidateGutter() {
  if (g_app.hwndGutter)
    InvalidateRect(g_app.hwndGutter, nullptr, FALSE);
}

static void ClearRichEditVerifyHighlight(HWND hEdit) {
  if (!g_app.editorIsRichEdit || !hEdit)
    return;
  CHARFORMAT2W cf;
  ZeroMemory(&cf, sizeof(cf));
  cf.cbSize = sizeof(CHARFORMAT2W);
  cf.dwMask = CFM_BACKCOLOR;
  cf.crBackColor = GetSysColor(COLOR_WINDOW);
  SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

static void HighlightRichEditErrorLine(HWND hEdit, int line0Based) {
  if (!g_app.editorIsRichEdit || !hEdit)
    return;
  ClearRichEditVerifyHighlight(hEdit);
  const int nlines = (int)SendMessageW(hEdit, EM_GETLINECOUNT, 0, 0);
  if (line0Based < 0 || line0Based >= nlines)
    return;
  const int ls = (int)SendMessageW(hEdit, EM_LINEINDEX, (WPARAM)line0Based, 0);
  if (ls < 0)
    return;
  const int nextStart =
      (line0Based + 1 < nlines)
          ? (int)SendMessageW(hEdit, EM_LINEINDEX, (WPARAM)(line0Based + 1), 0)
          : GetWindowTextLengthW(hEdit);
  CHARRANGE cr;
  cr.cpMin = ls;
  cr.cpMax = nextStart;
  SendMessageW(hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
  CHARFORMAT2W cf;
  ZeroMemory(&cf, sizeof(cf));
  cf.cbSize = sizeof(CHARFORMAT2W);
  cf.dwMask = CFM_BACKCOLOR;
  cf.crBackColor = RGB(255, 90, 90);
  SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
  cr.cpMin = cr.cpMax = ls;
  SendMessageW(hEdit, EM_EXSETSEL, 0, (LPARAM)&cr);
  SendMessageW(hEdit, EM_SCROLLCARET, 0, 0);
  InvalidateGutter();
}

static LRESULT CALLBACK SubclassEditProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
  if (msg == WM_KEYDOWN && wParam == VK_F3) {
    SyncFindBufFromDialogIfOpen();
    if (g_app.findBuf[0]) {
      if (GetKeyState(VK_SHIFT) & 0x8000)
        DoFindPrev(g_app.hwndMain);
      else
        DoFindNext(g_app.hwndMain);
      return 0;
    }
  }
  switch (msg) {
  case WM_VSCROLL:
  case WM_HSCROLL:
  case WM_MOUSEWHEEL:
  case WM_KEYDOWN:
  case WM_SIZE:
    InvalidateGutter();
    break;
  default:
    break;
  }
  LRESULT r =
      CallWindowProcW(g_app.pfnOldEdit, hwnd, msg, wParam, lParam);
  if (g_app.editorIsRichEdit) {
    bool maybeTextChange =
        (msg == WM_CHAR || msg == WM_CUT || msg == WM_CLEAR || msg == WM_PASTE ||
         msg == WM_UNDO ||
         (msg == WM_KEYDOWN &&
          (wParam == VK_BACK || wParam == VK_DELETE ||
           (wParam == L'V' && (GetKeyState(VK_CONTROL) & 0x8000)))));
    if (maybeTextChange)
      ClearRichEditVerifyHighlight(hwnd);
  }
  return r;
}

static void SplitLines(const std::wstring& s, std::vector<std::wstring>* out) {
  out->clear();
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    while (j < s.size() && s[j] != L'\r' && s[j] != L'\n')
      ++j;
    out->push_back(s.substr(i, j - i));
    if (j < s.size() && s[j] == L'\r' && j + 1 < s.size() && s[j + 1] == L'\n')
      j += 2;
    else if (j < s.size())
      ++j;
    i = j;
  }
  if (!s.empty() && (s.back() == L'\n' || s.back() == L'\r'))
    out->push_back(L"");
}

static std::wstring JoinLines(const std::vector<std::wstring>& lines) {
  std::wstring r;
  for (size_t k = 0; k < lines.size(); ++k) {
    r += lines[k];
    if (k + 1 < lines.size())
      r += L"\r\n";
  }
  return r;
}

static void TrimTrailing(std::wstring* line) {
  while (!line->empty() && (line->back() == L' ' || line->back() == L'\t'))
    line->pop_back();
}

static void TrimLeading(std::wstring* line) {
  size_t p = 0;
  while (p < line->size() && (line->at(p) == L' ' || line->at(p) == L'\t'))
    ++p;
  if (p)
    line->erase(0, p);
}

static void CollapseSpaces(std::wstring* line) {
  std::wstring t;
  t.reserve(line->size());
  bool prevSpace = false;
  for (wchar_t c : *line) {
    bool sp = (c == L' ' || c == L'\t');
    if (sp) {
      if (!prevSpace)
        t += L' ';
      prevSpace = true;
    } else {
      t += c;
      prevSpace = false;
    }
  }
  *line = t;
}

/* 선행 공백·탭은 그대로 두고 본문만 반환 */
static size_t SplitLeadingWhitespace(const std::wstring& line,
                                    std::wstring* prefixOut) {
  size_t i = 0;
  while (i < line.size() && (line[i] == L' ' || line[i] == L'\t'))
    ++i;
  if (prefixOut)
    *prefixOut = line.substr(0, i);
  return i;
}

/* 공백(스페이스)만 축약. 탭은 유지 */
static void CollapseAsciiSpacesOnly(std::wstring* s) {
  if (!s || s->empty())
    return;
  std::wstring t;
  t.reserve(s->size());
  bool prevSpace = false;
  for (wchar_t c : *s) {
    if (c == L' ') {
      if (!prevSpace)
        t += L' ';
      prevSpace = true;
    } else {
      t += c;
      prevSpace = false;
    }
  }
  *s = t;
}

static bool ReadWholeFile(const WCHAR* path, std::wstring* out, HWND owner) {
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    MessageBoxW(owner, L"파일을 열 수 없습니다.", kAppTitle, MB_ICONERROR);
    return false;
  }
  DWORD sz = GetFileSize(h, nullptr);
  if (sz == INVALID_FILE_SIZE) {
    CloseHandle(h);
    MessageBoxW(owner, L"파일 크기를 확인할 수 없습니다.", kAppTitle,
                MB_ICONERROR);
    return false;
  }
  if (sz > kMaxFileBytes) {
    CloseHandle(h);
    WCHAR msg[128];
    wsprintfW(msg, L"파일이 너무 큽니다(%uMB 제한).", kMaxFileBytes / (1024u * 1024u));
    MessageBoxW(owner, msg, kAppTitle, MB_ICONWARNING);
    return false;
  }
  std::vector<char> raw((size_t)sz);
  DWORD read = 0;
  if (sz && !ReadFile(h, raw.data(), sz, &read, nullptr)) {
    CloseHandle(h);
    MessageBoxW(owner, L"파일 읽기에 실패했습니다.", kAppTitle, MB_ICONERROR);
    return false;
  }
  CloseHandle(h);

  if (sz >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
    size_t wchars = (sz - 2) / 2;
    out->assign((const WCHAR*)(raw.data() + 2), wchars);
    return true;
  }
  if (sz >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
      (unsigned char)raw[2] == 0xBF) {
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data() + 3, (int)(sz - 3), nullptr,
                                0);
    if (n <= 0) {
      MessageBoxW(owner, L"UTF-8 디코딩에 실패했습니다.", kAppTitle,
                  MB_ICONERROR);
      return false;
    }
    out->resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, raw.data() + 3, (int)(sz - 3), &(*out)[0],
                        n);
    return true;
  }
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)sz,
                              nullptr, 0);
  if (n > 0) {
    out->resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)sz, &(*out)[0], n);
    return true;
  }
  n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)sz, nullptr, 0);
  if (n <= 0) {
    MessageBoxW(owner, L"텍스트 변환에 실패했습니다.", kAppTitle, MB_ICONERROR);
    return false;
  }
  out->resize((size_t)n);
  MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)sz, &(*out)[0], n);
  return true;
}

/* 저장 시 VS Code 기준: Windows(CRLF). \n 또는 단독 \r 도 CRLF 로 통일 */
static std::wstring NormalizeNewlinesToCrLf(const std::wstring& s) {
  std::wstring o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'\r') {
      if (i + 1 < s.size() && s[i + 1] == L'\n')
        ++i;
      o += L"\r\n";
    } else if (s[i] == L'\n') {
      o += L"\r\n";
    } else {
      o += s[i];
    }
  }
  return o;
}

/* UTF-8(BOM 없음) + CRLF — 저장/다른 이름으로 저장 공통 */
static bool WriteWholeFileUtf8(const WCHAR* path, const std::wstring& text,
                               HWND owner) {
  const std::wstring normalized = NormalizeNewlinesToCrLf(text);
  std::string utf8;
  if (!normalized.empty()) {
    int n = WideCharToMultiByte(CP_UTF8, 0, normalized.c_str(),
                                (int)normalized.size() + 1, nullptr, 0, nullptr,
                                nullptr);
    if (n <= 0) {
      MessageBoxW(owner, L"UTF-8 인코딩에 실패했습니다.", kAppTitle,
                  MB_ICONERROR);
      return false;
    }
    utf8.resize((size_t)n - 1);
    WideCharToMultiByte(CP_UTF8, 0, normalized.c_str(), (int)normalized.size(),
                        &utf8[0], n - 1, nullptr, nullptr);
  }
  HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    MessageBoxW(owner, L"파일을 저장할 수 없습니다.", kAppTitle, MB_ICONERROR);
    return false;
  }
  DWORD w = 0;
  if (!utf8.empty() && !WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, nullptr)) {
    CloseHandle(h);
    MessageBoxW(owner, L"파일 쓰기에 실패했습니다.", kAppTitle, MB_ICONERROR);
    return false;
  }
  CloseHandle(h);
  return true;
}

static std::wstring GetEditText(HWND hEdit) {
  int n = GetWindowTextLengthW(hEdit);
  if (n <= 0)
    return L"";
  std::wstring s((size_t)n + 1, L'\0');
  GetWindowTextW(hEdit, &s[0], n + 1);
  s.resize((size_t)n);
  return s;
}

static void SetEditText(HWND hEdit, const std::wstring& s) {
  if (g_app.editorIsRichEdit)
    ClearRichEditVerifyHighlight(hEdit);
  SetWindowTextW(hEdit, s.c_str());
  InvalidateGutter();
}

static void DoOpenFile(HWND hwnd) {
  WCHAR path[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;
  ofn.lpstrFile = path;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter =
      L"텍스트 파일\0*.txt\0모든 파일\0*.*\0\0";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&ofn))
    return;
  std::wstring data;
  if (!ReadWholeFile(path, &data, hwnd))
    return;
  SetEditText(g_app.hwndEdit, data);
  wcsncpy_s(g_app.filePath, path, _TRUNCATE);
}

static void DoSave(HWND hwnd, bool saveAs) {
  WCHAR path[MAX_PATH];
  wcsncpy_s(path, g_app.filePath, _TRUNCATE);
  if (saveAs || path[0] == L'\0') {
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        L"텍스트 (UTF-8, CRLF)\0*.txt\0모든 파일\0*.*\0\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn))
      return;
  }
  std::wstring text = GetEditText(g_app.hwndEdit);
  if (!WriteWholeFileUtf8(path, text, hwnd))
    return;
  wcsncpy_s(g_app.filePath, path, _TRUNCATE);
}

static void DoClear(HWND hwnd) {
  (void)hwnd;
  SetEditText(g_app.hwndEdit, L"");
  g_app.filePath[0] = L'\0';
}

static void DoIndentConvert(HWND hwnd) {
  (void)hwnd;
  std::wstring t = GetEditText(g_app.hwndEdit);
  std::vector<std::wstring> lines;
  SplitLines(t, &lines);
  std::vector<std::wstring> out;
  for (auto& ln : lines) {
    TrimTrailing(&ln);
    TrimLeading(&ln);
    CollapseSpaces(&ln);
    if (!ln.empty())
      out.push_back(ln);
  }
  SetEditText(g_app.hwndEdit, JoinLines(out));
}

static bool ShouldStripSymbol(wchar_t c) {
  switch (c) {
  case L'(':
  case L')':
  case L'{':
  case L'}':
  case L'.':
  case L',':
  case L';':
  case L':':
  case L'_':
  case L'-':
  case L'`':
  case L'\'':
  case L'\"':
    return true;
  default:
    return false;
  }
}

/* 기호 변환: 괄호·구두점 등만 제거. walk/attack/effect 등 영문 액션명은 유지. */
static void DoSymbolConvert(HWND hwnd) {
  (void)hwnd;
  std::wstring t = GetEditText(g_app.hwndEdit);
  std::vector<std::wstring> lines;
  SplitLines(t, &lines);
  for (size_t li = 0; li < lines.size(); ++li) {
    std::wstring s = lines[li];
    TrimTrailing(&s);
    TrimLeading(&s);
    std::wstring w;
    w.reserve(s.size());
    for (wchar_t c : s) {
      if (!ShouldStripSymbol(c))
        w += c;
    }
    CollapseSpaces(&w);
    TrimTrailing(&w);
    TrimLeading(&w);
    lines[li].swap(w);
  }
  SetEditText(g_app.hwndEdit, JoinLines(lines));
}

static bool IsEnglishLetterW(wchar_t c) {
  return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

static bool LineIsHashHeader(const std::wstring& line, size_t* hashPosOut) {
  size_t i = 0;
  while (i < line.size() && (line[i] == L' ' || line[i] == L'\t'))
    ++i;
  if (i >= line.size() || line[i] != L'#')
    return false;
  if (hashPosOut)
    *hashPosOut = i;
  return true;
}

static void TrimTrailingUnderscoreHyphenSpaces(std::wstring* line) {
  while (!line->empty()) {
    const wchar_t c = line->back();
    if (c == L' ' || c == L'\t' || c == L'_' || c == L'-')
      line->pop_back();
    else
      break;
  }
}

/*
 * 4.walk onehandsword( → 4.( … 영문 삭제 후 남는 "4. (" 공백 제거 → 4.(
 * 숫자.숫자(0.0:4 등)는 건드리지 않음
 */
static void RemoveSpacesBetweenIndexDotAndOpenParen(std::wstring* body) {
  if (!body || body->empty())
    return;
  for (size_t i = 0; i < body->size();) {
    if (body->at(i) < L'0' || body->at(i) > L'9') {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < body->size() && body->at(j) >= L'0' && body->at(j) <= L'9')
      ++j;
    if (j < body->size() && body->at(j) == L'.') {
      ++j;
      const size_t gapStart = j;
      while (j < body->size() &&
             (body->at(j) == L' ' || body->at(j) == L'\t'))
        ++j;
      if (j < body->size() && body->at(j) == L'(' && gapStart < j)
        body->erase(gapStart, j - gapStart);
    }
    i = j;
  }
}

/* 0,walk( → 0.( 영문 제거 후 콤마 인덱스 보정 */
static void NormalizeIndexCommaBeforeOpenParen(std::wstring* body) {
  if (!body || body->empty())
    return;
  for (size_t i = 0; i < body->size();) {
    if (body->at(i) < L'0' || body->at(i) > L'9') {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < body->size() && body->at(j) >= L'0' && body->at(j) <= L'9')
      ++j;
    if (j < body->size() && body->at(j) == L',') {
      size_t k = j + 1;
      while (k < body->size() &&
             (body->at(k) == L' ' || body->at(k) == L'\t'))
        ++k;
      if (k < body->size() && body->at(k) == L'(')
        (*body)[j] = L'.';
    }
    i = j;
  }
}

static bool BodyIsActionDataLine(const std::wstring& body) {
  if (body.empty() || body[0] < L'0' || body[0] > L'9')
    return false;
  size_t i = 0;
  while (i < body.size() && body[i] >= L'0' && body[i] <= L'9')
    ++i;
  return i < body.size() && body[i] == L'.' && i + 1 < body.size() &&
         body[i + 1] == L'(';
}

/*
 * #0	312=3225 prince → #0	312=3225
 * 0.walk(1 4,…) → 0.(1 4,…)  /  101.shadow(3226) → 101.(3226)
 * 선행 탭·괄호 안 0.0:4<479 등은 그대로 유지
 */
static void StripAllEnglishLettersFromLine(std::wstring* line) {
  if (!line || line->empty())
    return;
  std::wstring prefix;
  const size_t bodyStart = SplitLeadingWhitespace(*line, &prefix);
  std::wstring body = line->substr(bodyStart);

  std::wstring stripped;
  stripped.reserve(body.size());
  for (wchar_t c : body) {
    if (!IsEnglishLetterW(c))
      stripped += c;
  }
  body.swap(stripped);
  NormalizeIndexCommaBeforeOpenParen(&body);
  RemoveSpacesBetweenIndexDotAndOpenParen(&body);
  CollapseAsciiSpacesOnly(&body);
  TrimTrailing(&body);

  if (BodyIsActionDataLine(body) && prefix.empty())
    prefix = L"\t";

  *line = prefix + body;

  if (LineIsHashHeader(*line, nullptr)) {
    TrimTrailingUnderscoreHyphenSpaces(line);
    TrimTrailing(line);
  } else {
    TrimTrailing(line);
  }
}

/* 영문자 삭제 → .e 리스트 표준 형식 (# 숫자만, N.(데이터…)) */
static void DoStripAllEnglish(HWND hwnd) {
  (void)hwnd;
  std::wstring t = GetEditText(g_app.hwndEdit);
  std::vector<std::wstring> lines;
  SplitLines(t, &lines);
  for (auto& ln : lines)
    StripAllEnglishLettersFromLine(&ln);
  SetEditText(g_app.hwndEdit, JoinLines(lines));
}

static wchar_t FoldAscii(wchar_t c) {
  if (c >= L'A' && c <= L'Z')
    return (wchar_t)(c + (L'a' - L'A'));
  return c;
}

static bool WStrContainsAt(const std::wstring& hay, size_t pos,
                           const std::wstring& nd, bool icase) {
  if (pos + nd.size() > hay.size())
    return false;
  for (size_t i = 0; i < nd.size(); ++i) {
    wchar_t a = hay[pos + i];
    wchar_t b = nd[i];
    if (icase) {
      a = FoldAscii(a);
      b = FoldAscii(b);
    }
    if (a != b)
      return false;
  }
  return true;
}

static void SyncFindBufFromDialogIfOpen() {
  if (g_app.hwndFindEdit && IsWindow(g_app.hwndFindEdit))
    GetWindowTextW(g_app.hwndFindEdit, g_app.findBuf, 256);
}

static void DoFindNext(HWND hwnd) {
  (void)hwnd;
  SyncFindBufFromDialogIfOpen();
  if (!g_app.findBuf[0])
    return;
  std::wstring hay = GetEditText(g_app.hwndEdit);
  std::wstring nd(g_app.findBuf);
  if (nd.empty())
    return;
  const bool icase = true;
  DWORD s0 = 0, s1 = 0;
  SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
  const size_t start = (size_t)s1;
  size_t found = std::wstring::npos;
  for (size_t i = start; i + nd.size() <= hay.size(); ++i) {
    if (WStrContainsAt(hay, i, nd, icase)) {
      found = i;
      break;
    }
  }
  if (found == std::wstring::npos) {
    for (size_t i = 0; i < start && i + nd.size() <= hay.size(); ++i) {
      if (WStrContainsAt(hay, i, nd, icase)) {
        found = i;
        break;
      }
    }
  }
  if (found == std::wstring::npos) {
    MessageBoxW(g_app.hwndMain, L"더 이상 찾을 수 없습니다.", L"찾기",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  SendMessageW(g_app.hwndEdit, EM_SETSEL, (WPARAM)found,
               (LPARAM)(found + nd.size()));
  SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
  InvalidateGutter();
}

static void DoFindPrev(HWND hwnd) {
  (void)hwnd;
  SyncFindBufFromDialogIfOpen();
  if (!g_app.findBuf[0])
    return;
  std::wstring hay = GetEditText(g_app.hwndEdit);
  std::wstring nd(g_app.findBuf);
  if (nd.empty())
    return;
  const bool icase = true;
  DWORD s0 = 0, s1 = 0;
  SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
  const size_t anchor = (size_t)std::min(s0, s1);
  size_t found = std::wstring::npos;
  for (size_t i = 0; i + nd.size() <= hay.size(); ++i) {
    if (i < anchor && WStrContainsAt(hay, i, nd, icase))
      found = i;
  }
  if (found == std::wstring::npos) {
    for (size_t i = 0; i + nd.size() <= hay.size(); ++i) {
      if (WStrContainsAt(hay, i, nd, icase))
        found = i;
    }
  }
  if (found == std::wstring::npos) {
    MessageBoxW(g_app.hwndMain, L"더 이상 찾을 수 없습니다.", L"찾기",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  SendMessageW(g_app.hwndEdit, EM_SETSEL, (WPARAM)found,
               (LPARAM)(found + nd.size()));
  SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
  InvalidateGutter();
}

static void DoFindAll(HWND hwnd) {
  SyncFindBufFromDialogIfOpen();
  if (!g_app.findBuf[0])
    return;
  SendMessageW(g_app.hwndEdit, EM_SETSEL, 0, 0);
  DoFindNext(hwnd);
}

static LRESULT CALLBACK FindDlgProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
  switch (msg) {
  case WM_CREATE: {
    const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    HINSTANCE hi = cs->hInstance;
    g_app.hwndFindDlg = hwnd;
    CreateWindowExW(0, L"STATIC", L"검색 키워드:", WS_CHILD | WS_VISIBLE, 12,
                    14, 100, 18, hwnd, nullptr, hi, nullptr);
    g_app.hwndFindEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_app.findBuf,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        12, 36, 360, 24, hwnd, (HMENU)(UINT_PTR)IDC_FIND_EDIT,
                        hi, nullptr);
    CreateWindowExW(0, L"BUTTON", L"검색 전체",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 12,
                    70, 88, 28, hwnd, (HMENU)(UINT_PTR)IDC_FIND_BTN_ALL, hi,
                    nullptr);
    CreateWindowExW(0, L"BUTTON", L"위로 찾기",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 108,
                    70, 88, 28, hwnd, (HMENU)(UINT_PTR)IDC_FIND_BTN_UP, hi,
                    nullptr);
    CreateWindowExW(0, L"BUTTON", L"아래로 찾기",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 204,
                    70, 88, 28, hwnd, (HMENU)(UINT_PTR)IDC_FIND_BTN_DOWN, hi,
                    nullptr);
    CreateWindowExW(0, L"BUTTON", L"지우기",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 300,
                    70, 88, 28, hwnd, (HMENU)(UINT_PTR)IDC_FIND_BTN_CLEAR, hi,
                    nullptr);
    CreateWindowExW(
        0, L"STATIC", L"F3: 아래로 찾기    Shift+F3: 위로 찾기",
        WS_CHILD | WS_VISIBLE, 12, 108, 380, 22, hwnd, nullptr, hi, nullptr);
    const HFONT hfGui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND ch = GetWindow(hwnd, GW_CHILD);
    while (ch) {
      SendMessageW(ch, WM_SETFONT, (WPARAM)hfGui, FALSE);
      ch = GetWindow(ch, GW_HWNDNEXT);
    }
    SetFocus(g_app.hwndFindEdit);
    return 0;
  }
  case WM_COMMAND: {
    if (HIWORD(wParam) == EN_CHANGE &&
        (HWND)lParam == g_app.hwndFindEdit) {
      GetWindowTextW(g_app.hwndFindEdit, g_app.findBuf, 256);
      return 0;
    }
    switch (LOWORD(wParam)) {
    case IDC_FIND_BTN_ALL:
      DoFindAll(g_app.hwndMain);
      return 0;
    case IDC_FIND_BTN_UP:
      DoFindPrev(g_app.hwndMain);
      return 0;
    case IDC_FIND_BTN_DOWN:
      DoFindNext(g_app.hwndMain);
      return 0;
    case IDC_FIND_BTN_CLEAR:
      g_app.findBuf[0] = L'\0';
      if (g_app.hwndFindEdit)
        SetWindowTextW(g_app.hwndFindEdit, L"");
      return 0;
    default:
      break;
    }
    return 0;
  }
  case WM_KEYDOWN:
    if (wParam == VK_F3) {
      SyncFindBufFromDialogIfOpen();
      if (g_app.findBuf[0]) {
        if (GetKeyState(VK_SHIFT) & 0x8000)
          DoFindPrev(g_app.hwndMain);
        else
          DoFindNext(g_app.hwndMain);
        return 0;
      }
    }
    break;
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    g_app.hwndFindDlg = nullptr;
    g_app.hwndFindEdit = nullptr;
    return 0;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void OpenFindDialog(HWND hwnd) {
  if (IsWindow(g_app.hwndFindDlg)) {
    SetForegroundWindow(g_app.hwndFindDlg);
    return;
  }
  static bool s_regFind = false;
  HINSTANCE hi = GetModuleHandleW(nullptr);
  if (!s_regFind) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = FindDlgProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"LineageFindDlg";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    s_regFind = true;
  }
  RECT r{};
  if (hwnd)
    GetWindowRect(hwnd, &r);
  const int x = r.left + 40;
  const int y = r.top + 80;
  HWND h = CreateWindowExW(
      WS_EX_TOOLWINDOW, L"LineageFindDlg", L"키워드 찾기",
      WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, 396, 168, hwnd,
      nullptr, hi, nullptr);
  if (!h)
    MessageBoxW(hwnd, L"찾기 창을 만들 수 없습니다.", kAppTitle,
                MB_OK | MB_ICONERROR);
}

static bool ParseFirstHashNumber(const std::wstring& line, int* outNum) {
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] != L'#')
      continue;
    size_t j = i + 1;
    if (j >= line.size() || line[j] < L'0' || line[j] > L'9')
      continue;
    size_t k = j;
    while (k < line.size() && line[k] >= L'0' && line[k] <= L'9')
      ++k;
    int v = _wtoi(line.substr(j, k - j).c_str());
    *outNum = v;
    return true;
  }
  return false;
}

static void DoVerifySequence(HWND hwnd) {
  HWND hEdit = g_app.hwndEdit;
  std::wstring t = GetEditText(hEdit);
  std::vector<std::wstring> lines;
  SplitLines(t, &lines);
  int expected = 0;
  size_t errLine0 = 0;
  bool hasErr = false;
  for (size_t li = 0; li < lines.size(); ++li) {
    std::wstring ln = lines[li];
    TrimTrailing(&ln);
    TrimLeading(&ln);
    if (ln.empty())
      continue;
    int num = 0;
    if (!ParseFirstHashNumber(ln, &num))
      continue;
    if (num != expected) {
      errLine0 = li;
      hasErr = true;
      break;
    }
    ++expected;
  }
  if (hasErr) {
    HighlightRichEditErrorLine(hEdit, (int)errLine0);
    WCHAR msg[320];
    wsprintfW(msg, L"순서 오류! 문제가 %u행에서 발생했습니다.",
              (unsigned)(errLine0 + 1));
    MessageBoxW(hwnd, msg, L"검증 결과", MB_OK | MB_ICONERROR);
    return;
  }
  ClearRichEditVerifyHighlight(hEdit);
  WCHAR sum[320];
  wsprintfW(sum,
            L"검증 완료: #0부터 #%d까지 순서가 올바릅니다.\n(#태그가 있는 "
            L"줄만 검사했습니다.)",
            expected > 0 ? expected - 1 : 0);
  MessageBoxW(hwnd, sum, L"검증 결과", MB_OK | MB_ICONINFORMATION);
}

static void DoAbout(HWND hwnd) {
  MessageBoxW(
      hwnd,
      L"제작: Light \n"
      L"지원: 갓!소피아 \n"
      L"업데이트: 2026-04-30\n\n"
      L"Windows 7 이상에서 동작하는 Win32 프로그램입니다.",
      L"정보",
      MB_OK);
}

static HMENU BuildMenuBar() {
  HMENU bar = CreateMenu();

  HMENU mFile = CreatePopupMenu();
  AppendMenuW(mFile, MF_STRING, IDM_FILE_OPEN, L"파일 읽기(&O)...");
  AppendMenuW(mFile, MF_STRING, IDM_FILE_SAVE, L"파일 저장(&S)");
  AppendMenuW(mFile, MF_STRING, IDM_FILE_SAVEAS, L"다른 이름으로 저장(&A)...");
  AppendMenuW(mFile, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(mFile, MF_STRING, IDM_FILE_CLEAR, L"창 지우기(&C)");
  AppendMenuW(mFile, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(mFile, MF_STRING, IDM_FILE_EXIT, L"종료(&X)");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mFile, L"파일(&F)");

  HMENU mEdit = CreatePopupMenu();
  AppendMenuW(mEdit, MF_STRING, IDM_EDIT_INDENT,
              L"들여쓰기 변환 (공백 줄 및 불필요한 공백 제거)");
  AppendMenuW(mEdit, MF_STRING, IDM_EDIT_SYMBOLS,
              L"기호 변환 (괄호·구두점 등 제거, 액션명 유지)");
  AppendMenuW(mEdit, MF_STRING, IDM_EDIT_SORT_ENGLISH,
              L"영문자 삭제 → #0 312=3225 / 0.(1 4,0.0:4…) 형식");
  AppendMenuW(mEdit, MF_STRING, IDM_EDIT_FIND,
              L"키워드 찾기\tCtrl+F (F3 / Shift+F3)");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mEdit, L"편집/변환(&E)");

  HMENU mVer = CreatePopupMenu();
  AppendMenuW(mVer, MF_STRING, IDM_VERIFY_SEQUENCE,
              L"순서 검증 (#0, #1, #2 ... 확인)");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mVer, L"검증(&C)");

  HMENU mAbout = CreatePopupMenu();
  AppendMenuW(mAbout, MF_STRING, IDM_HELP_ABOUT, L"이 프로그램 정보(&A)");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)mAbout, L"정보(&A)");

  return bar;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
  switch (msg) {
  case WM_CREATE: {
    g_app.hwndMain = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&g_app);
    SetMenu(hwnd, BuildMenuBar());

    static bool s_reg = false;
    if (!s_reg) {
      WNDCLASSW wc{};
      wc.lpfnWndProc = GutterWndProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"LineageListGutter";
      wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      RegisterClassW(&wc);
      s_reg = true;
    }

    g_app.hwndGutter =
        CreateWindowExW(0, L"LineageListGutter", L"", WS_CHILD | WS_VISIBLE, 0,
                        0, 44, 100, hwnd, (HMENU)(UINT_PTR)IDC_LINE_GUTTER,
                        GetModuleHandleW(nullptr), nullptr);

    g_app.editorIsRichEdit = false;
    LPCWSTR editClass = L"EDIT";
    if (!g_hMsfteditDll)
      g_hMsfteditDll = LoadLibraryW(L"Msftedit.dll");
    if (g_hMsfteditDll)
      editClass = L"RichEdit50W";

    DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                      ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                      ES_WANTRETURN | ES_NOOLEDRAGDROP;
    g_app.hwndEdit =
        CreateWindowExW(WS_EX_CLIENTEDGE, editClass, L"", editStyle, 44, 0,
                        200, 100, hwnd, (HMENU)(UINT_PTR)IDC_MAIN_EDIT,
                        GetModuleHandleW(nullptr), nullptr);
    if (!g_app.hwndEdit && g_hMsfteditDll && editClass == L"RichEdit50W") {
      g_app.hwndEdit =
          CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                          (editStyle & ~(DWORD)ES_NOOLEDRAGDROP), 44, 0, 200,
                          100, hwnd, (HMENU)(UINT_PTR)IDC_MAIN_EDIT,
                          GetModuleHandleW(nullptr), nullptr);
    } else if (g_app.hwndEdit && wcscmp(editClass, L"RichEdit50W") == 0) {
      g_app.editorIsRichEdit = true;
    }

    if (g_app.editorIsRichEdit) {
      SendMessageW(g_app.hwndEdit, EM_SETEVENTMASK, 0,
                   (LPARAM)(ENM_CHANGE | ENM_SCROLL));
      SendMessageW(g_app.hwndEdit, EM_EXLIMITTEXT, 0, (LPARAM)kMaxEditChars);
    }
    SendMessageW(g_app.hwndEdit, WM_SETFONT,
                 (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    g_app.pfnOldEdit =
        (WNDPROC)SetWindowLongPtrW(g_app.hwndEdit, GWLP_WNDPROC,
                                   (LONG_PTR)SubclassEditProc);
    LayoutChildren(hwnd);

    ACCEL acc[1];
    acc[0].fVirt = FVIRTKEY | FCONTROL;
    acc[0].key = 'F';
    acc[0].cmd = (WORD)IDM_EDIT_FIND;
    g_hAccel = CreateAcceleratorTableW(acc, 1);
    return 0;
  }
  case WM_SIZE:
    LayoutChildren(hwnd);
    InvalidateGutter();
    return 0;
  case WM_SETFOCUS:
    if (g_app.hwndEdit)
      SetFocus(g_app.hwndEdit);
    return 0;
  case WM_COMMAND: {
    int id = LOWORD(wParam);
    switch (id) {
    case IDM_FILE_OPEN:
      DoOpenFile(hwnd);
      break;
    case IDM_FILE_SAVE:
      DoSave(hwnd, false);
      break;
    case IDM_FILE_SAVEAS:
      DoSave(hwnd, true);
      break;
    case IDM_FILE_CLEAR:
      DoClear(hwnd);
      break;
    case IDM_FILE_EXIT:
      DestroyWindow(hwnd);
      break;
    case IDM_EDIT_INDENT:
      DoIndentConvert(hwnd);
      break;
    case IDM_EDIT_SYMBOLS:
      DoSymbolConvert(hwnd);
      break;
    case IDM_EDIT_SORT_ENGLISH:
      DoStripAllEnglish(hwnd);
      break;
    case IDM_EDIT_FIND:
      OpenFindDialog(hwnd);
      break;
    case IDM_VERIFY_SEQUENCE:
      DoVerifySequence(hwnd);
      break;
    case IDM_HELP_ABOUT:
      DoAbout(hwnd);
      break;
    default:
      if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == g_app.hwndEdit)
        InvalidateGutter();
      break;
    }
    return 0;
  }
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    if (g_hAccel) {
      DestroyAcceleratorTable(g_hAccel);
      g_hAccel = nullptr;
    }
    if (g_hMsfteditDll) {
      FreeLibrary(g_hMsfteditDll);
      g_hMsfteditDll = nullptr;
    }
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&icc);

  WNDCLASSW wc{};
  wc.lpfnWndProc = MainWndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = L"LineageListToolMain";
  wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                               GetSystemMetrics(SM_CXICON),
                               GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  RegisterClassW(&wc);

  HWND hwnd = CreateWindowExW(
      0, L"LineageListToolMain", kAppTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, 900, 650, nullptr, nullptr, hInst, nullptr);
  if (!hwnd)
    return 1;

  HICON hLarge =
      (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                        LR_DEFAULTCOLOR);
  HICON hSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON),
                                   IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON),
                                   LR_DEFAULTCOLOR);
  if (hLarge)
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hLarge);
  if (hSmall)
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    if (g_hAccel && g_app.hwndMain &&
        TranslateAcceleratorW(g_app.hwndMain, g_hAccel, &msg))
      continue;
    if (g_app.hwndFindDlg && IsDialogMessageW(g_app.hwndFindDlg, &msg))
      continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}
