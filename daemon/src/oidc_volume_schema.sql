CREATE TABLE oidc_provider_volumes(
 volume_id TEXT PRIMARY KEY REFERENCES resource_ownership(resource_id),
 role TEXT NOT NULL CHECK(role IN ('oidc-hydra','oidc-kratos')),
 owner_uid INTEGER NOT NULL CHECK(owner_uid>=0),
 producer_operation_id TEXT NOT NULL REFERENCES operations(operation_id),
 store_id TEXT NOT NULL CHECK(length(store_id)=32 AND store_id NOT GLOB '*[^0-9a-f]*'),
 root_device TEXT NOT NULL CHECK(length(root_device) BETWEEN 1 AND 20 AND root_device NOT GLOB '*[^0-9]*'),
 root_inode TEXT NOT NULL CHECK(length(root_inode) BETWEEN 1 AND 20 AND root_inode NOT GLOB '*[^0-9]*' AND root_inode!='0'),
 compatibility TEXT NOT NULL CHECK((role='oidc-hydra' AND compatibility='gitstored-oidc-hydra-sqlite-v1:b94007e19a1f7f78157e7f4ea340da8a55b5f104a0f1198755c256f38ef32b4b') OR (role='oidc-kratos' AND compatibility='gitstored-oidc-kratos-sqlite-v1:fe2428f103a6240c064b6ea77d3088610865c55fcebccb30355857eccdc25b4b')),
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
CREATE TRIGGER oidc_provider_volume_owner BEFORE INSERT ON oidc_provider_volumes
WHEN NEW.allocated!=0 OR NOT EXISTS(SELECT 1 FROM resource_ownership r JOIN operations o
 ON r.operation_id=o.operation_id AND r.deployment_id=o.deployment_id
 JOIN query_meta q ON q.store_id=NEW.store_id
 WHERE r.resource_id=NEW.volume_id AND r.resource_type='datastore-volume' AND
 r.ownership_state='held' AND r.lifecycle_stage='reserved' AND
 o.operation_id=NEW.producer_operation_id AND o.caller_uid=NEW.owner_uid AND
 o.method='deployment.start' AND o.state IN ('accepted','running')) OR
 (SELECT count(*) FROM oidc_provider_volumes)>=8192
BEGIN SELECT RAISE(ABORT,'provider volume requires a bounded current OIDC start owner'); END;
CREATE TRIGGER oidc_provider_volume_profile BEFORE INSERT ON oidc_provider_volumes
WHEN NOT EXISTS(SELECT 1 FROM operations o JOIN deployments d ON d.deployment_id=o.deployment_id
 WHERE o.operation_id=NEW.producer_operation_id AND d.last_operation_id=o.operation_id AND
 d.profile_id='core' AND d.profile_version='2' AND d.desired_state='running' AND
 d.recovery='normal' AND d.writer_ownership='held' AND
 EXISTS(SELECT 1 FROM json_each(o.intent,'$.components') WHERE value='oidc'))
BEGIN SELECT RAISE(ABORT,'provider volume requires the current OIDC profile'); END;
CREATE TRIGGER oidc_provider_volume_immutable BEFORE UPDATE ON oidc_provider_volumes
WHEN OLD.allocated=1 OR NEW.volume_id IS NOT OLD.volume_id OR NEW.role IS NOT OLD.role OR
 NEW.owner_uid IS NOT OLD.owner_uid OR NEW.producer_operation_id IS NOT OLD.producer_operation_id OR
 NEW.store_id IS NOT OLD.store_id OR NEW.root_device IS NOT OLD.root_device OR
 NEW.root_inode IS NOT OLD.root_inode OR NEW.compatibility IS NOT OLD.compatibility
BEGIN SELECT RAISE(ABORT,'immutable provider volume identity or receipt'); END;
CREATE TRIGGER oidc_provider_volume_retained BEFORE DELETE ON oidc_provider_volumes
BEGIN SELECT RAISE(ABORT,'provider volume metadata is retained'); END;
CREATE TRIGGER oidc_provider_volumes_oidc_root BEFORE INSERT ON oidc_provider_volumes
WHEN EXISTS(SELECT 1 FROM oidc_provider_volumes v WHERE v.store_id!=NEW.store_id OR v.root_device!=NEW.root_device OR v.root_inode!=NEW.root_inode) OR
 EXISTS(SELECT 1 FROM repository_volumes v WHERE v.store_id!=NEW.store_id OR v.root_device!=NEW.root_device OR v.root_inode!=NEW.root_inode) OR
 EXISTS(SELECT 1 FROM datastore_volumes v WHERE v.store_id!=NEW.store_id OR v.root_device!=NEW.root_device OR v.root_inode!=NEW.root_inode)
BEGIN SELECT RAISE(ABORT,'all managed volume roles require the recorded root'); END;
CREATE TRIGGER oidc_provider_volumes_oidc_separation BEFORE UPDATE ON oidc_provider_volumes
WHEN NEW.allocated=1 AND (EXISTS(SELECT 1 FROM oidc_provider_volumes v WHERE v.volume_id!=NEW.volume_id AND ((v.volume_device=NEW.volume_device AND v.volume_inode=NEW.volume_inode)
 OR (v.data_device=NEW.volume_device AND v.data_inode=NEW.volume_inode)
 OR (v.writer_device=NEW.volume_device AND v.writer_inode=NEW.volume_inode)
 OR (v.volume_device=NEW.data_device AND v.volume_inode=NEW.data_inode)
 OR (v.data_device=NEW.data_device AND v.data_inode=NEW.data_inode)
 OR (v.writer_device=NEW.data_device AND v.writer_inode=NEW.data_inode)
 OR (v.volume_device=NEW.writer_device AND v.volume_inode=NEW.writer_inode)
 OR (v.data_device=NEW.writer_device AND v.data_inode=NEW.writer_inode)
 OR (v.writer_device=NEW.writer_device AND v.writer_inode=NEW.writer_inode))) OR
 EXISTS(SELECT 1 FROM repository_volumes v WHERE ((v.volume_device=NEW.volume_device AND v.volume_inode=NEW.volume_inode)
 OR (v.data_device=NEW.volume_device AND v.data_inode=NEW.volume_inode)
 OR (v.writer_device=NEW.volume_device AND v.writer_inode=NEW.volume_inode)
 OR (v.volume_device=NEW.data_device AND v.volume_inode=NEW.data_inode)
 OR (v.data_device=NEW.data_device AND v.data_inode=NEW.data_inode)
 OR (v.writer_device=NEW.data_device AND v.writer_inode=NEW.data_inode)
 OR (v.volume_device=NEW.writer_device AND v.volume_inode=NEW.writer_inode)
 OR (v.data_device=NEW.writer_device AND v.data_inode=NEW.writer_inode)
 OR (v.writer_device=NEW.writer_device AND v.writer_inode=NEW.writer_inode))) OR
 EXISTS(SELECT 1 FROM datastore_volumes v WHERE ((v.volume_device=NEW.volume_device AND v.volume_inode=NEW.volume_inode)
 OR (v.data_device=NEW.volume_device AND v.data_inode=NEW.volume_inode)
 OR (v.writer_device=NEW.volume_device AND v.writer_inode=NEW.volume_inode)
 OR (v.volume_device=NEW.data_device AND v.volume_inode=NEW.data_inode)
 OR (v.data_device=NEW.data_device AND v.data_inode=NEW.data_inode)
 OR (v.writer_device=NEW.data_device AND v.writer_inode=NEW.data_inode)
 OR (v.volume_device=NEW.writer_device AND v.volume_inode=NEW.writer_inode)
 OR (v.data_device=NEW.writer_device AND v.data_inode=NEW.writer_inode)
 OR (v.writer_device=NEW.writer_device AND v.writer_inode=NEW.writer_inode))))
BEGIN SELECT RAISE(ABORT,'provider/core volume identity overlap'); END;
CREATE TRIGGER repository_volumes_oidc_root BEFORE INSERT ON repository_volumes
WHEN EXISTS(SELECT 1 FROM oidc_provider_volumes v WHERE v.store_id!=NEW.store_id OR v.root_device!=NEW.root_device OR v.root_inode!=NEW.root_inode)
BEGIN SELECT RAISE(ABORT,'all managed volume roles require the recorded root'); END;
CREATE TRIGGER repository_volumes_oidc_separation BEFORE UPDATE ON repository_volumes
WHEN NEW.allocated=1 AND (EXISTS(SELECT 1 FROM oidc_provider_volumes v WHERE ((v.volume_device=NEW.volume_device AND v.volume_inode=NEW.volume_inode)
 OR (v.data_device=NEW.volume_device AND v.data_inode=NEW.volume_inode)
 OR (v.writer_device=NEW.volume_device AND v.writer_inode=NEW.volume_inode)
 OR (v.volume_device=NEW.data_device AND v.volume_inode=NEW.data_inode)
 OR (v.data_device=NEW.data_device AND v.data_inode=NEW.data_inode)
 OR (v.writer_device=NEW.data_device AND v.writer_inode=NEW.data_inode)
 OR (v.volume_device=NEW.writer_device AND v.volume_inode=NEW.writer_inode)
 OR (v.data_device=NEW.writer_device AND v.data_inode=NEW.writer_inode)
 OR (v.writer_device=NEW.writer_device AND v.writer_inode=NEW.writer_inode))))
BEGIN SELECT RAISE(ABORT,'provider/core volume identity overlap'); END;
CREATE TRIGGER datastore_volumes_oidc_root BEFORE INSERT ON datastore_volumes
WHEN EXISTS(SELECT 1 FROM oidc_provider_volumes v WHERE v.store_id!=NEW.store_id OR v.root_device!=NEW.root_device OR v.root_inode!=NEW.root_inode)
BEGIN SELECT RAISE(ABORT,'all managed volume roles require the recorded root'); END;
CREATE TRIGGER datastore_volumes_oidc_separation BEFORE UPDATE ON datastore_volumes
WHEN NEW.allocated=1 AND (EXISTS(SELECT 1 FROM oidc_provider_volumes v WHERE ((v.volume_device=NEW.volume_device AND v.volume_inode=NEW.volume_inode)
 OR (v.data_device=NEW.volume_device AND v.data_inode=NEW.volume_inode)
 OR (v.writer_device=NEW.volume_device AND v.writer_inode=NEW.volume_inode)
 OR (v.volume_device=NEW.data_device AND v.volume_inode=NEW.data_inode)
 OR (v.data_device=NEW.data_device AND v.data_inode=NEW.data_inode)
 OR (v.writer_device=NEW.data_device AND v.writer_inode=NEW.data_inode)
 OR (v.volume_device=NEW.writer_device AND v.volume_inode=NEW.writer_inode)
 OR (v.data_device=NEW.writer_device AND v.data_inode=NEW.writer_inode)
 OR (v.writer_device=NEW.writer_device AND v.writer_inode=NEW.writer_inode))))
BEGIN SELECT RAISE(ABORT,'provider/core volume identity overlap'); END;
