class Gitstore < Formula
  desc "GitStore early-alpha CLI and user daemon"
  homepage "https://github.com/gitstore-dev/cli"
  version "0.1.0-alpha.5"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.5/gitstore-0.1.0-alpha.5-darwin-arm64.tar.gz"
      sha256 "c09a8522fff9f936139ae55e95b339e25e39651ea056739e9c6f5c97dcfd74a9"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.5/gitstore-0.1.0-alpha.5-darwin-x64.tar.gz"
      sha256 "1045dfef77603452a65e333dc8f97e3becd252d74de86b2a0f7dcdca800ea77e"
    end
  end
  on_linux do
    depends_on "ca-certificates"
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.5/gitstore-0.1.0-alpha.5-linux-arm64.tar.gz"
      sha256 "58020ad2a69708303dda968b0db8cac1d9f90d57f2411fabaddf9e67cb529047"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.5/gitstore-0.1.0-alpha.5-linux-x64.tar.gz"
      sha256 "a6a8be6eb4ec34b2ec4346103b6816642f70071037ac90f0f1cd819c20f1c389"
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
