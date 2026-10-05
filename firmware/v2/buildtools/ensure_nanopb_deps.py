"""PlatformIO pre-script: make sure the nanopb generator can run.

nanopb generates the wall link code (firmware/v2/link/wall_link.proto) during
the build with the build's own Python, and needs the protobuf packages there.
Its PlatformIO integration installs them with pip, but the pioarduino
environment the S3 builds with ships without pip, so that step fails. This
script installs them first, with whichever installer the environment has.
"""
import os
import shutil
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO)

PACKAGES = ["protobuf", "grpcio-tools"]


def _ready(python):
    # Ask the Python the generator will run in, not the one running this script:
    # under pioarduino they are different environments.
    probe = [python, "-c", "import google.protobuf, grpc_tools"]
    return subprocess.run(probe, capture_output=True).returncode == 0


def _install(python):
    attempts = [[python, "-m", "pip", "install", *PACKAGES]]
    uv = shutil.which("uv")
    if uv:
        attempts.append([uv, "pip", "install", "--python", python, *PACKAGES])
    for command in attempts:
        if subprocess.run(command, capture_output=True).returncode == 0:
            return True
    return False


def _pythons():
    # The generator runs with $PYTHONEXE as it stands when the library is
    # processed. pioarduino re-points that at its own environment
    # (<core dir>/penv) after this script has run, so cover both.
    found = [env.subst("$PYTHONEXE")]  # noqa: F821
    for name in ("bin/python", "Scripts/python.exe"):
        candidate = os.path.join(env.subst("$PROJECT_CORE_DIR"), "penv", name)  # noqa: F821
        if os.path.isfile(candidate) and candidate not in found:
            found.append(candidate)
    return found


for python in _pythons():
    if _ready(python):
        continue
    print(f"[nanopb deps] installing {', '.join(PACKAGES)} into {python}")
    if not (_install(python) and _ready(python)):
        raise SystemExit(
            "[nanopb deps] could not install protobuf and grpcio-tools into "
            f"{python}: it has neither pip nor a `uv` on PATH. Install one of them, "
            f"or run: uv pip install --python {python} protobuf grpcio-tools")
