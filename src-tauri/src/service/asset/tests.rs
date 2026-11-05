use super::*;
use axum::{extract::Query, http::StatusCode, routing::get, Json, Router};
use std::collections::HashMap;

async fn service(
    windows: (StatusCode, Value),
    ad: (StatusCode, Value),
) -> (AssetService, tokio::task::JoinHandle<()>) {
    let app = Router::new().route(
        endpoint::assets::USER_ASSETS,
        get(move |Query(query): Query<HashMap<String, String>>| {
            let response = if query.get("type").map(String::as_str) == Some("windows_ad") {
                ad.clone()
            } else {
                windows.clone()
            };
            async move { (response.0, Json(response.1)) }
        }),
    );
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let server = tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    let api = ApiRequestClient::with_origin(origin, String::new(), "test-org".into()).unwrap();
    (AssetService::new(api), server)
}

fn list(items: Vec<Value>) -> (StatusCode, Value) {
    (
        StatusCode::OK,
        json!({"count": items.len(), "results": items, "next": null, "previous": null}),
    )
}

fn invalid_type() -> (StatusCode, Value) {
    (
        StatusCode::BAD_REQUEST,
        json!({"type": ["选择一个有效的选项。 windows_ad 不在可用的选项中。"]}),
    )
}

#[tokio::test]
async fn community_windows_assets_keep_count_order_and_pagination() {
    let items = (1..=23)
        .rev()
        .map(|i| json!({"id": format!("{i:02}"), "name": format!("Windows-{i:02}")}))
        .collect();
    let (service, server) = service(list(items), invalid_type()).await;
    let mut query = AssetQuery::new(Category::Windows, "test-org".into());
    query.limit = Some(20);
    let response = service.get_category_assets(&query, false).await;
    assert!(response.success, "{}: {}", response.status, response.data);
    let body: Value = serde_json::from_str(&response.data).unwrap();
    assert_eq!(body["count"], 23);
    assert_eq!(body["results"].as_array().unwrap().len(), 20);
    assert_eq!(body["results"][0]["name"], "Windows-01");
    query.offset = Some(20);
    let response = service.get_category_assets(&query, false).await;
    assert!(response.success);
    let body: Value = serde_json::from_str(&response.data).unwrap();
    assert_eq!(body["count"], 23);
    assert_eq!(body["results"].as_array().unwrap().len(), 3);
    assert_eq!(body["results"][2]["name"], "Windows-23");
    server.abort();
}

#[tokio::test]
async fn supported_ad_assets_are_still_merged_and_deduplicated() {
    let shared = json!({"id": "1", "name": "B"});
    let (service, server) = service(
        list(vec![shared.clone()]),
        list(vec![shared, json!({"id": "2", "name": "A"})]),
    )
    .await;
    let response = service
        .get_category_assets(
            &AssetQuery::new(Category::Windows, "test-org".into()),
            false,
        )
        .await;
    assert!(response.success);
    let body: Value = serde_json::from_str(&response.data).unwrap();
    assert_eq!(body["count"], 2);
    assert_eq!(body["results"][0]["name"], "A");
    server.abort();
}

#[tokio::test]
async fn other_ad_failures_are_not_silently_ignored() {
    for error in [
        (StatusCode::UNAUTHORIZED, json!({"detail": "expired"})),
        (StatusCode::FORBIDDEN, json!({"detail": "denied"})),
        (
            StatusCode::INTERNAL_SERVER_ERROR,
            json!({"detail": "server error"}),
        ),
        (
            StatusCode::BAD_REQUEST,
            json!({"detail": "invalid request"}),
        ),
        (StatusCode::BAD_REQUEST, json!({"type": []})),
        (
            StatusCode::BAD_REQUEST,
            json!({"type": ["invalid"], "search": ["invalid"]}),
        ),
    ] {
        let (service, server) = service(list(vec![]), error.clone()).await;
        let response = service
            .get_category_assets(
                &AssetQuery::new(Category::Windows, "test-org".into()),
                false,
            )
            .await;
        assert!(!response.success, "must preserve {error:?}");
        assert_eq!(response.status, error.0.as_u16());
        assert_eq!(
            serde_json::from_str::<Value>(&response.data).unwrap(),
            error.1
        );
        server.abort();
    }
}

#[tokio::test]
async fn primary_windows_and_direct_ad_failures_are_preserved() {
    let (service, server) = service(invalid_type(), invalid_type()).await;
    for category in [Category::Windows, Category::WindowsAd, Category::Other] {
        let response = service
            .get_category_assets(&AssetQuery::new(category, "test-org".into()), false)
            .await;
        assert_eq!(response.status, 400);
        assert!(!response.success);
    }
    server.abort();
}
