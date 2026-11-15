class Gitstore < Formula
  desc "GitStore early-alpha CLI and user daemon"
  homepage "https://github.com/gitstore-dev/cli"
  version "0.1.0-alpha.8"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.8/gitstore-0.1.0-alpha.8-darwin-arm64.tar.gz"
      sha256 "540af44459a29eab1e9628cb1fe20c9d8f48fa7743b8c08c4e20b65693cf7860"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.8/gitstore-0.1.0-alpha.8-darwin-x64.tar.gz"
      sha256 "f87289c37f5cb778046a1b7824e663d748c8c71ae882367dcb11ca087f1277dd"
    end
  end
  on_linux do
    depends_on "ca-certificates"
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.8/gitstore-0.1.0-alpha.8-linux-arm64.tar.gz"
      sha256 "637b328b3151f9b5e9e3a7dea9154503f4c85bfc5ae7defa40948962c0c6f5d2"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.8/gitstore-0.1.0-alpha.8-linux-x64.tar.gz"
      sha256 "957792c04bd2ba91d9524877797399017982fe80d7443958f30a4714e82b2e8d"
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
