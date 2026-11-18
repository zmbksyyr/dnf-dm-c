"""Real PVF/UI and original-ELF database text boundaries, without changing user data."""
import ctypes as c
import ctypes.wintypes as w
import struct
import time

from . import core


def _cell(pid, control, row, column):
    """Read list text through a buffer owned by the destination GUI process."""
    k = core.k32
    k.OpenProcess.argtypes, k.OpenProcess.restype = [w.DWORD, w.BOOL, w.DWORD], w.HANDLE
    k.VirtualAllocEx.argtypes, k.VirtualAllocEx.restype = [w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD, w.DWORD], c.c_void_p
    k.WriteProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
    k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
    k.VirtualFreeEx.argtypes = [w.HANDLE, c.c_void_p, c.c_size_t, w.DWORD]
    k.CloseHandle.argtypes = [w.HANDLE]
    handle = k.OpenProcess(0x438, False, pid)
    if not handle:
        raise c.WinError()
    address = k.VirtualAllocEx(handle, None, 4096, 0x3000, 4)
    try:
        if not address:
            raise c.WinError()
        header = c.create_string_buffer(struct.pack('<IiiII4xQi', 0, row, column, 0, 0, address + 256, 1024))
        if not k.WriteProcessMemory(handle, address, header, c.sizeof(header), None):
            raise c.WinError()
        core.u.SendMessageW(control, 0x1073, row, address)  # LVM_GETITEMTEXTW
        result = c.create_unicode_buffer(1024)
        if not k.ReadProcessMemory(handle, address + 256, result, c.sizeof(result), None):
            raise c.WinError()
        return result.value
    finally:
        if address:
            k.VirtualFreeEx(handle, address, 0, 0x8000)
        k.CloseHandle(handle)


def _select_first(ctx):
    view = core.u.GetDlgItem(ctx.main, 113)
    core.click_at(view, 40, 28, .6)
    ctx.page(8)
    return core.wait_until(lambda: bool(core.txt(ctx.ctl(1801))), 15)


@core.case('encoding.pvf_ui', needs=('gui', 'db', 'pvf'), group='always',
           desc='真实 PVF 中文物品名经查询及发放队列显示，并逐页截图核对')
def pvf_ui(ctx):
    ctx.page(1)
    core.set_text(ctx.ctl(1101), '活')
    core.press(ctx.ctl(1104), .6)
    view = ctx.ctl(1102)
    ctx.ok('物品查询有结果', core.rows(view) > 0)
    coin_row = next(i for i in range(min(800, core.rows(view))) if _cell(ctx.proc.pid, view, i, 0) == '1')
    name = _cell(ctx.proc.pid, view, coin_row, 1)
    ctx.ok('编号 1 为 PVF 自身语言的复活币', name in ('復活幣', '复活币'), repr(name))
    ctx.shot('encoding-query')
    if ctx.ok('角色选中', _select_first(ctx)):
        ctx.page(2)
        core.set_text(ctx.ctl(1201), '1')
        core.press(ctx.ctl(1211), .5)  # Queue only; no inventory write or mail send.
        queue = ctx.ctl(1212)
        ctx.ok('发放队列有正确的中文物品名', any(_cell(ctx.proc.pid, queue, i, 0) == name for i in range(core.rows(queue))))
    for page in range(12):
        ctx.page(page)
        ctx.shot('encoding-page-%02d' % page)



@core.case('encoding.name_write', needs=('gui', 'db'), group='always',
           desc='中文改名按原 ELF latin1 取回 UTF-8 字节，GUI 搜索回填一致，原数据逐字节还原')
def name_write(ctx):
    cid = ctx.char_no()
    before = ctx.q1('SELECT HEX(charac_name) FROM taiwan_cain.charac_info WHERE charac_no=%s', (cid,))
    name = '编码验证甲'
    try:
        if not ctx.ok('角色选中', _select_first(ctx)):
            return
        edit = ctx.ctl(1801)
        core.set_text(edit, name)
        core.u.SendMessageW(edit, core.WM_KILLFOCUS, 0, 0)
        sql = 'SELECT HEX(CONVERT(charac_name USING latin1)) FROM taiwan_cain.charac_info WHERE charac_no=%s'
        expected = name.encode().hex().upper()
        ctx.ok('原 ELF 实际读取的名字字节等于 UTF-8 输入', core.wait_until(lambda: ctx.q1(sql, (cid,)) == expected, 15))
        ctx.check('界面回填中文名', core.txt(edit), name)
        core.set_text(core.u.GetDlgItem(ctx.main, 108), name)
        time.sleep(.5)
        ctx.ok('主列表按中文名搜索可找到角色', core.rows(core.u.GetDlgItem(ctx.main, 113)) > 0)
        ctx.shot('encoding-name')
    finally:
        ctx.cur.execute('UPDATE taiwan_cain.charac_info SET charac_name=UNHEX(%s) WHERE charac_no=%s', (before, cid))
        ctx.check('原名字字节完整恢复', ctx.q1('SELECT HEX(charac_name) FROM taiwan_cain.charac_info WHERE charac_no=%s', (cid,)), before)


@core.case('encoding.mail_write', needs=('gui', 'db'), group='always',
           desc='中文邮件按原 ELF 会话读取为正确 UTF-8，发送后删除本次测试邮件')
def mail_write(ctx):
    cid = ctx.char_no()
    before = ctx.q1('SELECT IFNULL(MAX(letter_id),0) FROM taiwan_cain_2nd.letter')
    text = '中文邮件编码验证'
    try:
        if not ctx.ok('角色选中', _select_first(ctx)):
            return
        ctx.page(3)
        core.set_text(ctx.ctl(1308), '')
        core.set_text(ctx.ctl(1310), text)
        core.press(ctx.ctl(1311), .5)
        sql = ('SELECT HEX(CONVERT(letter_text USING latin1)) FROM taiwan_cain_2nd.letter '
               'WHERE letter_id>%s AND charac_no=%s ORDER BY letter_id DESC LIMIT 1')
        ctx.ok('原 ELF 读取邮件正文等于 UTF-8 输入', core.wait_until(lambda: ctx.q1(sql, (before, cid)) == text.encode().hex().upper(), 15))
        ctx.shot('encoding-mail')
    finally:
        ctx.cur.execute('DELETE FROM taiwan_cain_2nd.letter WHERE letter_id>%s AND charac_no=%s', (before, cid))

