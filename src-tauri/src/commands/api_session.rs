use crate::{
    api::session::{ApiSessionContext, ApiSessionStore},
    service::client_session,
};
use tauri::{AppHandle, Emitter, State};

#[tauri::command]
pub fn set_api_session(
    state: State<'_, ApiSessionStore>,
    session_key: String,
    origin: String,
    bearer_token: String,
    org_id: String,
) {
    state.set_current_session(session_key, origin, bearer_token, org_id)
}

#[tauri::command]
pub fn set_api_org(state: State<'_, ApiSessionStore>, org_id: String) -> Result<(), String> {
    state.set_current_org(org_id)
}

async fn checked_token(
    app: &AppHandle,
    state: &ApiSessionStore,
    site: &str,
    validate_identity: bool,
) -> Result<String, String> {
    let result = if validate_identity {
        client_session::check_session(site).await
    } else {
        client_session::checked_token(site).await
    };
    match result {
        Ok(token) => Ok(token),
        Err(error) => {
            if client_session::is_expired(&error) {
                state.clear_site(site);
                let _ = app.emit(
                    "client-session-expired",
                    serde_json::json!({ "site": site }),
                );
            }
            Err(error.to_string())
        }
    }
}

#[tauri::command]
pub async fn check_client_session(
    app: AppHandle,
    state: State<'_, ApiSessionStore>,
    site: String,
) -> Result<Option<client_session::ClientSessionInfo>, String> {
    checked_token(&app, &state, &site, true).await?;
    client_session::session_info(&site)
        .await
        .map_err(|error| error.to_string())
}

pub(crate) fn session_error_status(error: &str) -> u16 {
    if error == client_session::EXPIRED {
        401
    } else {
        0
    }
}

pub(crate) async fn fresh_api_context(
    app: &AppHandle,
    state: &ApiSessionStore,
) -> Result<ApiSessionContext, String> {
    let mut context = state
        .current_context()
        .ok_or_else(|| "missing current api session".to_string())?;
    let bearer = checked_token(app, state, &context.origin, false).await?;
    state.update_bearer_token(&context.origin, bearer.clone())?;
    context.bearer_token = bearer;
    Ok(context)
}
