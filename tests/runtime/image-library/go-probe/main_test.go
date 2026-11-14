package main

import (
	"archive/tar"
	"bytes"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/google/go-containerregistry/pkg/authn"
	"github.com/google/go-containerregistry/pkg/name"
	"github.com/google/go-containerregistry/pkg/v1/remote"
	rspec "github.com/opencontainers/runtime-spec/specs-go"
	"github.com/opencontainers/umoci/oci/layer"
)

func options() *layer.UnpackOptions {
	return &layer.UnpackOptions{OnDiskFormat: layer.DirRootfs{MapOptions: layer.MapOptions{Rootless: true,
		UIDMappings: []rspec.LinuxIDMapping{{ContainerID: 0, HostID: uint32(os.Getuid()), Size: 1}},
		GIDMappings: []rspec.LinuxIDMapping{{ContainerID: 0, HostID: uint32(os.Getgid()), Size: 1}}}}}
}
func TestPrivateRegistryDigestAndWhiteouts(t *testing.T) {
	if err := selfTest(); err != nil {
		t.Fatal(err)
	}
}
func TestMalformedLayerRefused(t *testing.T) {
	if err := layer.UnpackLayer(t.TempDir(), strings.NewReader("truncated-tar"), options()); err == nil {
		t.Fatal("malformed layer accepted")
	}
}
func TestTraversalAndSymlinksCannotEscapeOwnedRoot(t *testing.T) {
	parent := t.TempDir()
	root := filepath.Join(parent, "root")
	outside := filepath.Join(parent, "outside")
	if err := os.Mkdir(root, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(outside, 0700); err != nil {
		t.Fatal(err)
	}
	sentinel := filepath.Join(outside, "sentinel")
	if err := os.WriteFile(sentinel, []byte("preserve"), 0600); err != nil {
		t.Fatal(err)
	}
	var buffer bytes.Buffer
	writer := tar.NewWriter(&buffer)
	headers := []tar.Header{{Name: "link", Typeflag: tar.TypeSymlink, Linkname: outside, Mode: 0700}, {Name: "link/sentinel", Mode: 0600, Size: 7}, {Name: "../outside/escaped", Mode: 0600, Size: 7}}
	for _, header := range headers {
		if err := writer.WriteHeader(&header); err != nil {
			t.Fatal(err)
		}
		if header.Size > 0 {
			if _, err := io.WriteString(writer, "changed"); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	_ = layer.UnpackLayer(root, bytes.NewReader(buffer.Bytes()), options())
	content, err := os.ReadFile(sentinel)
	if err != nil || string(content) != "preserve" {
		t.Fatal("symlink escaped owned root")
	}
	if _, err := os.Stat(filepath.Join(outside, "escaped")); !os.IsNotExist(err) {
		t.Fatal("parent traversal escaped owned root")
	}
}
func TestExplicitAuthenticationNeverInvokesAmbientHelpers(t *testing.T) {
	t.Setenv("PATH", "/nonexistent")
	config := t.TempDir()
	t.Setenv("DOCKER_CONFIG", config)
	if err := os.WriteFile(filepath.Join(config, "config.json"), []byte(`{"credsStore":"unexpected-fixture-helper"}`), 0600); err != nil {
		t.Fatal(err)
	}
	calls := 0
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		w.Header().Set("WWW-Authenticate", `Basic realm="owned"`)
		w.WriteHeader(http.StatusUnauthorized)
	}))
	defer server.Close()
	ref, err := name.NewDigest(strings.TrimPrefix(server.URL, "http://")+"/fixture@sha256:"+strings.Repeat("a", 64), name.Insecure)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, err = remote.Image(ref, remote.WithContext(ctx), remote.WithAuth(authn.FromConfig(authn.AuthConfig{Username: "synthetic", Password: "never-print-this-fixture"})), remote.WithTransport(server.Client().Transport))
	if err == nil || calls == 0 {
		t.Fatal("denial not observed")
	}
	if strings.Contains(err.Error(), "never-print-this-fixture") {
		t.Fatal("secret in diagnostic")
	}
}

func TestRedirectDoesNotForwardCredentialsToAnotherPort(t *testing.T) {
	observed := make(chan string, 8)
	destination := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case observed <- r.Header.Get("Authorization"):
		default:
		}
		w.WriteHeader(http.StatusNotFound)
	}))
	defer destination.Close()
	origin := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if _, _, ok := r.BasicAuth(); !ok {
			w.Header().Set("WWW-Authenticate", `Basic realm="origin"`)
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		if r.URL.Path == "/v2/" {
			w.WriteHeader(http.StatusOK)
			return
		}
		http.Redirect(w, r, destination.URL+"/fixture", http.StatusFound)
	}))
	defer origin.Close()
	ref, err := name.NewDigest(strings.TrimPrefix(origin.URL, "http://")+"/fixture@sha256:"+strings.Repeat("a", 64), name.Insecure)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, _ = remote.Image(ref, remote.WithContext(ctx), remote.WithAuth(authn.FromConfig(authn.AuthConfig{Username: "synthetic", Password: "private-port-bound-fixture"})), remote.WithTransport(origin.Client().Transport))
	select {
	case header := <-observed:
		if header != "" {
			t.Fatal("authorization escaped the selected registry authority")
		}
	default:
		t.Fatal("redirect not exercised")
	}
}
