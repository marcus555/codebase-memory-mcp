#!/usr/bin/env node
'use strict';
// Postinstall script: downloads the platform-appropriate binary from GitHub Releases.
// Runs automatically via `postinstall` in package.json.

const https = require('https');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execFileSync } = require('child_process');
const { pipeline } = require('stream');

const REPO = 'DeusData/codebase-memory-mcp';
const VERSION = require('./package.json').version;
const BIN_DIR = path.join(__dirname, 'bin');
const MAX_REDIRECTS = 5;
const DOWNLOAD_HOP_TIMEOUT_MS = 120_000;
const CANDIDATE_TIMEOUT_MS = 15_000;
const MAX_CHECKSUM_MANIFEST_BYTES = 1024 * 1024;

function getPlatform() {
  switch (process.platform) {
    case 'linux':  return 'linux';
    case 'darwin': return 'darwin';
    case 'win32':  return 'windows';
    default: throw new Error(`Unsupported platform: ${process.platform}`);
  }
}

function getArch() {
  switch (process.arch) {
    case 'arm64': return 'arm64';
    case 'x64':   return 'amd64';
    default: throw new Error(`Unsupported architecture: ${process.arch}`);
  }
}

// Security: only follow HTTPS URLs (defense-in-depth). Parse the URL instead
// of relying on a string prefix so every redirect is checked unambiguously.
function validateUrl(rawUrl) {
  let parsed;
  try {
    parsed = new URL(rawUrl);
  } catch (err) {
    throw new Error(`Invalid download URL ${rawUrl}: ${err.message}`);
  }
  if (parsed.protocol !== 'https:' || !parsed.hostname || parsed.username || parsed.password) {
    throw new Error(`Refusing non-HTTPS or credentialed URL: ${rawUrl}`);
  }
  return parsed.href;
}

function downloadHop(url, dest, maxBytes) {
  return new Promise((resolve, reject) => {
    let settled = false;
    let response = null;
    let timer = null;

    function finish(err, result) {
      if (settled) return;
      settled = true;
      if (timer) clearTimeout(timer);
      if (err) reject(err);
      else resolve(result);
    }

    const req = https.get(url, (res) => {
      response = res;
      res.on('error', (err) => finish(err));
      const redirectCodes = new Set([301, 302, 303, 307, 308]);
      if (redirectCodes.has(res.statusCode)) {
        const location = res.headers.location;
        if (!location) {
          res.destroy();
          finish(new Error(`Redirect with no location for ${url}`));
          return;
        }
        let next;
        try {
          next = validateUrl(new URL(location, url).href);
        } catch (err) {
          res.destroy();
          finish(err);
          return;
        }
        res.destroy();
        finish(null, { redirect: next });
        return;
      }
      if (res.statusCode !== 200) {
        res.destroy();
        finish(new Error(`HTTP ${res.statusCode} for ${url}`));
        return;
      }

      let received = 0;
      res.on('data', (chunk) => {
        received += chunk.length;
        if (maxBytes && received > maxBytes) {
          res.destroy(new Error(`Download exceeds the ${maxBytes}-byte safety limit`));
        }
      });
      const file = fs.createWriteStream(dest, { flags: 'w' });
      pipeline(res, file, (err) => {
        finish(err, { redirect: null });
      });
    });
    req.on('error', (err) => finish(err));
    timer = setTimeout(() => {
      const err = new Error(`Download hop timed out after ${DOWNLOAD_HOP_TIMEOUT_MS} ms: ${url}`);
      req.destroy(err);
      if (response) response.destroy(err);
    }, DOWNLOAD_HOP_TIMEOUT_MS);
  });
}

async function download(rawUrl, dest, maxBytes = 0) {
  let url = validateUrl(rawUrl);
  for (let redirects = 0; redirects <= MAX_REDIRECTS; redirects += 1) {
    const result = await downloadHop(url, dest, maxBytes);
    if (!result.redirect) return;
    if (redirects === MAX_REDIRECTS) throw new Error('Too many redirects');
    url = result.redirect;
  }
}

function parseExpectedChecksum(manifest, archiveName) {
  let expected = null;
  for (const line of manifest.split('\n')) {
    const fields = line.trim().split(/\s+/);
    if (fields.length < 2) continue;
    if (fields[1] !== archiveName && fields[1] !== `*${archiveName}`) continue;

    const digest = fields[0].toLowerCase();
    if (!/^[0-9a-f]{64}$/.test(digest)) {
      throw new Error(`Invalid SHA-256 checksum for ${archiveName}`);
    }
    if (expected !== null && expected !== digest) {
      throw new Error(`Conflicting SHA-256 checksums for ${archiveName}`);
    }
    expected = digest;
  }
  if (expected === null) {
    throw new Error(`No checksum for ${archiveName} in checksums.txt`);
  }
  return expected;
}

function verifyCandidate(candidatePath) {
  execFileSync(candidatePath, ['--version'], {
    stdio: ['ignore', 'pipe', 'pipe'],
    timeout: CANDIDATE_TIMEOUT_MS,
    windowsHide: true,
  });
}

