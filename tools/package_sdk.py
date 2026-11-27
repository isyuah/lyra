import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
WINDOWS = os.name == "nt"


def platform_tag():
    if WINDOWS:
        return "windows-x64"
    release = Path("/etc/os-release").read_text(encoding="utf-8")
    if re.search(r"^ID=ubuntu$", release, re.MULTILINE) and 'VERSION_ID="22.04"' in release:
        return "ubuntu22.04-x86_64"
    return "linux-x86_64"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as artifact:
        for block in iter(lambda: artifact.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def pack(build, output, tag=None):
    binary = Path("bin/lyra.exe" if WINDOWS else "bin/lyra")
    version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?", version):
        raise ValueError(f"Unexpected toolchain version: {version}")
    actual = subprocess.check_output([build / binary, "--version"], text=True, timeout=15).strip()
    if actual != f"lyra {version}":
        raise ValueError(f"Expected build version 'lyra {version}', got {actual!r}; rebuild the SDK")
    name = f"lyra-{version}-{platform_tag()}"
    if tag is None:
        revision = subprocess.check_output(["git", "-C", ROOT, "rev-parse", "--short=12", "HEAD"], text=True).strip()
        name += f"-{revision}"
    elif tag != f"v{version}":
        raise ValueError(f"Tag {tag!r} does not match VERSION; expected v{version}")
    output.mkdir(parents=True, exist_ok=True)
    artifact = output / (name + (".zip" if WINDOWS else ".tar.gz"))
    if artifact.exists():
        raise FileExistsError(artifact)
    with tempfile.TemporaryDirectory(prefix=".sdk-stage-", dir=output) as staging:
        stage = Path(staging) / name
        subprocess.run(["cmake", "--install", build, "--prefix", stage], check=True, timeout=300)
        files = sorted(path.relative_to(stage) for path in stage.rglob("*") if path.is_file())
        installed = {Path(path).relative_to(stage) for path in (build / "install_manifest.txt").read_text(encoding="utf-8").splitlines()}
        if installed != set(files) or any((stage / path).is_symlink() for path in files):
            raise ValueError("Installed SDK differs from the CMake install manifest")
        std = Path("share/lyra/std")
        expected_std = {std / path.relative_to(ROOT / "std") for path in (ROOT / "std").rglob("*.ly")}
        if expected_std != {path for path in files if path.is_relative_to(std)}:
            raise ValueError("The SDK standard library differs from the checkout")
        if binary not in installed or not (stage / "share/lyra/licenses/third-party.md").is_file():
            raise ValueError("The SDK is missing its executable or third-party notices")
        manifest = Path("share/lyra/sdk-sha256.txt")
        (stage / manifest).write_text("".join(f"{sha256(stage / path)}  {path.as_posix()}\n" for path in files), encoding="utf-8", newline="\n")
        archive = Path(staging) / artifact.name
        if WINDOWS:
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=5) as bundle:
                for path in [*files, manifest]:
                    bundle.write(stage / path, (Path(name) / path).as_posix())
        else:
            with tarfile.open(archive, "w:gz", compresslevel=5) as bundle:
                for path in [*files, manifest]:
                    bundle.add(stage / path, arcname=(Path(name) / path).as_posix(), recursive=False)
        os.link(archive, artifact)
    print(artifact)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Package a built Lyra SDK from a fresh install prefix.")
    parser.add_argument("build", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/dist")
    parser.add_argument("--tag", help="Require this v-prefixed VERSION and use the release asset name.")
    options = parser.parse_args()
    pack(options.build.resolve(), options.output.resolve(), options.tag)
