#!/usr/bin/env python3
"""Keep component -Werror builds compatible with the selected Apple compiler."""
import argparse
import json
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("core", type=Path)
p.add_argument("--compiler", default="")
a = p.parse_args()
device = Path(__file__).resolve().parents[1]
compiler = a.compiler or subprocess.check_output(
    ["xcrun", "--find", "clang++"], text=True).strip()
probe = subprocess.run(
    [compiler, "-x", "c++", "-fsyntax-only", "-Werror",
     "-Wunnecessary-virtual-specifier", "-"],
    input="class Probe { public: virtual ~Probe() = default; };\n",
    text=True, capture_output=True, check=False)
if probe.returncode and "unknown warning option '-Wunnecessary-virtual-specifier'" not in probe.stderr:
    raise SystemExit(probe.stderr)
supported = probe.returncode == 0
files = [a.core / "scripts/build-arm-driver.py",
         device / "scripts/build-sdhci-block.py",
         device / "scripts/build-a2-gop-display.py"]
changed = []
if not supported:
    for path in files:
        text = path.read_text()
        # Remove only this unsupported diagnostic suppression. Keep -Werror,
        # all real code warnings, and every original ABI check enabled.
        matches = [s for s in ("'-Wno-unnecessary-virtual-specifier',",
                               '"-Wno-unnecessary-virtual-specifier",')
                   if text.count(s) == 1]
        if len(matches) != 1:
            raise SystemExit(f"Unqualified compiler-option anchor: {path}")
        path.write_text(text.replace(matches[0], "", 1), newline="\n")
        changed.append(str(path))
record = dict(compiler=compiler,
              compiler_version=subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
              unnecessary_virtual_specifier_supported=supported,
              changed=changed, werror_preserved=True)
print(json.dumps(record, indent=2))
