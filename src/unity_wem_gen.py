#!/usr/bin/env python3
import os
import time
import shutil
import struct
import urllib.request
import subprocess
from pathlib import Path

import configparser
import json
import re
import sys
import threading

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
# All machine-specific paths come from unity_wem_gen.ini (next to this script,
# or pass a path as the first argument). Only StarfieldDir and BridgeDir are
# normally needed; every Creation Kit tool path is derived from StarfieldDir.
# See unity_wem_gen.example.ini for a commented template.

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SCRIPT_DIR, "unity_wem_gen.ini")

_cfg = configparser.ConfigParser(interpolation=None)
_cfg.optionxform = str
if os.path.exists(CONFIG_PATH):
    _cfg.read(CONFIG_PATH, encoding="utf-8")


def _get(key, default=""):
    v = _cfg.get("Paths", key, fallback="").strip().strip('"')
    return v if v else default


STARFIELD_DIR = _get("StarfieldDir", r"C:\Program Files (x86)\Steam\steamapps\common\Starfield")
_TOOLS = os.path.join(STARFIELD_DIR, "Tools")

WWISE_CONSOLE = _get("WwiseConsole", r"C:\Audiokinetic\Wwise_2021.1.14.8108\Authoring\x64\Release\bin\WwiseConsole.exe")
WWISE_PROJ = _get("WwiseProject", os.path.join(_TOOLS, "wwise", "Starfield", "Starfield.wproj"))

# Lipsync tools - same layout confirmed working in Mantella's starfield.py
LIPGENERATOR_EXE = _get("LipGenerator", os.path.join(_TOOLS, "LipGenerator", "LipGenerator.exe"))
FFXC_EXE = _get("Ffxc", os.path.join(_TOOLS, "FaceFX", "ffxc.exe"))
FACEFX_FEMALE = os.path.join(_TOOLS, "FaceFX", "StarfieldHumanFemale.facefx")
FACEFX_MALE = os.path.join(_TOOLS, "FaceFX", "StarfieldHumanMale.facefx")
FACEFX_ANALYSIS = os.path.join(_TOOLS, "FaceFX", "GenesisDefault.facefx")
INTERMEDIATE_FEMALE = os.path.join(STARFIELD_DIR, "Intermediate", "Slice_01")
INTERMEDIATE_MALE = os.path.join(STARFIELD_DIR, "Intermediate", "Slice_00")

# Registered folders - must match what's in the ffxcproj animset (same
# constraint as Mantella's starfield.py: LipGenerator/ffxc only recognize
# these two names as valid animation-group homes).
REGISTERED_FEMALE_FOLDER = "NPCFSarahMorgan"
REGISTERED_MALE_FOLDER = "NPCMBarrett"

MALE_VOICE_TYPES = {
    "NPCMBarrett", "NPCMWalterStroud", "NPCMSamCoe", "NPCMVladimirSall",
    "NPCMMatteoKhatri", "NPCMHeller", "GenericMaleEvenToned",
    "NPCMGrandInquisitor", "NPCMLukeSkywalker",
}

# The real Data voice folder. ffxc must compile into here (see CLAUDE.md §5).
DATA_VOICE_BASE = os.path.join(STARFIELD_DIR, "Data", "Sound", "Voice", "MantellaStarfield.esp")
# Optional second copy target - the MO2 mod folder, if the mod is installed
# through MO2. Empty = only write to DATA_VOICE_BASE.
MO2_VOICE_BASE = _get("MO2VoiceDir", "")
VOICE_BASES = [b for b in (MO2_VOICE_BASE, DATA_VOICE_BASE) if b]

# Folder shared with the plugin + Papyrus. Must be the REAL on-disk folder:
#   no MO2:   <StarfieldDir>\Data\SFSE\MantellaStarfield
#   with MO2: <MO2 instance>\overwrite\SFSE\MantellaStarfield
BRIDGE_DIR = _get("BridgeDir", os.path.join(STARFIELD_DIR, "Data", "SFSE", "MantellaStarfield"))
QUEUE_INI = os.path.join(BRIDGE_DIR, "MantellaQueue.ini")
WORK_FILE = os.path.join(BRIDGE_DIR, "UnityWork.ini")
HEARTBEAT_INI = os.path.join(BRIDGE_DIR, "UnityWatcher.ini")
SCRATCH = _get("ScratchDir", os.path.join(os.environ.get("TEMP", r"C:\Temp"), "unity_wemgen"))

