"""Downloads the codebase-memory-mcp binary on first run, then exec's it."""

import hashlib
import os
import platform
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO = "DeusData/codebase-memory-mcp"

# Security: only permit https fetches. urllib's default handlers accept
# file://, ftp://, and custom schemes — a redirect or tainted URL source
# could otherwise turn a download into an arbitrary-local-file read.
_ALLOWED_SCHEMES = frozenset({"https"})
_MAX_REDIRECTS = 5
_NETWORK_TIMEOUT_SECONDS = 120
_CANDIDATE_TIMEOUT_SECONDS = 15
_MAX_CHECKSUM_MANIFEST_BYTES = 1024 * 1024
_REDIRECT_CODES = frozenset({301, 302, 303, 307, 308})


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Expose redirects to _download_https for explicit per-hop validation."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_HTTPS_OPENER = urllib.request.build_opener(_NoRedirectHandler())


def _validate_url_scheme(url: str) -> None:
    """Reject non-https URLs before any network fetch."""
    parsed = urllib.parse.urlparse(url)
    if (
        parsed.scheme not in _ALLOWED_SCHEMES
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
    ):
        sys.exit(
            f"codebase-memory-mcp: refusing to fetch invalid, credentialed, or "
            f"non-https URL: {url}"
        )


def _download_https(url: str, dest: str, max_bytes: int = 0) -> None:
    """Download through a bounded HTTPS-only opener into dest."""
    current_url = url
    for redirect_count in range(_MAX_REDIRECTS + 1):
        _validate_url_scheme(current_url)
        request = urllib.request.Request(
            current_url, headers={"User-Agent": "codebase-memory-mcp-installer"}
        )
        try:
            response = _HTTPS_OPENER.open(
                request, timeout=_NETWORK_TIMEOUT_SECONDS
            )
        except urllib.error.HTTPError as exc:
            if exc.code not in _REDIRECT_CODES:
                raise
            location = exc.headers.get("Location")
            exc.close()
            if not location:
                raise RuntimeError(f"redirect has no Location: {current_url}")
            if redirect_count == _MAX_REDIRECTS:
                raise RuntimeError("too many redirects")
            current_url = urllib.parse.urljoin(current_url, location)
            _validate_url_scheme(current_url)
            continue

        with response:
            _validate_url_scheme(response.geturl())
            deadline = time.monotonic() + _NETWORK_TIMEOUT_SECONDS
            total = 0
            with open(dest, "wb") as out:
                while True:
                    if time.monotonic() >= deadline:
                        raise TimeoutError(
                            f"download hop timed out: {current_url}"
                        )
                    chunk = response.read(65536)
                    if not chunk:
                        return
                    total += len(chunk)
                    if max_bytes and total > max_bytes:
                        raise RuntimeError(
                            f"download exceeds the {max_bytes}-byte safety limit"
                        )
                    out.write(chunk)

    raise RuntimeError("too many redirects")


