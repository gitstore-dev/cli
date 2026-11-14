class Gitstore < Formula
  desc "GitStore early-alpha CLI and user daemon"
  homepage "https://github.com/gitstore-dev/cli"
  version "0.1.0-alpha.6"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.6/gitstore-0.1.0-alpha.6-darwin-arm64.tar.gz"
      sha256 "78c056aad590e62dfadf1bd243f2182ff43935a4ba22a00317e97aebb44fe7c0"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.6/gitstore-0.1.0-alpha.6-darwin-x64.tar.gz"
      sha256 "0f93d567166d320e8e8eaebf778f04e813eec4684e5b75df12e89cd0a9806afd"
    end
  end
  on_linux do
    depends_on "ca-certificates"
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.6/gitstore-0.1.0-alpha.6-linux-arm64.tar.gz"
      sha256 "9d07348d191c2b4be011c8e939cf98ec07465c9a0d96f762f4e7ccaaf5cc7291"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.6/gitstore-0.1.0-alpha.6-linux-x64.tar.gz"
      sha256 "94e39a6db9f0c371f252264b9b2c6c0947b3e3eefe13df6a248426522f4c0691"
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
