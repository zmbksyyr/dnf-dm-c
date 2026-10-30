// grab：只读游戏内存取物品编号（进程定位 → 内置特征码 → 等悬停/扫堆诊断）。
// 只读，绝不 WriteProcessMemory / 注入 / 挂钩。
#pragma once
#include "util.h"

typedef struct {
    u32  id;             // 物品编号，0 表示未命中
    char name[96];       // 名称（PVF 为准）
    char type[32];       // 类型文字
    char strategy[16];   // builtin / scan
    char wnd[64];        // 命中的窗口类名（可能为空）
    u32  pid;
    int  x, y;           // 屏幕坐标
    u32  cands;          // 候选数
    u32  ms;             // 耗时
} GrabHit;

// 扫一次；返回 1 命中 0 未命中，失败时 err 有原因
int  grab_hit(GrabHit *out, Err err);

// 连取用的快速版：只走 builtin 特征码路径，失败不回退扫堆。
// 扫堆单次要读几百 MB，按 250ms 的节奏跑会一直占着 CPU；
// 而 builtin 命中后每次只读几个地址，微秒级，适合持续轮询。
int  grab_hit_fast(GrabHit *out, Err err);

// 手动「取码」按钮用：builtin 立即试一次，之后按 80ms 重试，最多等 ms 毫秒。
// 按按钮会把鼠标带离游戏，游戏随即清空悬停槽——不等的话采到的只会是扫堆
// 猜的一件（全堆最高分），不是悬停的那件。所以这里是"等悬停回来"，
// 用户把鼠标放回游戏物品上即自动采到。同样不回退扫堆。
int  grab_hit_wait(GrabHit *out, int ms, Err err);

// 诊断文本（--grab-probe 落 config/grab-probe.txt）
void grab_probe(Str *out);

// 上一次扫描的候选（按分数降序，供诊断查看）
typedef struct {
    u32  id;
    u64  addr;
    int  score;
    u8   type;
} GrabCand;
int grab_last_cands(GrabCand *out, int max);

// 由 main 传入：物品名解析（PVF 加载后才有）与进程名
typedef const char *(*GrabNameFn)(u32 id, char *buf, int cap);
void grab_bind(const char *proc, GrabNameFn namer);
