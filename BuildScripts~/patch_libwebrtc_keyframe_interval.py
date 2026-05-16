"""Patch libwebrtc.aar to override HardwareVideoEncoderFactory's hardcoded
keyframe interval.

Why this exists
---------------
Upstream libwebrtc's `org.webrtc.HardwareVideoEncoderFactory.createEncoder()`
constructs `HardwareVideoEncoder` with a hardcoded `keyFrameIntervalSec = 3600`
(one hour). That value is passed to MediaCodec as `KEY_I_FRAME_INTERVAL`.

On Qualcomm `c2.qti.avc.encoder` (Quest 3), 3600 s is accepted on the INITIAL
configure but rejected on the resetCodec recovery path that fires after the
OS reclaims the codec (because that path only replays the param diff and is
missing required base params like width/height/color-format). The
half-configured codec then throws Java IllegalStateException into the next
Encode(), and libwebrtc's ~JniJavaCallContextChecked RTC_CHECK aborts the
process. See `webrtc_changelog.md` PM #8b for the full forensic chain.

This script rewrites the single `sipush 3600` bytecode instruction in
`HardwareVideoEncoderFactory.class` to `sipush <new_value>`. Bytecode size
is unchanged (3 bytes -> 3 bytes), so stack-maps and class metadata stay
valid -- no recompilation required.

Idempotent: re-running with a value already present is a no-op (script
detects the patched state and exits 0).

Loud failure: if the upstream class file changes such that the bytecode
sequence is not unique or moves to a different offset, the script aborts
before touching the AAR. The build then fails loudly rather than silently
shipping a stale or wrong-patched binary.

Usage
-----
    python patch_libwebrtc_keyframe_interval.py <aar_path> <key_frame_interval_seconds>

Example
-------
    python patch_libwebrtc_keyframe_interval.py \
        Runtime/Plugins/Android/libwebrtc.aar 2
"""

import io
import os
import sys
import zipfile

CLASS_NAME = "org/webrtc/HardwareVideoEncoderFactory.class"
NESTED_JAR = "classes.jar"

# sipush opcode = 0x11, operand = 2 bytes big-endian short
SIPUSH = 0x11
ORIGINAL_VALUE = 3600

# Expected unpatched byte sequence: sipush 3600 = 0x11 0x0E 0x10
ORIGINAL_BYTES = bytes([SIPUSH, (ORIGINAL_VALUE >> 8) & 0xFF, ORIGINAL_VALUE & 0xFF])


def _find_unique_offset(class_bytes: bytes, target: bytes) -> int:
    """Return the single offset of `target` in `class_bytes`, or raise."""
    offsets = [i for i in range(len(class_bytes) - len(target) + 1)
               if class_bytes[i:i + len(target)] == target]
    if len(offsets) == 0:
        return -1
    if len(offsets) > 1:
        raise RuntimeError(
            f"Expected a single occurrence of {target.hex()} in {CLASS_NAME}, "
            f"found {len(offsets)} at offsets {[hex(o) for o in offsets]}. "
            "Upstream libwebrtc may have changed; refusing to patch ambiguously."
        )
    return offsets[0]


def _patch_class(class_bytes: bytes, new_value: int) -> tuple[bytes, str]:
    """Patch `sipush 3600` -> `sipush new_value` in HardwareVideoEncoderFactory.class.

    Returns (patched_bytes, status_message).
    Idempotent: returns the input unchanged if already patched.
    """
    if not (0 <= new_value <= 32767):
        raise ValueError(
            f"new_value must be a non-negative short (0..32767), got {new_value}. "
            "sipush cannot encode values outside this range."
        )

    patched_bytes_target = bytes([SIPUSH, (new_value >> 8) & 0xFF, new_value & 0xFF])

    # Already-patched detection
    if _find_unique_offset(class_bytes, patched_bytes_target) != -1 \
            and _find_unique_offset(class_bytes, ORIGINAL_BYTES) == -1:
        return class_bytes, (
            f"already patched (sipush {new_value} present, sipush {ORIGINAL_VALUE} absent); no-op")

    off = _find_unique_offset(class_bytes, ORIGINAL_BYTES)
    if off == -1:
        raise RuntimeError(
            f"Could not find expected unpatched sequence {ORIGINAL_BYTES.hex()} "
            f"(sipush {ORIGINAL_VALUE}) in {CLASS_NAME}. The AAR may already be "
            "patched with a different value, or upstream libwebrtc removed the "
            "constant. Refusing to mutate."
        )

    mutable = bytearray(class_bytes)
    mutable[off:off + 3] = patched_bytes_target
    return bytes(mutable), (
        f"patched offset 0x{off:08X}: sipush {ORIGINAL_VALUE} -> sipush {new_value}")


