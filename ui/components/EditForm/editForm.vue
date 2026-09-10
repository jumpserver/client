<script setup lang="ts">
import type { SelectMenuItem } from "@nuxt/ui";
import type { ConnectMethod } from "~/composables/useConnectMethods";
import type { AssetPageType, PermedAccount, PermedProtocol } from "~/types/index";
import { useConnectMethods } from "~/composables/useConnectMethods";
import { useUserInfoStore } from "~/store/modules/userInfo";

const props = defineProps<{
  account: string
  protocol: string
  accounts: PermedAccount[]
  protocols: PermedProtocol[]
  manualUsername?: string
  manualPassword?: string
  dynamicPassword?: string
  rememberSecret?: boolean
  rememberSecretEnabled?: boolean
  connectMethod?: string
  assetType?: AssetPageType
}>();

const emits = defineEmits<{
  (e: "update:protocol", v: string): void
  (e: "update:account", v: string): void
  (e: "update:manualUsername", v: string): void
  (e: "update:manualPassword", v: string): void
  (e: "update:dynamicPassword", v: string): void
  (e: "update:rememberSecret", v: boolean): void
  (e: "update:connectMethod", v: string): void
  (e: "update:connectMethodValid", v: boolean): void
}>();

const { t, te } = useI18n();
const { getMethodsForProtocol } = useConnectMethods();
const { currentSite, currentUser, orgId } = storeToRefs(useUserInfoStore());
// prettier-ignore
const trailingIcon = "group-data-[state=open]:rotate-180 transition-transform duration-200";

const showManualInputArea = ref(false);
const showDynamicUserArea = ref(false);
const availableConnectMethods = ref<ConnectMethod[]>([]);
const methodsLoading = ref(true);
const methodsError = ref("");
const connectMethodValid = computed(() => !methodsLoading.value
  && availableConnectMethods.value.some((method) => method.value === props.connectMethod));

watch(connectMethodValid, (valid) => emits("update:connectMethodValid", valid), { immediate: true, flush: "sync" });
const connectMethodItems = computed<SelectMenuItem[]>(() => {
  return availableConnectMethods.value.map((method) => ({
    label: getConnectMethodLabel(method),
    value: method.value
  }));
});

function getConnectMethodLabel(method: { label?: string, value: string }) {
  const key = `ConnectMethod.${method.value}`;
  return te(key) ? t(key) : method.label || method.value;
}

const localManualUsername = computed<string>({
  get: () => props.manualUsername || "",
  set: (v: string) => emits("update:manualUsername", v ?? "")
});

const localManualPassword = computed<string>({
  get: () => props.manualPassword || "",
  set: (v: string) => emits("update:manualPassword", v ?? "")
});

const localDynamicPassword = computed<string>({
  get: () => props.dynamicPassword || "",
  set: (v: string) => emits("update:dynamicPassword", v ?? "")
});

const localRememberSecret = computed<boolean>({
  get: () => props.rememberSecret || false,
  set: (v: boolean) => emits("update:rememberSecret", !!v)
});

const localConnectMethod = computed<string>({
  get: () => props.connectMethod || "",
  set: (v: string) => emits("update:connectMethod", v ?? "")
});

watch(
  () => props.account,
  (newVal) => {
    handleSpecialAccount(newVal || "");
  },
  { immediate: true }
);

watch(
  [() => props.protocol, currentSite, orgId, () => currentUser.value?.bearerToken],
  async ([newProtocol], _oldValues, onCleanup) => {
    let cancelled = false;
    onCleanup(() => {
      cancelled = true;
    });
    methodsLoading.value = true;
    methodsError.value = "";
    availableConnectMethods.value = [];

    try {
      const methods = newProtocol ? await getMethodsForProtocol(newProtocol) : [];
      if (cancelled) return;
      availableConnectMethods.value = methods;
      if (!methods.some((method) => method.value === props.connectMethod)) {
        emits("update:connectMethod", methods[0]?.value || "");
      }
    } catch (error) {
      if (cancelled) return;
      console.debug("Failed to load connect methods:", error);
      emits("update:connectMethod", "");
      methodsError.value = t("ConnectError.ConnectMethodsFailed");
    } finally {
      if (!cancelled) methodsLoading.value = false;
    }
  },
  // 会话切换先同步到 Rust，再获取新会话的连接方式。
  { immediate: true }
);