function extractZipOnWindows(archivePath, destPath) {
  // -EncodedCommand is a constant program. Paths travel only through the child
  // environment and are consumed with -LiteralPath, so PowerShell never parses
  // user/TEMP path bytes as source code or wildcard syntax.
  const script = [
    "$ErrorActionPreference = 'Stop'",
    'Expand-Archive -LiteralPath $env:CBM_NPM_ARCHIVE_PATH ' +
      '-DestinationPath $env:CBM_NPM_DEST_PATH -Force',
  ].join('; ');
  const encoded = Buffer.from(script, 'utf16le').toString('base64');
  execFileSync('powershell', [
    '-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded,
  ], {
    env: {
      ...process.env,
      CBM_NPM_ARCHIVE_PATH: archivePath,
      CBM_NPM_DEST_PATH: destPath,
    },
    stdio: 'inherit',
    windowsHide: true,
  });
}

// Fetch checksums.txt and verify the archive hash.
async function verifyChecksum(archivePath, archiveName) {
  const url = `https://github.com/${REPO}/releases/download/v${VERSION}/checksums.txt`;
  const tmpChecksums = archivePath + '.checksums';
  try {
    await download(url, tmpChecksums, MAX_CHECKSUM_MANIFEST_BYTES);
    const manifestSize = fs.statSync(tmpChecksums).size;
    if (manifestSize > MAX_CHECKSUM_MANIFEST_BYTES) {
      throw new Error('checksums.txt exceeds the 1 MiB safety limit');
    }
    const expected = parseExpectedChecksum(
      fs.readFileSync(tmpChecksums, 'utf-8'), archiveName,
    );
    const actual = crypto
      .createHash('sha256')
      .update(fs.readFileSync(archivePath))
      .digest('hex');
    if (expected !== actual) {
      throw new Error(
        `Checksum mismatch for ${archiveName}:\n  expected: ${expected}\n  actual:   ${actual}`,
      );
    }
    process.stdout.write('codebase-memory-mcp: checksum verified.\n');
  } finally {
    try { fs.unlinkSync(tmpChecksums); } catch (_) { /* ignore */ }
  }
}

async function main() {
  const platform = getPlatform();
  const arch = getArch();
  const ext = platform === 'windows' ? 'zip' : 'tar.gz';
  const binName = platform === 'windows' ? 'codebase-memory-mcp.exe' : 'codebase-memory-mcp';
  const binPath = path.join(BIN_DIR, binName);

  if (fs.existsSync(binPath)) {
    try {
      verifyCandidate(binPath);
      return; // already installed and runnable, nothing to do
    } catch (_) {
      fs.rmSync(binPath, { force: true });
    }
  }

  fs.mkdirSync(BIN_DIR, { recursive: true });

  // Linux ships a fully-static "-portable" build; the standard linux binary
  // dynamically links glibc 2.38+ and fails on older distros. macOS/Windows
  // have no such variant. Keep in sync with install.sh / pypi _cli.py / cli.c.
  const variant = platform === 'linux' ? '-portable' : '';
  // Opt into the UI build (embedded graph visualization) with CBM_VARIANT=ui.
  // Default is the standard (headless) build. Mirrors install.sh --ui.
  const ui = (process.env.CBM_VARIANT || '').toLowerCase() === 'ui' ? 'ui-' : '';
  const archive = `codebase-memory-mcp-${ui}${platform}-${arch}${variant}.${ext}`;
  const url = `https://github.com/${REPO}/releases/download/v${VERSION}/${archive}`;

  const uiLabel = ui ? '(ui) ' : '';
  process.stdout.write(`codebase-memory-mcp: downloading v${VERSION} ${uiLabel}for ${platform}/${arch}...\n`);

  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'cbm-install-'));
  const tmpArchive = path.join(tmpDir, `cbm.${ext}`);

  try {
    await download(url, tmpArchive);
    await verifyChecksum(tmpArchive, archive);

    // Extract using execFileSync (array args — no shell injection).
    if (ext === 'tar.gz') {
      execFileSync('tar', ['-xzf', tmpArchive, '-C', tmpDir, '--no-same-owner']);
    } else {
      extractZipOnWindows(tmpArchive, tmpDir);
    }

    // Validate extracted path doesn't escape tmpDir (tar-slip defense).
    const extracted = path.join(tmpDir, binName);
    const resolvedExtracted = path.resolve(extracted);
    const resolvedTmpDir = path.resolve(tmpDir);
    if (!resolvedExtracted.startsWith(resolvedTmpDir + path.sep)) {
      throw new Error(`Path traversal detected in archive: ${binName}`);
    }
    if (!fs.existsSync(extracted)) {
      throw new Error(`Binary not found after extraction at ${extracted}`);
    }

    fs.chmodSync(extracted, 0o755);
    verifyCandidate(extracted);

    const stagedSuffix = platform === 'windows' ? '.tmp.exe' : '.tmp';
    const staged = path.join(
      BIN_DIR,
      `.${binName}.${process.pid}.${crypto.randomBytes(8).toString('hex')}${stagedSuffix}`,
    );
    try {
      fs.copyFileSync(extracted, staged, fs.constants.COPYFILE_EXCL);
      fs.chmodSync(staged, 0o755);
      verifyCandidate(staged);
      fs.renameSync(staged, binPath);
    } finally {
      try { fs.unlinkSync(staged); } catch (_) { /* renamed or never created */ }
    }

    process.stdout.write('codebase-memory-mcp: ready.\n');
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

main().catch((err) => {
  process.stderr.write(`\ncodebase-memory-mcp: install failed — ${err.message}\n`);
  process.stderr.write(`You can install manually: https://github.com/${REPO}#installation\n`);
  // Non-fatal: don't block the rest of npm install
  process.exit(0);
});
