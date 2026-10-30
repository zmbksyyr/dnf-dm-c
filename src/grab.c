// grab：只读游戏内存取物品编号。
//
// 思路：
//   1. CreateToolhelp32Snapshot 按进程名找 PID 与主模块；
//   2. 复刻参考工具：AOB 动态定位悬停对象指针槽，读取 id / 名称 / 品质（builtin）；
//   3. 鼠标不在物品上时悬停槽是空的：连续取码按节奏轮询，空槽就跳过，绝不猜；
//   4. 名称优先用 PVF，PVF 不认识时保留内存中的 UTF-16 名称。
// 全程只用 ReadProcessMemory / VirtualQueryEx，不写、不注入、不挂钩。
// 扫堆（scan）只留在 --grab-probe 诊断里：它分不清谁被悬停，取的是全堆最高分，
// 当成取码结果用只会填错编号。
#include "grab.h"
#include "item.h"
#include <windows.h>
#include <tlhelp32.h>

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

typedef struct {
    u32   id;
    u64   addr;
    int   score;
    u8    type;
} Cand;

#define MAX_CAND 256
#define SCAN_CHUNK (1024 * 1024)
#define SCAN_BUDGET (256u * 1024 * 1024)   // 单次最多读 256MB

static char   g_proc[64] = "DNF.exe";
static GrabNameFn g_namer;

// 物品代码获取器 1.3 的唯一定位方案：命中点前 11 字节是 32 位对象指针槽地址。
static const u8 k_builtin_sig[] = {
    0x8B, 0xF8, 0x8B, 0x4F, 0x04, 0x8B, 0x47, 0x08, 0x2B,
    0xC1, 0xC1, 0xF8, 0x02, 0x33, 0xF6, 0x85, 0xC0
};
static u32 g_builtin_pid;
static u64 g_builtin_base, g_builtin_match, g_builtin_slot;

// 最近一次扫描的候选，供诊断
static Cand g_last[MAX_CAND];
static int  g_last_n;

int grab_last_cands(GrabCand *out, int max) {
    int n = g_last_n < max ? g_last_n : max;
    // 按分数降序冒泡（最多 64 个，够用）
    for (int i = 0; i < g_last_n; i++)
        for (int j = i + 1; j < g_last_n; j++)
            if (g_last[j].score > g_last[i].score) {
                Cand t = g_last[i];
                g_last[i] = g_last[j];
                g_last[j] = t;
            }
    for (int i = 0; i < n; i++) {
        out[i].id = g_last[i].id;
        out[i].addr = g_last[i].addr;
        out[i].score = g_last[i].score;
        out[i].type = g_last[i].type;
    }
    return n;
}

void grab_bind(const char *proc, GrabNameFn namer) {
    if (proc && proc[0]) snprintf(g_proc, sizeof(g_proc), "%s", proc);
    g_namer = namer;
}

// ---------------------------------------------------------------- 进程/模块
static u32 find_pid(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);
    u32 pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            char nm[128];
            utf8(pe.szExeFile, nm, sizeof(nm));
            if (lstrcmpiA(nm, g_proc) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static u64 module_base(u32 pid, char *name, int cap, u32 *size) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me;
    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    u64 base = 0;
    if (size) *size = 0;
    if (Module32FirstW(snap, &me)) {
        base = (u64)(uintptr_t)me.modBaseAddr;
        if (name) utf8(me.szModule, name, cap);
        if (size) *size = me.modBaseSize;
    }
    CloseHandle(snap);
    return base;
}

// ---------------------------------------------------------------- 读内存
static int read_mem(HANDLE hp, u64 addr, void *buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(hp, (LPCVOID)(uintptr_t)addr, buf, n, &got) && got == n;
}

static int page_readable(DWORD protect) {
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
           !(protect & (PAGE_GUARD | PAGE_NOACCESS));
}

