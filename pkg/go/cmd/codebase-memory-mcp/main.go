// codebase-memory-mcp — Go installer wrapper.
//
// On first run, downloads the pre-built binary for the current platform from
// GitHub Releases, caches it, and replaces the current process with it.
// Subsequent runs skip directly to exec.
//
// Install:
//
//	go install github.com/DeusData/codebase-memory-mcp/pkg/go/cmd/codebase-memory-mcp@latest
package main

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
)

const (
	repo    = "DeusData/codebase-memory-mcp"
	version = "0.8.1"

	maxRedirects            = 5
	requestTimeout          = 2 * time.Minute
	connectTimeout          = 15 * time.Second
	responseHeaderTimeout   = 30 * time.Second
	candidateTimeout        = 15 * time.Second
	maxChecksumManifestSize = 1024 * 1024
)

func main() {
	bin, err := ensureBinary()
	if err != nil {
		fmt.Fprintf(os.Stderr, "codebase-memory-mcp: %v\n", err)
		os.Exit(1)
	}
	if err := execBinary(bin, os.Args[1:]); err != nil {
		fmt.Fprintf(os.Stderr, "codebase-memory-mcp: %v\n", err)
		os.Exit(1)
	}
}

func ensureBinary() (string, error) {
	bin := binPath()
	if _, err := os.Stat(bin); err == nil {
		return bin, nil
	}
	if err := download(bin); err != nil {
		return "", err
	}
	return bin, nil
}

func binPath() string {
	name := "codebase-memory-mcp"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	return filepath.Join(cacheDir(), version, name)
}

func cacheDir() string {
	if d := os.Getenv("CBM_CACHE_DIR"); d != "" {
		return d
	}
	switch runtime.GOOS {
	case "windows":
		if d := os.Getenv("LOCALAPPDATA"); d != "" {
			return filepath.Join(d, "codebase-memory-mcp")
		}
	case "darwin":
		if home, err := os.UserHomeDir(); err == nil {
			return filepath.Join(home, "Library", "Caches", "codebase-memory-mcp")
		}
	}
	if d := os.Getenv("XDG_CACHE_HOME"); d != "" {
		return filepath.Join(d, "codebase-memory-mcp")
	}
	if home, err := os.UserHomeDir(); err == nil {
		return filepath.Join(home, ".cache", "codebase-memory-mcp")
	}
	return filepath.Join(os.TempDir(), "codebase-memory-mcp")
}

func goos() string {
	switch runtime.GOOS {
	case "darwin":
		return "darwin"
	case "linux":
		return "linux"
	case "windows":
		return "windows"
	default:
		return runtime.GOOS
	}
}

func goarch() string {
	switch runtime.GOARCH {
	case "amd64":
		return "amd64"
	case "arm64":
		return "arm64"
	default:
		return runtime.GOARCH
	}
}

func download(dest string) error {
	platform := goos()
	arch := goarch()
	ext := "tar.gz"
	if platform == "windows" {
		ext = "zip"
	}

	portable := ""
	if platform == "linux" {
		portable = "-portable"
	}
	archive := fmt.Sprintf("codebase-memory-mcp-%s-%s%s.%s", platform, arch, portable, ext)
	url := fmt.Sprintf("https://github.com/%s/releases/download/v%s/%s", repo, version, archive)
	checksumURL := fmt.Sprintf("https://github.com/%s/releases/download/v%s/checksums.txt", repo, version)

	fmt.Fprintf(os.Stderr, "codebase-memory-mcp: downloading v%s for %s/%s...\n", version, platform, arch)

	tmp, err := os.MkdirTemp("", "cbm-install-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)

	archivePath := filepath.Join(tmp, "cbm."+ext)
	if err := httpGet(url, archivePath); err != nil {
		return fmt.Errorf("download failed: %w", err)
	}

	// A release binary is executable input, so checksum verification is a
	// mandatory precondition rather than a best-effort warning.
	checksums, err := fetchChecksums(checksumURL)
	if err != nil {
		return fmt.Errorf("checksum manifest unavailable: %w", err)
	}
	expected, ok := checksums[archive]
	if !ok {
		return fmt.Errorf("checksum manifest has no entry for %s", archive)
	}
	if err := verifyChecksum(archivePath, expected); err != nil {
		return err
	}

	binName := "codebase-memory-mcp"
	if platform == "windows" {
		binName += ".exe"
	}

	if ext == "tar.gz" {
		if err := extractTarGz(archivePath, tmp, binName); err != nil {
			return fmt.Errorf("extraction failed: %w", err)
		}
	} else {
		if err := extractZip(archivePath, tmp, binName); err != nil {
			return fmt.Errorf("extraction failed: %w", err)
		}
	}
	extracted := filepath.Join(tmp, binName)
	if err := os.Chmod(extracted, 0755); err != nil {
		return fmt.Errorf("could not set candidate permissions: %w", err)
	}
	if err := verifyCandidate(extracted); err != nil {
		return err
	}

	if err := os.MkdirAll(filepath.Dir(dest), 0755); err != nil {
		return fmt.Errorf("could not create cache dir: %w", err)
	}

	if err := installCandidateAtomically(extracted, dest); err != nil {
		return fmt.Errorf("could not install binary: %w", err)
	}

	return nil
}

// validateURLScheme rejects non-https URLs before any fetch (defense-in-depth).
func validateURLScheme(rawURL string) error {
	parsed, err := url.Parse(rawURL)
	if err != nil || parsed.Scheme != "https" || parsed.Host == "" || parsed.User != nil {
		return fmt.Errorf("refusing non-https URL: %s", rawURL)
	}
	return nil
}

// httpsOnlyClient returns an HTTP client that rejects non-HTTPS redirects.
var httpsOnlyClient = &http.Client{
	Timeout: requestTimeout,
	Transport: &http.Transport{
		Proxy:                 http.ProxyFromEnvironment,
		DialContext:           (&net.Dialer{Timeout: connectTimeout}).DialContext,
		ForceAttemptHTTP2:     true,
		TLSHandshakeTimeout:   connectTimeout,
		ResponseHeaderTimeout: responseHeaderTimeout,
		ExpectContinueTimeout: time.Second,
	},
	CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if err := validateURLScheme(req.URL.String()); err != nil {
			return fmt.Errorf("refusing unsafe redirect: %w", err)
		}
		if len(via) > maxRedirects {
			return fmt.Errorf("too many redirects")
		}
		return nil
	},
}

