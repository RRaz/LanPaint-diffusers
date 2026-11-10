/*
 * lanpaint.c - "LanPaint", a Photoshop-type .8bf filter for Affinity Photo.
 *
 * The layer is 16-bit; its alpha channel is the mask (0 edits, maximum keeps).
 * The filter hands the layer and the mask to affinity_runner.py, which runs
 * LanPaint with the chosen model, and puts the result back into the image. The
 * alpha channel is passed through untouched, so the mask survives a run.
 *
 * lanpaint.ini, beside the .8bf:
 *
 *     home=D:\Projects\LanPaint-diffusers
 *     python=D:\Projects\LanPaint-diffusers\.venv\Scripts\python.exe
 *     model=sd3
 *     steps=40
 *     guidance=7.0
 *     timeout=900000
 *     max_area=2097152
 *     smart_crop=0
 *
 * Build: build_lanpaint.bat (MSVC x64), same SDK headers as the RRAZ filters.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* This toolchain leaves WIN32 defined but empty, which makes the SDK's
   "#if WIN32" an invalid constant expression. */
#undef WIN32
#define WIN32 1

#include "PIFilter.h"

#define LP_TITLE "LanPaint"

/* dialog resources, see lanpaint.rc */
#define IDD_PROMPT   200
#define IDC_PROMPT   201
#define IDC_STEPS    202
#define IDC_GUIDANCE 203
#define IDC_NEGATIVE 204
#define IDC_SELECTION 205
#define IDC_STRENGTH 206
#define IDC_T2I      207
#define IDC_I2I      208
#define IDC_HISTORY  209
#define IDD_RUN      210
#define IDC_RUN_TEXT 211
#define IDC_RUN_BAR  212

/* ------------------------------------------------------------------ */
/* state                                                               */

static HINSTANCE g_hinst;

void PluginMain(int16 selector, FilterRecordPtr pb, intptr_t *data, int16 *result);

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

static char g_dir[MAX_PATH];        /* folder the .8bf lives in */
static char g_home[MAX_PATH];       /* the repository with affinity_runner.py */
static char g_python[MAX_PATH];
static char g_model[64];
static char g_model_id[MAX_PATH];
static char g_prompt[4096];
static char g_negative[2048];
static int  g_steps;
static int  g_offload;
static int  g_keep_mask;
static char g_device[16];
static int  g_smart_crop;
static long g_selection_limit;
static long g_sel_w, g_sel_h;
static int  g_no_mask;
static double g_strength;
static double g_img_strength;   /* last strength entered with image to image */
static int g_t2i;
static long g_crop_area;
static int  g_mask_grow;
static int  g_blend_ramp;
static long g_max_area;
static double g_guidance;
static DWORD g_timeout;
static char g_log[MAX_PATH];
static char g_ini[MAX_PATH];
static char g_history[MAX_PATH];

/* Recent prompt/negative pairs, newest first, kept in lanpaint-history.ini. */
#define HISTORY_MAX 20
static char g_hist_prompt[HISTORY_MAX][4096];
static char g_hist_negative[HISTORY_MAX][2048];
static int  g_hist_count;

static void history_load(void);

static int  g_planes;
static int  g_depth;
static long g_w, g_h;
static int  g_use32;
static int  g_processed;
static int  g_failed;
static int  g_cancelled;
static char g_message[2048];

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void dll_dir(void)
{
    HMODULE self = g_hinst;
    DWORD n;
    char *p;

    if (!self) {
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&PluginMain, &self)) {
            self = NULL;
        }
    }
    n = GetModuleFileNameA(self, g_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { g_dir[0] = 0; return; }
    p = strrchr(g_dir, '\\');
    if (p) *p = 0;
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = 0;
    return s;
}

static void logmsg(const char *fmt, ...)
{
    char line[4096];
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    f = g_log[0] ? fopen(g_log, "a") : NULL;
    if (f) { fputs(line, f); fputc('\n', f); fclose(f); }
}

static void fail(const char *fmt, ...)
{
    va_list ap;

    if (g_failed) return;
    g_failed = 1;
    va_start(ap, fmt);
    vsnprintf(g_message, sizeof g_message, fmt, ap);
    va_end(ap);
    logmsg("error  : %s", g_message);
}

