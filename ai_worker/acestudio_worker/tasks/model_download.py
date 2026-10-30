"""Downloads model files with resume and SHA-256 verification.

The host only submits this task after the user has approved the model's
licence; the request must name the licence that was accepted, and the task
refuses to run without it. Files are written to `<name>.part` and renamed
only after their hash matches, so a partial or corrupt download is never
mistaken for an installed model.
"""

import hashlib
import os
import time
import urllib.error
import urllib.request

from ..jobs import JobError

NAME = "model.download"
CHUNK = 1 << 20


def available():
    return True, ""


def _hash_file(path, ctx, done_before, total):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            ctx.check_cancel()
            block = f.read(CHUNK * 4)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def _download(ctx, url, part, expected_size, headers, done_before, total):
    have = os.path.getsize(part) if os.path.exists(part) else 0
    if expected_size and have > expected_size:
        os.remove(part)
        have = 0
    req = urllib.request.Request(url, headers=dict(headers))
    req.add_header("User-Agent", "AceStudio-ModelManager/1")
    if have:
        req.add_header("Range", "bytes=%d-" % have)
    try:
        resp = urllib.request.urlopen(req, timeout=60)
    except urllib.error.HTTPError as e:
        if e.code == 416 and have:   # already complete
            return
        if e.code in (401, 403):
            raise JobError("Access denied (%d) for %s. The model may be gated: accept its terms on the "
                           "website and provide an access token." % (e.code, url), kind="missing_model")
        raise JobError("HTTP %d downloading %s" % (e.code, url))
    except urllib.error.URLError as e:
        raise JobError("Cannot reach %s: %s" % (url, e.reason))
    with resp:
        if have and resp.status != 206:   # server ignored Range: start over
            have = 0
        mode = "ab" if have else "wb"
        last = time.monotonic()
        with open(part, mode) as f:
            while True:
                ctx.check_cancel()
                block = resp.read(CHUNK)
                if not block:
                    break
                f.write(block)
                have += len(block)
                now = time.monotonic()
                if now - last > 0.2:
                    last = now
                    frac = (done_before + have) / total if total else None
                    ctx.progress(frac, "%s: %.1f / %.1f MB" % (os.path.basename(part)[:-5], have / 1e6,
                                                               (expected_size or 0) / 1e6))


def run(ctx):
    files = ctx.param("files", required=True, kind=list)
    dest = ctx.param("dest_dir", required=True, kind=str)
    if not ctx.param("license_accepted"):
        raise JobError("Download refused: the model licence has not been accepted.", kind="invalid_input")
    headers = ctx.param("headers", {}, kind=dict)
    total = sum(int(f.get("size") or 0) for f in files)
    done = 0
    written = {}
    hashes = {}   # computed digests, so files without a pinned hash can be pinned now
    for spec in files:
        rel = spec.get("path", "")
        if not rel or os.path.isabs(rel) or ".." in rel.replace("\\", "/").split("/"):
            raise JobError("Unsafe file path in model manifest: %r" % rel, kind="invalid_input")
        target = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(target) or dest, exist_ok=True)
        size = int(spec.get("size") or 0)
        sha = (spec.get("sha256") or "").lower()
        if os.path.exists(target) and (not size or os.path.getsize(target) == size):
            digest = _hash_file(target, ctx, done, total)
            if not sha or digest == sha:
                done += os.path.getsize(target)
                written[rel] = target
                hashes[rel] = digest
                continue
        part = target + ".part"
        _download(ctx, spec["url"], part, size, headers, done, total)
        got = os.path.getsize(part)
        if size and got != size:
            raise JobError("%s: expected %d bytes, got %d. Retry to resume." % (rel, size, got))
        ctx.progress(None, "Verifying %s" % rel, force=True)
        digest = _hash_file(part, ctx, done, total)
        if sha and digest != sha:
            os.remove(part)
            raise JobError("%s failed verification (SHA-256 %s, expected %s). The partial file was deleted."
                           % (rel, digest, sha))
        os.replace(part, target)
        done += got
        written[rel] = target
        hashes[rel] = digest
        ctx.progress(done / total if total else None, "Downloaded %s" % rel, force=True)
    return {"outputs": written, "info": {"bytes": done, "sha256": hashes}}
