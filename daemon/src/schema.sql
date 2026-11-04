CREATE TABLE profiles(
 profile_id TEXT NOT NULL CHECK(gstd_identifier(profile_id)),
 profile_version TEXT NOT NULL CHECK(gstd_identifier(profile_version)),
 server_revision TEXT NOT NULL, manifest_digest TEXT NOT NULL, platform TEXT NOT NULL,
 PRIMARY KEY(profile_id,profile_version)
) STRICT;
CREATE TRIGGER profile_immutable BEFORE UPDATE ON profiles BEGIN
 SELECT RAISE(ABORT,'registered profile versions are immutable');
END;
CREATE TABLE deployments(
 deployment_id TEXT PRIMARY KEY CHECK(gstd_identifier(deployment_id)),
 profile_id TEXT NOT NULL, profile_version TEXT NOT NULL,
 desired_state TEXT NOT NULL CHECK(desired_state IN ('running','stopped')),
 generation INTEGER NOT NULL CHECK(generation BETWEEN 1 AND 9007199254740991),
 observed_generation INTEGER NOT NULL CHECK(observed_generation BETWEEN 0 AND generation),
 readiness TEXT NOT NULL CHECK(readiness IN ('pending','ready','failed')),
 recovery TEXT NOT NULL CHECK(recovery IN ('normal','reconciling','uncertain')),
 writer_ownership TEXT NOT NULL CHECK(writer_ownership IN ('held','released','uncertain')),
 observed_at TEXT NOT NULL CHECK(observed_at GLOB '????-??-??T??:??:??*Z' AND julianday(observed_at) IS NOT NULL),
 last_operation_id TEXT,
 FOREIGN KEY(profile_id,profile_version) REFERENCES profiles(profile_id,profile_version),
 FOREIGN KEY(last_operation_id) REFERENCES operations(operation_id) DEFERRABLE INITIALLY DEFERRED,
 CHECK(readiness!='ready' OR generation=observed_generation)
) STRICT;
CREATE TABLE operations(
 operation_id TEXT PRIMARY KEY CHECK(gstd_identifier(operation_id)),
 caller_uid INTEGER NOT NULL CHECK(caller_uid>=0), method TEXT NOT NULL CHECK(method IN ('deployment.start','deployment.stop','operation.cancel','migration.start')),
 deployment_id TEXT REFERENCES deployments(deployment_id) DEFERRABLE INITIALLY DEFERRED,
 target_operation_id TEXT REFERENCES operations(operation_id),
 intent_fingerprint TEXT NOT NULL CHECK(length(intent_fingerprint)=64 AND intent_fingerprint NOT GLOB '*[^0-9a-f]*'),
 intent TEXT NOT NULL CHECK(json_valid(intent)),
 accepted_at TEXT NOT NULL CHECK(accepted_at GLOB '????-??-??T??:??:??*Z' AND julianday(accepted_at) IS NOT NULL),
 updated_at TEXT NOT NULL CHECK(updated_at GLOB '????-??-??T??:??:??*Z' AND julianday(updated_at) IS NOT NULL),
 state TEXT NOT NULL CHECK(state IN ('accepted','running','succeeded','failed','cancelled')),
 recovery TEXT NOT NULL CHECK(recovery IN ('normal','reconciling','uncertain')),
 result TEXT CHECK(result IS NULL OR json_valid(result)), error TEXT CHECK(error IS NULL OR json_valid(error)),
 CHECK(result IS NULL OR error IS NULL),
 CHECK(state IN ('succeeded','failed','cancelled') OR (result IS NULL AND error IS NULL))
) STRICT;
CREATE TABLE dedupe(
 caller_uid INTEGER NOT NULL CHECK(caller_uid>=0), method TEXT NOT NULL,
 idempotency_key TEXT NOT NULL CHECK(gstd_identifier(idempotency_key)),
 fingerprint TEXT NOT NULL CHECK(length(fingerprint)=64 AND fingerprint NOT GLOB '*[^0-9a-f]*'),
 operation_id TEXT NOT NULL REFERENCES operations(operation_id),
 expires_after INTEGER NOT NULL CHECK(expires_after>=0),
 PRIMARY KEY(caller_uid,method,idempotency_key)
) STRICT;
CREATE TABLE components(
 deployment_id TEXT NOT NULL REFERENCES deployments(deployment_id),
 component TEXT NOT NULL CHECK(component IN ('api','git','controller','oidc','admin','storefront')),
 backend_id TEXT, image_digest TEXT NOT NULL,
 desired_state TEXT NOT NULL CHECK(desired_state IN ('running','stopped')),
 observed_state TEXT NOT NULL CHECK(observed_state IN ('pending','running','stopped','failed','unknown')),
 generation INTEGER NOT NULL CHECK(generation BETWEEN 1 AND 9007199254740991),
 observed_at TEXT NOT NULL,
 PRIMARY KEY(deployment_id,component)
) STRICT;
CREATE TABLE resource_ownership(
 resource_id TEXT PRIMARY KEY CHECK(gstd_identifier(resource_id)),
 resource_type TEXT NOT NULL CHECK(resource_type IN ('repository-volume','datastore-volume','network','workload','asset-root','configuration')),
 deployment_id TEXT NOT NULL REFERENCES deployments(deployment_id),
 operation_id TEXT NOT NULL REFERENCES operations(operation_id),
 lifecycle_stage TEXT NOT NULL CHECK(lifecycle_stage IN ('reserved','allocated','attached','running','stopping','retained','released')),
 backend_identity TEXT,
 ownership_state TEXT NOT NULL CHECK(ownership_state IN ('held','released','uncertain')),
 observed_at TEXT NOT NULL
) STRICT;
CREATE INDEX operations_deployment ON operations(deployment_id,state);
CREATE TABLE network_attachments(
 workload_id TEXT PRIMARY KEY REFERENCES resource_ownership(resource_id),
 network_id TEXT NOT NULL REFERENCES resource_ownership(resource_id),
 operation_id TEXT NOT NULL REFERENCES operations(operation_id),
 boot_id TEXT NOT NULL CHECK(length(boot_id)=36),
 user_device TEXT NOT NULL CHECK(user_device NOT GLOB '*[^0-9]*' AND length(user_device) BETWEEN 1 AND 20),
 user_inode TEXT NOT NULL CHECK(user_inode NOT GLOB '*[^0-9]*' AND length(user_inode) BETWEEN 1 AND 20),
 network_device TEXT NOT NULL CHECK(network_device NOT GLOB '*[^0-9]*' AND length(network_device) BETWEEN 1 AND 20),
 network_inode TEXT NOT NULL CHECK(network_inode NOT GLOB '*[^0-9]*' AND length(network_inode) BETWEEN 1 AND 20)
) STRICT;
CREATE TRIGGER network_attachment_ownership BEFORE INSERT ON network_attachments
WHEN NOT EXISTS(SELECT 1 FROM resource_ownership w JOIN resource_ownership n
 ON w.deployment_id=n.deployment_id AND w.operation_id=n.operation_id
 JOIN operations o ON w.operation_id=o.operation_id
 WHERE w.resource_id=NEW.workload_id AND w.resource_type='workload' AND
 n.resource_id=NEW.network_id AND n.resource_type='network' AND
 w.operation_id=NEW.operation_id AND w.ownership_state='held' AND n.ownership_state='held' AND
 w.lifecycle_stage='reserved' AND n.lifecycle_stage IN ('allocated','running') AND
 n.backend_identity IS NOT NULL AND o.method='deployment.start' AND
 o.state IN ('accepted','running'))
