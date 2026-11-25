class Gitstore < Formula
  desc "GitStore early-alpha CLI and user daemon"
  homepage "https://github.com/gitstore-dev/cli"
  version "0.1.0-alpha.9"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.9/gitstore-0.1.0-alpha.9-darwin-arm64.tar.gz"
      sha256 "e15ca0efec4e3b7c2f7a1e564451d50316e96dcc06ebe948507fe87138e16328"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.9/gitstore-0.1.0-alpha.9-darwin-x64.tar.gz"
      sha256 "20acfd034d750469bf16fde4b659c1c81f4135edb7e52eff71a12a7ad340a562"
    end
  end
  on_linux do
    depends_on "ca-certificates"
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.9/gitstore-0.1.0-alpha.9-linux-arm64.tar.gz"
      sha256 "7b648c29057f6e68bae5e234254ed8f5093cf0b4f7efdf0e0b0a110191936b0d"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.9/gitstore-0.1.0-alpha.9-linux-x64.tar.gz"
      sha256 "ceba10861741ca486c80b6c46090bd32a3b6495c98083ce506a081c8f04c56b0"
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