func httpGet(rawURL, dest string) error {
	if err := validateURLScheme(rawURL); err != nil {
		return err
	}
	resp, err := httpsOnlyClient.Get(rawURL) //nolint:gosec
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %d for %s", resp.StatusCode, rawURL)
	}
	f, err := os.Create(dest)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(f, resp.Body)
	closeErr := f.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}

func fetchChecksums(url string) (map[string]string, error) {
	if err := validateURLScheme(url); err != nil {
		return nil, err
	}
	resp, err := httpsOnlyClient.Get(url) //nolint:gosec
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxChecksumManifestSize+1))
	if err != nil {
		return nil, err
	}
	if len(body) > maxChecksumManifestSize {
		return nil, fmt.Errorf("checksums.txt exceeds the 1 MiB safety limit")
	}
	result := make(map[string]string)
	for _, line := range strings.Split(string(body), "\n") {
		parts := strings.Fields(line)
		if len(parts) == 2 {
			name := parts[1]
			if strings.HasPrefix(name, "*") {
				name = name[1:]
			}
			digest := strings.ToLower(parts[0])
			if len(digest) != sha256.Size*2 {
				return nil, fmt.Errorf("invalid SHA-256 checksum for %s", name)
			}
			if _, err := hex.DecodeString(digest); err != nil {
				return nil, fmt.Errorf("invalid SHA-256 checksum for %s: %w", name, err)
			}
			if prior, exists := result[name]; exists && prior != digest {
				return nil, fmt.Errorf("conflicting SHA-256 checksums for %s", name)
			}
			result[name] = digest
		}
	}
	return result, nil
}

func verifyChecksum(path, expected string) error {
	expected = strings.ToLower(expected)
	if len(expected) != sha256.Size*2 {
		return fmt.Errorf("invalid SHA-256 checksum length")
	}
	if _, err := hex.DecodeString(expected); err != nil {
		return fmt.Errorf("invalid SHA-256 checksum: %w", err)
	}
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return err
	}
	actual := hex.EncodeToString(h.Sum(nil))
	if actual != expected {
		return fmt.Errorf("checksum mismatch: expected %s, got %s", expected, actual)
	}
	return nil
}

func extractTarGz(archivePath, destDir, targetFile string) error {
	f, err := os.Open(archivePath)
	if err != nil {
		return err
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	defer gz.Close()
	tr := tar.NewReader(gz)
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		if filepath.Base(hdr.Name) == targetFile {
			out, err := os.Create(filepath.Join(destDir, targetFile))
			if err != nil {
				return err
			}
			defer out.Close()
			_, err = io.Copy(out, tr) //nolint:gosec
			return err
		}
	}
	return fmt.Errorf("%s not found in archive", targetFile)
}

func extractZip(archivePath, destDir, targetFile string) error {
	r, err := zip.OpenReader(archivePath)
	if err != nil {
		return err
	}
	defer r.Close()
	for _, f := range r.File {
		if filepath.Base(f.Name) == targetFile {
			rc, err := f.Open()
			if err != nil {
				return err
			}
			defer rc.Close()
			out, err := os.Create(filepath.Join(destDir, targetFile))
			if err != nil {
				return err
			}
			defer out.Close()
			_, err = io.Copy(out, rc) //nolint:gosec
			return err
		}
	}
	return fmt.Errorf("%s not found in archive", targetFile)
}

func verifyCandidate(path string) error {
	ctx, cancel := context.WithTimeout(context.Background(), candidateTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, path, "--version")
	cmd.Stdin = nil
	cmd.Stdout = io.Discard
	cmd.Stderr = io.Discard
	if err := cmd.Run(); err != nil {
		if ctx.Err() != nil {
			return fmt.Errorf("downloaded binary verification timed out: %w", ctx.Err())
		}
		return fmt.Errorf("downloaded binary failed to run: %w", err)
	}
	return nil
}

func installCandidateAtomically(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	pattern := "." + filepath.Base(dst) + ".*.tmp"
	if runtime.GOOS == "windows" {
		pattern += ".exe"
	}
	out, err := os.CreateTemp(filepath.Dir(dst), pattern)
	if err != nil {
		return err
	}
	staged := out.Name()
	defer os.Remove(staged)
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	if err := out.Chmod(0755); err != nil {
		out.Close()
		return err
	}
	if err := out.Sync(); err != nil {
		out.Close()
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	if err := verifyCandidate(staged); err != nil {
		return err
	}
	if err := os.Rename(staged, dst); err != nil {
		return err
	}
	return nil
}

func execBinary(bin string, args []string) error {
	if runtime.GOOS == "windows" {
		cmd := exec.Command(bin, args...)
		cmd.Stdin = os.Stdin
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		return cmd.Run()
	}
	return syscall.Exec(bin, append([]string{bin}, args...), os.Environ())
}