def _rebuild_zip(original_path: str, replacements: dict[str, bytes]) -> bytes:
    """Read a zip, replace entries with `replacements`, return new zip bytes.

    Preserves the original compression scheme per-entry. Other entries pass
    through unchanged.
    """
    out_io = io.BytesIO()
    with zipfile.ZipFile(original_path, "r") as src, \
            zipfile.ZipFile(out_io, "w") as dst:
        for info in src.infolist():
            data = replacements.get(info.filename, src.read(info.filename))
            new_info = zipfile.ZipInfo(filename=info.filename, date_time=info.date_time)
            new_info.compress_type = info.compress_type
            new_info.external_attr = info.external_attr
            new_info.create_system = info.create_system
            dst.writestr(new_info, data)
    return out_io.getvalue()


def _rebuild_zip_from_bytes(zip_bytes: bytes, replacements: dict[str, bytes]) -> bytes:
    """Same as `_rebuild_zip` but the source zip is in memory."""
    out_io = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as src, \
            zipfile.ZipFile(out_io, "w") as dst:
        for info in src.infolist():
            data = replacements.get(info.filename, src.read(info.filename))
            new_info = zipfile.ZipInfo(filename=info.filename, date_time=info.date_time)
            new_info.compress_type = info.compress_type
            new_info.external_attr = info.external_attr
            new_info.create_system = info.create_system
            dst.writestr(new_info, data)
    return out_io.getvalue()


def patch_aar(aar_path: str, new_value: int) -> None:
    """Patch the AAR in-place. Aborts loudly on any unexpected condition."""
    if not os.path.isfile(aar_path):
        raise FileNotFoundError(f"AAR not found: {aar_path}")

    with zipfile.ZipFile(aar_path, "r") as aar:
        if NESTED_JAR not in aar.namelist():
            raise RuntimeError(
                f"{NESTED_JAR} not found in {aar_path}. Not a Unity.WebRTC AAR?")
        if not any(NESTED_JAR == n for n in aar.namelist()):
            raise RuntimeError(f"{NESTED_JAR} entry missing in {aar_path}")

        classes_jar_bytes = aar.read(NESTED_JAR)

    # Locate and patch the class file inside classes.jar (in memory)
    with zipfile.ZipFile(io.BytesIO(classes_jar_bytes), "r") as jar:
        if CLASS_NAME not in jar.namelist():
            raise RuntimeError(
                f"{CLASS_NAME} not found in {NESTED_JAR}. Unexpected libwebrtc layout?")
        class_bytes = jar.read(CLASS_NAME)

    patched_class_bytes, class_status = _patch_class(class_bytes, new_value)

    if patched_class_bytes is class_bytes:
        # No-op: already patched. Don't rewrite the AAR; saves IO and keeps mtime stable.
        print(f"[patch_libwebrtc] {CLASS_NAME}: {class_status}")
        return

    # Rebuild classes.jar with patched class
    new_classes_jar_bytes = _rebuild_zip_from_bytes(
        classes_jar_bytes, {CLASS_NAME: patched_class_bytes})

    # Rebuild AAR with patched classes.jar
    new_aar_bytes = _rebuild_zip(aar_path, {NESTED_JAR: new_classes_jar_bytes})

    # Atomic-ish write: write to .tmp then replace
    tmp_path = aar_path + ".patch.tmp"
    with open(tmp_path, "wb") as f:
        f.write(new_aar_bytes)
    os.replace(tmp_path, aar_path)

    print(f"[patch_libwebrtc] {CLASS_NAME}: {class_status}")
    print(f"[patch_libwebrtc] {aar_path}: rewritten ({len(new_aar_bytes)} bytes)")


def _main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    aar_path = argv[1]
    try:
        new_value = int(argv[2])
    except ValueError:
        print(f"ERROR: key_frame_interval_seconds must be an integer, got '{argv[2]}'",
              file=sys.stderr)
        return 2

    try:
        patch_aar(aar_path, new_value)
    except (RuntimeError, ValueError, FileNotFoundError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv))
