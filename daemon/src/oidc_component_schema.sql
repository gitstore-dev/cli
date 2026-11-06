DROP TRIGGER component_insert_revision;
DROP TRIGGER component_update_revision;
DROP TRIGGER component_delete_revision;
CREATE TABLE components_v6(
 deployment_id TEXT NOT NULL REFERENCES deployments(deployment_id),
 component TEXT NOT NULL CHECK(component IN ('api','git','controller','oidc','admin','storefront','oidc-smtp','oidc-hydra','oidc-kratos','oidc-ui','oidc-bridge')),
 backend_id TEXT, image_digest TEXT NOT NULL,
 desired_state TEXT NOT NULL CHECK(desired_state IN ('running','stopped')),
 observed_state TEXT NOT NULL CHECK(observed_state IN ('pending','running','stopped','failed','unknown')),
 generation INTEGER NOT NULL CHECK(generation BETWEEN 1 AND 9007199254740991),
 observed_at TEXT NOT NULL,
 PRIMARY KEY(deployment_id,component)
) STRICT;
INSERT INTO components_v6 SELECT * FROM components;
DROP TABLE components;
ALTER TABLE components_v6 RENAME TO components;
CREATE TRIGGER component_insert_revision AFTER INSERT ON components
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER component_update_revision AFTER UPDATE ON components
BEGIN UPDATE query_meta SET generation=generation+1; END;
CREATE TRIGGER component_delete_revision AFTER DELETE ON components
BEGIN UPDATE query_meta SET generation=generation+1; END;