BEGIN SELECT RAISE(ABORT,'network attachment requires owned start resources'); END;
CREATE TRIGGER network_attachment_immutable BEFORE UPDATE ON network_attachments
BEGIN SELECT RAISE(ABORT,'network attachment identity is immutable'); END;
CREATE TRIGGER network_attachment_retained BEFORE DELETE ON network_attachments
BEGIN SELECT RAISE(ABORT,'network attachment evidence is retained'); END;
CREATE TRIGGER network_identity_immutable BEFORE UPDATE OF backend_identity ON resource_ownership
WHEN OLD.resource_type='network' AND OLD.backend_identity IS NOT NULL AND
 NEW.backend_identity IS NOT OLD.backend_identity
BEGIN SELECT RAISE(ABORT,'network worker identity is immutable'); END;
CREATE INDEX dedupe_retention ON dedupe(expires_after);
CREATE TRIGGER terminal_outcome_immutable BEFORE UPDATE OF state,result,error ON operations
WHEN OLD.state IN ('succeeded','failed','cancelled') AND
 (NEW.state!=OLD.state OR NEW.result IS NOT OLD.result OR NEW.error IS NOT OLD.error)
BEGIN SELECT RAISE(ABORT,'terminal operation outcome is immutable'); END;
CREATE TRIGGER operation_progression BEFORE UPDATE OF state ON operations
WHEN NEW.state!=OLD.state AND NOT
 ((OLD.state='accepted' AND NEW.state IN ('running','cancelled')) OR
  (OLD.state='running' AND NEW.state IN ('succeeded','failed','cancelled')))
