DROP TRIGGER network_attachment_ownership;
CREATE TRIGGER network_attachment_ownership BEFORE INSERT ON network_attachments
WHEN NOT EXISTS(SELECT 1 FROM resource_ownership w JOIN resource_ownership n
 ON w.deployment_id=n.deployment_id AND w.operation_id=n.operation_id
 JOIN operations o ON w.operation_id=o.operation_id
 WHERE w.resource_id=NEW.workload_id AND w.resource_type='workload' AND
 n.resource_id=NEW.network_id AND n.resource_type='network' AND
 w.operation_id=NEW.operation_id AND w.ownership_state='held' AND n.ownership_state='held' AND
 w.lifecycle_stage='reserved' AND n.lifecycle_stage IN ('allocated','running') AND
 n.backend_identity IS NOT NULL AND o.method IN ('deployment.start','migration.start') AND
 o.state IN ('accepted','running'))
BEGIN SELECT RAISE(ABORT,'network attachment requires owned start or migration resources'); END;
CREATE TRIGGER migration_attachment_guard BEFORE INSERT ON network_attachments
WHEN EXISTS(SELECT 1 FROM operations WHERE operation_id=NEW.operation_id AND method='migration.start')
AND NOT EXISTS(SELECT 1 FROM operations o JOIN deployments d ON d.deployment_id=o.deployment_id
 JOIN resource_ownership m ON m.operation_id=o.operation_id AND m.deployment_id=d.deployment_id
 WHERE (o.operation_id=NEW.operation_id AND o.method='migration.start' AND o.state='running' AND o.recovery='normal')
 AND (d.desired_state='stopped' AND d.recovery='normal' AND d.writer_ownership='released')
 AND (NEW.network_id='net-'||o.operation_id AND NEW.workload_id IN
  ('run-scylladb-'||o.operation_id,'run-cql-'||o.operation_id,'run-schema-'||o.operation_id))
 AND (m.resource_id='migration-'||o.operation_id AND m.resource_type='configuration' AND
  m.ownership_state='held' AND gstd_migration_preflight(m.backend_identity,d.generation)=1))
BEGIN SELECT RAISE(ABORT,'migration attachment requires current preflight resources'); END;
