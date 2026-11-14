class Gitstore < Formula
  desc "GitStore early-alpha CLI and user daemon"
  homepage "https://github.com/gitstore-dev/cli"
  version "0.1.0-alpha.7"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-0.1.0-alpha.7-darwin-arm64.tar.gz"
      sha256 "0986f42d566f713af379df6ae59e301760cf91abe81ce2384b981f0e2ce4ff32"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-0.1.0-alpha.7-darwin-x64.tar.gz"
      sha256 "009cf3daa16a39bb4ec97af0f76652f33b7532be1af7eea7b56634e7b17b4163"
    end
  end
  on_linux do
    depends_on "ca-certificates"
    on_arm do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-0.1.0-alpha.7-linux-arm64.tar.gz"
      sha256 "3aac39a151a26f57a1334e6922c1ae94381cce4817436acbd566c26aa38a89e0"
    end
    on_intel do
      url "https://github.com/gitstore-dev/cli/releases/download/v0.1.0-alpha.7/gitstore-0.1.0-alpha.7-linux-x64.tar.gz"
      sha256 "2b5288aaa084a1165a10f35bedeb3f16946a4375971993c31b8ef1e9e4611817"
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
