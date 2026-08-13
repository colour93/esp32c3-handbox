Import("env")

import os
import subprocess


def git_short_sha():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip() or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


generated_dir = os.path.join(env.subst("$BUILD_DIR"), "generated")
generated_header = os.path.join(generated_dir, "build_meta.h")
header_content = (
    '#pragma once\n\n#define HANDBOX_GIT_SHA "%s"\n' % git_short_sha()
)

os.makedirs(generated_dir, exist_ok=True)
current_content = None
try:
    with open(generated_header, encoding="utf-8") as header:
        current_content = header.read()
except OSError:
    pass

if current_content != header_content:
    with open(generated_header, "w", encoding="utf-8") as header:
        header.write(header_content)

env.Append(CPPPATH=[generated_dir])
