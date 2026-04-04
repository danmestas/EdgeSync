use iroh::{Endpoint, SecretKey, endpoint::presets};
use std::path::Path;
use tokio::fs;

/// Load or generate an Ed25519 keypair, then bind an iroh Endpoint.
pub async fn create_endpoint(
    key_path: &Path,
    alpn: &[u8],
) -> anyhow::Result<(Endpoint, String)> {
    let secret_key = load_or_generate_key(key_path).await?;
    let endpoint_id = secret_key.public().to_string();

    let endpoint = Endpoint::builder(presets::N0)
        .secret_key(secret_key)
        .alpns(vec![alpn.to_vec()])
        .bind()
        .await?;

    tracing::info!(%endpoint_id, "iroh endpoint bound");
    Ok((endpoint, endpoint_id))
}

async fn load_or_generate_key(path: &Path) -> anyhow::Result<SecretKey> {
    if path.exists() {
        let bytes = fs::read(path).await?;
        // Expect exactly 32 raw bytes (Ed25519 secret scalar).
        let arr: [u8; 32] = bytes
            .try_into()
            .map_err(|_| anyhow::anyhow!("key file is not 32 bytes"))?;
        let key = SecretKey::from_bytes(&arr);
        tracing::info!("loaded existing keypair from {}", path.display());
        Ok(key)
    } else {
        let key = SecretKey::generate(&mut rand::rng());
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).await?;
        }
        fs::write(path, key.to_bytes()).await?;
        tracing::info!("generated new keypair at {}", path.display());
        Ok(key)
    }
}
