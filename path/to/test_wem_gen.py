#!/usr/bin/env python3
"""
Standalone wem+ffxanim generator test.

Reproduces the Mantella _generate_voice_assets pipeline on a single wav, with NO
Mantella dependencies (no Starfield class, no Sentence, no ConfigLoader). Purpose:
prove the Windows toolchain (WwiseConsole -> wem, LipGenerator -> animset,
ffxc -> ffxanim) still produces valid Starfield assets from a UNITY-sourced
Cartesia wav.

Usage:
    python test_wem_gen.py C:\\Temp\\unity_test.wav

Reports PASS/FAIL for each of the three tool stages and prints output file sizes.
Deploys nothing to game folders — everything lands in a scratch dir so this test
can't disturb your live setup.
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path

# ---- Tool paths (copied verbatim from the Mantella Starfield.py) ------------
WWISE_CONSOLE = r"C:\Audiokinetic\Wwise_2021.1.14.8108\Authoring\x64\Release\bin\WwiseConsole.exe"
WWISE_PROJ    = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\wwise\Starfield\Starfield.wproj"
LIPGENERATOR  = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\LipGenerator\LipGenerator.exe"
FFXC          = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\FaceFX\ffxc.exe"
FACEFX_FEMALE = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\FaceFX\StarfieldHumanFemale.facefx"
FACEFX_MALE   = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\FaceFX\StarfieldHumanMale.facefx"
FACEFX_ANALYSIS = r"E:\SteamLibrary\steamapps\common\Starfield\Tools\FaceFX\GenesisDefault.facefx"

# Scratch workspace — isolated, nothing here touches the game/MO2.
SCRATCH = r"C:\Temp\unity_wemtest"

# Use the female rig for the test (arbitrary; the point is tool validity).
FACEFX_ACTOR      = FACEFX_FEMALE
FACEFX_ACTOR_NAME = "StarfieldHumanFemale"
REGISTERED_FOLDER = "NPCFSarahMorgan"

FILENAME = "00666A79"  # a real slot name from the WEM_SLOTS pool


def banner(msg):
    print("\n" + "=" * 60)
    print(msg)
    print("=" * 60)


def check(label, ok, detail=""):
    status = "PASS" if ok else "FAIL"
    print(f"  [{status}] {label}" + (f"  ({detail})" if detail else ""))
    return ok


def gen_wsources(wav_dir, filename):
    """Write the .wsources XML WwiseConsole needs for external-source conversion."""
    xml = f'''<?xml version="1.0" encoding="UTF-8"?>
<ExternalSourcesList SchemaVersion="1" Root="{wav_dir}">
    <Source Path="{filename}.wav" Conversion="Vorbis Quality High" Destination="{filename}"/>
</ExternalSourcesList>'''
    path = os.path.join(SCRATCH, "test_sources.wsources")
    with open(path, "w", encoding="utf-8") as f:
        f.write(xml)
    return path


def main():
    if len(sys.argv) < 2:
        print("usage: python test_wem_gen.py <path-to-wav>")
        sys.exit(1)

    src_wav = sys.argv[1]
    if not os.path.exists(src_wav):
        print(f"input wav not found: {src_wav}")
        sys.exit(1)

    banner("SETUP")
    os.makedirs(SCRATCH, exist_ok=True)
    work_wav = os.path.join(SCRATCH, f"{FILENAME}.wav")
    shutil.copyfile(src_wav, work_wav)
    print(f"  input wav : {src_wav} ({os.path.getsize(src_wav)} bytes)")
    print(f"  scratch   : {SCRATCH}")
    print(f"  work wav  : {work_wav}")

    all_ok = True

    # ---- Stage 1: WwiseConsole -> wem --------------------------------------
    banner("STAGE 1: WwiseConsole  (wav -> wem)")
    wsources = gen_wsources(SCRATCH, FILENAME)
    # Clean any stale output
    win_out = os.path.join(SCRATCH, "Windows", f"{FILENAME}.wem")
    if os.path.exists(win_out):
        os.remove(win_out)

    try:
        r = subprocess.run(
            [
                WWISE_CONSOLE, "convert-external-source", WWISE_PROJ,
                "--source-file", wsources,
                "--output", SCRATCH,
                "--platform", "Windows",
            ],
            capture_output=True, timeout=120,
        )
        if r.returncode != 0:
            print("  stderr:", r.stderr.decode(errors="replace")[:500])
    except subprocess.TimeoutExpired:
        print("  WwiseConsole TIMED OUT")
        r = None

    # Locate the produced wem (path can vary; search scratch)
    wem_found = list(Path(SCRATCH).rglob(f"{FILENAME}.wem"))
    wem_ok = bool(wem_found) and wem_found[0].stat().st_size > 0
    detail = f"{wem_found[0]} ({wem_found[0].stat().st_size} bytes)" if wem_found else "no wem produced"
    all_ok &= check("wem generated", wem_ok, detail)

    # ---- Stage 2: LipGenerator -> animset ----------------------------------
    banner("STAGE 2: LipGenerator  (wav -> animset)")
    animation_group = f"{REGISTERED_FOLDER}_{FILENAME}"
    animset_path = os.path.join(SCRATCH, f"{animation_group}.animset")
    if os.path.exists(animset_path):
        os.remove(animset_path)

    try:
        r = subprocess.run(
            [
                LIPGENERATOR, work_wav, "hello there, can you hear me",
                FACEFX_ACTOR, FACEFX_ANALYSIS,
                f"-OutputFileName:{animset_path}",
                f"-AnimationGroupName:{animation_group}",
            ],
            capture_output=True, timeout=120,
        )
        if r.returncode != 0:
            print("  stderr:", r.stderr.decode(errors="replace")[:500])
    except subprocess.TimeoutExpired:
        print("  LipGenerator TIMED OUT")

    animset_ok = os.path.exists(animset_path) and os.path.getsize(animset_path) > 0
    detail = f"{os.path.getsize(animset_path)} bytes" if os.path.exists(animset_path) else "no animset produced"
    all_ok &= check("animset generated", animset_ok, detail)

    # ---- Stage 3: ffxc -> ffxanim ------------------------------------------
    banner("STAGE 3: ffxc  (animset -> ffxanim)")
    per_line_proj = os.path.join(SCRATCH, f"{animation_group}.ffxcproj")
    proj_yaml = (
        "version: 10300\n"
        "items:\n"
        f" - facefx_file: {FACEFX_ACTOR}\n"
        "   mount_files:\n"
        f"    - {animset_path}\n"
    )
    with open(per_line_proj, "w", encoding="utf-8") as f:
        f.write(proj_yaml)

    if animset_ok:
        try:
            r = subprocess.run(
                [FFXC, f"-o={SCRATCH}\\", "-p=x86", "--rebuild", per_line_proj],
                capture_output=True, timeout=120,
            )
            if r.returncode != 0:
                print("  stderr:", r.stderr.decode(errors="replace")[:500])
        except subprocess.TimeoutExpired:
            print("  ffxc TIMED OUT")
    else:
        print("  SKIPPED (no animset from stage 2)")

    ffxanim_found = list(Path(SCRATCH).rglob(f"{FILENAME}.ffxanim"))
    ffxanim_ok = bool(ffxanim_found) and ffxanim_found[0].stat().st_size > 0
    detail = f"{ffxanim_found[0]} ({ffxanim_found[0].stat().st_size} bytes)" if ffxanim_found else "no ffxanim produced"
    all_ok &= check("ffxanim generated", ffxanim_ok, detail)

    # ---- Summary -----------------------------------------------------------
    banner("RESULT")
    if all_ok:
        print("  ALL STAGES PASSED — the toolchain produces valid wem + ffxanim")
        print("  from a UNITY-sourced Cartesia wav. Pipeline port is viable.")
    else:
        print("  ONE OR MORE STAGES FAILED — see the FAIL lines above.")
        print("  (Check stderr output and the input wav format.)")
    print(f"\n  scratch dir kept for inspection: {SCRATCH}")


if __name__ == "__main__":
    main()
