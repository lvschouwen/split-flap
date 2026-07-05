"""Terminal prompts + colors. Windows: enable VT processing, no deps."""
import os
import sys

_GREEN, _YELLOW, _RED, _BOLD, _RESET = "\033[32m", "\033[33m", "\033[31m", "\033[1m", "\033[0m"
_ansi = False


def enable_ansi() -> None:
    global _ansi
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if k32.GetConsoleMode(h, ctypes.byref(mode)):
            _ansi = bool(k32.SetConsoleMode(h, mode.value | 0x0004))
    else:
        _ansi = sys.stdout.isatty()


def _c(color: str, text: str) -> str:
    return f"{color}{text}{_RESET}" if _ansi else text


def heading(text: str) -> None:
    print(f"\n{_c(_BOLD, '=== ' + text + ' ===')}")


def ok(text: str) -> None:
    print(_c(_GREEN, "[ok] " + text))


def warn(text: str) -> None:
    print(_c(_YELLOW, "[warn] " + text))


def fail(text: str) -> None:
    print(_c(_RED, "[fail] " + text))


def say(text: str) -> None:
    print(text)


def ask(prompt: str) -> str:
    return input(f"{prompt} ").strip()


def ask_int(prompt: str, lo: int, hi: int) -> int:
    while True:
        raw = ask(f"{prompt} ({lo}-{hi}):")
        try:
            v = int(raw)
            if lo <= v <= hi:
                return v
        except ValueError:
            pass
        print(f"  please enter a number between {lo} and {hi}")


def ask_yn(prompt: str, default: bool = False) -> bool:
    suffix = "[Y/n]" if default else "[y/N]"
    raw = ask(f"{prompt} {suffix}").lower()
    if raw == "":
        return default
    return raw in ("y", "yes")


def pause(prompt: str = "Press Enter to continue...") -> None:
    input(prompt)
