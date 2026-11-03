use serde_json::Value;
use tauri::{AppHandle, Emitter, State};

use crate::api::client::{api_client_for_origin, oauth_client_for_origin};
use crate::service::oauth::{
    build_oauth_client, create_authorization_request, exchange_authorization_code,
    fetch_oauth_config, AuthFlowState,
};
use crate::service::user::UserService;

#[tauri::command]
pub async fn auth_login(
    app: AppHandle,
    flow_state: State<'_, AuthFlowState>,
    site: String,
) -> Result<(), String> {
    // 获取 OAuth 配置
    let config_http_client = api_client_for_origin(&site).map_err(|e| e.to_string())?;
    let oauth_config = match fetch_oauth_config(&site, &config_http_client).await {
        Ok(config) => config,
        Err(e) => {
            let msg = format!("Failed to fetch OAuth config: {}", e);
            log::error!("{}", msg);

            let _ = app.emit(
                "login-failed-detected",
                serde_json::json!({
                    "status": "failure",
                    "reason": "invalid-site",
                    "message": msg.clone(),
                    "site": site,
                }),
            );

            return Err(msg);
        }
    };

    let policy_supported = oauth_config.client_login_policy_version.is_some()
        || oauth_config.client_login_policy.is_some();
    let client_id = oauth_config.client_id;

    let fut = async {
        let client = build_oauth_client(&site, &client_id)?;

        // 生成 PKCE + 授权 URL
        let authorize = create_authorization_request(&client);
        let pending = flow_state.register_authorization(authorize);

        if let Err(e) = app.emit("auth_url", pending.auth_url) {
            log::warn!("emit auth_url failed: {}", e);
        }

        let http_client = oauth_client_for_origin(&site)?;

        // 等待 deep link 回调传回 code/state
        let callback = match pending.callback_rx.await {
            Ok(callback) => callback,
            Err(_) => {
                log::info!("auth flow cancelled");
                return Ok(());
            }
        };

        let tokens = exchange_authorization_code(&client, &http_client, callback).await?;

        // 保存 OAuth token，供后续请求自动刷新使用。
        tokens
            .persist(
                &site,
                &client_id,
                oauth_config.client_login_policy,
                policy_supported,
            )
            .await?;
        let access_token = crate::service::client_session::checked_token(&site).await?;

        // 发起请求
        let user_service = UserService::new(site.clone(), access_token.clone())?;
        let (profile, permission_orgs, current_org, xpack_message) = tokio::join!(
            user_service.get_user_profile(),
            user_service.get_permission_orgs(),
            user_service.get_current_org(),
            user_service.get_xpack_message(),
        );

        if profile.status != 200 || !profile.success {
            anyhow::bail!("Unable to validate the user profile");
        }

        let license_valid = if xpack_message.status == 200 && xpack_message.success {
            serde_json::from_str::<Value>(&xpack_message.data)
                .ok()
                .and_then(|value| {
                    value
                        .get("XPACK_LICENSE_IS_VALID")
                        .and_then(|v| v.as_bool())
                })
                .unwrap_or(false)
        } else {
            false
        };

        let _ = app.emit(
            "login-success-detected",
            serde_json::json!({
                "status": "success",
                "bearer": access_token,
                "profile": profile,
                "resolved_site": site,
                "current_org": current_org,
                "xpack_license_valid": license_valid,
                "permission_orgs": permission_orgs,
                "client_login_session": crate::service::client_session::session_info(&site).await?,
            }),
        );

        Ok::<(), anyhow::Error>(())
    };

    fut.await.map_err(|e| e.to_string())
}

#[tauri::command]
pub fn auth_cancel(flow_state: State<'_, AuthFlowState>) -> Result<(), String> {
    flow_state.cancel();
    Ok(())
}
