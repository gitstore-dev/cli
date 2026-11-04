// Build/API experiment only. Not a production registration, helper or C API.
package main

import (
	"archive/tar"
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync/atomic"
	"time"

	"github.com/google/go-containerregistry/pkg/authn"
	"github.com/google/go-containerregistry/pkg/name"
	"github.com/google/go-containerregistry/pkg/registry"
	v1 "github.com/google/go-containerregistry/pkg/v1"
	"github.com/google/go-containerregistry/pkg/v1/empty"
	"github.com/google/go-containerregistry/pkg/v1/layout"
	"github.com/google/go-containerregistry/pkg/v1/mutate"
	"github.com/google/go-containerregistry/pkg/v1/remote"
	"github.com/google/go-containerregistry/pkg/v1/tarball"
	"github.com/google/go-containerregistry/pkg/v1/types"
	rspec "github.com/opencontainers/runtime-spec/specs-go"
	"github.com/opencontainers/umoci"
	"github.com/opencontainers/umoci/oci/layer"
)

func makeLayer(files map[string]string) (v1.Layer, error) {
	var buffer bytes.Buffer
	writer := tar.NewWriter(&buffer)
	keys := make([]string, 0, len(files))
	for path := range files {
		keys = append(keys, path)
	}
	sort.Strings(keys)
	for _, path := range keys {
		content := files[path]
		if err := writer.WriteHeader(&tar.Header{Name: path, Mode: 0600, Size: int64(len(content)), Uid: 0, Gid: 0}); err != nil {
			return nil, err
		}
		if _, err := writer.Write([]byte(content)); err != nil {
			return nil, err
		}
	}
	if err := writer.Close(); err != nil {
		return nil, err
	}
	return tarball.LayerFromReader(bytes.NewReader(buffer.Bytes()), tarball.WithMediaType(types.OCILayer))
}
func fixtureImage() (v1.Image, error) {
	first, err := makeLayer(map[string]string{"retained": "one", "gone": "old"})
	if err != nil {
		return nil, err
	}
	second, err := makeLayer(map[string]string{"retained": "two", ".wh.gone": ""})
	if err != nil {
		return nil, err
	}
	image, err := mutate.AppendLayers(empty.Image, first, second)
	if err != nil {
		return nil, err
	}
	image = mutate.MediaType(image, types.OCIManifestSchema1)
	image = mutate.ConfigMediaType(image, types.OCIConfigJSON)
	config, err := image.ConfigFile()
	if err != nil {
		return nil, err
	}
	config.OS = "linux"
	config.Architecture = "arm64"
	image, err = mutate.ConfigFile(image, config)
	if err != nil {
		return nil, err
	}

	return image, nil
}
func selfTest() error {
	root, err := os.MkdirTemp("", "gitstore-image-library-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(root)
	image, err := fixtureImage()
	if err != nil {
		return err
	}
	hash, err := image.Digest()
	if err != nil {
		return err
	}
	handler := registry.New(registry.Logger(log.New(io.Discard, "", 0)))
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		user, password, ok := r.BasicAuth()
		if !ok || user != "synthetic" || password != "fixture-only" {
			w.Header().Set("WWW-Authenticate", `Basic realm="isolated"`)
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		handler.ServeHTTP(w, r)
	}))
	defer server.Close()
	tag, err := name.NewTag(strings.TrimPrefix(server.URL, "http://")+"/fixture:owned", name.Insecure)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	options := []remote.Option{remote.WithContext(ctx), remote.WithAuth(authn.FromConfig(authn.AuthConfig{Username: "synthetic", Password: "fixture-only"})), remote.WithTransport(server.Client().Transport)}
	if err = remote.Write(tag, image, options...); err != nil {
		return err
	}
	reference := tag.Context().Digest(hash.String())
	downloaded, err := remote.Image(reference, options...)
	if err != nil {
		return err
	}
	actual, err := downloaded.Digest()
	if err != nil || actual != hash {
		return fmt.Errorf("digest mismatch")
	}
	directory, err := layout.Write(filepath.Join(root, "layout"), empty.Index)
	if err != nil {
		return err
	}
	if err = directory.AppendImage(downloaded, layout.WithAnnotations(map[string]string{"org.opencontainers.image.ref.name": "fixture"})); err != nil {
		return err
	}
	engine, err := umoci.OpenLayout(string(directory))
	if err != nil {
		return err
	}
	defer engine.Close()
	bundle := filepath.Join(root, "bundle")
	if err = umoci.Unpack(engine, "fixture", bundle, layer.UnpackOptions{OnDiskFormat: layer.DirRootfs{MapOptions: layer.MapOptions{Rootless: true, UIDMappings: []rspec.LinuxIDMapping{{ContainerID: 0, HostID: uint32(os.Getuid()), Size: 1}}, GIDMappings: []rspec.LinuxIDMapping{{ContainerID: 0, HostID: uint32(os.Getgid()), Size: 1}}}}}); err != nil {
		return err
	}
	content, err := os.ReadFile(filepath.Join(bundle, "rootfs/retained"))
	if err != nil || string(content) != "two" {
		return fmt.Errorf("layer replacement failed")
	}
	if _, err = os.Lstat(filepath.Join(bundle, "rootfs/gone")); !os.IsNotExist(err) {
		return fmt.Errorf("whiteout failed")
	}
	return nil
}

