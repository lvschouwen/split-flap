# flashing/flasher/flasher.spec
# Build (Windows, from flashing/): pyinstaller flasher/flasher.spec
#
# NOTE: PyInstaller resolves relative Analysis() paths against the .spec
# file's own directory (flashing/flasher/), NOT the CWD it was invoked
# from — so these are written relative to flasher/, not flashing/.
a = Analysis(
    ["__main__.py"],
    pathex=[".."],
    datas=[("assets", "assets")],
    hiddenimports=["esptool", "serial", "serial.tools.list_ports"],
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, a.binaries, a.datas,
    name="split-flap-flasher",
    console=True,
    upx=False,
)
