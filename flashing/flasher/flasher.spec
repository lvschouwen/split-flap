# flashing/flasher/flasher.spec
# Build (Windows, from flashing/): pyinstaller flasher/flasher.spec
a = Analysis(
    ["flasher/__main__.py"],
    pathex=["."],
    datas=[("flasher/assets", "assets")],
    hiddenimports=["esptool", "serial", "serial.tools.list_ports"],
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, a.binaries, a.datas,
    name="split-flap-flasher",
    console=True,
    upx=False,
)
