import argparse
import hashlib
import os
import subprocess
from pathlib import Path

def cache_dir_for(path: Path) -> Path:
    st = path.stat()
    identity = f"{path.resolve()}|{st.st_size}|{st.st_mtime_ns}"
    digest = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:24]
    base = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "foo_stem_separator" / "cache"
    return base / digest

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("track")
    args = ap.parse_args()

    src = Path(args.track).resolve()
    out = cache_dir_for(src)
    out.mkdir(parents=True, exist_ok=True)

    cmd = [
        "py", "-m", "demucs",
        "--two-stems=vocals",
        "-n", "htdemucs",
        "-o", str(out),
        str(src),
    ]
    print("Running:", subprocess.list2cmdline(cmd))
    subprocess.check_call(cmd)

    track_dir = out / "htdemucs" / src.stem
    print("\nVocals:      ", track_dir / "vocals.wav")
    print("Instrumental:", track_dir / "no_vocals.wav")

if __name__ == "__main__":
    main()
