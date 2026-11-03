use crate::api::client::oauth_client_for_origin;
use crate::service::{
    oauth,
    token::{TokenRecord, TokenService},
};
use anyhow::Result;
use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    sync::{Arc, LazyLock, Mutex},
    time::{Instant, SystemTime},
};
use tokio::sync::Mutex as AsyncMutex;

pub const EXPIRED: &str = "client_session_expired";
static LOCKS: LazyLock<Mutex<HashMap<String, Arc<AsyncMutex<()>>>>> =
    LazyLock::new(Default::default);

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq)]
pub struct ClientLoginPolicy {
    pub reauth_on_exit: bool,
    pub max_session_time: u64,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ClientLoginSession {
    expires_at: Option<String>,
    server_time: String,
}

/// UI-safe lifetime information; credentials stay in the native token store.
#[derive(Debug, Serialize)]
pub struct ClientSessionInfo {
    pub max_session_time: u64,
    pub expires_at: Option<String>,
    pub remaining_seconds: Option<u64>,
}

pub async fn session_info(site: &str) -> Result<Option<ClientSessionInfo>> {
    let Some(record) = TokenService::new(site).load().await? else {
        return Ok(None);
    };
    let Some(policy) = record.policy else {
        return Ok(None);
    };
    let now = SystemTime::now();
    let remaining = record.deadline.map(|deadline| {
        let wall = deadline.duration_since(now).unwrap_or_default();
        record.monotonic_deadline.map_or(wall, |deadline| {
            wall.min(deadline.saturating_duration_since(Instant::now()))
        })
    });
    Ok(Some(ClientSessionInfo {
        max_session_time: policy.max_session_time,
        expires_at: remaining.map(|time| DateTime::<Utc>::from(now + time).to_rfc3339()),
        remaining_seconds: remaining.map(|time| time.as_secs()),
    }))
}

impl ClientLoginSession {
    pub fn apply_to(&self, record: &mut TokenRecord) -> Result<()> {
        let server_time = DateTime::parse_from_rfc3339(&self.server_time)?;
        let deadline = self
            .expires_at
            .as_ref()
            .map(|value| DateTime::parse_from_rfc3339(value))
            .transpose()?;
        if deadline.is_some_and(|time| time <= server_time) {
            anyhow::bail!(EXPIRED);
        }
        anyhow::ensure!(
            deadline.is_some()
                || !record
                    .policy
                    .as_ref()
                    .is_some_and(|p| p.max_session_time > 0),
            "Missing client login expiry"
        );
        if let Some(deadline) = deadline {
            let remaining = (deadline - server_time).to_std()?;
            let wall = SystemTime::now() + remaining;
            let monotonic = Instant::now() + remaining;
            record.deadline = Some(record.deadline.map_or(wall, |old| old.min(wall)));
            record.monotonic_deadline = Some(
                record
                    .monotonic_deadline
                    .map_or(monotonic, |old| old.min(monotonic)),
            );
        }
        Ok(())
    }
}

pub fn site_lock(site: &str) -> Arc<AsyncMutex<()>> {
    LOCKS
        .lock()
        .expect("session locks poisoned")
        .entry(site.to_string())
        .or_default()
        .clone()
}

pub fn is_expired(error: &anyhow::Error) -> bool {
    error.to_string() == EXPIRED
}

fn expired_locally(record: &TokenRecord) -> bool {
    record
        .deadline
        .is_some_and(|time| SystemTime::now() >= time)
        || record
            .monotonic_deadline
            .is_some_and(|time| Instant::now() >= time)
}

async fn expire(service: &TokenService) -> Result<String> {
    // Tombstone memory before touching the keyring; a failed deletion must not restore a session.
    if let Err(error) = service.delete().await {
        log::warn!("clear expired token failed: {}", error);
    }
    anyhow::bail!(EXPIRED)
}

/// Check the server-controlled absolute session lifetime before using credentials.
pub async fn checked_token(site: &str) -> Result<String> {
    check(site, false).await
}

/// Background/startup identity checks reuse the existing profile API.
pub async fn check_session(site: &str) -> Result<String> {
    check(site, true).await
}

async fn check(site: &str, validate_identity: bool) -> Result<String> {
    let lock = site_lock(site);
    let _guard = lock.lock().await;
    let service = TokenService::new(site);
    let Some(mut record) = service.load().await? else {
        anyhow::bail!(EXPIRED);
    };
    if expired_locally(&record) {
        return expire(&service).await;
    }

    let http = oauth_client_for_origin(site)?;
    if record.policy_supported.is_none()
        || (validate_identity && record.policy_supported != Some(true))
    {
        // Recheck legacy servers too: an in-place server upgrade may enable the policy.
        let config = oauth::fetch_oauth_config(site, &http).await?;
        let supported =
            config.client_login_policy_version.is_some() || config.client_login_policy.is_some();
        anyhow::ensure!(
            !supported
                || (config.client_login_policy_version == Some(1)
                    && config.client_login_policy.is_some()),
            "Unsupported client login policy metadata"
        );
        anyhow::ensure!(
            record.policy.is_none() || supported,
            "Client login policy metadata disappeared"
        );
        let supported = Some(supported);
        let changed =
            record.policy_supported != supported || record.policy != config.client_login_policy;
        record.policy_supported = supported;
        record.policy = config.client_login_policy;
        if changed {
            service.save(&record).await?;
        }
        if expired_locally(&record) {
            return expire(&service).await;
        }
    }

    if oauth::should_refresh_token(record.expires_at) {
        let Some(refresh) = record.refresh_token.as_deref() else {
            return expire(&service).await;
        };
        let tokens = match oauth::refresh_access_token(
            site,
            record.client_id.as_deref().unwrap_or_default(),
            refresh,
            &http,
        )
        .await
        {
            Ok(tokens) => tokens,
            Err(error) if is_expired(&error) => return expire(&service).await,
            Err(error) => return Err(error),
        };
        if let Err(error) = tokens.apply_to(&mut record) {
            if is_expired(&error) {
                return expire(&service).await;
            }
            return Err(error);
        }
        service.save(&record).await?;
    }

    if validate_identity {
        // Validate identity, never organization availability. Ordinary APIs validate
        // their bearer directly; no additional session endpoint is needed.
        let response = http
            .get(format!(
                "{}{}",
                site.trim_end_matches('/'),
                crate::api::endpoint::user::PROFILE
            ))
            .bearer_auth(&record.access_token)
            .send()
            .await?;
        if response.status() == reqwest::StatusCode::UNAUTHORIZED {
            return expire(&service).await;
        }
        if response.status() == reqwest::StatusCode::FORBIDDEN {
            // V4's first authentication backend has no challenge header, so DRF
            // can report missing/revoked credentials as 403. Permission 403s stay errors.
            let body: serde_json::Value = response.json().await?;
            if matches!(
                body.get("code").and_then(|value| value.as_str()),
                Some("not_authenticated" | "authentication_failed" | "client_session_expired")
            ) {
                return expire(&service).await;
            }
            anyhow::bail!("Profile access denied");
        }
        response.error_for_status()?;
    }
    if expired_locally(&record) {
        return expire(&service).await;
    }
    Ok(record.access_token)
}

/// Graceful exit revokes best-effort; memory-only credentials also cover crashes and forced exits.
pub async fn revoke_on_exit() {
    let sites = TokenService::exit_sites();
    let mut tasks = tokio::task::JoinSet::new();
    for site in sites {
        tasks.spawn(async move {
            let _ = oauth::revoke_and_clear_tokens(&site).await;
        });
    }
    while tasks.join_next().await.is_some() {}
}