BEGIN SELECT RAISE(ABORT,'invalid operation progression'); END;
CREATE TABLE jobs(
 operation_id TEXT PRIMARY KEY REFERENCES operations(operation_id),
 kind TEXT NOT NULL CHECK(kind IN ('runtime','control')),
 state TEXT NOT NULL CHECK(state IN ('queued','reserved','running','done')),
 worker_slot INTEGER UNIQUE CHECK(worker_slot BETWEEN 0 AND 7),
 CHECK(kind!='control' OR worker_slot IS NULL),
 CHECK(kind!='runtime' OR
  ((state IN ('queued','done') AND worker_slot IS NULL) OR
   (state IN ('reserved','running') AND worker_slot IS NOT NULL)))
) STRICT;
CREATE TABLE query_meta(
 store_id TEXT NOT NULL, generation INTEGER NOT NULL CHECK(generation BETWEEN 1 AND 9007199254740991)
) STRICT;
INSERT INTO query_meta VALUES(lower(hex(randomblob(16))),1);
CREATE TABLE query_cursors(
 cursor_id TEXT PRIMARY KEY,store_id TEXT NOT NULL,generation INTEGER NOT NULL,
 last_id TEXT NOT NULL,expires_after INTEGER NOT NULL
) STRICT;
CREATE INDEX query_cursor_expiry ON query_cursors(expires_after);
CREATE TABLE asset_cache_binding(
 singleton INTEGER PRIMARY KEY CHECK(singleton=1),
 store_id TEXT NOT NULL CHECK(length(store_id)=32 AND store_id NOT GLOB '*[^0-9a-f]*'),
 device TEXT NOT NULL CHECK(length(device) BETWEEN 1 AND 20 AND device NOT GLOB '*[^0-9]*'),
 inode TEXT NOT NULL CHECK(length(inode) BETWEEN 1 AND 20 AND inode NOT GLOB '*[^0-9]*')
) STRICT;
CREATE TRIGGER asset_cache_binding_immutable BEFORE UPDATE ON asset_cache_binding
BEGIN SELECT RAISE(ABORT,'immutable cache binding'); END;
CREATE TRIGGER asset_cache_binding_retained BEFORE DELETE ON asset_cache_binding
BEGIN SELECT RAISE(ABORT,'cache binding remains owned'); END;
CREATE TABLE assets(
 asset_key TEXT PRIMARY KEY CHECK(length(asset_key)=64 AND asset_key NOT GLOB '*[^0-9a-f]*'),
 descriptor TEXT NOT NULL CHECK(json_valid(descriptor)),
 producer_operation_id TEXT NOT NULL CHECK(gstd_identifier(producer_operation_id)),
 operation_id TEXT REFERENCES operations(operation_id),
 staging_name TEXT NOT NULL UNIQUE CHECK(staging_name='staging-'||asset_key||'-'||producer_operation_id),
 staging_device TEXT, staging_inode TEXT,
 acquisition_mode TEXT NOT NULL DEFAULT 'registry' CHECK(acquisition_mode IN ('registry','offline')),
 source_device TEXT, source_inode TEXT,
 state TEXT NOT NULL CHECK(state IN ('staging','verified','active','evicting')),
 reserved_bytes INTEGER NOT NULL CHECK(reserved_bytes BETWEEN 0 AND 68719476736),
 download_limit INTEGER NOT NULL CHECK(download_limit BETWEEN 1 AND 2147483648),
 merge_limit INTEGER NOT NULL CHECK(merge_limit BETWEEN 1024 AND 8589934592),
 tree_digest TEXT CHECK(tree_digest IS NULL OR (length(tree_digest)=64 AND tree_digest NOT GLOB '*[^0-9a-f]*')),
 logical_bytes INTEGER NOT NULL DEFAULT 0 CHECK(logical_bytes BETWEEN 0 AND 8589934592),
 downloaded_bytes INTEGER NOT NULL DEFAULT 0 CHECK(downloaded_bytes BETWEEN 0 AND 2147483648),
 merged_bytes INTEGER NOT NULL DEFAULT 0 CHECK(merged_bytes BETWEEN 0 AND 8589934592),
 CHECK((staging_device IS NULL)=(staging_inode IS NULL)),
 CHECK((acquisition_mode='registry' AND source_device IS NULL AND source_inode IS NULL) OR
       (acquisition_mode='offline' AND source_device IS NOT NULL AND source_inode IS NOT NULL AND
        length(source_device) BETWEEN 1 AND 20 AND source_device NOT GLOB '*[^0-9]*' AND
        length(source_inode) BETWEEN 1 AND 20 AND source_inode NOT GLOB '*[^0-9]*')),
 CHECK(staging_device IS NULL OR (length(staging_device) BETWEEN 1 AND 20 AND staging_device NOT GLOB '*[^0-9]*')),
 CHECK(staging_inode IS NULL OR (length(staging_inode) BETWEEN 1 AND 20 AND staging_inode NOT GLOB '*[^0-9]*')),
 CHECK((state IN ('active','evicting') AND operation_id IS NULL AND reserved_bytes=0) OR
       (state IN ('staging','verified') AND operation_id IS NOT NULL AND operation_id=producer_operation_id AND
        reserved_bytes=download_limit+2*merge_limit+1048576)),
 CHECK(logical_bytes<=merge_limit AND downloaded_bytes<=download_limit AND merged_bytes<=merge_limit),
 CHECK((state='staging' AND tree_digest IS NULL AND logical_bytes=0 AND downloaded_bytes=0 AND merged_bytes=0) OR
       (state!='staging' AND tree_digest IS NOT NULL AND staging_inode IS NOT NULL AND downloaded_bytes>0 AND merged_bytes>=1024))
) STRICT;
CREATE TABLE asset_refs(
 resource_id TEXT PRIMARY KEY REFERENCES resource_ownership(resource_id),
 asset_key TEXT NOT NULL REFERENCES assets(asset_key)
) STRICT;
CREATE INDEX asset_refs_asset ON asset_refs(asset_key);
CREATE TRIGGER asset_progression BEFORE UPDATE OF state ON assets
WHEN NEW.state!=OLD.state AND NOT ((OLD.state='staging' AND NEW.state='verified') OR
 (OLD.state='verified' AND NEW.state='active') OR
 (OLD.state='active' AND NEW.state='evicting' AND NOT EXISTS(SELECT 1 FROM asset_refs WHERE asset_key=OLD.asset_key)))
