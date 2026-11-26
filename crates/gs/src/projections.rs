//! Exact alpha.9 core resource projections (plus alpha.12 list kinds). Source: GitStore
//! 68f8471edc947e417c98b6e8299a1367ab6b20da shared/schemas/admin/*.graphqls.
use crate::resources::Kind;

const REFERENCE: &str = "apiVersion kind name namespace uid resourceVersion fieldPath";
const MEDIA: &str = "media { fileRef { name kind optional } }";
const CONDITIONS: &str =
    "conditions { type status observedGeneration lastTransitionTime reason message }";
const FILE: &str = "name url contentType resolvedVariants { name url }";
const POLICY: &str = "maxPackSizeBytes maxFileSizeBytes receivePackHooks { preReceive { enabled } update { enabled } postReceive { enabled } procReceive { enabled } postUpdate { enabled } referenceTransaction { enabled } } schemaValidation { phase timeoutSeconds } admissionControl { phase branchPattern }";

pub fn fields(kind: Kind) -> String {
    if kind == Kind::ServiceAccount {
        // Actor shape (alpha.12): no spec/status/body. Source: GitStore
        // v0.1.0-alpha.12 shared/schemas/admin/serviceaccount.graphqls.
        return "id apiVersion kind metadata { name uid resourceVersion generation revision } keyIDs groups lifecycle { state }".into();
    }
    let metadata = "metadata { name namespace labels annotations uid resourceVersion generation creationTimestamp revision ownerReferences { apiVersion kind name uid blockOwnerDeletion } owner { kind name } finalizers deletionTimestamp }";
    let spec = match kind {
        Kind::Namespace => format!("title tier repositoryDefaults {{ visibility defaultBranch }} pushPolicyDefaults {{ {POLICY} }}"),
        Kind::Repository => format!("defaultBranch visibility pushPolicy {{ {POLICY} }}"),
        Kind::Product => format!("title categoryRef {{ {REFERENCE} }} tags {MEDIA} options {{ name title values }} lifecycle {{ state }}"),
        Kind::Category => format!("title parentRef {{ {REFERENCE} }} {MEDIA}"),
        Kind::Collection => format!("title selector {{ matchLabels matchExpressions {{ key operator values }} }} {MEDIA}"),
        // Core subset: pricing, media and resolved status are not projected. Source: GitStore
        // v0.1.0-alpha.12 shared/schemas/admin/product_variant.graphqls (unchanged since alpha.9).
        Kind::Variant => format!("title sku productRef {{ {REFERENCE} }} selectedOptions {{ name value }} inventory {{ managed policy }}"),
        Kind::ServiceAccount => unreachable!("returned above"),
        Kind::File => "contentType type provider source { uri checksum { algorithm value } credentialsRef { kind type secretRef { kind name key namespace } } } processing { image { variants { name } } }".into(),
    };
    let resolved = match kind {
        Kind::Namespace => String::new(),
        Kind::Repository => "resolved { storagePath storageClass }".into(),
        Kind::Product => format!(
            "resolved {{ category {{ name path uid }} priceRange {{ currencyCode min max }} totalInventory variantSummary {{ total ready unavailable }} defaultVariantRef {{ {REFERENCE} }} media {{ {FILE} }} }}"
        ),
        Kind::Category => "resolved { depth path childCount productCount }".into(),
        Kind::Collection => "resolved { memberCount }".into(),
        Kind::File => format!("resolved {{ {FILE} }}"),
        Kind::Variant => String::new(),
        Kind::ServiceAccount => unreachable!("returned above"),
    };
    format!(
        "id apiVersion kind {metadata} spec {{ {spec} }} status {{ observedGeneration lastAppliedRevision {CONDITIONS} {resolved} }} body"
    )
}
