"""PlatformIO pre-script: compile the shared Arduino IDF libs again when they
lack this project's custom_sdkconfig (#634, see framework_libs.py).

Runs before the platform's framework script. Removing the project's
sdkconfig.defaults makes that script's hash check fail, so it reinstalls the
framework and compiles the libs with this project's options.
"""
import os
import pathlib
import sys

Import("env")  # noqa: F821  (provided by PlatformIO)

sys.path.insert(0, str(pathlib.Path(env.subst("$PROJECT_DIR")).parent / "buildtools"))  # noqa: F821
from framework_libs import missing  # noqa: E402


def _check():
    if not env.GetProjectOption("custom_sdkconfig", ""):  # noqa: F821
        return
    mcu = env.BoardConfig().get("build.mcu", "esp32")  # noqa: F821
    libs_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")  # noqa: F821
    if not libs_dir:
        return  # first build: the framework is not installed yet and will be compiled
    lib_sdkconfig = os.path.join(libs_dir, mcu, "sdkconfig")
    if not os.path.isfile(lib_sdkconfig):
        return
    with open(lib_sdkconfig, encoding="utf-8") as f:
        wrong = missing(env.GetProjectOption("custom_sdkconfig"), f.read())  # noqa: F821
    if not wrong:
        return
    print("*** The shared Arduino IDF libs lack this project's custom_sdkconfig:")
    for line in wrong:
        print(f"***   {line}")
    stamp = os.path.join(env.subst("$PROJECT_DIR"), "sdkconfig.defaults")  # noqa: F821
    if os.path.isfile(stamp):
        os.remove(stamp)
    print("*** compiling them again for this project")


_check()