func serveFixture(mode string) error {
	image, err := fixtureImage()
	if err != nil {
		return err
	}
	hash, err := image.Digest()
	if err != nil {
		return err
	}
	handler := registry.New(registry.Logger(log.New(io.Discard, "", 0)))
	var active atomic.Bool
	var tokenObserved, scopeObserved atomic.Bool
	var server *httptest.Server
	server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if active.Load() && mode != "basic" {
			if r.URL.Path == "/token" {
				tokenObserved.Store(true)
				if r.URL.Query().Get("scope") == "repository:fixture:pull" {
					scopeObserved.Store(true)
				}
				if mode == "scope-denied" {
					w.WriteHeader(http.StatusForbidden)
					return
				}
				w.Header().Set("Content-Type", "application/json")
				json.NewEncoder(w).Encode(map[string]any{"token": "expired-fixture-only", "expires_in": 1,
					"issued_at": "2000-01-01T00:00:00Z"})
				return
			}
			// Expired bearer credentials are never accepted, even if the client
			// retries. Scope denial happens only at the token issuer above.
			w.Header().Set("WWW-Authenticate", fmt.Sprintf(`Bearer realm="%s/token",service="isolated"`, server.URL))
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		user, password, ok := r.BasicAuth()
		if !ok || user != "synthetic" || password != "fixture-only" {
			w.Header().Set("WWW-Authenticate", `Basic realm="isolated"`)
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		handler.ServeHTTP(w, r)
	}))
	defer server.Close()
	tag, err := name.NewTag(strings.TrimPrefix(server.URL, "http://")+"/fixture:owned", name.Insecure)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err = remote.Write(tag, image, remote.WithContext(ctx), remote.WithAuth(authn.FromConfig(authn.AuthConfig{Username: "synthetic", Password: "fixture-only"})), remote.WithTransport(server.Client().Transport)); err != nil {
		return err
	}
	configDigest, err := image.ConfigName()
	if err != nil {
		return err
	}
	active.Store(true)
	if err = json.NewEncoder(os.Stdout).Encode(map[string]string{"reference": tag.Context().Digest(hash.String()).Name(), "configDigest": configDigest.String()}); err != nil {
		return err
	}
	_, err = os.Stdin.Read(make([]byte, 1))
	if err == io.EOF {
		if mode != "basic" && (!tokenObserved.Load() || !scopeObserved.Load()) {
			return fmt.Errorf("intended token scope was not observed")
		}
		return nil
	}
	return err
}

func fixtureTLSServer(handler http.Handler) (*httptest.Server, string, error) {
	rootKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, "", err
	}
	root := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "Owned fixture CA"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(24 * time.Hour),
		BasicConstraintsValid: true, IsCA: true, KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageCRLSign}
	rootDER, err := x509.CreateCertificate(rand.Reader, root, root, &rootKey.PublicKey, rootKey)
	if err != nil {
		return nil, "", err
	}
	issuer, err := x509.ParseCertificate(rootDER)
	if err != nil {
		return nil, "", err
	}
	leafKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, "", err
	}
	leaf := &x509.Certificate{SerialNumber: big.NewInt(2), Subject: pkix.Name{CommonName: "localhost"},
		NotBefore: root.NotBefore, NotAfter: root.NotAfter, BasicConstraintsValid: true,
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames: []string{"localhost"}, IPAddresses: []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")}}
	leafDER, err := x509.CreateCertificate(rand.Reader, leaf, issuer, &leafKey.PublicKey, rootKey)
	if err != nil {
		return nil, "", err
	}
	server := httptest.NewUnstartedServer(handler)
	server.TLS = &tls.Config{Certificates: []tls.Certificate{{Certificate: [][]byte{leafDER}, PrivateKey: leafKey}}, MinVersion: tls.VersionTLS12}
	server.StartTLS()
	return server, string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: rootDER})), nil
}