static u64 scan_module_sig(HANDLE hp, u64 base, u32 size, u32 *slot_out) {
    if (!base || size < sizeof(k_builtin_sig)) return 0;
    u64 end = base + size, addr = base;
    u8 *buf = (u8 *)xalloc(SCAN_CHUNK);
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQueryEx(hp, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi))) break;
        u64 reg = (u64)(uintptr_t)mbi.BaseAddress;
        u64 stop = reg + (u64)mbi.RegionSize;
        u64 st = reg < base ? base : reg;
        if (stop > end) stop = end;
        if (stop > st && mbi.State == MEM_COMMIT && page_readable(mbi.Protect)) {
            u64 off = 0, len = stop - st;
            while (off < len) {
                size_t n = (size_t)((len - off) > SCAN_CHUNK ? SCAN_CHUNK : (len - off));
                if (read_mem(hp, st + off, buf, n)) {
                    for (size_t i = 0; i + sizeof(k_builtin_sig) <= n; i++) {
                        if (memcmp(buf + i, k_builtin_sig, sizeof(k_builtin_sig)) != 0) continue;
                        u64 hit = st + off + i;
                        u32 slot = 0, probe = 0;
                        // 可能在数据段碰到同字节串；只有前置 DWORD 指向可读槽才算真命中。
                        if (hit >= base + 11 && read_mem(hp, hit - 11, &slot, sizeof(slot)) &&
                            slot >= 0x10000u && read_mem(hp, slot, &probe, sizeof(probe))) {
                            *slot_out = slot;
                            free(buf);
                            return hit;
                        }
                    }
                }
                if (n <= sizeof(k_builtin_sig)) break;
                off += n - sizeof(k_builtin_sig) + 1;   // 相邻块保留签名长度的重叠
            }
        }
        if (stop <= addr) break;
        addr = stop;
    }
    free(buf);
    return 0;
}

static int builtin_locate(HANDLE hp, u32 pid, u64 base, u32 size) {
    if (g_builtin_pid == pid && g_builtin_base == base && g_builtin_slot) return 1;
    g_builtin_pid = pid;
    g_builtin_base = base;
    g_builtin_match = g_builtin_slot = 0;
    u32 slot = 0;
    u64 hit = scan_module_sig(hp, base, size, &slot);
    if (!hit) return 0;
    g_builtin_match = hit;
    g_builtin_slot = slot;
    return 1;
}

static int builtin_read(HANDLE hp, u32 pid, u64 base, u32 size, GrabHit *out) {
    if (!builtin_locate(hp, pid, base, size)) return 0;
    u32 obj = 0, id = 0;
    if (!read_mem(hp, g_builtin_slot, &obj, sizeof(obj)) || obj < 0x10000u) return 0;
    if (!read_mem(hp, (u64)obj + 0x1C, &id, sizeof(id)) || !id || id > 200000000u) return 0;

    out->id = id;
    snprintf(out->strategy, sizeof(out->strategy), "builtin");

    u32 name_ptr = 0;
    WCHAR wn[27];
    memset(wn, 0, sizeof(wn));
    if (read_mem(hp, (u64)obj + 0x20, &name_ptr, sizeof(name_ptr)) && name_ptr >= 0x10000u &&
        read_mem(hp, name_ptr, wn, 52)) {
        wn[26] = 0;
        if (wn[0]) utf8(wn, out->name, sizeof(out->name));
    }

    static const char *quality[] = {"白色", "蓝色", "紫色", "粉色", "橙色", "异界"};
    u32 q = 0;
    if (read_mem(hp, (u64)obj + 0xF4, &q, sizeof(q)) && q < COUNT_OF(quality))
        snprintf(out->type, sizeof(out->type), "%s", quality[q]);
    return 1;
}

// ---------------------------------------------------------------- 打分
static int valid_type(u8 t) {
    return t == 1 || t == 2 || t == 3 || t == 4 || t == 5 || t == 6 || t == 7 || t == 0x0A;
}

static int score_item(u32 id, u8 type, int heap) {
    if (id == 0 || id > 200000000u) return -1;
    if (!valid_type(type)) return -1;
    int sc = 10;
    if (g_namer) {
        char nm[96];
        const char *n = g_namer(id, nm, sizeof(nm));
        if (n && n[0] && n[0] != '#') sc += 100;   // 编号在 PVF 里，几乎就能确定
    }
    if (heap) sc += 2;
    return sc;
}

