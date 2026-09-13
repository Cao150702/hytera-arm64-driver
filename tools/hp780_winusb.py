"""WinUSB 传输层 — Windows 上通过 ctypes 直调 winusb.dll/setupapi.dll
处理接口号=2 的非标准 WinUSB 设备(libusb winusb 后端不认这种设备)。
"""
import ctypes
from ctypes import wintypes

if hasattr(ctypes, 'WinDLL'):
    _setupapi = ctypes.WinDLL('setupapi', use_last_error=True)
    _winusb = ctypes.WinDLL('winusb', use_last_error=True)
    _kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
else:
    _setupapi = _winusb = _kernel32 = None

GUID_DEVINTERFACE_USB_DEVICE = '{A5DCBF10-6530-11D2-901F-00C04FB951ED}'
DIGCF_PRESENT = 0x00000002
DIGCF_DEVICEINTERFACE = 0x00000010
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD),
                ('InterfaceClassGuid', ctypes.c_byte * 16),
                ('Flags', wintypes.DWORD),
                ('Reserved', ctypes.c_void_p)]


def _declare():
    if _winusb is None:
        return
    _winusb.WinUsb_Initialize.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
    _winusb.WinUsb_Initialize.restype = wintypes.BOOL
    _winusb.WinUsb_GetAssociatedInterface.argtypes = [ctypes.c_void_p, ctypes.c_ubyte,
                                                       ctypes.POINTER(ctypes.c_void_p)]
    _winusb.WinUsb_GetAssociatedInterface.restype = wintypes.BOOL
    _winusb.WinUsb_WritePipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p,
                                         wintypes.ULONG, ctypes.POINTER(wintypes.ULONG),
                                         ctypes.POINTER(wintypes.ULONG)]
    _winusb.WinUsb_WritePipe.restype = wintypes.BOOL
    _winusb.WinUsb_ReadPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p,
                                        wintypes.ULONG, ctypes.POINTER(wintypes.ULONG),
                                        ctypes.POINTER(wintypes.ULONG)]
    _winusb.WinUsb_ReadPipe.restype = wintypes.BOOL
    _winusb.WinUsb_Free.argtypes = [ctypes.c_void_p]
    _winusb.WinUsb_Free.restype = wintypes.BOOL
    _kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, wintypes.DWORD, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.HANDLE]
    _kernel32.CreateFileW.restype = wintypes.HANDLE
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel32.CloseHandle.restype = wintypes.BOOL


_declare()


class WinUsbError(Exception):
    pass


class WinUsbDevice:
    """Windows WinUSB 设备:自动找接口号 2 的关联接口。"""

    def __init__(self, vid, pid):
        self.vid, self.pid = vid, pid
        self.winusb_handle = None
        self.interface2_handle = None
        self._setup()

    def _find_paths_via_registry(self):
        """Fallback: 注册表 DeviceClasses 下找该设备的所有接口路径。"""
        import winreg
        want = f'vid_{self.vid:04x}&pid_{self.pid:04x}'.lower()
        paths = []
        base = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                              r'SYSTEM\CurrentControlSet\Control\DeviceClasses')
        try:
            n = 0
            while True:
                try:
                    guid_name = winreg.EnumKey(base, n)
                except OSError:
                    break
                n += 1
                try:
                    gk = winreg.OpenKey(base, guid_name)
                except OSError:
                    continue
                try:
                    m = 0
                    while True:
                        try:
                            sub = winreg.EnumKey(gk, m)
                        except OSError:
                            break
                        m += 1
                        if want in sub.lower():
                            paths.append(sub.replace('##?#', '\\\\?\\'))
                finally:
                    winreg.CloseKey(gk)
        finally:
            winreg.CloseKey(base)
        return paths

    def _setup(self):
        if _setupapi is None:
            raise WinUsbError('非 Windows 平台')
        paths = self._find_paths_via_registry()
        if not paths:
            raise WinUsbError(f'未找到 USB 设备接口 vid_{self.vid:04x}&pid_{self.pid:04x}')
        # 逐个尝试:需要 WinUsb_Initialize 成功且能查到 3 个管道
        last_err = None
        for path in paths:
            h = _kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
            if h == INVALID_HANDLE_VALUE:
                last_err = f'CreateFile {ctypes.get_last_error()}'
                continue
            wh = ctypes.c_void_p()
            if not _winusb.WinUsb_Initialize(h, ctypes.byref(wh)):
                last_err = f'WinUsb_Initialize {ctypes.get_last_error()}'
                _kernel32.CloseHandle(h)
                continue
            # 验证管道存在
            class PIPE_INFO(ctypes.Structure):
                _fields_ = [('PipeType', wintypes.ULONG), ('PipeId', ctypes.c_ubyte),
                            ('MaximumPacketSize', ctypes.c_ubyte), ('Interval', ctypes.c_ubyte)]
            pi = PIPE_INFO()
            if not _winusb.WinUsb_QueryPipe(wh, 0, ctypes.byref(pi)):
                last_err = f'QueryPipe {ctypes.get_last_error()}'
                _winusb.WinUsb_Free(wh)
                _kernel32.CloseHandle(h)
                continue
            self.file_handle = h
            self.winusb_handle = wh
            self.interface2_handle = wh
            # 读管道设传输超时,避免无数据时挂起请求堆积
            PIPE_TRANSFER_TIMEOUT = 2
            tmo = wintypes.ULONG(300)
            _winusb.WinUsb_SetPipePolicy(wh, 0x81, PIPE_TRANSFER_TIMEOUT,
                                         ctypes.sizeof(tmo), ctypes.byref(tmo))
            return
        raise WinUsbError(f'无可用 WinUSB 接口: {last_err}')

    def write(self, data, timeout_ms=2000):
        written = wintypes.ULONG(0)
        tmo = wintypes.ULONG(timeout_ms)
        buf = (ctypes.c_byte * len(data)).from_buffer_copy(data)
        ok = _winusb.WinUsb_WritePipe(self.interface2_handle, 0x01, buf,
                                      len(data), ctypes.byref(written), ctypes.byref(tmo))
        if not ok:
            raise WinUsbError(f'WritePipe err={ctypes.get_last_error()}')
        return written.value

    def read(self, size, timeout_ms=3000):
        buf = (ctypes.c_byte * size)()
        nread = wintypes.ULONG(0)
        tmo = wintypes.ULONG(timeout_ms)
        ok = _winusb.WinUsb_ReadPipe(self.interface2_handle, 0x81, buf,
                                     size, ctypes.byref(nread), ctypes.byref(tmo))
        if not ok:
            err = ctypes.get_last_error()
            if err in (121, 997):  # ERROR_SEM_TIMEOUT / ERROR_IO_PENDING(设备无数据)
                raise TimeoutError()
            raise WinUsbError(f'ReadPipe err={err}')
        return bytes(buf[:nread.value])

    def close(self):
        if self.interface2_handle:
            _winusb.WinUsb_Free(self.interface2_handle)
            self.interface2_handle = None
        if self.winusb_handle:
            _winusb.WinUsb_Free(self.winusb_handle)
            self.winusb_handle = None
        if getattr(self, 'file_handle', None):
            _kernel32.CloseHandle(self.file_handle)
            self.file_handle = None


def make_winusb_device(vid, pid):
    return WinUsbDevice(vid, pid)
