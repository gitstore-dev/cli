// Developer fixture producer only; no image extraction or product registration.
package main

import (
	"archive/tar"
	"fmt"
	"github.com/google/go-containerregistry/pkg/v1/empty"
	"github.com/google/go-containerregistry/pkg/v1/layout"
	"github.com/google/go-containerregistry/pkg/v1/mutate"
	"github.com/google/go-containerregistry/pkg/v1/tarball"
	"github.com/google/go-containerregistry/pkg/v1/types"
	"io"
	"io/fs"
	"os"
	"path/filepath"
)

func run() error {
	if len(os.Args) != 3 {
		return fmt.Errorf("usage: fixture-image OWNED_ROOTFS_TAR NEW_OCI_TAR")
	}
	entry, err := tarball.LayerFromFile(os.Args[1], tarball.WithMediaType(types.OCILayer))
	if err != nil {
		return err
	}
	image, err := mutate.AppendLayers(empty.Image, entry)
	if err != nil {
		return err
	}
	image = mutate.MediaType(image, types.OCIManifestSchema1)
	image = mutate.ConfigMediaType(image, types.OCIConfigJSON)
	config, err := image.ConfigFile()
	if err != nil {
		return err
	}
	config.OS = "linux"
	config.Architecture = "arm64"
	config.Config.Cmd = []string{"/sbin/init"}
	config.Config.Env = []string{"container=container", "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"}
	image, err = mutate.ConfigFile(image, config)
	if err != nil {
		return err
	}
	root, err := os.MkdirTemp("", "gitstore-fixture-layout-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(root)
	directory, err := layout.Write(root, empty.Index)
	if err != nil {
		return err
	}
	if err = directory.AppendImage(image, layout.WithAnnotations(map[string]string{"org.opencontainers.image.ref.name": "docker.io/library/gitstore-m2-fixture:a1a13c-ubuntu2404-aclcompat"})); err != nil {
		return err
	}
	output, err := os.OpenFile(os.Args[2], os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer output.Close()
	writer := tar.NewWriter(output)
	err = filepath.WalkDir(root, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path == root {
			return nil
		}
		stat, err := entry.Info()
		if err != nil {
			return err
		}
		header, err := tar.FileInfoHeader(stat, "")
		if err != nil {
			return err
		}
		header.Name, err = filepath.Rel(root, path)
		if err != nil {
			return err
		}
		header.Name = filepath.ToSlash(header.Name)
		if err = writer.WriteHeader(header); err != nil {
			return err
		}
		if entry.IsDir() {
			return nil
		}
		input, err := os.Open(path)
		if err != nil {
			return err
		}
		defer input.Close()
		_, err = io.Copy(writer, input)
		return err
	})
	if err != nil {
		return err
	}
	if err = writer.Close(); err != nil {
		return err
	}
	return output.Sync()
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
