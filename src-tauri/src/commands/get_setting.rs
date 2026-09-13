use crate::commands::auth_login::ensure_fresh_token;
use crate::service::setting::SettingService;
use crate::service::user::{parse_remember_auth_enabled, UserService};
use log::{error, info};
use serde::Serialize;
use tauri::AppHandle;

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GetSettingResult {
    remember_auth_enabled: bool,
    setting_success: bool,
    setting_status: u16,
    setting_data: Option<String>,
}

#[tauri::command]
pub async fn get_setting(app: AppHandle, site: String, bearer_token: String) -> GetSettingResult {
    let bearer = match ensure_fresh_token(&app, &site, Some(&bearer_token)).await {
        Ok(b) => b,
        Err(e) => {
            error!("获取 Setting 数据失败: {}", e);
            return GetSettingResult {
                remember_auth_enabled: true,
                setting_success: false,
                setting_status: 401,
                setting_data: None,
            };
        }
    };

    let setting_service = SettingService::new(site.clone(), bearer.clone());
    let user_service = UserService::new(site, bearer);
    let (setting_data, public_settings) = tokio::join!(
        setting_service.get_setting(),
        user_service.get_xpack_message(),
    );
    let remember_auth_enabled = parse_remember_auth_enabled(&public_settings);

    if !setting_data.success {
        error!("获取 Setting 数据失败");
    } else {
        info!("获取 Setting 数据成功");
    }

    GetSettingResult {
        remember_auth_enabled,
        setting_success: setting_data.success,
        setting_status: setting_data.status,
        setting_data: setting_data.success.then_some(setting_data.data),
    }
}