DISCOVERY_URL = "http://127.0.0.1:7135/discover?game=starfield"
# Used if the Dwemer launcher's discovery service doesn't answer. With WSL2's
# default localhost forwarding, 127.0.0.1 reaches the server inside WSL.
FALLBACK_HOSTPORT = _get("ServerHostPort", "127.0.0.1:8087")

MAX_LINE_ATTEMPTS = 3

WEM_SLOTS = [f"{i:08X}" for i in range(0x00666A79, 0x00666A79 + 200)]

_slot_idx = 0


def log(msg):
    print(f"[unity-wemgen] {msg}", flush=True)


def _filter_ffxc_noise(text):
    """Strips FaceFX's own bone-pose/unused-track warnings from ffxc's
    stdout/stderr before logging it. These two warning types alone can
    produce 100+ lines on essentially every single compile - the shared
    rig has far more bones/tracks than any one short animation actually
    uses - and they've never once indicated a real problem, just drowned
    out anything that would (genuine errors, the build summary). Keeps
    every other line untouched."""
    if not text:
        return text
    kept = [
        line for line in text.splitlines()
        if "[-Wempty-bone-pose]" not in line and "[-Wunused-track]" not in line
    ]
    return "\n".join(kept)


def next_slot():
    global _slot_idx
    slot = WEM_SLOTS[_slot_idx % len(WEM_SLOTS)]
    _slot_idx += 1
    return slot


_server_hostport = None


def discover_server():
    """Ask the Dwemer launcher's discovery service once, cache the answer.
    Short timeout so a missing launcher costs ~1s once, not every turn."""
    global _server_hostport
    if _server_hostport:
        return _server_hostport
    try:
        with urllib.request.urlopen(DISCOVERY_URL, timeout=1.5) as r:
            body = r.read().decode("utf-8", errors="replace").strip()
        hostport = None
        try:
            j = json.loads(body)
            if isinstance(j, dict):
                host = j.get("host") or j.get("ip") or j.get("address")
                port = j.get("port")
                if host and port:
                    hostport = f"{host}:{port}"
                elif isinstance(j.get("url"), str):
                    body = j["url"]
        except ValueError:
            pass
        if not hostport:
            m = re.search(r"([A-Za-z0-9_.\-]+):(\d{2,5})", body)
            if m:
                hostport = f"{m.group(1)}:{m.group(2)}"
        if hostport:
            _server_hostport = hostport
            log(f"server discovered: {hostport}")
            return hostport
        log(f"discovery returned unexpected '{body[:80]}', using {FALLBACK_HOSTPORT}")
    except Exception as e:
        log(f"discovery unavailable ({e.__class__.__name__}), using {FALLBACK_HOSTPORT}")
    _server_hostport = FALLBACK_HOSTPORT
    return FALLBACK_HOSTPORT


# ---------------------------------------------------------------------------
# Heartbeat - lets the plugin tell the player in game when this watcher isn't
# running or is misconfigured (see UpdateStatus in main.cpp). Written from its
# own thread so a long WwiseConsole/ffxc run never makes us look dead.
# ---------------------------------------------------------------------------
_startup_problems = ""      # e.g. "missing LipGenerator, ffxc"
_fetch_problem = ""         # set while audio downloads from the server are failing


def _write_heartbeat():
    import ctypes
    W = ctypes.windll.kernel32.WritePrivateProfileStringA
    path = HEARTBEAT_INI.encode("mbcs", "replace")
    problems = "; ".join(p for p in (_startup_problems, _fetch_problem) if p)
    W(b"watcher", b"heartbeat", str(int(time.time())).encode(), path)
    W(b"watcher", b"problems", problems.encode("ascii", "replace"), path)


def _heartbeat_loop():
    while True:
        try:
            _write_heartbeat()
        except Exception as e:
            log(f"heartbeat write failed: {e}")
        time.sleep(2.0)


