# GitHub-hosted Homebrew tap

The release producer renders `Formula/gitstore.rb` from the four final archive
URLs and SHA-256 digests using `scripts/render-homebrew-formula.py`. Commit that
generated formula only after the matching release is promoted. The publisher
opens a single-file PR using the release App; it does not write protected `main`.
Confirm the PR still targets the newest published prerelease and merge it through
normal checks and the merge queue. Homebrew delivery remains pending until then. The source
template is `packaging/homebrew/gitstore.rb.in`; no installable formula exists
before qualified release assets are available.

Consumers then use the explicit repository Git URL:

```sh
brew tap gitstore-dev/cli https://github.com/gitstore-dev/cli.git
brew install gitstore-dev/cli/gitstore
```

No Homebrew registry publication or account is required. The formula selects
prebuilt archives and installs both real executables in a private directory.
It does not compile or start services during installation. First local invocation
uses the CLI's ordinary-user setup on qualified Linux sessions. macOS has only
remote/control support until a local execution backend is qualified.
