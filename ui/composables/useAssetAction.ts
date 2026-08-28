import type { UnlistenFn } from "@tauri-apps/api/event";
import type { ConnectionBody, PermedAccount, PermedProtocol, TokenResponse } from "~/types";

import { useConnectMethods } from "~/composables/useConnectMethods";
import { useSettingManager } from "~/composables/useSettingManager";
import { useUserInfoStore } from "~/store/modules/userInfo";

let tauriListenersInitialized = false;
let tauriListenersRegistering = false;
let tauriListenersRefCount = 0;
let unlistenGetTokenFailure: UnlistenFn | null = null;
let unlistenGetTokenSuccess: UnlistenFn | null = null;
let unlistenFavoriteSuccess: UnlistenFn | null = null;
let unlistenFavoriteFailed: UnlistenFn | null = null;
let unlistenUnfavoriteSuccess: UnlistenFn | null = null;
let unlistenUnfavoriteFailed: UnlistenFn | null = null;
let unlistenGetAssetDetailSuccess: UnlistenFn | null = null;
let unlistenGetAssetDetailFailed: UnlistenFn | null = null;
let unlistenRenameSuccess: UnlistenFn | null = null;
let unlistenRenameError: UnlistenFn | null = null;
let unlistenPullUpFailure: UnlistenFn | null = null;

function releaseTauriEventListeners() {
  tauriListenersRefCount = Math.max(tauriListenersRefCount - 1, 0);
  if (!tauriListenersInitialized || tauriListenersRegistering) return;
  if (tauriListenersRefCount === 0) {
    unlistenGetTokenSuccess?.();
    unlistenFavoriteSuccess?.();
    unlistenFavoriteFailed?.();
    unlistenGetTokenFailure?.();
    unlistenUnfavoriteSuccess?.();
    unlistenUnfavoriteFailed?.();
    unlistenGetAssetDetailSuccess?.();
    unlistenGetAssetDetailFailed?.();
    unlistenRenameSuccess?.();
    unlistenRenameError?.();
    unlistenPullUpFailure?.();
    unlistenGetTokenFailure = null;
    unlistenGetTokenSuccess = null;
    unlistenFavoriteSuccess = null;
    unlistenUnfavoriteFailed = null;
    unlistenUnfavoriteSuccess = null;
    unlistenFavoriteFailed = null;
    unlistenGetAssetDetailSuccess = null;
    unlistenGetAssetDetailFailed = null;
    unlistenRenameSuccess = null;
    unlistenRenameError = null;
    unlistenPullUpFailure = null;
    tauriListenersInitialized = false;
  }
}

