<script setup lang="ts">
import type { AssetItem, AssetPageType, ConnectionInfo, PermedAccount, PermedProtocol } from "~/types/index";
import EditForm from "~/components/EditForm/editForm.vue";
import { useUserInfoStore } from "~/store/modules/userInfo";

const props = defineProps<{
  assetType?: AssetPageType
}>();

const { t, locale } = useI18n();
const toast = useToast();
const { getAssetDetail } = useAssetAction();
const userInfoStore = useUserInfoStore();
const { rememberAuthEnabled } = storeToRefs(userInfoStore);
const ASSET_DETAIL_TIMEOUT_MS = 15000;

const open = ref(false);
const currentAsset = ref<AssetItem | null>(null);

const draftProtocol = ref<string>("");
const draftAccount = ref<string>("");
const draftManualUsername = ref<string>("");
const draftManualPassword = ref<string>("");
const draftDynamicPassword = ref<string>("");
const draftRememberSecret = ref<boolean>(false);
const draftConnectMethod = ref<string>("");

let pendingResolve: ((info: any) => void) | null = null;
let pendingReject: ((reason?: any) => void) | null = null;

const modalTitle = computed(() => {
  const name = currentAsset.value?.name || "";
  return `${t("EditModal.ModifyConnectionInfo")} - ${name}`;
});

/**
 * @description 初始化 Form
 * @param asset
 */
const initDraft = (asset: AssetItem, preferredProtocol?: string) => {
  const saved: ConnectionInfo | undefined = asset.savedConnection;

  const protocols = sortPermedProtocols(asset.permedProtocols || ([] as PermedProtocol[]));
  const accounts = asset.permedAccounts || ([] as PermedAccount[]);

  // 协议默认：保存的协议 -> 第一个协议 -> 空
  draftProtocol.value = protocols.some((protocol) => protocol.name === preferredProtocol)
    ? preferredProtocol!
    : saved?.protocol || protocols[0]?.name || "";

  // 账号默认：保存的用户名 -> 第一条托管账号 -> 动态账号(@USER) -> 手动输入(@INPUT) -> 空
  if (saved?.username) {
    draftAccount.value = saved!.username;
  } else {
    const hosted = accounts.find((a) => a?.alias && !a.alias.startsWith("@"));

    if (hosted) {
      draftAccount.value = hosted.name;
    } else {
      const dynamic = accounts.find((a) => a.alias === "@USER");
      const manual = accounts.find((a) => a.alias === "@INPUT");

      if (dynamic) {
        draftAccount.value
          = locale.value === "zh" ? `${dynamic.name}(${dynamic.username})` : `Dynamic user(${dynamic.username})`;
      } else if (manual) {
        draftAccount.value = locale.value === "zh" ? manual.name : "Manual input";
      } else {
        draftAccount.value = "";
      }
    }
  }

  draftManualUsername.value = saved?.manualUsername || "";
  draftManualPassword.value = rememberAuthEnabled.value ? saved?.manualPassword || "" : "";
  draftDynamicPassword.value = rememberAuthEnabled.value ? saved?.dynamicPassword || "" : "";
  draftRememberSecret.value = rememberAuthEnabled.value && !!saved?.rememberSecret;
  draftConnectMethod.value
    = !preferredProtocol || preferredProtocol === saved?.protocol ? saved?.connectMethod || "" : "";
};

/**
 * @description 拼凑连接信息
 */
const normalizeProtocols = () => {
  return sortPermedProtocols(currentAsset.value?.permedProtocols || [])
    .map((p) => (p?.name ? p.name.trim() : ""))
    .filter((name) => name.length > 0);
};

const buildConnectionInfo = () => {
  let accountMode: "hosted" | "dynamic" | "manual" | "anonymous" = "hosted";
  let normalizedAccount = draftAccount.value || "";
  let accountId: string | undefined;

  const v = draftAccount.value || "";

  if (v === "手动输入" || v === "Manual input") accountMode = "manual";
  if (v.includes("@ANON") || v === "匿名账号" || v === "Anonymous") {
    accountMode = "anonymous";
    normalizedAccount = "@ANON";
  }
  if (v.includes("同名账号") || v.includes("Dynamic user")) {
    accountMode = "dynamic";

    const accs = currentAsset.value?.permedAccounts || [];
    const dynamicAcc = accs.find((a) => a.alias === "@USER");

    if (dynamicAcc) normalizedAccount = dynamicAcc.name;
    else normalizedAccount = v.replace(/\(.+\)/, "");
  }

  if (accountMode === "hosted") {
    const accs = currentAsset.value?.permedAccounts || [];
    const matched = accs.find(
      (a) => a.name === normalizedAccount || a.username === normalizedAccount || a.alias === normalizedAccount
    );
    accountId = matched?.id;
  }

  return {
    protocol: draftProtocol.value || "",
    account: normalizedAccount,
    accountId,
    accountMode,
    manualUsername: draftManualUsername.value || "",
    manualPassword: draftManualPassword.value || "",
    dynamicPassword: draftDynamicPassword.value || "",
    rememberSecret: rememberAuthEnabled.value && !!draftRememberSecret.value,
    connectMethod: draftConnectMethod.value || "",
    availableProtocols: normalizeProtocols()
  };
};