static void add_cand(Cand *cs, int *n, u32 id, u64 addr, int sc, u8 type) {
    if (*n < MAX_CAND) {
        cs[*n].id = id;
        cs[*n].addr = addr;
        cs[*n].score = sc;
        cs[*n].type = type;
        (*n)++;
        return;
    }
    // 满员就替掉最差的（同分留地址小的，数组头更稳定）
    int low = 0;
    for (int i = 1; i < *n; i++)
        if (cs[i].score < cs[low].score || (cs[i].score == cs[low].score && cs[i].addr > cs[low].addr))
            low = i;
    if (sc > cs[low].score || (sc == cs[low].score && addr < cs[low].addr)) {
        cs[low].id = id;
        cs[low].addr = addr;
        cs[low].score = sc;
        cs[low].type = type;
    }
}

// 在 [addr, addr+len) 里按 61 字节物品结构找候选。
// 除单条合法性外，还看 ±61 字节的相邻记录：真实背包/仓库是一整片数组，
// 孤立命中多半是巧合，这个信号能把两者分开。
static void scan_buf(const u8 *b, size_t len, u64 base, int heap, Cand *cs, int *n) {
    for (size_t i = 0; i + 61 <= len; i += 4) {
        u8 type = b[i + 1];
        if (!valid_type(type)) continue;
        u32 id = rd32(b + i + 2);
        int sc = score_item(id, type, heap);
        if (sc < 0) continue;
        if ((b[i + 6] & 0x1F) > 25) continue;          // 强化位越界
        // 相邻记录加分
        const int step = 61;
        for (int k = -1; k <= 1; k += 2) {
            long j = (long)i + (long)k * step;
            if (j < 0 || (size_t)j + 61 > len) continue;
            const u8 *nb = b + j;
            if (valid_type(nb[1]) && nb[1] != 0 && score_item(rd32(nb + 2), nb[1], heap) > 0)
                sc += 30;
            else if (nb[1] == 0)
                sc += 5;
        }
        add_cand(cs, n, id, base + i, sc, type);
    }
}

// ---------------------------------------------------------------- 窗口判定
// 只用于诊断信息：鼠标落在哪个窗口上（非交互会话读不到鼠标，返回 0 即可）
static int cursor_window(u32 pid, char *cls, int cap, int *x, int *y) {
    POINT pt;
    if (!GetCursorPos(&pt)) {
        *x = *y = 0;
        return 0;
    }
    *x = pt.x;
    *y = pt.y;
    HWND h = WindowFromPoint(pt);
    if (!h) return 0;
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid != pid) return 0;
    WCHAR w[128] = L"";
    GetClassNameW(h, w, 128);
    utf8(w, cls, cap);
    return 1;
}

// ---------------------------------------------------------------- 主流程
// 打开目标进程、取主模块。hp 由调用方 CloseHandle。
static int open_target(u32 *pid, HANDLE *hp, u64 *base, u32 *size, char *mod, Err err) {
    *pid = find_pid();
    if (!*pid) {
        snprintf(err, ERRBUF, "没找到进程 %s（请先启动游戏）", g_proc);
        return 0;
    }
    *hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, *pid);
    if (!*hp) *hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid);
    if (!*hp) {
        snprintf(err, ERRBUF, "打开进程失败（错误码 %lu），请以管理员身份运行", GetLastError());
        return 0;
    }
    *size = 0;
    *base = module_base(*pid, mod, 128, size);
    return 1;
}

// 命中后的收尾：名称优先 PVF，PVF 不认识时保留内存里读到的
static void grab_finish(GrabHit *out, DWORD t0) {
    if (g_namer) {
        char nm[96] = "";
        const char *n = g_namer(out->id, nm, sizeof(nm));
        if (n && n[0] && n[0] != '#') snprintf(out->name, sizeof(out->name), "%s", n);
    }
    if (!out->name[0]) snprintf(out->name, sizeof(out->name), "#%u", out->id);
    out->ms = GetTickCount() - t0;
}

