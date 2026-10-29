# Upstream contract fixtures

Source: GitStore committed snapshot `b6c866421dbcf43994565f847553c1f6aec83994`. Hashes are recorded in provenance.json. No working-tree source or secrets were copied.

The client uses public GraphQL operations: namespace/namespaces, repository/repositories, product/products, category/categories, collection/collections, and file(namespace,name). The File lifecycle spec will normalize its selector later.

Creates use Product/CategoryTaxonomy author projections; deletes use resolved opaque IDs and return TERMINATION_STARTED or ALREADY_TERMINATING. Full input and response assertions live in resources.rs and authoring.rs. GraphQL and Git listeners are configured independently. Source schemas are AGPL-3.0-or-later, Copyright 2026 GitStore contributors; this directory records hashes and derived test cases rather than vendoring server code.
