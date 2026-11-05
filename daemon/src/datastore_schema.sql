CREATE TABLE datastore_volumes(
 volume_id TEXT PRIMARY KEY REFERENCES resource_ownership(resource_id),
 role TEXT NOT NULL CHECK(role='scylladb'),
 owner_uid INTEGER NOT NULL CHECK(owner_uid>=0),
 producer_operation_id TEXT NOT NULL REFERENCES operations(operation_id),
 store_id TEXT NOT NULL CHECK(length(store_id)=32 AND store_id NOT GLOB '*[^0-9a-f]*'),
 root_device TEXT NOT NULL CHECK(length(root_device) BETWEEN 1 AND 20 AND root_device NOT GLOB '*[^0-9]*'),
 root_inode TEXT NOT NULL CHECK(length(root_inode) BETWEEN 1 AND 20 AND root_inode NOT GLOB '*[^0-9]*' AND root_inode!='0'),
 compatibility TEXT NOT NULL CHECK(compatibility='scylla-2026.1:gitstore-06152d-schema-012'),
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
CREATE TRIGGER datastore_volume_owner BEFORE INSERT ON datastore_volumes
WHEN NOT EXISTS(SELECT 1 FROM resource_ownership r JOIN operations o ON r.operation_id=o.operation_id
 AND r.deployment_id=o.deployment_id JOIN query_meta q ON q.store_id=NEW.store_id
 WHERE r.resource_id=NEW.volume_id AND r.resource_type='datastore-volume' AND
 r.ownership_state='held' AND r.lifecycle_stage='reserved' AND
 o.operation_id=NEW.producer_operation_id AND o.caller_uid=NEW.owner_uid AND
 o.method='deployment.start' AND json_extract(o.intent,'$.datastore')='scylladb' AND o.state IN ('accepted','running')) OR
 EXISTS(SELECT 1 FROM datastore_volumes WHERE store_id!=NEW.store_id OR
 root_device!=NEW.root_device OR root_inode!=NEW.root_inode) OR
 EXISTS(SELECT 1 FROM repository_volumes WHERE store_id!=NEW.store_id OR
 root_device!=NEW.root_device OR root_inode!=NEW.root_inode) OR
 (SELECT count(*) FROM datastore_volumes)>=4096
BEGIN SELECT RAISE(ABORT,'datastore volume requires a bounded reserved owner/root'); END;
CREATE TRIGGER datastore_volume_immutable BEFORE UPDATE ON datastore_volumes
WHEN OLD.allocated=1 OR NEW.volume_id IS NOT OLD.volume_id OR NEW.role IS NOT OLD.role OR
 NEW.owner_uid IS NOT OLD.owner_uid OR NEW.producer_operation_id IS NOT OLD.producer_operation_id OR
 NEW.store_id IS NOT OLD.store_id OR NEW.root_device IS NOT OLD.root_device OR
 NEW.root_inode IS NOT OLD.root_inode OR NEW.compatibility IS NOT OLD.compatibility
BEGIN SELECT RAISE(ABORT,'immutable datastore volume identity or receipt'); END;
CREATE TRIGGER datastore_volume_retained BEFORE DELETE ON datastore_volumes
BEGIN SELECT RAISE(ABORT,'datastore volume metadata is retained'); END;
CREATE TRIGGER datastore_volume_role_separation BEFORE UPDATE ON datastore_volumes
WHEN NEW.allocated=1 AND EXISTS(SELECT 1 FROM repository_volumes r WHERE
 (r.volume_device=NEW.volume_device AND r.volume_inode=NEW.volume_inode) OR
 (r.data_device=NEW.data_device AND r.data_inode=NEW.data_inode) OR
 (r.writer_device=NEW.writer_device AND r.writer_inode=NEW.writer_inode))
BEGIN SELECT RAISE(ABORT,'datastore/repository volume identity overlap'); END;
CREATE TRIGGER repository_volume_role_separation BEFORE UPDATE ON repository_volumes
WHEN NEW.allocated=1 AND EXISTS(SELECT 1 FROM datastore_volumes d WHERE
 (d.volume_device=NEW.volume_device AND d.volume_inode=NEW.volume_inode) OR
 (d.data_device=NEW.data_device AND d.data_inode=NEW.data_inode) OR
 (d.writer_device=NEW.writer_device AND d.writer_inode=NEW.writer_inode))
BEGIN SELECT RAISE(ABORT,'repository/datastore volume identity overlap'); END;
CREATE TRIGGER repository_volume_datastore_root BEFORE INSERT ON repository_volumes
WHEN EXISTS(SELECT 1 FROM datastore_volumes WHERE store_id!=NEW.store_id OR
 root_device!=NEW.root_device OR root_inode!=NEW.root_inode)
BEGIN SELECT RAISE(ABORT,'repository/datastore require the same recorded root'); END;
