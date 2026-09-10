import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
// Use the built-in runner without adding a separate test framework.
// eslint-disable-next-line test/no-import-node-test
import test from "node:test";
import { runInNewContext } from "node:vm";
import ts from "typescript";
import * as vue from "vue";

const root = new URL("../", import.meta.url);
const client = { value: "db_client", type: "native", label: "DB Client" };
const guide = { value: "db_guide", type: "native", label: "DB Guide" };
const ssh = { value: "ssh_client", type: "native", label: "SSH Client" };
const flush = async () => {
  await new Promise((resolve) => setImmediate(resolve));
};
const deferred = () => {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
};

// Run the actual composables/SFC setup with Vue reactivity and mocked desktop I/O.
function load(path, globals, imports = {}, expose = []) {
  let source = readFileSync(new URL(path, root), "utf8");
  if (path.endsWith(".vue")) source = source.match(/<script setup lang="ts">([\s\S]*?)<\/script>/)[1];
  if (expose.length) source += `\nexport { ${expose.join(", ")} };`;
  const { outputText } = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 }
  });
  const exports = {};
  runInNewContext(outputText, {
    ...vue,
    exports,
    console: { debug() {}, error() {} },
    useI18n: () => ({ t: (key) => key, te: () => false }),
    storeToRefs: vue.toRefs,
    require: (id) => {
      assert.ok(id in imports, `Unexpected import: ${id}`);
      return imports[id];
    },
    ...globals
  }, { filename: path });
  return exports;
}

function methodsHarness(invoke) {
  const store = vue.reactive({ currentSite: "https://example.test", orgId: "org-a", currentUser: { bearerToken: "user-a" } });
  const imports = { "~/store/modules/userInfo": { useUserInfoStore: () => store } };
  const { useConnectMethods } = load("ui/composables/useConnectMethods.ts", { useTauriCoreInvoke: invoke }, imports);
  return { store, imports, useConnectMethods };
}

test("uses fresh permissions, filters disabled methods and guides, keeps supported HTTP applets", async () => {
  let response = { mariadb: [client, guide], http: [{ value: "chrome", type: "applet" }, { value: "web_gui", type: "web" }] };
  const { useConnectMethods } = methodsHarness(async () => JSON.stringify(response));
  const methods = useConnectMethods();
  assert.deepEqual(Array.from(await methods.getMethodsForProtocol("mariadb"), (m) => m.value), ["db_client"]);
  assert.deepEqual(Array.from(await methods.getMethodsForProtocol("http"), (m) => m.value), ["chrome"]);
  for (const entries of [[], [guide], [{ ...client, disabled: true }]]) {
    response = { mariadb: entries };
    assert.equal((await methods.getMethodsForProtocol("mariadb")).length, 0);
  }
});

test("rejects failed/malformed responses and results from an earlier user, organization or site", async () => {
  for (const response of ["invalid", "null", "[]"]) {
    const { useConnectMethods } = methodsHarness(async () => response);
    await assert.rejects(useConnectMethods().getMethodsForProtocol("mariadb"));
  }
  for (const change of [
    (store) => {
      store.currentSite = "https://other.test";
    },
    (store) => {
      store.orgId = "org-b";
    },
    (store) => {
      store.currentUser.bearerToken = "user-b";
    }
  ]) {
    const pending = deferred();
    const { store, useConnectMethods } = methodsHarness(() => pending.promise);
    const result = useConnectMethods().getMethodsForProtocol("mariadb");
    change(store);
    pending.resolve(JSON.stringify({ mariadb: [client] }));
    await assert.rejects(result, /Session changed/);
  }
});

function actionHarness(fetchMethods) {
  const calls = [];
  const toasts = [];
  const writes = [];
  const store = vue.reactive({
    currentSite: "https://example.test",
    currentConnectionInfoMap: {},
    currentRdpClientOption: {},
    setConnectionInfoForAsset: (...args) => writes.push(args)
  });
  const settings = Object.fromEntries([
    "charset",
    "rdpResolution",
    "backspaceAsCtrlH",
    "keyboardLayout",
    "rdpClientOption",
    "rdpColorQuality",
    "rdpSmartSize"
  ].map((key) => [key, vue.ref(undefined)]));
  const { useAssetAction } = load("ui/composables/useAssetAction.ts", {
    useToast: () => ({ add: (toast) => toasts.push(toast) }),
    useTauriCoreInvoke: async (...args) => {
      calls.push(args);
    },
    onMounted() {},
    onBeforeUnmount() {}
  }, {
    "~/store/modules/userInfo": { useUserInfoStore: () => store },
    "~/composables/useConnectMethods": { useConnectMethods: () => ({ getMethodsForProtocol: fetchMethods }) },
    "~/composables/useSettingManager": { useSettingManager: () => settings }
  });
  return { ...useAssetAction(), store, calls, toasts, writes };
}