BEGIN SELECT RAISE(ABORT,'invalid asset progression'); END;
CREATE TRIGGER asset_identity_immutable BEFORE UPDATE ON assets
WHEN NEW.asset_key IS NOT OLD.asset_key OR NEW.descriptor IS NOT OLD.descriptor OR
 NEW.download_limit!=OLD.download_limit OR NEW.merge_limit!=OLD.merge_limit OR
 NEW.acquisition_mode IS NOT OLD.acquisition_mode OR NEW.source_device IS NOT OLD.source_device OR
 NEW.source_inode IS NOT OLD.source_inode OR
 NEW.producer_operation_id IS NOT OLD.producer_operation_id OR NEW.staging_name IS NOT OLD.staging_name OR
 (OLD.staging_inode IS NOT NULL AND (NEW.staging_inode IS NOT OLD.staging_inode OR NEW.staging_device IS NOT OLD.staging_device)) OR
 (OLD.state!='staging' AND (NEW.tree_digest IS NOT OLD.tree_digest OR NEW.logical_bytes!=OLD.logical_bytes OR
  NEW.downloaded_bytes!=OLD.downloaded_bytes OR NEW.merged_bytes!=OLD.merged_bytes))
BEGIN SELECT RAISE(ABORT,'immutable asset identity or receipt'); END;
CREATE TRIGGER asset_ref_owner BEFORE INSERT ON asset_refs
WHEN EXISTS(SELECT 1 FROM assets WHERE asset_key=NEW.asset_key AND state='evicting') OR
 NOT EXISTS(SELECT 1 FROM resource_ownership WHERE resource_id=NEW.resource_id AND
 resource_type='asset-root' AND ownership_state='held' AND lifecycle_stage IN ('reserved','allocated','attached','running'))
