package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"sort"
	"sync"
	"time"

	"github.com/google/go-containerregistry/pkg/authn"
	"github.com/google/go-containerregistry/pkg/name"
	"github.com/google/go-containerregistry/pkg/v1/remote"
)

// Developer discovery only: record origins, never paths, query strings, tokens or
// authorization headers. Product policy still requires exact registered origins.
type routingTransport struct {
	base      http.RoundTripper
	mu        sync.Mutex
	origins   map[string]bool
	redirects map[string]bool
}

func (t *routingTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	if request.URL.Scheme != "https" || request.URL.User != nil {
		return nil, errors.New("HTTPS required")
	}
	t.mu.Lock()
	t.origins[request.URL.Scheme+"://"+request.URL.Host] = true
	t.mu.Unlock()
	response, err := t.base.RoundTrip(request)
	if err == nil && response.StatusCode >= 300 && response.StatusCode < 400 {
		target, parseErr := response.Location()
		if parseErr == nil {
			if target.Scheme != "https" || target.User != nil {
				response.Body.Close()
				return nil, errors.New("HTTPS redirect required")
			}
			t.mu.Lock()
			t.redirects[target.Scheme+"://"+target.Host] = true
			t.mu.Unlock()
		}
	}
	return response, err
}
func routingEvidence(reference, output string) error {
	ref, err := name.NewDigest(reference, name.StrictValidation)
	if err != nil {
		return errors.New("immutable registered public reference required")
	}
	scylla := ref.RegistryStr() == "index.docker.io" && ref.RepositoryStr() == "scylladb/scylla" &&
		(ref.DigestStr() == "sha256:e65d9d7113cb5b324baa4cf982ab27f9ace4a2f11666942bd9a8c09293297768" ||
			ref.DigestStr() == "sha256:474d2cd76dc7d2edd6754ce82fd372a8bf8cce9de4bef000c55bcc843e3761a6")
	if ref.RegistryStr() != "ghcr.io" && !scylla {
		return errors.New("immutable GHCR or registered Scylla reference required")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	base := http.DefaultTransport.(*http.Transport).Clone()
	base.Proxy = nil
	transport := &routingTransport{base: base, origins: map[string]bool{}, redirects: map[string]bool{}}
	image, err := remote.Image(ref, remote.WithContext(ctx), remote.WithAuth(authn.Anonymous), remote.WithTransport(transport))
	if err != nil {
		return errors.New("public image metadata unavailable")
	}
	if _, err = image.RawConfigFile(); err != nil {
		return errors.New("public image config unavailable")
	}
	layers, err := image.Layers()
	if err != nil || len(layers) == 0 {
		return errors.New("public image layers unavailable")
	}
	blob, err := layers[0].Compressed()
	if err != nil {
		return errors.New("public layer routing unavailable")
	}
	_, err = io.CopyN(io.Discard, blob, 1)
	closeErr := blob.Close()
	if err != nil || closeErr != nil {
		return errors.New("public layer routing observation failed")
	}
	origins, redirects := []string{}, []string{}
	transport.mu.Lock()
	for origin := range transport.origins {
		origins = append(origins, origin)
	}
	for origin := range transport.redirects {
		redirects = append(redirects, origin)
	}
	transport.mu.Unlock()
	sort.Strings(origins)
	sort.Strings(redirects)
	record := map[string]any{"reference": reference, "observedAt": time.Now().UTC().Format(time.RFC3339),
		"requestOrigins": origins, "redirectOrigins": redirects, "auth": "explicit anonymous; ambient helpers disabled",
		"scope": "metadata and first byte of first compressed layer; no full layer digest/unpack/execution claim"}
	bytes, err := json.MarshalIndent(record, "", "  ")
	if err != nil {
		return err
	}
	file, err := os.OpenFile(output, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return errors.New("new evidence file required")
	}
	_, err = file.Write(append(bytes, '\n'))
	closeErr = file.Close()
	if err != nil || closeErr != nil {
		return errors.New("cannot persist routing evidence")
	}
	return nil
}