test("blocks token creation for denied saved methods, guide-only responses, missing permissions and errors", async () => {
  for (const [fetchMethods, preferred] of [
    [async () => [], "db_client"],
    [async () => [], "db_guide"],
    [async () => [], ""],
    [async () => [client], "db_guide"],
    [async () => {
      throw new Error("offline");
    }, "db_client"]
  ]) {
    const action = actionHarness(fetchMethods);
    action.store.currentConnectionInfoMap.asset = { protocol: "mariadb", connectMethod: preferred };
    await action.handleAssetConnection("root", "asset", "mariadb", [{ id: "account", name: "root" }]);
    await flush();
    assert.equal(action.calls.length, 0);
    assert.equal(action.writes.length, 0);
    assert.equal(action.toasts.length, 1);
  }
});

test("allows authorized database and SSH connections, including default selection", async () => {
  for (const [protocol, method, preferred] of [["mariadb", client, "db_client"], ["mariadb", client, ""], ["ssh", ssh, "ssh_client"]]) {
    const action = actionHarness(async () => [method]);
    await action.handleAssetConnection("root", "asset", protocol, [{ id: "account", name: "root" }], undefined, { connectMethod: preferred });
    await flush();
    assert.equal(action.toasts.length, 0);
    assert.equal(action.calls.length, 1);
    assert.equal(action.calls[0][0], "get_connect_token");
    assert.equal(action.calls[0][1].body.connect_method, method.value);
  }
});

function formHarness(fetchMethods) {
  const props = vue.reactive({ protocol: "mariadb", connectMethod: "db_client", account: "root", protocols: [], accounts: [] });
  const store = vue.reactive({ currentSite: "site", orgId: "org", currentUser: { bearerToken: "user" } });
  const validity = [];
  const scope = vue.effectScope();
  const form = scope.run(() => load("ui/components/EditForm/editForm.vue", {
    defineProps: () => props,
    defineEmits: () => (event, value) => {
      if (event === "update:connectMethod") props.connectMethod = value;
      if (event === "update:connectMethodValid") validity.push(value);
    }
  }, {
    "~/composables/useConnectMethods": { useConnectMethods: () => ({ getMethodsForProtocol: fetchMethods }) },
    "~/store/modules/userInfo": { useUserInfoStore: () => store }
  }, ["availableConnectMethods", "methodsLoading", "methodsError"]));
  return { ...form, props, store, validity, scope };
}

test("form clears denied saved selection and stays invalid on empty/error results", async () => {
  for (const result of [[], new Error("offline")]) {
    const pending = deferred();
    const form = formHarness(() => pending.promise);
    assert.equal(form.validity.at(-1), false);
    if (result instanceof Error) pending.reject(result);
    else pending.resolve(result);
    await flush();
    assert.equal(form.props.connectMethod, "");
    assert.equal(form.validity.at(-1), false);
    assert.equal(form.methodsLoading.value, false);
    form.scope.stop();
  }
});

test("form ignores out-of-order protocol responses and refreshes on user changes", async () => {
  const pending = deferred();
  let allowed = [ssh];
  const form = formHarness((protocol) => protocol === "mariadb" ? pending.promise : Promise.resolve(allowed));
  form.props.protocol = "ssh";
  await flush();
  assert.equal(form.props.connectMethod, "ssh_client");
  assert.equal(form.validity.at(-1), true);
  pending.resolve([client]);
  await flush();
  assert.equal(form.props.connectMethod, "ssh_client");
  allowed = [];
  form.store.currentUser.bearerToken = "other-user";
  await vue.nextTick();
  assert.equal(form.validity.at(-1), false);
  await flush();
  assert.equal(form.props.connectMethod, "");
  assert.equal(form.availableConnectMethods.value.length, 0);
  form.scope.stop();
});

test("connection editor blocks confirmation until the form is valid and resets validity when reopened", async () => {
  const editor = load("ui/components/ConnectionEditor/connectionEditor.vue", {
    defineProps: () => ({}),
    defineExpose() {},
    useAssetAction: () => ({ getAssetDetail() {} }),
    useI18n: () => ({ t: (key) => key, locale: vue.ref("en") }),
    sortPermedProtocols: (protocols) => protocols
  }, {
    "~/store/modules/userInfo": { useUserInfoStore: () => vue.reactive({ rememberAuthEnabled: true }) }
  }, ["openModal", "onConfirm", "onCancel", "connectMethodValid", "open"]);
  const asset = { id: "asset", permedProtocols: [{ name: "mariadb" }], permedAccounts: [{ alias: "root", name: "root" }] };
  const first = editor.openModal(asset);
  await flush();
  editor.onConfirm();
  assert.equal(editor.open.value, true);
  editor.connectMethodValid.value = true;
  editor.onConfirm();
  await first;
  assert.equal(editor.open.value, false);
  const second = editor.openModal(asset);
  const cancelled = assert.rejects(second, (error) => error === "cancelled");
  await flush();
  assert.equal(editor.connectMethodValid.value, false);
  editor.onCancel();
  await cancelled;
});

test("form waits for synchronous native session updates before refetching permissions", async () => {
  let nativeSession = "old-user";
  const fetchedSessions = [];
  const form = formHarness(async () => {
    fetchedSessions.push(nativeSession);
    return [];
  });
  await flush();
  form.store.currentUser.bearerToken = "new-user";
  // The user store synchronizes the native session after updating its refs.
  nativeSession = "new-user";
  await flush();
  assert.deepEqual(fetchedSessions, ["old-user", "new-user"]);
  form.scope.stop();
});