/**
 * @description 关闭 modal
 */
const close = () => {
  open.value = false;
  currentAsset.value = null;
};

/**
 * @description 点击确认
 */
const onConfirm = () => {
  const info = buildConnectionInfo();

  pendingResolve?.(info);
  pendingResolve = null;
  pendingReject = null;
  close();
};

/**
 * @description 点击取消
 */
const onCancel = () => {
  pendingReject?.("cancelled");
  pendingResolve = null;
  pendingReject = null;
  close();
};

async function ensureDetails(asset: AssetItem) {
  const noAccounts = !asset.permedAccounts || asset.permedAccounts.length === 0;
  const noProtocols = !asset.permedProtocols || asset.permedProtocols.length === 0;

  if (!noAccounts && !noProtocols) return asset;

  const bus = useEventBus();

  return await new Promise<AssetItem>((resolve, reject) => {
    let settled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let unsubscribeUpdated: (() => void) | undefined;
    let unsubscribeFailed: (() => void) | undefined;

    const cleanup = () => {
      if (timer) clearTimeout(timer);
      unsubscribeUpdated?.();
      unsubscribeFailed?.();
    };

    const finish = (next: () => void) => {
      if (settled) return;
      settled = true;
      cleanup();
      next();
    };

    unsubscribeUpdated = bus.on(
      "assetDetailUpdated",
      (payload) => {
        if (payload.assetId !== asset.id) return;

        currentAsset.value = {
          ...(currentAsset.value || asset),
          permedAccounts: payload.permedAccounts || [],
          permedProtocols: payload.permedProtocols || []
        } as AssetItem;

        finish(() => resolve(currentAsset.value!));
      },
      false
    );

    unsubscribeFailed = bus.on(
      "assetDetailFailed",
      (payload) => {
        if (payload.assetId !== asset.id) return;
        finish(() => reject(new Error("get asset detail failed")));
      },
      false
    );

    timer = setTimeout(() => {
      finish(() => reject(new Error("get asset detail timeout")));
    }, ASSET_DETAIL_TIMEOUT_MS);

    getAssetDetail(asset.id);
  });
}

/**
 * @description 打开 Modal
 * @param asset
 */
async function openModal(asset: AssetItem, preferredProtocol?: string): Promise<any> {
  currentAsset.value = asset;

  try {
    await ensureDetails(asset);
  } catch (error) {
    const timedOut = error instanceof Error && error.message.includes("timeout");

    if (timedOut) {
      toast.add({
        title: t("Asset.GetAssetFailed"),
        description: t("ConnectError.ConnectFailed"),
        color: "error",
        icon: "line-md:close-circle",
        progress: true,
        duration: 4000
      });
    }

    throw error;
  }

  initDraft(currentAsset.value!, preferredProtocol);
  open.value = true;

  return new Promise((resolve, reject) => {
    pendingResolve = resolve;
    pendingReject = reject;
  });
}

defineExpose({ open: openModal, close });
</script>

<template>
  <Modal
    :open="open"
    :title="modalTitle"
    :description="t('EditModal.Description')"
    @confirm="onConfirm"
    @update:open="(v) => (v ? (open = true) : onCancel())"
  >
    <EditForm
      v-if="currentAsset"
      v-model:protocol="draftProtocol"
      v-model:account="draftAccount"
      v-model:manual-username="draftManualUsername"
      v-model:manual-password="draftManualPassword"
      v-model:dynamic-password="draftDynamicPassword"
      v-model:remember-secret="draftRememberSecret"
      v-model:connect-method="draftConnectMethod"
      :remember-secret-enabled="rememberAuthEnabled"
      :accounts="currentAsset.permedAccounts || []"
      :protocols="currentAsset.permedProtocols || []"
      :asset-type="props.assetType"
    />
  </Modal>
</template>
