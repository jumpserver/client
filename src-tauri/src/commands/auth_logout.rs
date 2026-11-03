use crate::api::session::ApiSessionStore;
use crate::service::oauth::revoke_and_clear_tokens;
use log::warn;
use tauri::{AppHandle, State};

#[tauri::command]
pub async fn logout(
    _app: AppHandle,
    state: State<'_, ApiSessionStore>,
    _name: String,
    site: String,
) -> Result<(), String> {
    state.clear_site(&site);
    if let Err(e) = revoke_and_clear_tokens(&site).await {
        warn!("revoke token failed: {}", e);
    }

    Ok(())
}
