import platform


def windows_only(func):
    if platform.system() != 'Windows':
        return lambda *args, **kwargs: None
    return func


@windows_only
def hide_file(path):
    """
    Set the hidden attribute on a file or directory.

    From https://stackoverflow.com/questions/19622133/

    `path` must be text.
    """
    import ctypes

    __import__('ctypes.wintypes')
    SetFileAttributes = ctypes.windll.kernel32.SetFileAttributesW
    SetFileAttributes.argtypes = ctypes.wintypes.LPWSTR, ctypes.wintypes.DWORD
    SetFileAttributes.restype = ctypes.wintypes.BOOL

    FILE_ATTRIBUTE_HIDDEN = 0x02

    ret = SetFileAttributes(path, FILE_ATTRIBUTE_HIDDEN)
    if not ret:
        raise ctypes.WinError()