static void show_message(void)
{
    if (!g_failed) return;
    MessageBoxA(GetForegroundWindow(), g_message, LP_TITLE,
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */

static void escape_prompt(const char *in, char *out, size_t outsz);
static void unescape_prompt(const char *in, char *out, size_t outsz);

static void load_config(void)
{
    char line[8192];
    FILE *f;

    dll_dir();
    g_home[0] = g_python[0] = g_model_id[0] = g_prompt[0] = g_negative[0] = 0;
    lstrcpynA(g_model, "sd3", sizeof g_model);
    g_steps = 28;
    g_strength = 1.0;
    g_img_strength = 1.0;
    g_t2i = 0;
    g_offload = 1;
    g_keep_mask = 0;
    lstrcpynA(g_device, "cuda", sizeof g_device);
    g_smart_crop = 0;
    g_selection_limit = 629145L;
    g_crop_area = 1048576L;
    g_mask_grow = 24;
    g_blend_ramp = 12;
    g_max_area = 2097152L;
    g_guidance = 7.0;
    g_timeout = 900000UL;

    snprintf(g_ini, MAX_PATH, "%s\\lanpaint.ini", g_dir);
    snprintf(g_history, MAX_PATH, "%s\\lanpaint-history.ini", g_dir);
    f = fopen(g_ini, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *key = trim(line);
            char *eq;
            char *value;

            if (key[0] == 0 || key[0] == '#' || key[0] == ';') continue;
            eq = strchr(key, '=');
            if (!eq) continue;
            *eq = 0;
            value = trim(eq + 1);
            key = trim(key);

            if (!_stricmp(key, "home")) lstrcpynA(g_home, value, MAX_PATH);
            else if (!_stricmp(key, "python")) lstrcpynA(g_python, value, MAX_PATH);
            else if (!_stricmp(key, "model")) lstrcpynA(g_model, value, sizeof g_model);
            else if (!_stricmp(key, "model_id")) lstrcpynA(g_model_id, value, MAX_PATH);
            else if (!_stricmp(key, "prompt")) unescape_prompt(value, g_prompt, sizeof g_prompt);
            else if (!_stricmp(key, "negative")) unescape_prompt(value, g_negative, sizeof g_negative);
            else if (!_stricmp(key, "steps")) g_steps = atoi(value);
            else if (!_stricmp(key, "guidance")) g_guidance = atof(value);
            else if (!_stricmp(key, "strength")) {
                g_strength = atof(value);
                g_img_strength = g_strength;
            }
            else if (!_stricmp(key, "t2i")) g_t2i = atoi(value);
            else if (!_stricmp(key, "max_area")) g_max_area = atol(value);
            else if (!_stricmp(key, "offload")) g_offload = atoi(value);
            else if (!_stricmp(key, "keep_mask")) g_keep_mask = atoi(value);
            else if (!_stricmp(key, "device")) lstrcpynA(g_device, value, sizeof g_device);
            else if (!_stricmp(key, "smart_crop")) g_smart_crop = atoi(value);
            else if (!_stricmp(key, "selection_limit")) g_selection_limit = atol(value);
            else if (!_stricmp(key, "crop_area")) g_crop_area = atol(value);
            else if (!_stricmp(key, "mask_grow")) g_mask_grow = atoi(value);
            else if (!_stricmp(key, "blend_ramp")) g_blend_ramp = atoi(value);
            else if (!_stricmp(key, "timeout")) g_timeout = (DWORD)atol(value);
        }
        fclose(f);
    }

    if (g_python[0] == 0 && g_home[0])
        snprintf(g_python, MAX_PATH, "%s\\.venv\\Scripts\\python.exe", g_home);

    snprintf(g_log, MAX_PATH, "%s\\lanpaint.log", g_dir);
    history_load();
}

/* One line in the ini has to hold a prompt that may be several lines long:
   backslashes and line breaks are escaped, and unescaped when it is read. */
static void escape_prompt(const char *in, char *out, size_t outsz)
{
    size_t o = 0;

    for (; *in && o + 3 < outsz; in++) {
        if (*in == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else if (*in == '\r') continue;
        else if (*in == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static void unescape_prompt(const char *in, char *out, size_t outsz)
{
    size_t o = 0;

    while (*in && o + 1 < outsz) {
        if (*in == '\\' && in[1] == 'n') { out[o++] = '\n'; in += 2; }
        else if (*in == '\\' && in[1] == '\\') { out[o++] = '\\'; in += 2; }
        else out[o++] = *in++;
    }
    out[o] = 0;
}
static void save_setting(const char *name, const char *value)
{
    char backup[MAX_PATH + 4];
    char escaped[8192];
    char line[8192];
    FILE *in;
    FILE *out;
    int replaced = 0;
    size_t name_len = strlen(name);

    if (g_ini[0] == 0) return;
    escape_prompt(value, escaped, sizeof escaped);

    snprintf(backup, sizeof backup, "%s.bak", g_ini);
    in = fopen(g_ini, "r");
    out = fopen(backup, "w");
    if (!out) { if (in) fclose(in); return; }

    if (in) {
        while (fgets(line, sizeof line, in)) {
            char probe[8192];
            char *key;

            lstrcpynA(probe, line, sizeof probe);
            key = trim(probe);
            if (!_strnicmp(key, name, name_len)) {
                char *after = key + name_len;
                while (*after == ' ' || *after == '\t') after++;
                if (*after == '=') {
                    fprintf(out, "%s=%s\n", name, escaped);
                    replaced = 1;
                    continue;
                }
            }
            fputs(line, out);
        }
        fclose(in);
    }
    if (!replaced) fprintf(out, "%s=%s\n", name, escaped);
    fclose(out);

    DeleteFileA(g_ini);
    MoveFileA(backup, g_ini);
}

/* ------------------------------------------------------------------ */
/* prompt history                                                      */

static void history_load(void)
{
    char line[8192];
    FILE *f;
    int open = -1;

    g_hist_count = 0;
    if (g_history[0] == 0) return;
    f = fopen(g_history, "r");
    if (!f) return;

    while (fgets(line, sizeof line, f)) {
        char *key, *value, *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        key = trim(line);
        value = trim(eq + 1);

        if (!_stricmp(key, "prompt")) {
            if (g_hist_count >= HISTORY_MAX) break;
            open = g_hist_count++;
            unescape_prompt(value, g_hist_prompt[open], sizeof g_hist_prompt[open]);
            g_hist_negative[open][0] = 0;
        } else if (!_stricmp(key, "negative") && open >= 0) {
            unescape_prompt(value, g_hist_negative[open],
                            sizeof g_hist_negative[open]);
        }
    }
    fclose(f);
}

static void history_save(void)
{
    char escaped[8192];
    char temp[MAX_PATH + 4];
    FILE *f;
    int i;

    if (g_history[0] == 0) return;
    snprintf(temp, sizeof temp, "%s.tmp", g_history);
    f = fopen(temp, "w");
    if (!f) return;
    for (i = 0; i < g_hist_count; i++) {
        escape_prompt(g_hist_prompt[i], escaped, sizeof escaped);
        fprintf(f, "prompt=%s\n", escaped);
        escape_prompt(g_hist_negative[i], escaped, sizeof escaped);
        fprintf(f, "negative=%s\n", escaped);
    }
    fclose(f);
    MoveFileExA(temp, g_history, MOVEFILE_REPLACE_EXISTING);
}

static void history_add(const char *prompt, const char *negative)
{
    int i, found = -1;

    if (!prompt || !*prompt) return;
    for (i = 0; i < g_hist_count; i++) {
        if (!strcmp(g_hist_prompt[i], prompt) &&
            !strcmp(g_hist_negative[i], negative)) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        if (g_hist_count < HISTORY_MAX) g_hist_count++;
        found = g_hist_count - 1;
    }
    for (i = found; i > 0; i--) {
        lstrcpynA(g_hist_prompt[i], g_hist_prompt[i - 1],
                  sizeof g_hist_prompt[i]);
        lstrcpynA(g_hist_negative[i], g_hist_negative[i - 1],
                  sizeof g_hist_negative[i]);
    }
    lstrcpynA(g_hist_prompt[0], prompt, sizeof g_hist_prompt[0]);
    lstrcpynA(g_hist_negative[0], negative, sizeof g_hist_negative[0]);
    history_save();
}

static void history_label(int i, char *out, size_t outsz)
{
    const char *s = g_hist_prompt[i];
    size_t o = 0;

    while (*s && o + 4 < outsz && o < 64) {
        out[o++] = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
        s++;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    if (*s) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* prompt dialog                                                       */

typedef struct {
    char prompt[4096];
    char negative[2048];
    double strength;
    int t2i;
    int i2i;
    int steps;
    double guidance;
} PromptData;

/* Strength belongs to an unmasked run with Image to image ticked. A masked run
   is inpainting: it always starts from full strength, so the field is disabled
   and reads 1.00. Text to image ignores both. */
static void sync_strength(HWND hwnd, PromptData *data)
{
    int t2i = IsDlgButtonChecked(hwnd, IDC_T2I) == BST_CHECKED;
    int i2i = g_no_mask && !t2i &&
              IsDlgButtonChecked(hwnd, IDC_I2I) == BST_CHECKED;
    char value[32];

    data->t2i = t2i;
    data->i2i = i2i;

    EnableWindow(GetDlgItem(hwnd, IDC_I2I), g_no_mask && !t2i);
    EnableWindow(GetDlgItem(hwnd, IDC_STRENGTH), i2i);

    if (i2i)
        snprintf(value, sizeof value, "%.2f", data->strength);
    else
        lstrcpynA(value, "1.00", sizeof value);
    SetDlgItemTextA(hwnd, IDC_STRENGTH, value);
}

static INT_PTR CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PromptData *data = (PromptData *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_INITDIALOG:
        data = (PromptData *)lp;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        SetDlgItemTextA(hwnd, IDC_PROMPT, data->prompt);
        SetDlgItemTextA(hwnd, IDC_NEGATIVE, data->negative);
        {
            int i, sel = -1;
            SendDlgItemMessageA(hwnd, IDC_HISTORY, CB_RESETCONTENT, 0, 0);
            SendDlgItemMessageA(hwnd, IDC_HISTORY, CB_SETDROPPEDWIDTH, 380, 0);
            for (i = 0; i < g_hist_count; i++) {
                char label[96];
                history_label(i, label, sizeof label);
                SendDlgItemMessageA(hwnd, IDC_HISTORY, CB_ADDSTRING, 0,
                                    (LPARAM)label);
                if (!strcmp(g_hist_prompt[i], data->prompt) &&
                    !strcmp(g_hist_negative[i], data->negative))
                    sel = i;
            }
            if (g_hist_count == 0) {
                SendDlgItemMessageA(hwnd, IDC_HISTORY, CB_ADDSTRING, 0,
                                    (LPARAM)"(no history yet)");
                SendDlgItemMessageA(hwnd, IDC_HISTORY, CB_SETCURSEL, 0, 0);
            } else {
                SendDlgItemMessageA(hwnd, IDC_HISTORY, CB_SETCURSEL,
                                    (WPARAM)sel, 0);
            }
            EnableWindow(GetDlgItem(hwnd, IDC_HISTORY), g_hist_count > 0);
        }
        {
            char text[160];
            if (g_no_mask) {
                snprintf(text, sizeof text,
                         "Nothing painted: the whole layer %ld x %ld = %.2f MP "
                         "will be processed.",
                         g_sel_w, g_sel_h, g_sel_w * g_sel_h / 1000000.0);
            } else if (!g_smart_crop) {
                snprintf(text, sizeof text,
                         "Whole layer %ld x %ld = %.2f MP; only the mask changes.",
                         g_w, g_h, g_w * g_h / 1000000.0);
            } else {
                snprintf(text, sizeof text,
                         "Selection: %ld x %ld = %.2f MP; the run uses about %.2f MP around it.",
                         g_sel_w, g_sel_h,
                         g_sel_w * g_sel_h / 1000000.0,
                         g_crop_area / 1000000.0);
            }
            SetDlgItemTextA(hwnd, IDC_SELECTION, text);
        }
        SetDlgItemInt(hwnd, IDC_STEPS, (UINT)data->steps, FALSE);
        {
            char value[32];
            snprintf(value, sizeof value, "%.1f", data->guidance);
            SetDlgItemTextA(hwnd, IDC_GUIDANCE, value);
        }
        CheckDlgButton(hwnd, IDC_T2I, data->t2i ? BST_CHECKED : BST_UNCHECKED);
        data->i2i = g_no_mask;
        CheckDlgButton(hwnd, IDC_I2I, data->i2i ? BST_CHECKED : BST_UNCHECKED);
        sync_strength(hwnd, data);
        SendDlgItemMessageA(hwnd, IDC_PROMPT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(hwnd, IDC_PROMPT));
        return FALSE;

    case WM_COMMAND:
        if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == IDC_HISTORY) {
            int sel = (int)SendDlgItemMessageA(hwnd, IDC_HISTORY,
                                               CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_hist_count) {
                SetDlgItemTextA(hwnd, IDC_PROMPT, g_hist_prompt[sel]);
                SetDlgItemTextA(hwnd, IDC_NEGATIVE, g_hist_negative[sel]);
            }
            return TRUE;
        }
        if (HIWORD(wp) == BN_CLICKED &&
            (LOWORD(wp) == IDC_T2I || LOWORD(wp) == IDC_I2I)) {
            char value[32];
            /* Keep whatever was typed before this click disables the field. */
            if (IsWindowEnabled(GetDlgItem(hwnd, IDC_STRENGTH))) {
                GetDlgItemTextA(hwnd, IDC_STRENGTH, value, sizeof value);
                data->strength = atof(value);
                if (data->strength <= 0.0) data->strength = 1.0;
                if (data->strength > 1.0) data->strength = 1.0;
            }
            sync_strength(hwnd, data);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextA(hwnd, IDC_PROMPT, data->prompt, sizeof data->prompt);
            GetDlgItemTextA(hwnd, IDC_NEGATIVE, data->negative, sizeof data->negative);
            data->steps = (int)GetDlgItemInt(hwnd, IDC_STEPS, NULL, FALSE);
            {
                char value[32];
                GetDlgItemTextA(hwnd, IDC_GUIDANCE, value, sizeof value);
                data->guidance = atof(value);
                data->t2i = IsDlgButtonChecked(hwnd, IDC_T2I) == BST_CHECKED;
                data->i2i = g_no_mask && !data->t2i &&
                            IsDlgButtonChecked(hwnd, IDC_I2I) == BST_CHECKED;
                if (data->i2i) {
                    GetDlgItemTextA(hwnd, IDC_STRENGTH, value, sizeof value);
                    data->strength = atof(value);
                    if (data->strength <= 0.0) data->strength = 1.0;
                    if (data->strength > 1.0) data->strength = 1.0;
                } else {
                    data->strength = 1.0;
                }
            }
            if (data->prompt[0])
                history_add(data->prompt, data->negative);
            EndDialog(hwnd, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(hwnd, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static int ask_prompt(PromptData *data)
{
    lstrcpynA(data->prompt, g_prompt, sizeof data->prompt);
    lstrcpynA(data->negative, g_negative, sizeof data->negative);
    data->steps = g_steps;
    data->guidance = g_guidance;
    data->strength = g_no_mask ? g_img_strength : 1.0;
    data->t2i = g_t2i;
    data->i2i = g_no_mask ? 1 : 0;
    return (int)DialogBoxParamA(g_hinst, MAKEINTRESOURCEA(IDD_PROMPT),
                                GetForegroundWindow(), prompt_proc, (LPARAM)data);
}

/* ------------------------------------------------------------------ */
/* running the runner                                                  */

static void drain_available(HANDLE pipe, char *capture, size_t caplen, size_t *used)
{
    DWORD avail, got, want;

    for (;;) {
        char sink[4096];

        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) || avail == 0) return;

        if (capture && *used < caplen - 1) {
            want = (DWORD)(caplen - 1 - *used);
            if (want > avail) want = avail;
            if (!ReadFile(pipe, capture + *used, want, &got, NULL)) return;
            *used += got;
            capture[*used] = 0;
        } else {
            want = (avail < (DWORD)sizeof sink) ? avail : (DWORD)sizeof sink;
            if (!ReadFile(pipe, sink, want, &got, NULL)) return;
        }
    }
}

typedef struct {
    const char *exe;
    const char *args;
    DWORD timeout;
    char *capture;
    size_t caplen;
    HANDLE process;
    HANDLE done;
    volatile LONG cancel;
    int code;
} RunJob;

static DWORD WINAPI run_thread(LPVOID param)
{
    RunJob *job = (RunJob *)param;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE pipe_r = NULL, pipe_w = NULL, null_in;
    char *cmdline;
    DWORD started;
    size_t used = 0;

    cmdline = (char *)malloc(strlen(job->exe) + strlen(job->args) + 8);
    if (!cmdline) { job->code = -1; SetEvent(job->done); return 0; }
    sprintf(cmdline, "\"%s\" %s", job->exe, job->args);

    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&pipe_r, &pipe_w, &sa, 0)) {
        free(cmdline);
        job->code = -1;
        SetEvent(job->done);
        return 0;
    }
    SetHandleInformation(pipe_r, HANDLE_FLAG_INHERIT, 0);

    null_in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          &sa, OPEN_EXISTING, 0, NULL);

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = pipe_w;
    si.hStdError = pipe_w;
    si.hStdInput = (null_in == INVALID_HANDLE_VALUE) ? NULL : null_in;

    ZeroMemory(&pi, sizeof pi);
    if (CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                       NULL, (g_home[0] ? g_home : NULL), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pipe_w);
        pipe_w = NULL;
        job->process = pi.hProcess;
        started = GetTickCount();

        for (;;) {
            DWORD waited;

            drain_available(pipe_r, job->capture, job->caplen, &used);

            if (InterlockedCompareExchange(&job->cancel, 1, 1) == 1) {
                TerminateProcess(pi.hProcess, 1);
                logmsg("run    : cancelled by the user");
                break;
            }
            waited = WaitForSingleObject(pi.hProcess, 50);
            if (waited == WAIT_OBJECT_0) {
                drain_available(pipe_r, job->capture, job->caplen, &used);
                break;
            }
            if (GetTickCount() - started > job->timeout) {
                TerminateProcess(pi.hProcess, 1);
                logmsg("run    : timed out after %lu ms", (unsigned long)job->timeout);
                break;
            }
        }

        GetExitCodeProcess(pi.hProcess, &job->code);
        CloseHandle(pi.hProcess);
        job->process = NULL;
    } else {
        logmsg("run    : CreateProcess failed (%lu)", (unsigned long)GetLastError());
        job->code = -1;
    }

    if (pipe_w) CloseHandle(pipe_w);
    if (pipe_r) CloseHandle(pipe_r);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    free(cmdline);
    SetEvent(job->done);
    return 0;
}

static INT_PTR CALLBACK run_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RunJob *job = (RunJob *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_INITDIALOG: {
        DWORD started = GetTickCount();

        job = (RunJob *)lp;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)job);
        SetWindowLongPtrA(hwnd, DWLP_USER, (LONG_PTR)started);
        SendDlgItemMessageA(hwnd, IDC_RUN_BAR, PBM_SETMARQUEE, TRUE, 40);
        SetTimer(hwnd, 1, 250, NULL);
        return TRUE;
    }

    case WM_TIMER: {
        DWORD started = (DWORD)GetWindowLongPtrA(hwnd, DWLP_USER);
        char text[128];

        if (job && WaitForSingleObject(job->done, 0) == WAIT_OBJECT_0) {
            KillTimer(hwnd, 1);
            EndDialog(hwnd, 1);
            return TRUE;
        }
        snprintf(text, sizeof text, "Working... %lu s",
                 (unsigned long)((GetTickCount() - started) / 1000));
        SetDlgItemTextA(hwnd, IDC_RUN_TEXT, text);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            if (job) InterlockedExchange(&job->cancel, 1);
            KillTimer(hwnd, 1);
            EndDialog(hwnd, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* Runs the runner with a small window that shows the time passing and offers
   Cancel. The dialog's own message loop keeps the host responsive. */
static int run_ui(const char *exe, const char *args, DWORD timeout_ms,
                  char *capture, size_t caplen)
{
    RunJob job;
    INITCOMMONCONTROLSEX icc;
    HANDLE thread;

    memset(&job, 0, sizeof job);
    job.exe = exe;
    job.args = args;
    job.timeout = timeout_ms;
    job.capture = capture;
    job.caplen = caplen;
    job.code = -1;
    job.done = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!job.done) return -1;

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    thread = CreateThread(NULL, 0, run_thread, &job, 0, NULL);
    if (!thread) {
        CloseHandle(job.done);
        return -1;
    }

    DialogBoxParamA(g_hinst, MAKEINTRESOURCEA(IDD_RUN), GetForegroundWindow(),
                    run_proc, (LPARAM)&job);

    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(job.done);

    if (job.cancel) return -1;
    return job.code;
}
typedef struct { long left, top, right, bottom; } IRect;

static int has_big(FilterRecordPtr pb) { return pb->bigDocumentData != 0; }

static void get_filter_rect(FilterRecordPtr pb, IRect *r)
{
    if (g_use32 && has_big(pb)) {
        VRect *v = &pb->bigDocumentData->filterRect32;
        r->left = v->left; r->top = v->top;
        r->right = v->right; r->bottom = v->bottom;
    } else {
        r->left = pb->filterRect.left; r->top = pb->filterRect.top;
        r->right = pb->filterRect.right; r->bottom = pb->filterRect.bottom;
    }
}

static void get_in_rect(FilterRecordPtr pb, IRect *r)
{
    if (g_use32 && has_big(pb)) {
        VRect *v = &pb->bigDocumentData->inRect32;
        r->left = v->left; r->top = v->top;
        r->right = v->right; r->bottom = v->bottom;
    } else {
        r->left = pb->inRect.left; r->top = pb->inRect.top;
        r->right = pb->inRect.right; r->bottom = pb->inRect.bottom;
    }
}

static void get_out_rect(FilterRecordPtr pb, IRect *r)
{
    if (g_use32 && has_big(pb)) {
        VRect *v = &pb->bigDocumentData->outRect32;
        r->left = v->left; r->top = v->top;
        r->right = v->right; r->bottom = v->bottom;
    } else {
        r->left = pb->outRect.left; r->top = pb->outRect.top;
        r->right = pb->outRect.right; r->bottom = pb->outRect.bottom;
    }
}

static void request_rect(FilterRecordPtr pb, long l, long t, long r, long b)
{
    if (g_use32 && has_big(pb)) {
        pb->bigDocumentData->inRect32.left = (int32)l;
        pb->bigDocumentData->inRect32.top = (int32)t;
        pb->bigDocumentData->inRect32.right = (int32)r;
        pb->bigDocumentData->inRect32.bottom = (int32)b;
        pb->bigDocumentData->outRect32.left = (int32)l;
        pb->bigDocumentData->outRect32.top = (int32)t;
        pb->bigDocumentData->outRect32.right = (int32)r;
        pb->bigDocumentData->outRect32.bottom = (int32)b;
        pb->bigDocumentData->maskRect32.left = 0;
        pb->bigDocumentData->maskRect32.top = 0;
        pb->bigDocumentData->maskRect32.right = 0;
        pb->bigDocumentData->maskRect32.bottom = 0;
    } else {
        pb->inRect.left = (int16)l;
        pb->inRect.top = (int16)t;
        pb->inRect.right = (int16)r;
        pb->inRect.bottom = (int16)b;
        pb->outRect.left = (int16)l;
        pb->outRect.top = (int16)t;
        pb->outRect.right = (int16)r;
        pb->outRect.bottom = (int16)b;
        pb->maskRect.left = 0;
        pb->maskRect.top = 0;
        pb->maskRect.right = 0;
        pb->maskRect.bottom = 0;
    }
}

/* ------------------------------------------------------------------ */
/* the run                                                             */

static void copy_through(unsigned char *out, long outrowbytes,
                         const unsigned char *in, long inrowbytes)
{
    size_t row = (size_t)g_w * g_planes * 2u;
    long y;

    for (y = 0; y < g_h; y++) {
        memcpy(out + (size_t)y * outrowbytes, in + (size_t)y * inrowbytes, row);
    }
}

static void apply_result(unsigned char *out, long outrowbytes,
                         const unsigned char *in, long inrowbytes,
                         const unsigned short *rgb)
{
    long x, y;

    for (y = 0; y < g_h; y++) {
        unsigned char *o = out + (size_t)y * outrowbytes;
        const unsigned char *i = in + (size_t)y * inrowbytes;
        const unsigned short *r = rgb + (size_t)y * g_w * 3;

        for (x = 0; x < g_w; x++) {
            unsigned short *o16 = (unsigned short *)o;
            const unsigned short *i16 = (const unsigned short *)i;

            o16[0] = (unsigned short)(r[x * 3 + 0] >> 1);
            o16[1] = (unsigned short)(r[x * 3 + 1] >> 1);
            o16[2] = (unsigned short)(r[x * 3 + 2] >> 1);
            if (g_planes == 4) {
                /* The mask has done its work: unless keep_mask says to leave it,
                   every partly transparent pixel becomes opaque. Thresholding at
                   half left the soft rim of the selection behind, which read as a
                   faint outline of the selection in the result. */
                if (!g_keep_mask && i16[3] < 32767)
                    o16[3] = 32767;
                else
                    o16[3] = i16[3];
            }
            o += g_planes * 2;
            i += g_planes * 2;
        }
    }
}

static int make_temp(char *dir, size_t dirsz, char *inpath, size_t insz,
                     char *outpath, size_t outsz, char *promptpath, size_t psz)
{
    char base[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, base);

    if (n == 0 || n >= MAX_PATH) return 0;
    snprintf(dir, dirsz, "%srraz-lanpaint", base);
    CreateDirectoryA(dir, NULL);
    snprintf(inpath, insz, "%s\\%lu.rgba16", dir, (unsigned long)GetCurrentProcessId());
    snprintf(outpath, outsz, "%s\\%lu.rgb16", dir, (unsigned long)GetCurrentProcessId());
    snprintf(promptpath, psz, "%s\\%lu.txt", dir, (unsigned long)GetCurrentProcessId());
    return 1;
}

/* Smallest rectangle holding every painted pixel of the mask (alpha < half),
   measured in the layer, so the size can be judged before anything runs. */
static int mask_box(const unsigned char *in, long inrowbytes,
                    long *box_w, long *box_h, long *box_x, long *box_y)
{
    long x, y;
    long left = g_w, top = g_h, right = -1, bottom = -1;

    for (y = 0; y < g_h; y++) {
        const unsigned short *row =
            (const unsigned short *)(in + (size_t)y * inrowbytes);

        for (x = 0; x < g_w; x++) {
            if (row[x * g_planes + 3] < 32767) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
    }

    if (right < 0) return 0;
    *box_x = left;
    *box_y = top;
    *box_w = right - left + 1;
    *box_h = bottom - top + 1;
    return 1;
}
static unsigned short *run_job(const unsigned char *in, long inrowbytes)
{
    char dir[MAX_PATH];
    char inpath[MAX_PATH];
    char outpath[MAX_PATH];
    char promptpath[MAX_PATH];
    char negpath[MAX_PATH];
    char argline[4096];
    char capture[65536];
    char preview[MAX_PATH];
    FILE *f;
    unsigned short *rgb = NULL;
    size_t bytes = (size_t)g_w * (size_t)g_h * 2u;
    long y;
    int rc;

    memset(capture, 0, sizeof capture);
    if (!make_temp(dir, sizeof dir, inpath, sizeof inpath,
                   outpath, sizeof outpath, promptpath, sizeof promptpath)) {
        fail("No temporary folder is available.");
        return NULL;
    }
    snprintf(negpath, sizeof negpath, "%s\\%lu.neg.txt", dir, (unsigned long)GetCurrentProcessId());
    snprintf(preview, sizeof preview, "%s\\%lu.png", dir, (unsigned long)GetCurrentProcessId());

    if (g_python[0] == 0 || GetFileAttributesA(g_python) == INVALID_FILE_ATTRIBUTES) {
        fail("The Python interpreter was not found.\n\n"
             "Set python= in lanpaint.ini, beside the filter.");
        return NULL;
    }
    if (g_home[0] == 0 || GetFileAttributesA(g_home) == INVALID_FILE_ATTRIBUTES) {
        fail("The LanPaint folder was not found.\n\n"
             "Set home= in lanpaint.ini, beside the filter.");
        return NULL;
    }

    f = fopen(inpath, "wb");
    if (!f) { fail("Could not write\n\n%s", inpath); return NULL; }
    for (y = 0; y < g_h; y++) {
        size_t row = (size_t)g_w * g_planes * 2u;
        if (fwrite(in + (size_t)y * inrowbytes, 1, row, f) != row) {
            fclose(f);
            fail("Could not write the layer to\n\n%s", inpath);
            return NULL;
        }
    }
    fclose(f);

    f = fopen(promptpath, "w");
    if (!f) { fail("Could not write\n\n%s", promptpath); return NULL; }
    fputs(g_prompt, f);
    fclose(f);

    f = fopen(negpath, "w");
    if (!f) { fail("Could not write\n\n%s", negpath); return NULL; }
    fputs(g_negative, f);
    fclose(f);

    snprintf(argline, sizeof argline,
             "\"%s\\affinity_runner.py\" --input \"%s\" --width %ld --height %ld "
             "--planes %d --prompt-file \"%s\" --negative-prompt-file \"%s\" --output \"%s\" --preview \"%s\" "
             "--model %s --steps %d --guidance %.2f --max-area %ld --offload %d --device %s --smart-crop %d --selection-limit %ld --crop-area %ld --mask-grow %d --blend-ramp %d --no-mask %d --strength %.2f --t2i %d%s%s",
             g_home, inpath, g_w, g_h, g_planes, promptpath, negpath, outpath, preview,
             g_model, g_steps, g_guidance, g_max_area, g_offload, g_device,
             g_smart_crop, g_selection_limit, g_crop_area, g_mask_grow, g_blend_ramp, g_no_mask, g_strength, g_t2i,
             g_model_id[0] ? " --model-id " : "",
             g_model_id[0] ? g_model_id : "");

    logmsg("cmd    : %s %s", g_python, argline);
    rc = run_ui(g_python, argline, g_timeout, capture, sizeof capture);
    logmsg("runner : %s", capture[0] ? capture : "(no output)");

    if (rc == -1) {
        /* Cancelled from the progress window: the layer stays as it was and
           nothing is reported as a failure. */
        logmsg("run    : cancelled, nothing written");
        goto done;
    }
    if (rc != 0) {
        fail("LanPaint did not finish (exit %d).\n\n%s",
             rc, capture[0] ? capture : "(no output)");
        goto done;
    }

    rgb = (unsigned short *)malloc(bytes * 3u);
    if (!rgb) { fail("Out of memory (%ld x %ld x 3).", g_w, g_h); goto done; }

    f = fopen(outpath, "rb");
    if (!f) { fail("The runner wrote no result.\n\n%s", outpath); free(rgb); rgb = NULL; goto done; }
    if (fread(rgb, 2, (size_t)g_w * (size_t)g_h * 3u, f) != (size_t)g_w * (size_t)g_h * 3u) {
        fclose(f);
        fail("The result is the wrong size. Expected %ld x %ld.", g_w, g_h);
        free(rgb); rgb = NULL;
        goto done;
    }
    fclose(f);

done:
    DeleteFileA(inpath);
    DeleteFileA(outpath);
    DeleteFileA(promptpath);
    DeleteFileA(negpath);
    return rgb;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */

void PluginMain(int16 selector, FilterRecordPtr pb, intptr_t *data, int16 *result)
{
    (void)data;

    load_config();
    logmsg("select %d", (int)selector);

    switch (selector) {
    case filterSelectorAbout:
        logmsg("about  : %s 1.0.0", LP_TITLE);
        *result = 0;
        return;

    case filterSelectorParameters:
        *result = 0;
        return;

    case filterSelectorPrepare:
        if (has_big(pb)) pb->bigDocumentData->PluginUsing32BitCoordinates = 1;
        logmsg("---- %s", LP_TITLE);
        logmsg("mode   : %d  depth=%d  planes=%d",
               (int)pb->imageMode, (int)pb->depth, (int)pb->planes);
        *result = 0;
        return;

    case filterSelectorStart: {
        IRect fr;

        g_failed = 0;
        g_processed = 0;
        g_cancelled = 0;
        g_no_mask = 0;
        g_planes = pb->planes;
        g_depth = pb->depth;
        g_use32 = has_big(pb);
        g_message[0] = 0;

        if (g_use32) pb->bigDocumentData->PluginUsing32BitCoordinates = 1;

        get_filter_rect(pb, &fr);
        g_w = fr.right - fr.left;
        g_h = fr.bottom - fr.top;

        if (g_w <= 0 || g_h <= 0) {
            fail("The layer has no pixels.");
            request_rect(pb, 0, 0, 0, 0);
            *result = 0;
            return;
        }
        if (g_depth != 16) {
            fail("This is a %d-bit document. LanPaint takes 16-bit layers.\n\n"
                 "Convert it first: Document > Colour Format > RGB (16 bit).",
                 (int)g_depth);
            request_rect(pb, 0, 0, 0, 0);
            *result = 0;
            return;
        }
        if (g_planes < 4) {
            fail("The layer has no alpha channel.\n\nThe alpha channel is the mask: "
                 "0 marks the area to replace, maximum keeps the rest.");
            request_rect(pb, 0, 0, 0, 0);
            *result = 0;
            return;
        }

        logmsg("start  : %ld x %ld, %d planes, %d bits", g_w, g_h, g_planes, g_depth);

        pb->inLoPlane = pb->outLoPlane = 0;
        pb->inHiPlane = pb->outHiPlane = (int16)(g_planes - 1);
        request_rect(pb, fr.left, fr.top, fr.right, fr.bottom);

        if (pb->advanceState) {
            int16 adv = pb->advanceState();
            logmsg("start  : advanceState -> %d", (int)adv);
        }
        *result = 0;
        return;
    }

    case filterSelectorContinue: {
        IRect ir, orr;
        long w, h;

        get_in_rect(pb, &ir);
        get_out_rect(pb, &orr);
        w = ir.right - ir.left;
        h = ir.bottom - ir.top;

        if (!pb->inData || !pb->outData || w <= 0 || h <= 0) {
            fail("The host handed over no pixels.");
            goto tile_done;
        }

        if (w != g_w || h != g_h) {
            copy_through((unsigned char *)pb->outData, pb->outRowBytes,
                         (const unsigned char *)pb->inData, pb->inRowBytes);
            fail("This layer arrived in tiles (%ld x %ld of %ld x %ld).\n\n"
                 "LanPaint needs the whole layer in one pass.", w, h, g_w, g_h);
            goto tile_done;
        }

        /* The layer is passed through by default: cancelling the dialog, a
           selection that is too large, or a failed run all leave it as it was. */
        copy_through((unsigned char *)pb->outData, pb->outRowBytes,
                     (const unsigned char *)pb->inData, pb->inRowBytes);

        if (!g_processed) {
            PromptData prompt;
            long box_w = 0, box_h = 0, box_x = 0, box_y = 0;

            g_processed = 1;

            /* The mask is measured here, where the pixels are, so an impossible
               selection stops before the dialog rather than after a run. It is
               measured whether or not smart crop is on: the dialog needs to
               know if anything is painted either way. */
            if (g_planes == 4) {
                if (!mask_box((const unsigned char *)pb->inData, pb->inRowBytes,
                              &box_w, &box_h, &box_x, &box_y)) {
                    /* Nothing painted: treat the whole layer as the subject, as
                       long as it fits the working area. */
                    if (g_w * g_h > g_max_area) {
                        fail("Nothing is painted, so the whole layer would be "
                             "processed, but it is %ld x %ld = %.2f MP.\n\n"
                             "The limit is %.2f MP. Crop the layer in Affinity "
                             "first (its crops are non-destructive), paint an "
                             "area, or raise max_area in lanpaint.ini.",
                             g_w, g_h, g_w * g_h / 1000000.0,
                             g_max_area / 1000000.0);
                        goto tile_done;
                    }
                    g_no_mask = 1;
                    box_w = g_w;
                    box_h = g_h;
                    g_sel_w = g_w;
                    g_sel_h = g_h;
                    logmsg("mask   : none painted, whole layer %ld x %ld", g_w, g_h);
                } else {
                    g_sel_w = box_w;
                    g_sel_h = box_h;
                    logmsg("mask   : %ld x %ld at %ld,%ld", box_w, box_h, box_x, box_y);
                }

                if (!g_smart_crop) {
                    /* The whole layer goes to the model, painted or not. */
                    if (g_w * g_h > g_max_area) {
                        fail("Smart crop is off, so the whole layer is processed, "
                             "but it is %ld x %ld = %.2f MP.\n\nThe limit is "
                             "%.2f MP. Crop the layer in Affinity first (its crops "
                             "are non-destructive), or turn smart_crop back on.",
                             g_w, g_h, g_w * g_h / 1000000.0,
                             g_max_area / 1000000.0);
                        goto tile_done;
                    }
                } else if (!g_no_mask && box_w * box_h > g_selection_limit) {
                    fail("The selection is %ld x %ld, which is %.2f MP.\n\n"
                         "The limit is %.2f MP. Select a smaller area, or raise "
                         "selection_limit in lanpaint.ini.",
                         box_w, box_h, box_w * box_h / 1000000.0,
                         g_selection_limit / 1000000.0);
                    goto tile_done;
                }
            }

            if (!ask_prompt(&prompt)) {
                logmsg("dialog : cancelled");
                g_cancelled = 1;
                goto tile_done;
            }
            lstrcpynA(g_prompt, prompt.prompt, sizeof g_prompt);
            lstrcpynA(g_negative, prompt.negative, sizeof g_negative);
            g_steps = prompt.steps;
            g_strength = prompt.strength;
            g_t2i = prompt.t2i;
            g_guidance = prompt.guidance;
            save_setting("prompt", g_prompt);
            save_setting("negative", g_negative);
            if (prompt.i2i) {
                /* Keep the img2img strength for the next visit; a masked run
                   always uses 1.0, so it must not overwrite this. */
                char strength[32];
                g_img_strength = prompt.strength;
                snprintf(strength, sizeof strength, "%.2f", g_img_strength);
                save_setting("strength", strength);
            }
            logmsg("mode   : %s, strength %.2f",
                   prompt.t2i ? "text to image" :
                   prompt.i2i ? "image to image" : "inpaint",
                   prompt.strength);

            if (g_prompt[0] == 0) {
                fail("The prompt is empty.");
                goto tile_done;
            }
            logmsg("prompt : %s", g_prompt);
            logmsg("negative: %s", g_negative);

            /* An unmasked run is the experimental image-to-image path. It is
               easy to reach by accident when a mask sat on another layer or the
               alpha was cleared, so ask once, with No as the default. */
            if (g_no_mask && !g_t2i) {
                char text[512];
                snprintf(text, sizeof text,
                         "Nothing is painted in this layer's alpha channel.\n\n"
                         "The whole layer %ld x %ld (%.2f MP) will be re-rendered "
                         "at strength %.2f.\n\n"
                         "This is the experimental image-to-image mode, not "
                         "inpainting. Continue?",
                         g_w, g_h, g_w * g_h / 1000000.0, g_strength);
                if (MessageBoxA(GetForegroundWindow(), text, "LanPaint",
                                MB_ICONWARNING | MB_YESNO |
                                MB_DEFBUTTON2) != IDYES) {
                    logmsg("dialog : unmasked run cancelled");
                    g_cancelled = 1;
                    goto tile_done;
                }
            }

            {
                unsigned short *rgb = run_job((const unsigned char *)pb->inData,
                                              pb->inRowBytes);
                if (!rgb) goto tile_done;
                apply_result((unsigned char *)pb->outData, pb->outRowBytes,
                             (const unsigned char *)pb->inData, pb->inRowBytes, rgb);
                free(rgb);
                logmsg("done   : result written at %ld x %ld", g_w, g_h);
            }
        }

tile_done:
        request_rect(pb, 0, 0, 0, 0);
        *result = 0;
        return;
    }

    case filterSelectorFinish:
        logmsg("finish : %s", g_failed ? "failed" : (g_processed ? "ok" : "nothing"));
        show_message();
        *result = 0;
        return;

    default:
        *result = 0;
        return;
    }
}
