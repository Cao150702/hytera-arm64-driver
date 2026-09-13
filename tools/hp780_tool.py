#!/usr/bin/env python3
"""HP780 写频线工具 — 直连读取 (macOS: libusb / Windows: WinUSB)

用法:
  python3 hp780_tool.py read -o radio.bin          # 读频,保存码片镜像 (95604B)
  python3 hp780_tool.py info                       # 读设备信息
  python3 hp780_tool.py write-param ID VALUE       # 写参数 (如 write-param 0x76 0x20)

协议文档 (2026-09-07):
  帧 = [7e][type][00][flag][dir 2B][seq 2B BE][len 2B BE][prefix 2B][HRCP 包]
  HRCP = [02][ax 2B LE][len_lo][b][data][cks][03]
  cks = (~sum(HRCP[1:-2]) + 0x33) & 0xFF
  写参数命令 (ax=0x01CE): data = [param_id 2B LE][len=2 2B][value 2B LE]
    prefix = 0x59A4 - (value>>8) + ((value>>8)<<8)  (高字节减, 低字节加)
"""
import sys, time, struct, math, json, os

try:
    import usb.core
except ImportError:
    print('需要 pyusb: pip install pyusb', file=sys.stderr)
    sys.exit(1)

# Windows 上 pyusb 默认找不到 libusb-1.0.dll(即使与 python.exe 同目录),
# 显式指定后端:优先工具目录下的 DLL
_backend = None
try:
    import usb.backend.libusb1 as _libusb1
    _local_dll = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'libusb-1.0.dll')
    if os.path.exists(_local_dll):
        _backend = _libusb1.get_backend(find_library=lambda x: _local_dll)
    if _backend is None:
        _backend = _libusb1.get_backend()
except Exception:
    _backend = None

VID, PID = 0x238B, 0x0A11
BLOCK_SIZE = 0x5DC  # 1500
TOTAL_BLOCKS = 64
TOTAL_BYTES = TOTAL_BLOCKS * BLOCK_SIZE

# 读块命令的 prefix 表 (由 CPS 读频会话实测整理, 与 addr=1500*k 一一对应)
READ_PREFIXES = [
    0x53a4, 0x4da9, 0x47af, 0x41b5, 0x3bbb, 0x36c1, 0x30c7, 0x2acd,
    0x24d2, 0x1ed8, 0x18de, 0x12e4, 0x0cea, 0x07f0, 0x01f6, 0xfbfa,
    0xf600, 0xf006, 0xea0c, 0xe412, 0xde18, 0xd81e, 0xd323, 0xcd29,
    0xc72f, 0xc135, 0xbb3b, 0xb541, 0xaf47, 0xa94c, 0xa452, 0x9e58,
    0x985e, 0x9264, 0x8c6a, 0x8670, 0x8075, 0x7a7b, 0x7481, 0x6f87,
    0x698d, 0x6393, 0x5d99, 0x579e, 0x51a5, 0x4bab, 0x45b1, 0x40b7,
    0x3abd, 0x34c3, 0x2ec8, 0x28ce, 0x22d4, 0x1cda, 0x16e0, 0x10e6,
    0x0bec, 0x05f1, 0xfff6, 0xf9fc, 0xf402, 0xee08, 0xe80e, 0xe413,
]

INIT_FRAMES = [
    '7e0000fe20100000000c60e5',
    '7e01000020100000001433d10205020100002a03',
    '7e0000fe20100000000c60e5',
    '7e01000020100000001431d30203020100002c03',
    '7e01000020100000002402f102c501110000000000000000000000000000000000005b03',
    '7e01000020100000001441c30201020100121c03',
    '7e01000020100000001c08f302d30109000000000000000000005503',
]
END_FRAME = '7e010000201000000014f40f02c6010100006a03'