// TLS peers exist only for selected Rust transport tests. No product trust root
// or public registry endpoint is changed by this developer fixture.
func servePolicyFixture(graphqlOnly bool) error {
	var leaked atomic.Bool
	var received atomic.Bool
	var graphqlReceived atomic.Bool
	destination, destinationCA, err := fixtureTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		received.Store(true)
		if r.Header.Get("Authorization") != "" {
			leaked.Store(true)
		}
		w.Write([]byte("owned-destination"))
	}))
	if err != nil {
		return err
	}
	defer destination.Close()
	var source *httptest.Server
	var sourceCA string
	source, sourceCA, err = fixtureTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/graphql":
			graphqlReceived.Store(true)
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(`{"data":{"__typename":"Query"}}`))
		case "/redirect":
			http.Redirect(w, r, destination.URL+"/final", http.StatusTemporaryRedirect)
		case "/downgrade":
			http.Redirect(w, r, "http://"+strings.TrimPrefix(destination.URL, "https://")+"/final", http.StatusTemporaryRedirect)
		case "/foreign":
			http.Redirect(w, r, "https://unapproved.invalid/final", http.StatusTemporaryRedirect)
		case "/loop":
			http.Redirect(w, r, source.URL+"/loop", http.StatusTemporaryRedirect)
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	if err != nil {
		return err
	}
	defer source.Close()
	certificates := []string{sourceCA, destinationCA}
	if err := json.NewEncoder(os.Stdout).Encode(map[string]any{"source": source.URL, "destination": destination.URL, "certificates": certificates}); err != nil {
		return err
	}
	_, err = os.Stdin.Read(make([]byte, 1))
	if err != io.EOF {
		return fmt.Errorf("fixture input did not close")
	}
	if leaked.Load() || (graphqlOnly && !graphqlReceived.Load()) || (!graphqlOnly && !received.Load()) {
		return fmt.Errorf("redirect isolation assertion failed")
	}
	return nil
}

func runSelfTest() int {
	if err := selfTest(); err != nil {
		fmt.Fprintln(os.Stderr, "image-library experiment:", err)
		return 1
	}
	return 0
}
func main() {
	if len(os.Args) == 4 && os.Args[1] == "optional-evidence" {
		if err := optionalEvidence(os.Args[2], os.Args[3]); err != nil {
			fmt.Fprintln(os.Stderr, "optional metadata evidence failed")
			os.Exit(1)
		}
		return
	}
	if len(os.Args) == 4 && os.Args[1] == "routing-evidence" {
		if err := routingEvidence(os.Args[2], os.Args[3]); err != nil {
			fmt.Fprintln(os.Stderr, "Public routing evidence failed:", err)
			os.Exit(1)
		}
		fmt.Println("Recorded public HTTPS origins only; no layer integrity or execution claim")
		return
	}
	if len(os.Args) == 4 && os.Args[1] == "manifest-evidence" {
		ref, err := name.NewDigest(os.Args[2], name.StrictValidation)
		if err != nil || ref.RegistryStr() != "ghcr.io" {
			fmt.Fprintln(os.Stderr, "Select an immutable public GHCR evidence reference")
			os.Exit(2)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		transport := http.DefaultTransport.(*http.Transport).Clone()
		transport.Proxy = nil
		descriptor, err := remote.Get(ref, remote.WithContext(ctx), remote.WithAuth(authn.Anonymous), remote.WithTransport(transport))
		if err != nil || len(descriptor.Manifest) > 1024*1024 || descriptor.Digest.String() != ref.DigestStr() {
			fmt.Fprintln(os.Stderr, "Public manifest evidence acquisition failed")
			os.Exit(1)
		}
		hash, _, err := v1.SHA256(bytes.NewReader(descriptor.Manifest))
		if err != nil || hash.String() != ref.DigestStr() {
			fmt.Fprintln(os.Stderr, "Public manifest evidence digest mismatch")
			os.Exit(1)
		}
		file, err := os.OpenFile(os.Args[3], os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err == nil {
			_, err = file.Write(descriptor.Manifest)
			closeErr := file.Close()
			if err == nil {
				err = closeErr
			}
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, "Cannot write new owned evidence file")
			os.Exit(1)
		}
		fmt.Println("Recorded digest-verified public image manifest metadata only")
		return
	}
	if len(os.Args) == 2 && os.Args[1] == "serve-policy-fixture" {
		if err := servePolicyFixture(false); err != nil {
			fmt.Fprintln(os.Stderr, "policy fixture failed")
			os.Exit(1)
		}
		return
	}
	if len(os.Args) == 2 && os.Args[1] == "serve-cli-tls-fixture" {
		if err := servePolicyFixture(true); err != nil {
			fmt.Fprintln(os.Stderr, "CLI TLS fixture failed")
			os.Exit(1)
		}
		return
	}
	if len(os.Args) == 2 && os.Args[1] == "serve-fixture" {
		if err := serveFixture("basic"); err != nil {
			fmt.Fprintln(os.Stderr, "fixture startup failed:", err)
			os.Exit(1)
		}
		return
	}
	if len(os.Args) == 3 && os.Args[1] == "serve-auth-fixture" &&
		(os.Args[2] == "expired" || os.Args[2] == "scope-denied") {
		if err := serveFixture(os.Args[2]); err != nil {
			fmt.Fprintln(os.Stderr, "auth fixture startup failed")
			os.Exit(1)
		}
		return
	}
	if len(os.Args) == 2 && os.Args[1] == "selftest" {
		os.Exit(runSelfTest())
	}
	fmt.Println("Image API/size experiment; use selftest for isolated registry and rootfs checks")
}
