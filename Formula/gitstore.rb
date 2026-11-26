class Gitstore < Formula
  desc "GitStore early-alpha CLI and user daemon"
  homepage "https://github.com/gitstore-dev/cli"
  version "0.1.0-alpha.11"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.11/gitstore-0.1.0-alpha.11-darwin-arm64.tar.gz"
      sha256 "e5650d0946471c5af5d25eb65a95815e7a1e0bd162a043bdaf5e13d9b851cead"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.11/gitstore-0.1.0-alpha.11-darwin-x64.tar.gz"
      sha256 "2b83d713d755f689bd653af51096c51243d37f657e710e26c5947f19fe9c5858"
    end
  end
  on_linux do
    depends_on "ca-certificates"
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.11/gitstore-0.1.0-alpha.11-linux-arm64.tar.gz"
      sha256 "e07f5b74a9956c651d8e06922f771719806e75b76e2aef7c12bc59204d387c13"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.11/gitstore-0.1.0-alpha.11-linux-x64.tar.gz"
      sha256 "2dcd365628c23a612edee6f9442ef818c1aa878112e0b62547655860b5b76f27"
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