static int grab_hit_ex(GrabHit *out, int allow_scan, Err err) {
    memset(out, 0, sizeof(*out));
    g_last_n = 0;
    DWORD t0 = GetTickCount();

    u32 pid;
    HANDLE hp;
    u64 base;
    u32 size;
    char mod[128] = "";
    if (!open_target(&pid, &hp, &base, &size, mod, err)) return 0;
    out->pid = pid;

    // 1) builtin：参考工具的 AOB 动态定位，正常端无需任何配置
    if (base && size && builtin_read(hp, pid, base, size, out)) goto done;

    // 2) 记录鼠标所在窗口（只认属于目标进程的窗口），仅用于诊断显示
    cursor_window(pid, out->wnd, sizeof(out->wnd), &out->x, &out->y);

    // 3) 自适应回退：扫堆。只有诊断（--grab-probe）还走这条路——它分不清
    //    谁被悬停，取的是全堆最高分，当结果用只会填错编号。连续取码不扫堆：
    //    单次最多读 256MB，按 250ms 一轮的节奏会把 CPU 吃满。
    if (allow_scan) {
        Cand cs[MAX_CAND];
        int n = 0;
        u64 scanned = 0;
        u8 *buf = (u8 *)xalloc(SCAN_CHUNK);
        u64 addr = 0;
        while (addr < 0x00007FFFFFFF0000ull && scanned < SCAN_BUDGET) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQueryEx(hp, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi))) break;
            u64 reg = (u64)(uintptr_t)mbi.BaseAddress;
            u64 sz = (u64)mbi.RegionSize;
            int readable = (mbi.State == MEM_COMMIT) && page_readable(mbi.Protect);
            int heap = (mbi.Type == MEM_PRIVATE);
            // 物品结构只在堆里，跳过映像段能省一半以上时间
            if (readable && heap && sz >= 61) {
                u64 off = 0;
                while (off < sz && scanned < SCAN_BUDGET) {
                    size_t chunk = (size_t)((sz - off) > SCAN_CHUNK ? SCAN_CHUNK : (sz - off));
                    if (read_mem(hp, reg + off, buf, chunk)) {
                        scan_buf(buf, chunk, reg + off, heap, cs, &n);
                        scanned += chunk;
                    }
                    off += chunk;
                }
            }
            u64 next = reg + sz;
            if (next <= addr) break;
            addr = next;
        }
        free(buf);
        out->cands = (u32)n;
        g_last_n = n;
        if (n > 0) memcpy(g_last, cs, sizeof(Cand) * (size_t)n);

        // 取最高分；同分取地址最低的（数组头，结果稳定）
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (best < 0 || cs[i].score > cs[best].score ||
                (cs[i].score == cs[best].score && cs[i].addr < cs[best].addr))
                best = i;
        }
        if (best >= 0) {
            out->id = cs[best].id;
            snprintf(out->type, sizeof(out->type), "%s", item_type_name(cs[best].type));
            snprintf(out->strategy, sizeof(out->strategy), "scan");
        }
    }

done:
    CloseHandle(hp);
    if (!out->id) {
        snprintf(err, ERRBUF, "%s 里没扫到可用物品（确认游戏已启动并悬停到物品上）", g_proc);
        out->ms = GetTickCount() - t0;
        return 0;
    }
    grab_finish(out, t0);
    return 1;
}

int grab_hit(GrabHit *out, Err err)      { return grab_hit_ex(out, 1, err); }
int grab_hit_fast(GrabHit *out, Err err) { return grab_hit_ex(out, 0, err); }