def fetch_wav(hostport, hash_, dest):
    global _fetch_problem
    url = f"http://{hostport}/UNITYServer/soundcache/{hash_}.wav"
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
            data = r.read()
        if len(data) < 128:
            log(f"  fetched wav suspiciously small ({len(data)}b) for {hash_}")
            return False
        with open(dest, "wb") as f:
            f.write(data)
        _fetch_problem = ""
        return True
    except Exception as e:
        log(f"  fetch failed for {hash_}: {e}")
        _fetch_problem = f"can't download audio from {hostport}"
        return False


def make_wsources(wav_dir, slot):
    xml = f'''<?xml version="1.0" encoding="UTF-8"?>
<ExternalSourcesList SchemaVersion="1" Root="{wav_dir}">
    <Source Path="{slot}.wav" Conversion="Vorbis Quality High" Destination="{slot}"/>
</ExternalSourcesList>'''
    p = os.path.join(SCRATCH, f"{slot}.wsources")
    with open(p, "w", encoding="utf-8") as f:
        f.write(xml)
    return p


def build_wem(slot):
    wsources = make_wsources(SCRATCH, slot)

    for stale in Path(SCRATCH).rglob(f"{slot}.wem"):
        try:
            stale.unlink()
        except OSError:
            pass

    try:
        r = subprocess.run(
            [
                WWISE_CONSOLE,
                "convert-external-source",
                WWISE_PROJ,
                "--source-file", wsources,
                "--output", SCRATCH,
                "--platform", "Windows",
            ],
            capture_output=True,
            timeout=120,
        )
        if r.returncode != 0:
            log(f"  WwiseConsole rc={r.returncode}: {r.stderr.decode(errors='replace')[:300]}")
    except subprocess.TimeoutExpired:
        log("  WwiseConsole timed out")
        return None

    found = [p for p in Path(SCRATCH).rglob(f"{slot}.wem") if p.stat().st_size > 0]
    if found:
        return str(max(found, key=lambda p: p.stat().st_mtime))
    return None


def is_male_voice_folder(voice_folder):
    if voice_folder in MALE_VOICE_TYPES:
        return True
    if voice_folder.startswith("NPCM") or voice_folder.startswith("GenericMale"):
        return True
    return False