BEGIN SELECT RAISE(ABORT,'asset reference requires owned resource'); END;
CREATE TRIGGER asset_ref_release BEFORE DELETE ON asset_refs
WHEN NOT EXISTS(SELECT 1 FROM resource_ownership WHERE resource_id=OLD.resource_id AND
 ownership_state='released' AND lifecycle_stage='released')
BEGIN SELECT RAISE(ABORT,'asset reference remains held or uncertain'); END;
CREATE TRIGGER asset_ref_immutable BEFORE UPDATE ON asset_refs
BEGIN SELECT RAISE(ABORT,'asset reference is immutable'); END;
CREATE TRIGGER asset_delete_evicted BEFORE DELETE ON assets
WHEN OLD.state!='evicting'
BEGIN SELECT RAISE(ABORT,'asset removal requires recorded eviction'); END;
CREATE TABLE repository_volumes(
 volume_id TEXT PRIMARY KEY REFERENCES resource_ownership(resource_id),
 role TEXT NOT NULL CHECK(role='repositories'),
 owner_uid INTEGER NOT NULL CHECK(owner_uid>=0),
 producer_operation_id TEXT NOT NULL REFERENCES operations(operation_id),
 store_id TEXT NOT NULL CHECK(length(store_id)=32 AND store_id NOT GLOB '*[^0-9a-f]*'),
 root_device TEXT NOT NULL CHECK(length(root_device) BETWEEN 1 AND 20 AND root_device NOT GLOB '*[^0-9]*'),
 root_inode TEXT NOT NULL CHECK(length(root_inode) BETWEEN 1 AND 20 AND root_inode NOT GLOB '*[^0-9]*' AND root_inode!='0'),
 compatibility TEXT NOT NULL CHECK(compatibility='gitstore-repositories-v1'),
 allocated INTEGER NOT NULL DEFAULT 0 CHECK(allocated IN (0,1)),
 volume_device TEXT CHECK(volume_device IS NULL OR (length(volume_device) BETWEEN 1 AND 20 AND volume_device NOT GLOB '*[^0-9]*')),
 volume_inode TEXT CHECK(volume_inode IS NULL OR (length(volume_inode) BETWEEN 1 AND 20 AND volume_inode NOT GLOB '*[^0-9]*' AND volume_inode!='0')),
 data_device TEXT CHECK(data_device IS NULL OR (length(data_device) BETWEEN 1 AND 20 AND data_device NOT GLOB '*[^0-9]*')),
 data_inode TEXT CHECK(data_inode IS NULL OR (length(data_inode) BETWEEN 1 AND 20 AND data_inode NOT GLOB '*[^0-9]*' AND data_inode!='0')),
 writer_device TEXT CHECK(writer_device IS NULL OR (length(writer_device) BETWEEN 1 AND 20 AND writer_device NOT GLOB '*[^0-9]*')),
 writer_inode TEXT CHECK(writer_inode IS NULL OR (length(writer_inode) BETWEEN 1 AND 20 AND writer_inode NOT GLOB '*[^0-9]*' AND writer_inode!='0')),
 CHECK((allocated=0 AND volume_device IS NULL AND volume_inode IS NULL AND data_device IS NULL AND data_inode IS NULL AND writer_device IS NULL AND writer_inode IS NULL) OR
       (allocated=1 AND volume_device IS NOT NULL AND volume_inode IS NOT NULL AND data_device IS NOT NULL AND data_inode IS NOT NULL AND writer_device IS NOT NULL AND writer_inode IS NOT NULL)),
 UNIQUE(volume_device,volume_inode), UNIQUE(data_device,data_inode), UNIQUE(writer_device,writer_inode)
) STRICT;
CREATE TRIGGER repository_volume_owner BEFORE INSERT ON repository_volumes
WHEN NOT EXISTS(SELECT 1 FROM resource_ownership r JOIN operations o ON r.operation_id=o.operation_id
 AND r.deployment_id=o.deployment_id JOIN query_meta q ON q.store_id=NEW.store_id
 WHERE r.resource_id=NEW.volume_id AND r.resource_type='repository-volume' AND
 r.ownership_state='held' AND r.lifecycle_stage='reserved' AND
 o.operation_id=NEW.producer_operation_id AND o.caller_uid=NEW.owner_uid AND
 o.method='deployment.start' AND o.state IN ('accepted','running')) OR
 EXISTS(SELECT 1 FROM repository_volumes WHERE store_id!=NEW.store_id OR
 root_device!=NEW.root_device OR root_inode!=NEW.root_inode) OR
 (SELECT count(*) FROM repository_volumes)>=4096
