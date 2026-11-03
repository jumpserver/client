use super::client_session::ClientLoginPolicy;
use anyhow::Result;
use keyring_core::{Entry, Error as KeyringError};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    sync::{LazyLock, Mutex},
    time::{Instant, SystemTime},
};

const SERVICE_NAME: &str = "com.jumpserver.client.auth";
static TOKENS: LazyLock<Mutex<HashMap<String, Option<TokenRecord>>>> =
    LazyLock::new(Default::default);

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct TokenRecord {
    pub access_token: String,
    pub refresh_token: Option<String>,
    pub expires_at: Option<i64>,
    pub client_id: Option<String>,
    #[serde(default)]
    pub policy: Option<ClientLoginPolicy>,
    #[serde(default)]
    pub policy_supported: Option<bool>,
    #[serde(default)]
    pub deadline: Option<SystemTime>,
    #[serde(skip)]
    pub monotonic_deadline: Option<Instant>,
}

pub struct TokenService {
    site: String,
}

fn token_entry(site: &str) -> Result<Entry> {
    keyring::use_native_store(false)?;
    Ok(Entry::new(SERVICE_NAME, site)?)
}

impl TokenService {
    pub fn new(site: impl Into<String>) -> Self {
        Self { site: site.into() }
    }

    pub fn cache(&self, record: TokenRecord) {
        TOKENS
            .lock()
            .expect("token cache poisoned")
            .insert(self.site.clone(), Some(record));
    }

    pub async fn save(&self, record: &TokenRecord) -> Result<()> {
        if record
            .policy
            .as_ref()
            .is_some_and(|policy| policy.reauth_on_exit)
        {
            // Exit-invalidated credentials live only in memory, including across refreshes.
            self.delete().await?;
            self.cache(record.clone());
            return Ok(());
        }
        let payload = serde_json::to_string(record)?;
        let site = self.site.clone();
        tokio::task::spawn_blocking(move || -> Result<()> {
            token_entry(&site)?.set_password(&payload)?;
            Ok(())
        })
        .await??;
        self.cache(record.clone());
        Ok(())
    }

    pub async fn load(&self) -> Result<Option<TokenRecord>> {
        if let Some(record) = TOKENS.lock().expect("token cache poisoned").get(&self.site) {
            return Ok(record.clone());
        }
        let site = self.site.clone();
        let mut record = tokio::task::spawn_blocking(move || -> Result<Option<TokenRecord>> {
            match token_entry(&site)?.get_password() {
                Ok(val) => Ok(Some(serde_json::from_str(&val)?)),
                Err(KeyringError::NoEntry) => Ok(None),
                Err(e) => Err(e.into()),
            }
        })
        .await??;
        if record.as_ref().is_some_and(|record| {
            record
                .policy
                .as_ref()
                .is_some_and(|policy| policy.reauth_on_exit)
        }) {
            self.delete().await?;
            return Ok(None);
        }
        if let Some(record) = &mut record {
            // Rediscover once per process so upgrading a legacy server cannot freeze old policy.
            record.policy_supported = None;
            self.cache(record.clone());
        }
        Ok(record)
    }

    pub async fn delete(&self) -> Result<()> {
        TOKENS
            .lock()
            .expect("token cache poisoned")
            .insert(self.site.clone(), None);
        let site = self.site.clone();
        tokio::task::spawn_blocking(move || -> Result<()> {
            match token_entry(&site)?.delete_credential() {
                Ok(()) | Err(KeyringError::NoEntry) => Ok(()),
                Err(error) => Err(error.into()),
            }
        })
        .await??;
        Ok(())
    }

    pub fn exit_sites() -> Vec<String> {
        TOKENS
            .lock()
            .expect("token cache poisoned")
            .iter()
            .filter(|(_, record)| {
                record
                    .as_ref()
                    .and_then(|r| r.policy.as_ref())
                    .is_some_and(|p| p.reauth_on_exit)
            })
            .map(|(site, _)| site.clone())
            .collect()
    }
}
