## gs

```text
GitStore CLI

Usage: gs [OPTIONS] <COMMAND>

Commands:
  completion     Generate a shell completion script without accessing configuration
  reference      Generate the Markdown command reference without accessing configuration
  context        
  config         
  auth           
  api            
  read           
  list           
  delete         
  create         
  repository     
  update         
  watch          
  serve          
  deployment     
  migrate        
  git            
  controller     
  oidc           
  admin          
  storefront     
  api-resources  
  explain        

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help (see a summary with '-h')

  -V, --version
          Print version

Early alpha: upstream GitStore schemas continue changing incompatibly. No upgrade compatibility, data preservation or recovery guarantees. Keep independent backups; do not use prereleases for irreplaceable data.

```

## gs completion

```text
Generate a shell completion script without accessing configuration

Usage: gs completion [OPTIONS] <SHELL>

Arguments:
  <SHELL>
          [possible values: bash, elvish, fish, powershell, zsh]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs reference

```text
Generate the Markdown command reference without accessing configuration

Usage: gs reference [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs context

```text
Usage: gs context [OPTIONS] <COMMAND>

Commands:
  list  
  show  
  use   

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs context list

```text
Usage: gs context list [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs context show

```text
Usage: gs context show [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs context use

```text
Usage: gs context use [OPTIONS] <NAME>

Arguments:
  <NAME>
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs config

```text
Usage: gs config [OPTIONS] <COMMAND>

Commands:
  validate  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs config validate

```text
Usage: gs config validate [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs auth

```text
Usage: gs auth [OPTIONS] <COMMAND>

Commands:
  status   
  token    
  logout   
  switch   
  login    
  refresh  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs auth status

```text
Usage: gs auth status [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs auth token

```text
Usage: gs auth token [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs auth logout

```text
Usage: gs auth logout [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --remote
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs auth switch

```text
Usage: gs auth switch [OPTIONS] <ACCOUNT>

Arguments:
  <ACCOUNT>
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs auth login

```text
Usage: gs auth login [OPTIONS] --username <USERNAME>

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --username <USERNAME>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --password-file <PASSWORD_FILE>
          

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --password-stdin
          

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs auth refresh

```text
Usage: gs auth refresh [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs api

```text
Usage: gs api [OPTIONS] <COMMAND>

Commands:
  graphql  
  serve    
  config   

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs api graphql

```text
Usage: gs api graphql [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

  -f <FIELDS>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

  -F <TYPED>
          

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --input <INPUT>
          

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs api serve

```text
Usage: gs api serve [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --add <ADD>
          [possible values: oidc, admin, storefront]

      --detach
          

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --allow-provision
          

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs api config

```text
Usage: gs api config [OPTIONS] <COMMAND>

Commands:
  validate  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs api config validate

```text
Usage: gs api config validate [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs read

```text
Usage: gs read [OPTIONS] <KIND> [NAME]

Arguments:
  <KIND>
          Resource kind, or `<kind>/<name>`

  [NAME]
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --id <ID>
          

      --authoring
          Export only the exact writable authoring projection

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs list

```text
Usage: gs list [OPTIONS] <KIND>

Arguments:
  <KIND>
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --first <FIRST>
          [default: 100]

      --after <AFTER>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs delete

```text
Usage: gs delete [OPTIONS] <KIND> [NAME]

Arguments:
  <KIND>
          Resource kind, or `<kind>/<name>`

  [NAME]
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --id <ID>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs create

```text
Usage: gs create [OPTIONS] -f <FILES>

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

  -f <FILES>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs repository

```text
Usage: gs repository [OPTIONS] <COMMAND>

Commands:
  clone  
  sync   

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs repository clone

```text
Usage: gs repository clone [OPTIONS] <REPOSITORY> [DIRECTORY]

Arguments:
  <REPOSITORY>
          

  [DIRECTORY]
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --remote <REMOTE>
          [default: gitstore]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs repository sync

```text
Usage: gs repository sync [OPTIONS] [REPOSITORY]

Arguments:
  [REPOSITORY]
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --remote <REMOTE>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs update

```text
Usage: gs update [OPTIONS] -f <FILES>

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

  -f <FILES>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs watch

```text
Usage: gs watch [OPTIONS] <KIND> [NAME]

Arguments:
  <KIND>
          

  [NAME]
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --id <ID>
          ID watches are unavailable until deletion events identify the incarnation

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --selector <SELECTOR>
          JSON LabelSelectorInput; matchLabels and matchExpressions only

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --resource-version <RESOURCE_VERSION>
          Resume the exact cursor recorded by the matching local checkpoint

      --checkpoint-file <CHECKPOINT_FILE>
          Private checkpoint path; defaults to per-selection user state

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --max-events <MAX_EVENTS>
          

      --max-frame-bytes <MAX_FRAME_BYTES>
          [default: 1048576]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs serve

```text
Usage: gs serve [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --add <ADD>
          [possible values: oidc, admin, storefront]

      --detach
          

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --allow-provision
          

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs deployment

```text
Usage: gs deployment [OPTIONS] <COMMAND>

Commands:
  list     
  inspect  
  stop     
  logs     

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs deployment list

```text
Usage: gs deployment list [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --limit <LIMIT>
          [default: 100]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --cursor <CURSOR>
          

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs deployment inspect

```text
Usage: gs deployment inspect [OPTIONS] <ID>

Arguments:
  <ID>
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --operation <OPERATION>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs deployment stop

```text
Usage: gs deployment stop [OPTIONS] <ID>

Arguments:
  <ID>
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --detach
          

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs deployment logs

```text
Usage: gs deployment logs [OPTIONS] <ID>

Arguments:
  <ID>
          

Options:
      --component <COMPONENT>
          [possible values: api, git, controller, oidc, oidc-smtp, oidc-hydra, oidc-kratos, oidc-ui, oidc-bridge, admin, storefront, scylladb]

      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --cursor <CURSOR>
          

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs migrate

```text
Usage: gs migrate [OPTIONS] --target <TARGET>

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --target <TARGET>
          

      --backup-acknowledged
          

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --detach
          

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs git

```text
Usage: gs git [OPTIONS] <COMMAND>

Commands:
  serve   
  config  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs git serve

```text
Usage: gs git serve [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --add <ADD>
          [possible values: oidc, admin, storefront]

      --detach
          

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --allow-provision
          

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs git config

```text
Usage: gs git config [OPTIONS] <COMMAND>

Commands:
  validate  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs git config validate

```text
Usage: gs git config validate [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs controller

```text
Usage: gs controller [OPTIONS] <COMMAND>

Commands:
  serve   
  config  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs controller serve

```text
Usage: gs controller serve [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --add <ADD>
          [possible values: oidc, admin, storefront]

      --detach
          

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --allow-provision
          

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs controller config

```text
Usage: gs controller config [OPTIONS] <COMMAND>

Commands:
  validate  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs controller config validate

```text
Usage: gs controller config validate [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs oidc

```text
Usage: gs oidc [OPTIONS] <COMMAND>

Commands:
  serve   
  config  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs oidc serve

```text
Usage: gs oidc serve [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --add <ADD>
          [possible values: oidc, admin, storefront]

      --detach
          

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --allow-provision
          

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs oidc config

```text
Usage: gs oidc config [OPTIONS] <COMMAND>

Commands:
  validate  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs oidc config validate

```text
Usage: gs oidc config validate [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs admin

```text
Usage: gs admin [OPTIONS] <COMMAND>

Commands:
  serve   
  config  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs admin serve

```text
Usage: gs admin serve [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --add <ADD>
          [possible values: oidc, admin, storefront]

      --detach
          

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --allow-provision
          

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs admin config

```text
Usage: gs admin config [OPTIONS] <COMMAND>

Commands:
  validate  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs admin config validate

```text
Usage: gs admin config validate [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs storefront

```text
Usage: gs storefront [OPTIONS] <COMMAND>

Commands:
  serve   
  config  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs storefront serve

```text
Usage: gs storefront serve [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --add <ADD>
          [possible values: oidc, admin, storefront]

      --detach
          

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --allow-provision
          

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --idempotency-key <IDEMPOTENCY_KEY>
          

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs storefront config

```text
Usage: gs storefront config [OPTIONS] <COMMAND>

Commands:
  validate  

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs storefront config validate

```text
Usage: gs storefront config validate [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --deployment <DEPLOYMENT>
          [env: GITSTORE_DEPLOYMENT=]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --profile <PROFILE>
          [env: GITSTORE_PROFILE=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --profile-version <PROFILE_VERSION>
          [env: GITSTORE_PROFILE_VERSION=]

      --datastore <DATASTORE>
          [possible values: memdb, scylladb]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs api-resources

```text
Usage: gs api-resources [OPTIONS]

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --profile <PROFILE>
          [default: gitstore-alpha9-2026-11-05]
          [possible values: gitstore-alpha9-2026-11-05, m0-2026-10-27]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

## gs explain

```text
Usage: gs explain [OPTIONS] <RESOURCE>

Arguments:
  <RESOURCE>
          

Options:
      --config <CONFIG>
          [env: GITSTORE_CONFIG=]

      --profile <PROFILE>
          [default: gitstore-alpha9-2026-11-05]
          [possible values: gitstore-alpha9-2026-11-05, m0-2026-10-27]

      --context <CONTEXT>
          [env: GITSTORE_CONTEXT=]

      --daemon-socket <DAEMON_SOCKET>
          [env: GITSTORE_DAEMON_SOCKET=]

      --endpoint <ENDPOINT>
          [env: GITSTORE_ENDPOINT=]

      --account <ACCOUNT>
          [env: GITSTORE_ACCOUNT=]

      --namespace <NAMESPACE>
          [env: GITSTORE_NAMESPACE=]

      --repository <REPOSITORY>
          [env: GITSTORE_REPOSITORY=]

      --ref <GIT_REF>
          [env: GITSTORE_REF=]

      --credential-file <CREDENTIAL_FILE>
          

      --credential-stdin
          

      --non-interactive
          [env: GITSTORE_NO_INPUT=]

      --timeout <TIMEOUT>
          

  -o, --output <OUTPUT>
          Output format; JSON and YAML use the v2 result envelope except for raw protocol output
          
          [possible values: json, yaml, table, markdown]

      --wait <WAIT>
          

  -h, --help
          Print help

```