BEGIN SELECT RAISE(ABORT,'repository volume requires a bounded reserved owner/root'); END;
CREATE TRIGGER repository_volume_immutable BEFORE UPDATE ON repository_volumes
WHEN OLD.allocated=1 OR NEW.volume_id IS NOT OLD.volume_id OR NEW.role IS NOT OLD.role OR
 NEW.owner_uid IS NOT OLD.owner_uid OR NEW.producer_operation_id IS NOT OLD.producer_operation_id OR
 NEW.store_id IS NOT OLD.store_id OR NEW.root_device IS NOT OLD.root_device OR
 NEW.root_inode IS NOT OLD.root_inode OR NEW.compatibility IS NOT OLD.compatibility
BEGIN SELECT RAISE(ABORT,'immutable repository volume identity or receipt'); END;
CREATE TRIGGER repository_volume_retained BEFORE DELETE ON repository_volumes
BEGIN SELECT RAISE(ABORT,'repository volume metadata is retained'); END;
CREATE TRIGGER deployment_insert_revision AFTER INSERT ON deployments
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER deployment_update_revision AFTER UPDATE ON deployments
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER deployment_delete_revision AFTER DELETE ON deployments
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER component_insert_revision AFTER INSERT ON components
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER component_update_revision AFTER UPDATE ON components
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER component_delete_revision AFTER DELETE ON components
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER resource_insert_revision AFTER INSERT ON resource_ownership
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER resource_update_revision AFTER UPDATE ON resource_ownership
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER resource_delete_revision AFTER DELETE ON resource_ownership
BEGIN UPDATE query_meta SET generation=generation+1; END;
