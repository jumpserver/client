import type { AssetItem, ConnectionInfo as StoredConnectionInfo } from "~/types/index";
import { useUserInfoStore } from "~/store/modules/userInfo";

interface ConnectionFormInfo {
  protocol: string
  account: string
  manualUsername: string
  manualPassword: string
  dynamicPassword: string
  rememberSecret: boolean
  connectMethod: string

  accountId?: string
  availableProtocols?: string[]
  accountMode: "hosted" | "dynamic" | "manual" | "anonymous"
}

export function useAssetConnection() {
  const { handleAssetConnection } = useAssetAction();
  const userInfoStore = useUserInfoStore();
  const { rememberAuthEnabled } = storeToRefs(userInfoStore);

  /**
   * 仅保存连接信息（不触发连接）
   */
  const saveConnectionInfo = (asset: AssetItem, connectionInfo: ConnectionFormInfo) => {
    let resolvedAccountId: string | undefined = connectionInfo.accountId;

    const candidateProtocols = (
      connectionInfo.availableProtocols && connectionInfo.availableProtocols.length > 0
        ? connectionInfo.availableProtocols
        : (asset.permedProtocols || []).map((p) => (typeof p?.name === "string" ? p.name.trim() : ""))
    ) as string[];

    const availableProtocols = Array.from(
      new Set(candidateProtocols.map((name) => (typeof name === "string" ? name.trim() : "")).filter((name) => name))
    );

    if (connectionInfo.accountMode === "hosted" && !resolvedAccountId) {
      const accs = asset.permedAccounts || [];
      const matched = accs.find(
        (a) =>
          a.name === connectionInfo.account
          || a.username === connectionInfo.account
          || a.alias === connectionInfo.account
      );

      resolvedAccountId = matched?.id;
    }

    const payload: StoredConnectionInfo = {
      protocol: connectionInfo.protocol,
      username: connectionInfo.account,
      accountId: resolvedAccountId,
      accountMode: connectionInfo.accountMode,
      connectMethod: connectionInfo.connectMethod,
      availableProtocols,
      ...(rememberAuthEnabled.value
        ? {
          manualUsername: connectionInfo.rememberSecret ? connectionInfo.manualUsername : "",
          manualPassword: connectionInfo.rememberSecret ? connectionInfo.manualPassword : "",
          dynamicPassword: connectionInfo.rememberSecret ? connectionInfo.dynamicPassword : "",
          rememberSecret: connectionInfo.rememberSecret
        }
        : {})
    };

    userInfoStore.setConnectionInfoForAsset(asset.id, payload);
  };

  /**
   * 处理连接确认（从模态框）
   */
  const confirmConnection = (asset: AssetItem, connectionInfo: ConnectionFormInfo) => {
    handleAssetConnection(connectionInfo.account, asset.id, connectionInfo.protocol, asset.permedAccounts!, undefined, {
      formInput: true,
      accountMode: connectionInfo.accountMode,
      accountId: connectionInfo.accountId,
      manualUsername: connectionInfo.manualUsername,
      manualPassword: connectionInfo.manualPassword,
      dynamicPassword: connectionInfo.dynamicPassword,
      connectMethod: connectionInfo.connectMethod
    });

    saveConnectionInfo(asset, connectionInfo);
  };

  return {
    confirmConnection,
    saveConnectionInfo
  };
}