const protocolItems = computed(() => props.protocols.map((p: PermedProtocol) => p.name));

const accountItems = computed(() => {
  // web 类型的资产需要保留匿名账号，其它类型不展示 @ANON
  const filteredAnonymous = props.accounts.filter((a: PermedAccount) => {
    return a.alias !== "@ANON" || props.assetType?.toLowerCase() === "web";
  });

  // 账号分组
  const hosted = filteredAnonymous
    .filter((acc: PermedAccount) => !acc.alias.includes("@"))
    .map((acc: PermedAccount) => {
      return acc.name;
    });

  const manual = filteredAnonymous
    .filter((acc: PermedAccount) => acc.alias.includes("@"))
    .map((acc: PermedAccount) => {
      if (acc.alias === "@USER") {
        const base = t("Account.DynamicUser");
        const username = acc.username || "";
        return username ? `${base}(${username})` : base;
      }

      if (acc.alias === "@INPUT") {
        return t("Account.ManualInput");
      }

      if (acc.alias === "@ANON") {
        const base = t("Account.Anonymous");
        return {
          label: base,
          value: "@ANON"
        };
      }

      return acc.name;
    });

  return [
    { type: "label", label: t("Account.Hosted") },
    ...hosted,
    ...(manual.length ? [{ type: "separator" }, { type: "label", label: t("Account.Manual") }, ...manual] : [])
  ] as SelectMenuItem[];
});

const selectedProtocol = computed<string>({
  get: () => props.protocol,
  set: (v: string) => emits("update:protocol", v ?? "")
});

const selectedAccount = computed<string>({
  get: () => props.account,
  set: (v: string) => emits("update:account", v ?? "")
});

function handleSpecialAccount(v: string) {
  showManualInputArea.value = false;
  showDynamicUserArea.value = false;

  if (v === "手动输入" || v === "Manual input") {
    showManualInputArea.value = true;
  }

  if (v.includes("同名账号") || v.includes("Dynamic user")) {
    showDynamicUserArea.value = true;
  }
}
</script>

<template>
  <div class="flex flex-col gap-4">
    <UFormField :label="t('EditModal.OptionalProtocol')" size="md">
      <USelect
        v-model="selectedProtocol"
        :items="protocolItems"
        :ui="{
          trailingIcon
        }"
        icon="mingcute:plugin-line"
        class="w-full"
      />
    </UFormField>

    <UFormField
      :label="t('EditModal.ConnectMethod')"
      :error="methodsError || (!methodsLoading && !connectMethodItems.length ? t('ConnectError.NoAvailableConnectMethod') : undefined)"
      size="md"
    >
      <USelect
        v-model="localConnectMethod"
        :items="connectMethodItems"
        :ui="{
          trailingIcon
        }"
        icon="lucide:zap"
        class="w-full"
        :loading="methodsLoading"
        :disabled="methodsLoading || connectMethodItems.length === 0"
      />
    </UFormField>

    <UFormField :label="t('EditModal.OptionalAccount')" size="md">
      <USelectMenu
        v-model="selectedAccount"
        :items="accountItems"
        value-key="value"
        label-key="label"
        :ui="{
          trailingIcon
        }"
        icon="lucide:user-round"
        class="w-full"
      />
    </UFormField>

    <template v-if="showManualInputArea">
      <UFormField :label="t('Account.Username')" size="md">
        <UInput
          v-model="localManualUsername"
          autocapitalize="none"
          autocorrect="off"
          :placeholder="t('Account.Username')"
        />
      </UFormField>

      <UFormField :label="t('Account.Password')" size="md">
        <UInput
          v-model="localManualPassword"
          type="password"
          autocapitalize="none"
          autocorrect="off"
          :placeholder="t('Account.Password')"
        />
      </UFormField>

      <div v-if="rememberSecretEnabled !== false" class="flex justify-end items-center w-full">
        <USwitch v-model="localRememberSecret" :label="t('Account.RememberPassword')" />
      </div>
    </template>

    <template v-if="showDynamicUserArea">
      <UFormField :label="t('Account.Password')" size="md">
        <UInput
          v-model="localDynamicPassword"
          type="password"
          autocapitalize="none"
          autocorrect="off"
          :placeholder="t('Account.Password')"
        />
      </UFormField>
    </template>
  </div>
</template>
