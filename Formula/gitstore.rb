class Gitstore < Formula
  desc "GitStore early-alpha CLI and user daemon"
  homepage "https://github.com/gitstore-dev/cli"
  version "0.1.0-alpha.12"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.12/gitstore-0.1.0-alpha.12-darwin-arm64.tar.gz"
      sha256 "2a239323f8644463c3591c8f48435673592839aa57f6bf8b4543d2ee4b812e9d"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.12/gitstore-0.1.0-alpha.12-darwin-x64.tar.gz"
      sha256 "4e2852928a2d689fd2e2c9c57730bbbd222ad36a388f98028c4d7841d00e5996"
    end
  end
  on_linux do
    depends_on "ca-certificates"
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.12/gitstore-0.1.0-alpha.12-linux-arm64.tar.gz"
      sha256 "d398514edf0f84a62c05c996a5ae6ef7fa5ca8f22f940d6e3c97705d32ae5890"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.12/gitstore-0.1.0-alpha.12-linux-x64.tar.gz"
      sha256 "df9765f85a3496734cdd67c1d65e0c6d7a3720b52ac6fcf0596458f98d0afe38"
    end
  end

  def install
    libexec.install "bin", "share"
    libexec.install "lib" if File.directory?("lib")
    bin.install_symlink libexec/"bin/gs", libexec/"bin/gitstored"
    bash_completion.install_symlink libexec/"share/completions/gs.bash" => "gs"
    zsh_completion.install_symlink libexec/"share/completions/gs.zsh" => "_gs"
    fish_completion.install_symlink libexec/"share/completions/gs.fish"
  end

  def caveats
    <<~EOS
      Early alpha: upstream schemas change incompatibly. Upgrade compatibility,
      data preservation and recovery are not guaranteed. Keep independent backups;
      do not use irreplaceable data. Consumer attestation verification is optional.
      First local use launches the matching user daemon on qualified Linux hosts.
      macOS provides remote/control commands; local workload execution is unavailable.
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/gs --version")
    assert_match version.to_s, shell_output("#{bin}/gitstored --version")
  end
end