class Radio:
    def __init__(self):
        self.buf = b''
        self.transport = None
        self._winusb = None
        if sys.platform == 'win32':
            try:
                from hp780_winusb import make_winusb_device
                self._winusb = make_winusb_device(VID, PID)
            except Exception as e:
                print(f'WinUSB 打开失败: {e}', file=sys.stderr)
        else:
            kw = dict(idVendor=VID, idProduct=PID)
            if _backend is not None:
                kw['backend'] = _backend
            self.dev = usb.core.find(**kw)
            if self.dev is None:
                raise RuntimeError('未找到写频线设备 (238b:0a11)。检查电台是否开机、线是否插好。')
            try:
                self.dev.set_configuration(1)
            except Exception:
                pass
            try:
                import usb.util as _usbutil
                _usbutil.claim_interface(self.dev, 2)
            except Exception:
                pass
        if self._winusb is None and not hasattr(self, 'dev'):
            raise RuntimeError('设备打开失败。检查电台是否开机、线是否插好。')

    def _write(self, data):
        if self._winusb is not None:
            return self._winusb.write(data, timeout_ms=2000)
        return self.dev.write(0x04, data, timeout=2000)

    def _read(self, size, timeout_ms):
        if self._winusb is not None:
            try:
                return self._winusb.read(size, timeout_ms=timeout_ms)
            except TimeoutError:
                return None
        try:
            return bytes(self.dev.read(0x84, size, timeout=timeout_ms))
        except Exception:
            return None

    def poll(self, t):
        r = self._read(256, int(t * 1000))
        if r:
            self.buf += r
            return True
        return False

    def get_frame(self):
        while True:
            i = self.buf.find(b'\x7e')
            if i < 0:
                return None
            self.buf = self.buf[i:]
            if len(self.buf) < 12:
                return None
            l = struct.unpack('>H', self.buf[8:10])[0]
            if 12 <= l <= 8192 and len(self.buf) >= l:
                fr = self.buf[:l]
                self.buf = self.buf[l:]
                return fr
            return None

    def read_response(self, timeout=6.0, minsize=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            fr = self.get_frame()
            if fr and fr[1] == 0x04 and len(fr) >= minsize:
                return fr
            self.poll(0.25)
        return self.get_frame()

    def drain(self, dur=0.8):
        t0 = time.time()
        while time.time() - t0 < dur:
            self.poll(0.1)

    def send(self, hexstr):
        self._write(bytes.fromhex(hexstr))

    @staticmethod
    def hrcp(ax, ln, b, data):
        p = bytes([0x02]) + struct.pack('<H', ax) + bytes([ln, b]) + data
        s = sum(p[1:]) & 0xFF
        return p + bytes([((~s) + 0x33) & 0xFF, 0x03])

    @staticmethod
    def frame(ftype, seq, prefix, h):
        return (bytes([0x7e, ftype, 0x00, 0x00, 0x20, 0x10]) +
                struct.pack('>H', seq) + struct.pack('>H', 12 + len(h)) +
                struct.pack('>H', prefix) + h)

    @staticmethod
    def calc_prefix(addr, cks):
        x = addr >> 8
        lo = (0xA4 + x) & 0xFF
        borrow = 1 if 0xA4 + x >= 0x100 else 0
        hi = (0x54 - math.ceil((addr + cks) / 256) + borrow) & 0xFF
        return (hi << 8) | lo

    def init_session(self):
        self.drain()
        for f in INIT_FRAMES:
            self.send(f)
            self.read_response(4.0)
        # 0xD3 应答迟到,补读
        self.read_response(4.0)

    def read_block(self, k):
        addr = k * BLOCK_SIZE
        size = 0x450 if k == TOTAL_BLOCKS - 1 else 0x5DC  # 末块 1104 字节
        data = bytes.fromhex('000000010000') + struct.pack('<I', addr) + struct.pack('<H', size)
        h = self.hrcp(0x01C7, 12, 0x00, data)
        p = READ_PREFIXES[k]
        self._write(self.frame(0x01, 0, p, h))
        r = self.read_response(8.0, minsize=100)
        if r is None or len(r) < size + 32:
            return None
        got = struct.unpack('<I', r[24:28])[0]
        if got != addr:
            print(f'警告: 块地址不符 请求={addr:#x} 收到={got:#x}', file=sys.stderr)
        return r[30:-2][:size]

    def read_all(self, progress=True):
        self.init_session()
        image = bytearray()
        for k in range(TOTAL_BLOCKS):
            size = 0x450 if k == TOTAL_BLOCKS - 1 else BLOCK_SIZE
            blk = self.read_block(k)
            if blk is None:
                print(f'块 {k} (addr {k*BLOCK_SIZE:#x}) 读取失败', file=sys.stderr)
                return None
            if len(blk) != size:
                print(f'块 {k} 长度异常: {len(blk)} != {size}', file=sys.stderr)
                return None
            image += blk
            if progress:
                print(f'  块 {k+1}/{TOTAL_BLOCKS} (addr {k*BLOCK_SIZE:#08x}, {size}B) OK', flush=True)
        self.send(END_FRAME)
        self.read_response(3.0)
        return bytes(image)

    def write_param(self, param_id, value):
        self.init_session()
        data = struct.pack('<H', param_id) + struct.pack('<H', 2) + struct.pack('<H', value)
        h = self.hrcp(0x01CE, 6, 0x00, data)
        # prefix: 高字节 = 0x59 - (值>>8), 低字节 = 0xA4 + (值>>8)
        p = ((0x59 - (value >> 8)) << 8) | ((0xA4 + (value >> 8)) & 0xFF)
        self._write(self.frame(0x01, 0, p, h))
        r = self.read_response(6.0)
        if r is None:
            return False
        ok = len(r) >= 20 and r[12:14] == b'\x02\xce'
        print(f'写参数 0x{param_id:04x} = 0x{value:04x}: {"成功" if ok else "未确认"} (应答 {len(r)}B)')
        return ok

    def info(self):
        self.drain()
        self.send(INIT_FRAMES[0])
        self.read_response(2.0)
        self.send(INIT_FRAMES[1])
        r = self.read_response(3.0)
        if r and len(r) > 30:
            payload = r[17:-2]
            # 型号字符串区域
            print(f'设备应答 {len(r)}B')
            printable = ''.join(chr(c) if 32 <= c < 127 else '.' for c in payload[:60])
            print(f'数据预览: {printable}')
            print(f'帧 hex: {r[:40].hex(" ")}...')
        else:
            print('无应答')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    try:
        r = Radio()
    except RuntimeError as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)
    if cmd == 'info':
        r.info()
    elif cmd == 'read':
        out = 'radio.bin'
        if '-o' in sys.argv:
            out = sys.argv[sys.argv.index('-o') + 1]
        print(f'开始读频 (共 {TOTAL_BYTES} 字节)...', flush=True)
        img = r.read_all()
        if img is not None:
            with open(out, 'wb') as f:
                f.write(img)
            print(f'完成: {len(img)} 字节 -> {out}')
        else:
            sys.exit(1)
    elif cmd == 'write-param':
        args = sys.argv[2:]
        if len(args) < 2:
            print('用法: hp780_tool.py write-param ID VALUE', file=sys.stderr)
            sys.exit(1)
        pid = int(args[0], 0)
        val = int(args[1], 0)
        if not r.write_param(pid, val):
            sys.exit(1)
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