// ---------------------------------------------------------------- 诊断
void grab_probe(Str *out) {
    str_catf(out, "=== DNF GM 取码诊断 ===\n");
    str_catf(out, "目标进程: %s\n", g_proc);

    u32 pid = find_pid();
    if (!pid) {
        str_catf(out, "结果: 未找到进程（游戏没启动，或进程名不对）\n");
        return;
    }
    str_catf(out, "PID: %u\n", pid);

    char mod[128] = "";
    u32 mod_size = 0;
    u64 base = module_base(pid, mod, sizeof(mod), &mod_size);
    str_catf(out, "主模块: %s 基址 0x%llX 大小 %u MB\n", mod, (unsigned long long)base,
             (unsigned)(mod_size / (1024 * 1024)));

    HANDLE hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hp) {
        str_catf(out, "打开进程失败: 错误码 %lu（请以管理员身份运行）\n", GetLastError());
        return;
    }

    str_catf(out, "\n-- 内置基址（物品代码获取器 1.3）--\n");
    str_catf(out, "  AOB: 8B F8 8B 4F 04 8B 47 08 2B C1 C1 F8 02 33 F6 85 C0\n");
    if (builtin_locate(hp, pid, base, mod_size)) {
        u32 obj = 0, id = 0;
        read_mem(hp, g_builtin_slot, &obj, sizeof(obj));
        if (obj) read_mem(hp, (u64)obj + 0x1C, &id, sizeof(id));
        str_catf(out, "  命中 0x%llX · 指针槽 0x%llX · 对象 0x%X · id=%u\n",
                 (unsigned long long)g_builtin_match, (unsigned long long)g_builtin_slot, obj, id);
        str_catf(out, "  字段: id +0x1C · 名称指针 +0x20 · 品质 +0xF4\n");
    } else {
        str_catf(out, "  当前主模块没有命中特征码，将回退到堆扫描\n");
    }

    // 段表
    str_catf(out, "\n-- 内存段（前 24 个可读段）--\n");
    u64 addr = 0, total = 0;
    int shown = 0;
    while (addr < 0x00007FFFFFFF0000ull) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQueryEx(hp, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi))) break;
        u64 reg = (u64)(uintptr_t)mbi.BaseAddress, sz = (u64)mbi.RegionSize;
        if (mbi.State == MEM_COMMIT) {
            total += sz;
            if (shown < 24) {
                str_catf(out, "  0x%012llX  %8llu KB  prot=0x%lX type=%s\n",
                         (unsigned long long)reg, (unsigned long long)(sz / 1024),
                         (unsigned long)mbi.Protect,
                         mbi.Type == MEM_PRIVATE ? "private" : (mbi.Type == MEM_IMAGE ? "image" : "mapped"));
                shown++;
            }
        }
        u64 next = reg + sz;
        if (next <= addr) break;
        addr = next;
    }
    str_catf(out, "  已提交合计: %llu MB\n", (unsigned long long)(total / (1024 * 1024)));

    // DNF_WND_* 窗口
    str_catf(out, "\n-- 目标进程窗口 --\n");
    {
        HWND h = GetTopWindow(NULL);
        int cnt = 0;
        while (h && cnt < 200) {
            DWORD wpid = 0;
            GetWindowThreadProcessId(h, &wpid);
            if (wpid == pid) {
                WCHAR cw[128] = L"", tw[128] = L"";
                GetClassNameW(h, cw, 128);
                GetWindowTextW(h, tw, 128);
                RECT r;
                GetWindowRect(h, &r);
                char c8[128], t8[128];
                utf8(cw, c8, sizeof(c8));
                utf8(tw, t8, sizeof(t8));
                str_catf(out, "  %-24s \"%s\"  (%d,%d)-(%d,%d)\n", c8, t8, (int)r.left, (int)r.top,
                         (int)r.right, (int)r.bottom);
                cnt++;
            }
            h = GetWindow(h, GW_HWNDNEXT);
        }
        if (!cnt) str_catf(out, "  （无顶层窗口）\n");
    }

    // 鼠标与窗口
    {
        char cls[128] = "";
        int x = 0, y = 0;
        if (cursor_window(pid, cls, sizeof(cls), &x, &y))
            str_catf(out, "\n鼠标: (%d,%d) 窗口 %s\n", x, y, cls);
        else {
            POINT pt;
            GetCursorPos(&pt);
            str_catf(out, "\n鼠标: (%ld,%ld) 不在目标进程窗口上\n", pt.x, pt.y);
        }
    }

    // 实际扫一次
    str_catf(out, "\n-- 扫描 --\n");
    GrabHit hit;
    Err e;
    DWORD t0 = GetTickCount();
    if (grab_hit(&hit, e)) {
        str_catf(out, "  命中: id=%u 名称=%s 策略=%s 候选=%u 耗时=%lums\n", hit.id, hit.name,
                 hit.strategy, hit.cands, (unsigned long)(GetTickCount() - t0));
    } else {
        str_catf(out, "  未命中: %s\n", e);
    }

    GrabCand cd[MAX_CAND];
    int n = grab_last_cands(cd, 12);
    if (n > 0) {
        str_catf(out, "\n-- 候选（分数降序，前 %d）--\n", n);
        for (int i = 0; i < n; i++) {
            char nm[96] = "";
            if (g_namer) g_namer(cd[i].id, nm, sizeof(nm));
            str_catf(out, "  #%-10u score=%-4d type=0x%02X addr=0x%012llX  %s\n", cd[i].id, cd[i].score,
                     cd[i].type, (unsigned long long)cd[i].addr, nm);
        }
    }

    CloseHandle(hp);
}
