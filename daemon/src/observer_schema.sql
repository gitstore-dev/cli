CREATE TABLE observer_slots(
 worker_slot INTEGER PRIMARY KEY CHECK(worker_slot BETWEEN 0 AND 7),
 subscription_id TEXT NOT NULL UNIQUE CHECK(gstd_identifier(subscription_id)),
 deployment_id TEXT NOT NULL REFERENCES deployments(deployment_id)
) STRICT;
CREATE TRIGGER observer_slot_runtime_guard BEFORE INSERT ON observer_slots
WHEN EXISTS(SELECT 1 FROM jobs WHERE worker_slot=NEW.worker_slot)
BEGIN SELECT RAISE(ABORT,'worker slot is reserved by lifecycle work'); END;
CREATE TRIGGER observer_slot_immutable BEFORE UPDATE ON observer_slots
BEGIN SELECT RAISE(ABORT,'observer lease identity is immutable'); END;
CREATE TRIGGER runtime_slot_observer_insert BEFORE INSERT ON jobs
WHEN NEW.worker_slot IS NOT NULL AND EXISTS(SELECT 1 FROM observer_slots WHERE worker_slot=NEW.worker_slot)
BEGIN SELECT RAISE(ABORT,'worker slot is reserved by observation'); END;
CREATE TRIGGER runtime_slot_observer_update BEFORE UPDATE OF worker_slot ON jobs
WHEN NEW.worker_slot IS NOT NULL AND EXISTS(SELECT 1 FROM observer_slots WHERE worker_slot=NEW.worker_slot)
BEGIN SELECT RAISE(ABORT,'worker slot is reserved by observation'); END;