def parse_wav_pcm(data):
    """Manually walk a WAV's RIFF chunks and return
    (channels, sample_rate, bits_per_sample, corrected_byte_rate, data_bytes)
    or None if it doesn't parse as PCM WAV.

    Shared by sanitize_wav_for_lipgen (rewriting a clean file for
    LipGenerator) and duration calculation (so both agree on the real
    audio length instead of maintaining two different half-correct guesses).
    Handles two known-bad patterns from Cartesia's streaming writer:
      - placeholder/overflowing data chunk size (0 or 0xFFFFFFFF) - treated
        as "rest of file"
      - a declared byte_rate that doesn't match sample_rate*channels*bytes -
        recomputed from the other fields rather than trusted
    """
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None

    pos = 12
    fmt_chunk = None
    data_chunk = None
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        chunk_size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        chunk_start = pos + 8
        chunk_end = chunk_start + chunk_size

        if chunk_id == b"data" and (chunk_size == 0 or chunk_size == 0xFFFFFFFF
                                     or chunk_end > len(data)):
            chunk_end = len(data)
            chunk_size = chunk_end - chunk_start
            data_chunk = data[chunk_start:chunk_end]
            log(f"  data chunk had placeholder/overflowing size; "
                f"using rest-of-file ({chunk_size} bytes)")
            break

        if chunk_end > len(data):
            break
        if chunk_id == b"fmt ":
            fmt_chunk = data[chunk_start:chunk_end]
        elif chunk_id == b"data":
            data_chunk = data[chunk_start:chunk_end]
        pos = chunk_end
        if chunk_size % 2 == 1:
            pos += 1  # RIFF chunks are word-aligned

    if not fmt_chunk or len(fmt_chunk) < 16 or not data_chunk:
        return None

    audio_format, channels, sample_rate, byte_rate, block_align, bits_per_sample = \
        struct.unpack("<HHIIHH", fmt_chunk[:16])

    expected_byte_rate = sample_rate * channels * (bits_per_sample // 8)
    implied_duration = len(data_chunk) / byte_rate if byte_rate else -1
    log(f"  fmt check: sample_rate={sample_rate} channels={channels} "
        f"bits={bits_per_sample} declared_byte_rate={byte_rate} "
        f"expected_byte_rate={expected_byte_rate} "
        f"implied_duration={implied_duration:.2f}s")
    if byte_rate != expected_byte_rate:
        log(f"  WARNING: declared byte_rate does not match sample_rate*channels*bytes - "
            f"using expected_byte_rate instead")
        byte_rate = expected_byte_rate

    return channels, sample_rate, bits_per_sample, byte_rate, data_chunk


def sanitize_wav_for_lipgen(src_path, dst_path):
    """Rewrite a WAV with a clean, canonical 44-byte PCM header, using
    parse_wav_pcm to recover the real audio regardless of Cartesia's
    streaming-writer quirks (placeholder data-chunk size, wrong byte_rate).

    Returns True on success, False if the file doesn't parse as WAV/PCM
    (caller should fall back to the original file rather than fail the line).
    """
    try:
        with open(src_path, "rb") as f:
            data = f.read()
    except Exception as e:
        log(f"  could not read wav for sanitize: {e}")
        return False

    parsed = parse_wav_pcm(data)
    if parsed is None:
        return False
    channels, sample_rate, bits_per_sample, byte_rate, data_chunk = parsed
    block_align = channels * (bits_per_sample // 8)
    audio_format = 1  # PCM

    try:
        with open(dst_path, "wb") as f:
            f.write(b"RIFF")
            f.write(struct.pack("<I", 36 + len(data_chunk)))
            f.write(b"WAVE")
            f.write(b"fmt ")
            f.write(struct.pack("<I", 16))
            f.write(struct.pack("<HHIIHH", audio_format, channels, sample_rate,
                                 byte_rate, block_align, bits_per_sample))
            f.write(b"data")
            f.write(struct.pack("<I", len(data_chunk)))
            f.write(data_chunk)
    except Exception as e:
        log(f"  could not write sanitized wav: {e}")
        return False

    return True


def build_lipsync(wav_path, slot, voice_folder, subtitle):
    """Generate a .ffxanim for this line via LipGenerator + ffxc.
    Returns the compiled ffxanim path, or None on any failure (non-fatal -
    the line's audio/subtitle still play, it just won't move NPC lips).

    LipGenerator's own usage banner: `LipGenerator wav text actor.facefx
    actoranalysis.facefx ...` with example `LipGenerator Test.wav test.txt
    -OutputFileName:foo.ffxanim` - both `wav` and `text` are FILE PATHS.
    It wants the raw WAV (not the Wwise-encoded wem), and it wants the
    spoken line written to a .txt file, not passed inline as a string.
    """
    is_male = is_male_voice_folder(voice_folder)
    facefx_actor = FACEFX_MALE if is_male else FACEFX_FEMALE
    facefx_actor_name = "StarfieldHumanMale" if is_male else "StarfieldHumanFemale"
    intermediate_dir = INTERMEDIATE_MALE if is_male else INTERMEDIATE_FEMALE
    registered_folder = REGISTERED_MALE_FOLDER if is_male else REGISTERED_FEMALE_FOLDER
    # Mantella's starfield.py compiles ffxc's output directly into the real
    # game Data voice folder for the registered actor, not a scratch temp
    # dir - this is the one concrete difference from the proven-working
    # pipeline. Match it exactly.
    registered_data_path = os.path.join(DATA_VOICE_BASE, registered_folder)
    animation_group = f"{registered_folder}_{slot}"

    try:
        os.makedirs(intermediate_dir, exist_ok=True)
        os.makedirs(registered_data_path, exist_ok=True)
    except Exception as e:
        log(f"  could not create intermediate dir for {slot}: {e}")
        return None

    animset_path = os.path.join(intermediate_dir, f"{animation_group}.animset")
    safe_text = subtitle.strip() if subtitle.strip() else "dialogue"

    # NOTE: Mantella's own starfield.py passes safe_text directly as a raw
    # argv string here, NOT a path to a text file - and that's the
    # configuration confirmed to produce correct long-sentence lip sync.
    # An earlier version of this function wrote the subtitle to a .txt file
    # and passed that path instead (based on reading LipGenerator's usage
    # banner example "LipGenerator Test.wav test.txt ..."), which was never
    # actually verified against real behavior. If LipGenerator treats that
    # argument as literal text rather than transparently reading it as a
    # file, every line would have been fed a short, roughly-constant-length
    # file path as its "transcript" instead of the real subtitle - a
    # plausible cause of the fixed ~3s animation-length ceiling observed
    # regardless of true audio duration. Matching Mantella exactly here.

    # Keep the basename identical to the slot (just in a different subfolder)
    # rather than appending a suffix. ffxc derives the compiled animation's
    # output filename from the source audio's basename baked into the
    # animset - a "_clean" suffix here leaks straight into the compiled
    # ffxanim's name (e.g. "00666A7A_clean.ffxanim"), breaking the fixed
    # path this function (and the C++ lip-activation swap) expects.
    clean_wav_dir = os.path.join(SCRATCH, "clean")
    os.makedirs(clean_wav_dir, exist_ok=True)
    clean_wav_path = os.path.join(clean_wav_dir, f"{slot}.wav")
    if sanitize_wav_for_lipgen(wav_path, clean_wav_path):
        lipgen_wav = clean_wav_path
        log(f"  sanitized wav for {slot}: {os.path.getsize(clean_wav_path)} bytes "
            f"(orig {os.path.getsize(wav_path)} bytes)")
    else:
        log(f"  wav sanitize failed for {slot}, using original file")
        lipgen_wav = wav_path

    try:
        r = subprocess.run(
            [
                LIPGENERATOR_EXE,
                lipgen_wav,
                safe_text,
                facefx_actor,
                FACEFX_ANALYSIS,
                f"-OutputFileName:{animset_path}",
                f"-AnimationGroupName:{animation_group}",
            ],
            capture_output=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        log(f"  LipGenerator timed out for slot {slot}")
        return None

    if not os.path.exists(animset_path):
        err = r.stderr.decode(errors="replace") if r else ""
        log(f"  LipGenerator failed to produce animset for {slot}: {err}")
        return None

    animset_size = os.path.getsize(animset_path)
    lg_out = (r.stdout.decode(errors="replace").strip() if r and r.stdout else "")
    if lg_out:
        log(f"  LipGenerator stdout ({slot}): {lg_out}")
    log(f"  animset for {slot}: {animset_size} bytes")

    per_line_proj = os.path.join(intermediate_dir, f"{animation_group}.ffxcproj")
    proj_yaml = (
        "version: 10300\n"
        "items:\n"
        f" - facefx_file: {facefx_actor}\n"
        "   mount_files:\n"
        f"    - {animset_path}\n"
    )
    try:
        with open(per_line_proj, "w", encoding="utf-8") as f:
            f.write(proj_yaml)
    except Exception as e:
        log(f"  could not write ffxcproj for {slot}: {e}")
        return None

    try:
        r = subprocess.run(
            [FFXC_EXE, f"-o={registered_data_path}\\", "-p=x86", "--rebuild", per_line_proj],
            capture_output=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired:
        log(f"  ffxc timed out for slot {slot}")
        return None

    if r.returncode != 0:
        log(f"  ffxc failed for slot {slot}: {r.stderr.decode(errors='replace')}")
        return None

    ffxc_out = (r.stdout.decode(errors="replace").strip() if r.stdout else "")
    ffxc_out = _filter_ffxc_noise(ffxc_out)
    if ffxc_out:
        log(f"  ffxc stdout ({slot}): {ffxc_out}")
    ffxc_err = (r.stderr.decode(errors="replace").strip() if r.stderr else "")
    ffxc_err = _filter_ffxc_noise(ffxc_err)
    if ffxc_err:
        log(f"  ffxc stderr ({slot}): {ffxc_err}")

    ffxanim_src = os.path.join(registered_data_path, facefx_actor_name, "x86",
                                animation_group, f"{slot}.ffxanim")
    if not os.path.exists(ffxanim_src):
        # Only search within THIS build's own output subfolder. Searching all
        # of registered_data_path is unsafe - it also contains previously
        # deployed flat <slot>.ffxanim copies from earlier lines, and a glob
        # match there can silently return stale, unrelated content while
        # looking like success.
        build_subdir = os.path.join(registered_data_path, facefx_actor_name, "x86", animation_group)
        found = list(Path(build_subdir).glob("*.ffxanim")) if os.path.isdir(build_subdir) else []
        if found:
            ffxanim_src = str(max(found, key=lambda p: p.stat().st_size))
            log(f"  slot {slot}: matched compiled output by different name: {Path(ffxanim_src).name}")
        else:
            log(f"  ffxanim not found after ffxc run for slot {slot}")
            log(f"  expected at: {ffxanim_src}")
            log(f"  build subfolder exists: {os.path.isdir(build_subdir)}, "
                f"contents: {list(os.listdir(build_subdir)) if os.path.isdir(build_subdir) else 'N/A'}")
            return None

    ffxanim_size = os.path.getsize(ffxanim_src)
    log(f"  ffxanim for {slot}: {ffxanim_size} bytes")

    return ffxanim_src


def deploy_ffxanim(ffxanim_src, voice_folder, slot):
    dest_mo2 = None
    for base in VOICE_BASES:
        dest_dir = os.path.join(base, voice_folder)
        os.makedirs(dest_dir, exist_ok=True)
        dest = os.path.join(dest_dir, f"{slot}.ffxanim")
        shutil.copyfile(ffxanim_src, dest)
        log(f"  deployed {slot}.ffxanim -> {dest}")
        if dest_mo2 is None:
            dest_mo2 = dest
    return dest_mo2


def remove_stale_ffxanim(voice_folder, slot):
    for base in VOICE_BASES:
        ffx = os.path.join(base, voice_folder, f"{slot}.ffxanim")
        try:
            if os.path.exists(ffx):
                os.remove(ffx)
                log(f"  removed stale {slot}.ffxanim -> {ffx}")
        except Exception as e:
            log(f"  could not remove stale ffxanim {ffx}: {e}")


def deploy_wem(wem_path, voice_folder, slot):
    remove_stale_ffxanim(voice_folder, slot)

    for base in VOICE_BASES:
        dest_dir = os.path.join(base, voice_folder)
        os.makedirs(dest_dir, exist_ok=True)
        dest = os.path.join(dest_dir, f"{slot}.wem")
        shutil.copyfile(wem_path, dest)
        log(f"  deployed {slot}.wem -> {dest}")

    return os.path.join(VOICE_BASES[0], voice_folder, f"{slot}.wem")


def estimate_duration(wav_path):
    """Real audio duration from the actual parsed WAV data, not a guess.

    Mantella's own starfield.py parses the RIFF header directly for this
    exact reason (byte_rate/data_size), specifically because the stdlib
    `wave` module misreads Cartesia's output. The crude (size-44)/44100
    formula this replaced only happened to be numerically close for
    Cartesia's known 22050Hz/16-bit/mono format, and silently assumed the
    PCM payload starts exactly at byte 44 - which we've since confirmed is
    NOT always true (some files have chunks before `data`, and some have
    had a corrected byte_rate). Falls back to the crude estimate only if
    the file genuinely can't be parsed as PCM WAV at all.
    """
    try:
        with open(wav_path, "rb") as f:
            data = f.read()
        parsed = parse_wav_pcm(data)
        if parsed is not None:
            channels, sample_rate, bits_per_sample, byte_rate, data_chunk = parsed
            if byte_rate > 0:
                secs = len(data_chunk) / byte_rate
                return round(max(0.1, min(60.0, secs)), 2)
    except OSError:
        pass

    try:
        size = os.path.getsize(wav_path)
        secs = max(1.0, (size - 44) / 44100.0)
        log(f"  duration fallback (could not parse WAV header): {secs:.2f}s")
        return round(secs, 2)
    except OSError:
        return 3.0


def read_write_index():
    """[queue] write_index from MantellaQueue.ini.

    Parsed from raw bytes on purpose. The file holds subtitles written as
    UTF-8, and configparser decodes with the Windows locale codepage
    (cp1252), which has no mapping for bytes 0x81/0x8D/0x8F/0x90/0x9D. A
    subtitle containing e.g. "Á" or "č" made cfg.read() raise before this
    function's try block, the exception escaped to main()'s loop, the line
    was never marked consumed, and the watcher re-processed that same line
    forever - silently wedging all voice output for the session."""
    try:
        with open(QUEUE_INI, "rb") as f:
            data = f.read()
    except OSError:
        return 0
    in_queue = False
    for raw in data.splitlines():
        line = raw.strip()
        if line.startswith(b"["):
            in_queue = line.lower() == b"[queue]"
            continue
        if in_queue and b"=" in line:
            k, v = line.split(b"=", 1)
            if k.strip().lower() == b"write_index":
                try:
                    return int(v.strip())
                except ValueError:
                    return 0
    return 0


def reset_queue():
    os.makedirs(os.path.dirname(QUEUE_INI), exist_ok=True)
    with open(QUEUE_INI, "w", encoding="utf-8") as f:
        f.write("[queue]\nwrite_index=0\nread_index=0\nturn_done=0\n")


def write_turn_done():
    import ctypes
    W = ctypes.windll.kernel32.WritePrivateProfileStringA
    W(b"queue", b"turn_done", b"1", QUEUE_INI.encode())
    log("  marked queue turn_done=1")


def write_queue_line(idx, wem_dest, subtitle, duration, speaker_name=""):
    import ctypes
    W = ctypes.windll.kernel32.WritePrivateProfileStringA
    q = QUEUE_INI.encode()
    sec = f"line_{idx}".encode()

    W(sec, b"wem_path", wem_dest.encode(), q)
    W(sec, b"subtitle", subtitle.encode("utf-8", "replace"), q)
    W(sec, b"duration", f"{duration:.2f}".encode(), q)
    # Name of the NPC who speaks this line, threaded straight through from
    # UnityChat.cpp's work-file line (no resolution needed - it's already
    # the server's own actor name) -> here -> the queue -> Papyrus, so the
    # scene alias can retarget per-line via GetFormsByTESFullName instead
    # of staying fixed to whoever started the conversation.
    W(sec, b"speaker_name", str(speaker_name).encode(), q)
    W(b"queue", b"write_index", str(idx + 1).encode(), q)


def is_bad_subtitle(subtitle):
    s = subtitle.strip()

    if not s:
        return True

    if s.startswith("{") or s.startswith("["):
        return True

    if '"character"' in s or '"message"' in s or '"response"' in s:
        return True

    if s in ("null", "None", "undefined"):
        return True

    return False


def process_line(hostport, hash_, voice_folder, subtitle, speaker_name=""):
    subtitle = subtitle.strip()

    if is_bad_subtitle(subtitle):
        log(f"  skip malformed subtitle: {subtitle!r}")
        return False

    slot = next_slot()
    wav = os.path.join(SCRATCH, f"{slot}.wav")

    if not fetch_wav(hostport, hash_, wav):
        return False

    wem = build_wem(slot)
    if not wem:
        log(f"  wem build failed for slot {slot}")
        return False

    dest = deploy_wem(wem, voice_folder, slot)

    # LipGenerator's own usage banner confirms it wants the raw WAV
    # ("wav - Specifies the WAV file to analyze"), not the Wwise-encoded
    # wem. Text is passed as a raw string (matching Mantella's own
    # starfield.py exactly) - NOT a file path; an earlier attempt to write
    # it to a .txt file and pass that path instead was reverted.
    ffxanim_src = build_lipsync(wav, slot, voice_folder, subtitle)
    if ffxanim_src:
        deploy_ffxanim(ffxanim_src, voice_folder, slot)
    else:
        log(f"  lipsync skipped for slot {slot} (audio/subtitle unaffected)")

    duration = estimate_duration(wav)
    idx = read_write_index()

    write_queue_line(idx, dest, subtitle, duration, speaker_name)
    log(f"  queued line {idx}: slot={slot} dur={duration}s speaker_name={speaker_name} '{subtitle[:60]}'")
    return True


def read_work_lines():
    if not os.path.exists(WORK_FILE):
        return []
    with open(WORK_FILE, "r", encoding="utf-8", errors="replace") as f:
        return [ln.rstrip("\n") for ln in f if ln.strip()]


def main():
    global _startup_problems
    os.makedirs(SCRATCH, exist_ok=True)
    os.makedirs(os.path.dirname(WORK_FILE), exist_ok=True)

    if not os.path.exists(WORK_FILE):
        open(WORK_FILE, "w", encoding="utf-8").close()

    log(f"config: {CONFIG_PATH} ({'found' if os.path.exists(CONFIG_PATH) else 'NOT FOUND - using defaults'})")
    missing = [(n, p) for n, p in (
        ("StarfieldDir", STARFIELD_DIR), ("WwiseConsole", WWISE_CONSOLE), ("Wwise project", WWISE_PROJ),
        ("LipGenerator", LIPGENERATOR_EXE), ("ffxc", FFXC_EXE), ("FaceFX female actor", FACEFX_FEMALE),
        ("FaceFX male actor", FACEFX_MALE), ("FaceFX analysis actor", FACEFX_ANALYSIS),
    ) if not os.path.exists(p)]
    for n, p in missing:
        log(f"WARNING: {n} not found at {p}")
    if missing:
        log("WARNING: voice/lip generation will fail until the paths above are fixed in the config")
        _startup_problems = "missing " + ", ".join(n for n, _ in missing) + " - check unity_wem_gen.ini"
    os.makedirs(BRIDGE_DIR, exist_ok=True)
    threading.Thread(target=_heartbeat_loop, daemon=True).start()
    log(f"heartbeat -> {HEARTBEAT_INI}")
    for b in VOICE_BASES:
        log(f"voice output -> {b}")
    log(f"watching {WORK_FILE}")
    log(f"queue -> {QUEUE_INI}")
    log(f"wem slots: {len(WEM_SLOTS)}")

    reset_queue()

    # Important now that the watcher works:
    # ignore old UnityWork.ini lines on startup so previous inputs don't replay.
    consumed = len(read_work_lines())
    log(f"starting after existing work lines: consumed={consumed}")

    line_failures = {}  # work-line index -> failed attempts

    while True:
        try:
            lines = read_work_lines()

            # C++/UnityChat may truncate UnityWork.ini each new turn.
            # Only reset our consumed counter. Do NOT reset MantellaQueue.ini here;
            # C++ owns queue resets at conversation/turn boundaries.
            if len(lines) < consumed:
                log("work file was truncated; resetting consumed counter")
                consumed = 0
                line_failures.clear()

            if len(lines) > consumed:
                hostport = discover_server()

                for raw in lines[consumed:]:
                    if raw.strip() == "__ENDTURN__":
                        write_turn_done()
                        consumed += 1
                        continue

                    parts = raw.split("|", 3)
                    if len(parts) < 4:
                        log(f"  skip malformed work line: {raw!r}")
                        consumed += 1
                        continue

                    hash_, voice_folder, speaker_name, subtitle = parts

                    log(f"processing {hash_.strip()} voice={voice_folder.strip()} speaker_name={speaker_name.strip()}")

                    try:
                        process_line(
                            hostport,
                            hash_.strip(),
                            voice_folder.strip(),
                            subtitle,
                            speaker_name.strip(),
                        )
                    except Exception as e:
                        # One bad line must never wedge the watcher. Before,
                        # an exception here skipped the consumed += 1 below,
                        # so the same line was retried every second forever.
                        failures = line_failures.get(consumed, 0) + 1
                        line_failures[consumed] = failures
                        if failures < MAX_LINE_ATTEMPTS:
                            log(f"  error on this line (attempt {failures}/{MAX_LINE_ATTEMPTS}): {e!r} - retrying")
                            time.sleep(1.0)
                            break  # re-read the work file and retry this line
                        log(f"  giving up on this line after {failures} attempts: {e!r}")

                    line_failures.pop(consumed, None)
                    consumed += 1

            time.sleep(0.25)

        except KeyboardInterrupt:
            log("stopped")
            break
        except Exception as e:
            log(f"loop error: {e}")
            time.sleep(1.0)


if __name__ == "__main__":
    main()