export const useAssetAction = () => {
  const connectToken = ref<string | null>(null);

  const { t } = useI18n();
  const toast = useToast();
  const userInfoStore = useUserInfoStore();
  const settingManager = useSettingManager();
  const { getMethodsForProtocol } = useConnectMethods();
  // prettier-ignore
  const { currentSite, currentConnectionInfoMap, currentRdpClientOption } = storeToRefs(userInfoStore);
  const { charset, rdpResolution, backspaceAsCtrlH, keyboardLayout, rdpClientOption, rdpColorQuality, rdpSmartSize }
    = settingManager;

  function buildLocalRdpParams() {
    const prefs = resolveGraphicsPreferences();
    const params: Record<string, string> = {};

    if (prefs.resolvedResolution && prefs.resolvedResolution.includes("x")) {
      const [width, height] = prefs.resolvedResolution.split("x");
      if (width) params.width = width;
      if (height) params.height = height;
    }

    const options = prefs.resolvedClientOptions || [];
    if (options.includes("full_screen")) {
      params.full_screen = "1";
    }
    if (options.includes("multi_screen")) {
      params.multi_mon = "1";
    }
    if (options.includes("drives_redirect")) {
      params.drives_redirect = "1";
    }

    params.rdp_smart_size = prefs.resolvedSmartSize;
    params.rdp_color_quality = prefs.resolvedColorQuality;

    return params;
  }

  /**
   * @description 生成连接选项
   */
  function resolveGraphicsPreferences() {
    const resolvedKeyboardLayout
      = keyboardLayout.value || currentRdpClientOption.value.keyboard_layout || "en-us-qwerty";
    const resolvedClientOptions
      = Array.isArray(rdpClientOption.value) && rdpClientOption.value.length > 0
        ? [...rdpClientOption.value]
        : [...(currentRdpClientOption.value.rdp_client_option || [])];
    const resolvedColorQuality = rdpColorQuality.value || currentRdpClientOption.value.rdp_color_quality || "32";
    const resolvedSmartSize = rdpSmartSize.value || currentRdpClientOption.value.rdp_smart_size || "0";

    return {
      resolvedCharset: (charset.value || "default") as string,
      resolvedBackspace: backspaceAsCtrlH.value ?? false,
      resolvedResolution: (rdpResolution.value || "auto") as string,
      resolvedKeyboardLayout,
      resolvedClientOptions,
      resolvedColorQuality,
      resolvedSmartSize
    };
  }

  /**
   * @description 展示 user 信息,默认展示非 @ 开头的 user
   * @param assetId
   */
  const displayUser = (assetId: string, accounts?: PermedAccount[]) => {
    const saved = currentConnectionInfoMap.value[assetId];

    if (saved?.username) return saved.username;

    const list = accounts || [];
    const acc = list.find((a) => a && a.alias && !a.alias.startsWith("@"));

    return acc?.name || "-";
  };

  /**
   * @description 展示 protocol 信息
   * @param assetId
   */
  const displayProtocol = (assetId: string, protocols: PermedProtocol[]) => {
    const saved = currentConnectionInfoMap.value[assetId];
    return saved?.protocol || sortPermedProtocols(protocols)?.[0]?.name || "-";
  };

  /**
   * @description 获取 connect_token 接口需要的 account
   * @param accounts
   * @param assetId
   * @param user
   */
  const getUserId = (accounts: PermedAccount[], assetId: string, user: string) => {
    const _accounts = accounts || [];
    const saved = currentConnectionInfoMap.value[assetId];
    const username = saved?.username ?? user;

    // 同名账号 account 使用 @USER
    // 手动输入 account 使用 @INPUT
    // prettier-ignore
    const isManual = saved?.accountMode === "manual" || username === "手动输入" || username === "Manual input";

    const isDynamic
      = saved?.accountMode === "dynamic" || username.includes("同名账号") || username.includes("Dynamic user");

    const isAnonymous = saved?.accountMode === "anonymous" || username.includes("@ANON");

    // 已保存过托管账号的 ID 则优先使用
    if (!isManual && !isDynamic && !isAnonymous && saved?.accountId) {
      return saved.accountId as any;
    }
    if (isManual) return "@INPUT";
    if (isDynamic) return "@USER";
    if (isAnonymous) return "@ANON";

    if (username) {
      const matched = _accounts.find((a) => a.username === username || a.alias === username || a.name === username);
      if (matched) return matched.id;
    }

    return _accounts[0]?.id || "";
  };

  /**
   * @description 获取连接令牌
   */
  const getConnectToken = (body: ConnectionBody) => {
    const rdpParams = buildLocalRdpParams();
    useTauriCoreInvoke("get_connect_token", {
      body: {
        asset: body.asset,
        account: body.account,
        protocol: body.protocol,
        input_username: body.input_username,
        input_secret: body.input_secret,
        connect_method: body.connect_method,
        connect_options: body.connect_options
      },
      rdpParams
    });
  };

  const resolveConnectMethod = async (protocol: string, preferred?: string) => {
    const methods = await getMethodsForProtocol(protocol);
    const preferredMethod = preferred?.trim() || "";
    if (preferredMethod) {
      return methods.some((method) => method.value === preferredMethod) ? preferredMethod : "";
    }
    return methods[0]?.value || "";
  };

  const generateConnectOptions = (protocol: string) => {
    const prefs = resolveGraphicsPreferences();

    const options = {
      charset: prefs.resolvedCharset,
      backspaceAsCtrlH: prefs.resolvedBackspace,
      resolution: prefs.resolvedResolution,
      rdp_resolution: prefs.resolvedResolution,
      keyboard_layout: prefs.resolvedKeyboardLayout,
      rdp_client_option: prefs.resolvedClientOptions,
      rdp_color_quality: prefs.resolvedColorQuality,
      rdp_smart_size: prefs.resolvedSmartSize,
      token_reusable: false,
      disableautohash: false
    };
    const specificOptions
      = protocol === "http"
        ? {
          appletConnectMethod: "client",
          reusable: false
        }
        : {};
    return {
      ...options,
      ...specificOptions
    };
  };

  /**
   * @description 处理连接事件
   * @param user
   * @param assetId
   * @param displayProtocol
   * @param accounts
   * @param protocolOverride
   */
  const handleAssetConnection = async (
    user: string,
    assetId: string,
    displayProtocol: string,
    accounts?: PermedAccount[],
    protocolOverride?: string,
    ephemeral?: {
      formInput?: boolean
      accountMode?: "hosted" | "dynamic" | "manual" | "anonymous"
      accountId?: string
      manualUsername?: string
      manualPassword?: string
      dynamicPassword?: string
      connectMethod?: string
    }
  ) => {
    const saved = currentConnectionInfoMap.value[assetId];
    const isFormInput = ephemeral?.formInput === true;

    // 只有弹窗确认时才使用本次表单数据；直接连接仍沿用已保存配置。
    const effectiveMode = isFormInput ? ephemeral?.accountMode : saved?.accountMode ?? ephemeral?.accountMode;
    const selected = isFormInput ? user : saved?.username ?? user;

    let input_username = "";
    let input_secret = "";

    // 根据展示选择反查账号对象（name/username/alias 任意匹配）
    const _accounts = accounts || [];
    const matchedAccount = _accounts.find(
      (a) => a.username === selected || a.alias === selected || a.name === selected
    );

    if (effectiveMode === "manual" || selected === "手动输入" || selected === "Manual input") {
      // prettier-ignore
      input_username = isFormInput ? (ephemeral?.manualUsername || "") : saved?.manualUsername ?? matchedAccount?.username ?? "";
      input_secret = isFormInput ? (ephemeral?.manualPassword || "") : saved?.manualPassword ?? "";
    } else if (effectiveMode === "dynamic" || selected?.includes("同名账号") || selected?.includes("Dynamic user")) {
      // 同名账号仅需传递密码
      input_username = "";
      input_secret = isFormInput ? (ephemeral?.dynamicPassword || "") : saved?.dynamicPassword ?? "";
    } else if (effectiveMode === "anonymous" || selected?.includes("@ANON")) {
      input_username = "";
      input_secret = "";
    } else {
      // 托管账号：account 用 ID，input_username 用展示账号名
      input_username = selected || matchedAccount?.username || "";
      input_secret = "";
    }

    const protocol = protocolOverride || displayProtocol;

    const accountForToken = (() => {
      if (effectiveMode === "manual" || selected === "手动输入" || selected === "Manual input") {
        return "@INPUT";
      }
      if (effectiveMode === "dynamic" || selected?.includes("同名账号") || selected?.includes("Dynamic user")) {
        return "@USER";
      }
      if (effectiveMode === "anonymous" || selected?.includes("@ANON")) {
        return "@ANON";
      }

      if (isFormInput) {
        return ephemeral?.accountId || matchedAccount?.id || "";
      }

      return getUserId(accounts!, assetId, user);
    })();

    // 当前连接显式选择优先；仅在协议一致时复用已保存连接方法，避免跨协议复用错误的客户端
    let connectMethod: string;
    try {
      connectMethod = await resolveConnectMethod(
        protocol,
        ephemeral?.connectMethod?.trim() || (saved?.protocol === protocol ? saved?.connectMethod?.trim() : "")
      );
    } catch (error) {
      console.debug(`Failed to resolve connect method for ${protocol}:`, error);
      toast.add({
        title: t("ConnectError.ConnectFailed"),
        description: t("ConnectError.ConnectMethodsFailed"),
        color: "error"
      });
      return;
    }

    if (!connectMethod) {
      toast.add({
        title: t("ConnectError.ConnectFailed"),
        description: t("ConnectError.ConnectMethodUnavailable"),
        color: "error"
      });
      return;
    }

    const accountId
      = effectiveMode === "hosted"
        ? isFormInput
          ? ephemeral?.accountId || matchedAccount?.id
          : matchedAccount?.id || saved?.accountId
        : undefined;

    userInfoStore.setConnectionInfoForAsset(assetId, {
      protocol,
      username: selected || user,
      accountId,
      accountMode: effectiveMode,
      connectMethod
    });

    nextTick(() => {
      getConnectToken({
        asset: assetId,
        protocol,
        input_username,
        input_secret,
        account: accountForToken,
        connect_method: connectMethod,
        connect_options: generateConnectOptions(protocol)
      });
    });
  };

  /**
   * @description 处理重命名
   * @param assetId
   * @param name
   */
  const handleAssetRename = (assetId: string, name: string) => {
    if (!currentSite.value) return;

    useTauriCoreInvoke("rename", {
      assetId,
      name
    });
  };

  /**
   * @description 处理资产收藏
   * @param assetId
   */
  const handleAssetFavorite = (assetId: string) => {
    useTauriCoreInvoke("set_favorite", {
      assetId
    });
  };

  /**
   * @description 处理取消收藏
   * @param assetId
   */
  const handleAssetUnfavorite = (assetId: string) => {
    useTauriCoreInvoke("unfavorite", {
      assetId
    });
  };

  /**
   * @description 获取详情
   * @param assetId
   */
  const getAssetDetail = (assetId: string) => {
    useTauriCoreInvoke("get_asset_detail", {
      assetId
    });
  };

  /**
   * @description 监听 tauri 事件
   */
  const listenTauriEvent = async () => {
    if (tauriListenersInitialized || tauriListenersRegistering) {
      tauriListenersRefCount++;
      return;
    }

    tauriListenersRegistering = true;

    try {
      unlistenGetTokenSuccess = await useTauriEventListen("get-token-success", (event) => {
        interface eventPayload {
          status: number
          data: TokenResponse
        }

        const payload = event.payload as eventPayload;

        if (payload.status === 201) {
          connectToken.value = payload.data.id;
        }
      });

      unlistenGetTokenFailure = await useTauriEventListen("get-token-failure", (event) => {
        interface eventPayload {
          status: number
          data: string
        }

        const payload = event.payload as eventPayload;
        const errorData = JSON.parse(payload.data);
        const errorCode = errorData?.code as string;

        if (errorCode === "connect_method_not_allowed" || errorCode === "connect_methods_unavailable") {
          return toast.add({
            title: t("ConnectError.ConnectFailed"),
            description: t(errorCode === "connect_method_not_allowed"
              ? "ConnectError.ConnectMethodUnavailable"
              : "ConnectError.ConnectMethodsFailed"),
            color: "error"
          });
        }

        if (errorCode && errorCode.includes("acl")) {
          return toast.add({
            title: t("ConnectError.ConnectFailed"),
            description: t("ConnectError.AclFailed"),
            color: "error",
            icon: "line-md:close-circle",
            progress: true,
            duration: 4000
          });
        }

        toast.add({
          title: t("ConnectError.ConnectFailed"),
          description: errorData.detail,
          color: "error",
          icon: "line-md:close-circle",
          progress: true,
          duration: 4000
        });
      });

      unlistenFavoriteSuccess = await useTauriEventListen("set-favorite-success", (event) => {
        interface eventPayload {
          status: string
        }

        const payload = event.payload as eventPayload;
        if (payload.status === "success") {
          toast.add({
            title: t("ContextMenu.FavoriteSuccess"),
            color: "primary",
            icon: "line-md:check-all",
            progress: false,
            duration: 1000
          });
        }
      });

      unlistenFavoriteFailed = await useTauriEventListen("set-favorite-failure", (event) => {
        interface eventPayload {
          status: string
        }

        const payload = event.payload as eventPayload;
        if (payload.status === "failed") {
          toast.add({
            title: t("ContextMenu.FavoriteFailed"),
            color: "error",
            icon: "line-md:close-circle",
            progress: true,
            duration: 4000
          });
        }
      });

      unlistenUnfavoriteSuccess = await useTauriEventListen("unfavorite-success", (event) => {
        interface eventPayload {
          status: string
        }

        const payload = event.payload as eventPayload;

        if (payload.status === "success") {
          toast.add({
            title: t("ContextMenu.UnfavoriteSuccess"),
            color: "primary",
            icon: "line-md:check-all",
            progress: false,
            duration: 1000
          });
        }
      });

      unlistenUnfavoriteFailed = await useTauriEventListen("unfavorite-failure", (event) => {
        interface eventPayload {
          status: string
        }

        const payload = event.payload as eventPayload;
        if (payload.status === "failed") {
          toast.add({
            title: t("ContextMenu.UnfavoriteFailed"),
            color: "error",
            icon: "line-md:close-circle",
            progress: true,
            duration: 4000
          });
        }
      });

      unlistenGetAssetDetailSuccess = await useTauriEventListen("get-asset-detail-success", (event) => {
        interface eventPayload {
          status: string
          data: string
          asset_id: string
        }

        const payload = event.payload as eventPayload;

        if (payload.status === "success") {
          const assetDetail = JSON.parse(payload.data) as any;
          const permedAccounts = assetDetail.permed_accounts ?? [];
          const permedProtocols = assetDetail.permed_protocols ?? [];

          // 不支持目录服务的 winrm 协议
          const filteredPermedProtocols = permedProtocols.filter(
            (protocol: PermedProtocol) => protocol.name !== "winrm"
          );

          useEventBus().emit("assetDetailUpdated", {
            assetId: payload.asset_id,
            permedAccounts,
            permedProtocols: sortPermedProtocols(filteredPermedProtocols)
          });
        }
      });

      unlistenGetAssetDetailFailed = await useTauriEventListen("get-asset-detail-failure", (event) => {
        interface eventPayload {
          status?: number | string
          error?: string
          asset_id?: string
        }

        const payload = event.payload as eventPayload;
        const status = Number(payload.status);

        toast.add({
          title: status === 401 ? t("Login.LoginAuthenticationExpired") : t("Asset.GetAssetFailed"),
          description: status === 401
            ? t("Login.LoginAuthenticationExpiredDescription")
            : t("ConnectError.ConnectFailed"),
          color: "error",
          icon: "line-md:close-circle",
          progress: true,
          duration: 4000
        });

        if (payload.asset_id) {
          useEventBus().emit("assetDetailFailed", {
            assetId: payload.asset_id,
            status: Number.isFinite(status) ? status : undefined
          });
        }
      });

      unlistenRenameSuccess = await useTauriEventListen("rename-success", (event) => {
        interface eventPayload {
          success: boolean
          status?: number
          data?: string
        }

        const payload = event.payload as eventPayload;
        let assetId = "";
        let name = "";

        try {
          if (payload.data) {
            const info = JSON.parse(payload.data) as any;
            assetId = info?.asset || info?.asset_id || info?.id || "";
            name = info?.name || "";
          }
        } catch {}

        // 更新资产名称
        if (assetId && name) {
          try {
            useEventBus().emit("assetRenamed", { assetId, name });
          } catch {}
        }
      });

      unlistenRenameError = await useTauriEventListen("rename-error", (event) => {
        interface eventPayload {
          success: boolean
          status?: number
          data?: string
        }

        const payload = event.payload as eventPayload;
        let message = "";
        try {
          const err = payload.data ? JSON.parse(payload.data) : {};
          message = err?.detail || (Array.isArray(err?.asset) ? err.asset[0] : "");
        } catch {}

        toast.add({
          title: t("AssetCard.RenameFail"),
          description: message || t("Common.OperationFailed"),
          color: "error",
          icon: "line-md:close-circle",
          progress: true,
          duration: 4000
        });
      });

      unlistenPullUpFailure = await useTauriEventListen("pull-up-failure", (event) => {
        interface eventPayload {
          error: string
        }

        const payload = event.payload as eventPayload;
        const raw = payload.error || "";
        const lower = raw.toLowerCase();
        const extractClientPath = () => {
          const selectedPathToken = "selected path:";
          const selectedPathStart = lower.indexOf(selectedPathToken);
          if (selectedPathStart >= 0) {
            const valueStart = selectedPathStart + selectedPathToken.length;
            const reasonStart = lower.indexOf(", reason:", valueStart);
            const closeStart = lower.indexOf(")", valueStart);
            const valueEnd = reasonStart >= 0 ? reasonStart : closeStart >= 0 ? closeStart : raw.length;
            const path = raw.slice(valueStart, valueEnd).trim();
            if (path) return path;
          }

          const executableToken = "executable not found:";
          const executableStart = lower.indexOf(executableToken);
          if (executableStart >= 0) {
            const path = raw.slice(executableStart + executableToken.length).trim();
            if (path) return path;
          }

          return "";
        };
        const withPath = (base: string) => {
          const path = extractClientPath();
          return path ? `${base}\n${path}` : base;
        };

        let description = raw || t("ConnectError.ConnectFailed");

        if (lower.includes("executable not found")) {
          description = withPath(t("Setting.ExecutableNotFound"));
        } else if (lower.includes("failed to launch client")) {
          description = t("ConnectError.ClientLaunchFailed");
        } else if (lower.includes("client process exited")) {
          description = t("ConnectError.ClientExited");
        } else if (lower.includes("no rdp application")) {
          description = withPath(t("ConnectError.RdpAppMissing"));
        } else if (lower.includes("no vnc application")) {
          description = withPath(t("ConnectError.VncAppMissing"));
        } else if (lower.includes("no database application")) {
          description = withPath(t("ConnectError.DbAppMissing"));
        } else if (lower.includes("failed to execute rdp application")) {
          description = withPath(t("ConnectError.RdpAppFailed"));
        } else if (lower.includes("failed to execute vnc application")) {
          description = withPath(t("ConnectError.VncAppFailed"));
        } else if (lower.includes("failed to execute database application")) {
          description = withPath(t("ConnectError.DbAppFailed"));
        }

        toast.add({
          title: t("ConnectError.ConnectFailed"),
          description,
          color: "error",
          icon: "line-md:close-circle",
          actions: [
            {
              label: t("Common.Copy"),
              icon: "i-lucide-copy",
              color: "neutral",
              variant: "soft",
              onClick: () => {
                void useTauriClipboardManagerWriteText(`${t("ConnectError.ConnectFailed")}\n${description}`);
              }
            }
          ],
          progress: true,
          duration: 4000
        });
      });

      tauriListenersInitialized = true;
      tauriListenersRefCount++;
    } finally {
      tauriListenersRegistering = false;
    }
  };

  onMounted(() => {
    listenTauriEvent();
  });

  onBeforeUnmount(() => {
    releaseTauriEventListeners();
  });

  return {
    displayUser,
    getAssetDetail,
    displayProtocol,

    handleAssetRename,
    handleAssetFavorite,
    handleAssetUnfavorite,
    handleAssetConnection
  };
};