def _verify_candidate(path: Path) -> None:
    """Require a staged native candidate to execute successfully."""
    try:
        subprocess.run(
            [str(path), "--version"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
            timeout=_CANDIDATE_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError(f"downloaded binary failed to run: {exc}") from exc


def _safe_extract_tar(tf, dest: str) -> None:
    """Extract a tarfile to dest, rejecting path-traversal entries.

    Uses the tarfile data filter on Python >=3.12 (PEP 706), falls back to
    manual per-member path validation on older Pythons. Mitigates the
    classic tar-slip / Zip Slip vulnerability (CWE-22).
    """
    if hasattr(tf, "extraction_filter") or sys.version_info >= (3, 12):
        tf.extractall(dest, filter="data")
        return

    dest_abs = os.path.abspath(dest)
    for member in tf.getmembers():
        if member.issym() or member.islnk():
            sys.exit(
                f"codebase-memory-mcp: refusing unsafe tar entry "
                f"(link: {member.name!r})"
            )
        member_abs = os.path.abspath(os.path.join(dest_abs, member.name))
        if not (member_abs == dest_abs or member_abs.startswith(dest_abs + os.sep)):
            sys.exit(
                f"codebase-memory-mcp: refusing unsafe tar entry "
                f"(escapes dest: {member.name!r})"
            )
    tf.extractall(dest)


def _safe_extract_zip(zf, dest: str) -> None:
    """Extract a zipfile to dest, rejecting path-traversal entries."""
    dest_abs = os.path.abspath(dest)
    for name in zf.namelist():
        member_abs = os.path.abspath(os.path.join(dest_abs, name))
        if not (member_abs == dest_abs or member_abs.startswith(dest_abs + os.sep)):
            sys.exit(
                f"codebase-memory-mcp: refusing unsafe zip entry "
                f"(escapes dest: {name!r})"
            )
    zf.extractall(dest)


def _verify_checksum(archive_path: str, archive_name: str, version: str) -> None:
    """Verify SHA256 checksum against checksums.txt from the release."""
    url = f"https://github.com/{REPO}/releases/download/v{version}/checksums.txt"
    tmp_path = None
    try:
        _validate_url_scheme(url)
        with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as tmp:
            tmp_path = tmp.name
        _download_https(url, tmp_path, _MAX_CHECKSUM_MANIFEST_BYTES)
        expected = None
        with open(tmp_path, encoding="utf-8") as f:
            for line in f:
                fields = line.split()
                if len(fields) < 2 or fields[1] not in (
                    archive_name,
                    f"*{archive_name}",
                ):
                    continue
                digest = fields[0].lower()
                if len(digest) != 64 or any(
                    ch not in "0123456789abcdef" for ch in digest
                ):
                    sys.exit(
                        f"codebase-memory-mcp: invalid SHA256 checksum for "
                        f"{archive_name}"
                    )
                if expected is not None and expected != digest:
                    sys.exit(
                        f"codebase-memory-mcp: conflicting SHA256 checksums for "
                        f"{archive_name}"
                    )
                expected = digest
        if expected is None:
            sys.exit(
                f"codebase-memory-mcp: no checksum for {archive_name} in checksums.txt"
            )
        h = hashlib.sha256()
        with open(archive_path, "rb") as af:
            for chunk in iter(lambda: af.read(65536), b""):
                h.update(chunk)
        actual = h.hexdigest()
        if expected != actual:
            sys.exit(
                f"codebase-memory-mcp: CHECKSUM MISMATCH for {archive_name}\n"
                f"  expected: {expected}\n"
                f"  actual:   {actual}"
            )
        print("codebase-memory-mcp: checksum verified.", file=sys.stderr)
    except SystemExit:
        raise
    except Exception as exc:
        sys.exit(f"codebase-memory-mcp: checksum verification failed: {exc}")
    finally:
        if tmp_path is not None:
            try:
                os.unlink(tmp_path)
            except Exception:
                pass


def _version() -> str:
    try:
        from importlib.metadata import version
        return version("codebase-memory-mcp")
    except Exception:
        return "0.8.1"


def _os_name() -> str:
    p = sys.platform
    if p == "linux":
        return "linux"
    if p == "darwin":
        return "darwin"
    if p == "win32":
        return "windows"
    sys.exit(f"codebase-memory-mcp: unsupported platform: {p}")


def _arch() -> str:
    m = platform.machine().lower()
    if m in ("arm64", "aarch64"):
        return "arm64"
    if m in ("x86_64", "amd64"):
        return "amd64"
    sys.exit(f"codebase-memory-mcp: unsupported architecture: {m}")


def _cache_dir() -> Path:
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return base / "codebase-memory-mcp"


def _bin_path(version: str) -> Path:
    name = "codebase-memory-mcp.exe" if sys.platform == "win32" else "codebase-memory-mcp"
    return _cache_dir() / version / name


def _download(version: str) -> Path:
    os_name = _os_name()
    arch = _arch()
    ext = "zip" if os_name == "windows" else "tar.gz"
    # Linux ships a fully-static "-portable" build; the standard linux binary
    # dynamically links glibc 2.38+ and fails on older distros. macOS/Windows
    # have no such variant. Keep in sync with install.sh / install.js / cli.c.
    variant = "-portable" if os_name == "linux" else ""
    # Opt into the UI build (embedded graph visualization) with CBM_VARIANT=ui.
    # Default is the standard (headless) build. Mirrors install.sh --ui.
    ui = "ui-" if os.environ.get("CBM_VARIANT", "").lower() == "ui" else ""
    archive = f"codebase-memory-mcp-{ui}{os_name}-{arch}{variant}.{ext}"
    url = f"https://github.com/{REPO}/releases/download/v{version}/{archive}"
    _validate_url_scheme(url)

    dest = _bin_path(version)
    dest.parent.mkdir(parents=True, exist_ok=True)

    print(
        f"codebase-memory-mcp: downloading v{version} for {os_name}/{arch}...",
        file=sys.stderr,
    )

    with tempfile.TemporaryDirectory() as tmp:
        tmp_archive = os.path.join(tmp, f"cbm.{ext}")
        try:
            _download_https(url, tmp_archive)
        except (OSError, RuntimeError, urllib.error.URLError) as e:
            sys.exit(
                f"codebase-memory-mcp: download failed ({e})\n"
                f"URL: {url}\n"
                f"See https://github.com/{REPO}/releases for available versions."
            )

        _verify_checksum(tmp_archive, archive, version)

        if ext == "tar.gz":
            import tarfile
            with tarfile.open(tmp_archive) as tf:
                _safe_extract_tar(tf, tmp)
        else:
            import zipfile
            with zipfile.ZipFile(tmp_archive) as zf:
                _safe_extract_zip(zf, tmp)

        bin_name = "codebase-memory-mcp.exe" if os_name == "windows" else "codebase-memory-mcp"
        extracted = os.path.join(tmp, bin_name)
        if not os.path.exists(extracted):
            sys.exit("codebase-memory-mcp: binary not found after extraction")

        extracted_path = Path(extracted)
        current = extracted_path.stat().st_mode
        extracted_path.chmod(current | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        try:
            _verify_candidate(extracted_path)
        except RuntimeError as exc:
            sys.exit(f"codebase-memory-mcp: {exc}")

        staged_path = None
        try:
            staged_suffix = ".tmp.exe" if sys.platform == "win32" else ".tmp"
            with tempfile.NamedTemporaryFile(
                dir=str(dest.parent),
                prefix=f".{dest.name}.",
                suffix=staged_suffix,
                delete=False,
            ) as staged:
                staged_path = Path(staged.name)
            shutil.copy2(extracted_path, staged_path)
            staged_mode = staged_path.stat().st_mode
            staged_path.chmod(
                staged_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
            )
            _verify_candidate(staged_path)
            os.replace(staged_path, dest)
            staged_path = None
        except RuntimeError as exc:
            sys.exit(f"codebase-memory-mcp: {exc}")
        finally:
            if staged_path is not None:
                try:
                    staged_path.unlink()
                except OSError:
                    pass

    return dest


def main() -> None:
    version = _version()
    bin_path = _bin_path(version)

    if not bin_path.exists():
        bin_path = _download(version)

    # args is a list (not a shell string), so exec/subprocess treat each
    # element as a discrete argv entry — no shell interpretation, no
    # injection vector. sys.argv forwarding is the whole point of this
    # shim, so tainted-input suppression is intentional.
    args = [str(bin_path)] + sys.argv[1:]

    if sys.platform != "win32":
        os.execv(str(bin_path), args)  # noqa: S606 — list form, no shell
    else:
        result = subprocess.run(args)  # noqa: S603 — list form, no shell=True
        sys.exit(result.returncode)
