package main

// Developer-only metadata evidence. Never registered in gs or gitstored.
import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/google/go-containerregistry/pkg/authn"
	"github.com/google/go-containerregistry/pkg/name"
	v1 "github.com/google/go-containerregistry/pkg/v1"
	"github.com/google/go-containerregistry/pkg/v1/remote"
	"github.com/google/go-containerregistry/pkg/v1/remote/transport"
	"net/http"
	"os"
	"regexp"
	"time"
)

type optionalPlatform struct {
	Architecture   string `json:"architecture"`
	ActualPlatform string `json:"actualPlatform,omitempty"`
	Digest         string `json:"digest,omitempty"`
	ConfigDigest   string `json:"configDigest,omitempty"`
	Revision       string `json:"sourceRevision,omitempty"`
	User           string `json:"user,omitempty"`
	Entrypoint     string `json:"entrypoint,omitempty"`
	Command        string `json:"command,omitempty"`
	Status         string `json:"status"`
}

func optionalEvidence(reference, output string) error {
	allowed := map[string]bool{
		// Persistent-profile candidate; resolved digests remain disabled until native checks.
		"scylladb/scylla:2026.1":                         true,
		"ghcr.io/gitstore-dev/oidc-bridge:0.1.0-alpha.8": true,
		"ghcr.io/gitstore-dev/oidc-bridge:latest":        true,
		"postgres:16-alpine":                             true, "oryd/hydra:v2.3.0": true,
		"oryd/kratos:v1.3.1": true, "oryd/kratos-selfservice-ui-node:v1.3.1": true,
		"oryd/mailslurper:latest-smtps": true,
		// Candidate SMTP replacement; separately sourced, not an upstream reference input.
		"axllent/mailpit:v1.33.0": true,
	}
	if !allowed[reference] {
		return fmt.Errorf("select a committed profile-candidate image")
	}
	tag, err := name.NewTag(reference)
	if err != nil {
		return err
	}
	record := map[string]any{"schemaVersion": 1, "reference": reference, "status": "unavailable", "scope": "manifest/config metadata only; no layers, execution or attestation prerequisite"}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	network := http.DefaultTransport.(*http.Transport).Clone()
	network.Proxy = nil
	network.ResponseHeaderTimeout = 10 * time.Second
	options := []remote.Option{remote.WithContext(ctx), remote.WithAuth(authn.Anonymous), remote.WithTransport(network)}
	descriptor, err := remote.Get(tag, options...)
	if err != nil {
		var response *transport.Error
		if errors.As(err, &response) {
			record["httpStatus"] = response.StatusCode
		}
		record["reason"] = "anonymous metadata request unavailable; no ambient credential fallback"
	} else if len(descriptor.Manifest) > 1048576 {
		record["reason"] = "metadata exceeds selected bound"
	} else {
		record["resolvedDigest"] = descriptor.Digest.String()
		record["mediaType"] = descriptor.MediaType
		platforms := []optionalPlatform{}
		revisionPattern := regexp.MustCompile(`^[0-9a-f]{40}$`)
		ordinary := regexp.MustCompile(`^[a-zA-Z0-9_./:-]{1,256}$`)
		for _, architecture := range []string{"amd64", "arm64"} {
			item := optionalPlatform{Architecture: architecture, Status: "metadata-unavailable"}
			if descriptor.MediaType.IsIndex() {
				index, indexErr := descriptor.ImageIndex()
				if indexErr != nil {
					platforms = append(platforms, item)
					continue
				}
				manifest, manifestErr := index.IndexManifest()
				if manifestErr != nil {
					platforms = append(platforms, item)
					continue
				}
				present := false
				for _, entry := range manifest.Manifests {
					if entry.Platform != nil && entry.Platform.OS == "linux" && entry.Platform.Architecture == architecture {
						present = true
					}
				}
				if !present {
					item.Status = "platform-absent"
					platforms = append(platforms, item)
					continue
				}
			}
			selection := append(append([]remote.Option{}, options...), remote.WithPlatform(v1.Platform{OS: "linux", Architecture: architecture}))
			image, fetchErr := remote.Image(tag.Context().Digest(descriptor.Digest.String()), selection...)
			if fetchErr == nil {
				config, configErr := image.ConfigFile()
				digest, digestErr := image.Digest()
				configDigest, configDigestErr := image.ConfigName()
				if configErr == nil {
					item.ActualPlatform = config.OS + "/" + config.Architecture
					if config.OS != "linux" || config.Architecture != architecture {
						item.Status = "platform-mismatch"
					}
				}
				if configErr == nil && digestErr == nil && configDigestErr == nil && config.OS == "linux" && config.Architecture == architecture {
					item.Status = "metadata-verified"
					item.Digest = digest.String()
					item.ConfigDigest = configDigest.String()
					revision := config.Config.Labels["org.opencontainers.image.revision"]
					if revisionPattern.MatchString(revision) {
						item.Revision = revision
					}
					if ordinary.MatchString(config.Config.User) {
						item.User = config.Config.User
					}
					if len(config.Config.Entrypoint) > 0 && ordinary.MatchString(config.Config.Entrypoint[0]) {
						item.Entrypoint = config.Config.Entrypoint[0]
					}
					if len(config.Config.Cmd) > 0 && ordinary.MatchString(config.Config.Cmd[0]) {
						item.Command = config.Config.Cmd[0]
					}
				}
			}
			platforms = append(platforms, item)
		}
		record["platforms"] = platforms
		record["status"] = "metadata-reviewed"
	}
	file, err := os.OpenFile(output, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	defer file.Close()
	encoder := json.NewEncoder(file)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(record); err != nil {
		return err
	}
	if err := file.Sync(); err != nil {
		return err
	}
	fmt.Println("Recorded optional image metadata only")
	return nil
}
