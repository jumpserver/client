import type { UnlistenFn } from "@tauri-apps/api/event";
import type { ClientSessionInfo } from "~/types";
import { h } from "vue";
import { useUserInfoStore } from "~/store/modules/userInfo";

/** Native credentials and server policy are authoritative; persisted UI state is only a cache. */
export function useClientSession() {
  const desktop = useTauriCoreIsTauri();
  const store = useUserInfoStore();
  const ready = ref(!desktop);
  const error = ref(false);
  const toast = useToast();
  const { t, locale } = useI18n();
  const pending = new Map<string, Promise<boolean>>();
  const warned = new Map<string, string>();
  const now = ref(Date.now());
  let timer: ReturnType<typeof setInterval> | undefined;
  let countdownTimer: ReturnType<typeof setInterval> | undefined;
  let unlisten: UnlistenFn | undefined;
  let unlistenLogin: UnlistenFn | undefined;
  let started = false;
  let stopped = false;

  function clearWarning(site: string) {
    const id = warned.get(site);
    if (id) toast.remove(id);
    warned.delete(site);
  }

  function expire(site: string) {
    clearWarning(site);
    if (!store.userMap[site]) return;
    store.deleteUserData(site, false);
    toast.add({
      title: t("Login.LoginAuthenticationExpired"),
      icon: "i-lucide-log-out",
      color: "warning"
    });
  }

  async function check(site: string): Promise<boolean> {
    if (!store.userMap[site]) return false;
    const existing = pending.get(site);
    if (existing) return existing;
    const request = useTauriCoreInvoke<ClientSessionInfo | null>("check_client_session", { site })
      .then((session) => {
        if (store.userMap[site] && session?.expires_at && session.remaining_seconds !== null
          && session.remaining_seconds > 0 && session.remaining_seconds < 600 && !warned.has(site)) {
          const expiresAt = session.expires_at;
          const warning = toast.add({
            title: t("Login.LifetimeSoon"),
            description: () => {
              const seconds = Math.max(0, Math.ceil((Date.parse(expiresAt) - now.value) / 1000));
              const remaining = t("Login.LifetimeCountdown", { minutes: Math.floor(seconds / 60), seconds: String(seconds % 60).padStart(2, "0") });
              return h("span", { "aria-live": "off" }, t("Login.LifetimeSoonDescription", { site, remaining, at: new Date(expiresAt).toLocaleString(locale.value) }));
            },
            icon: "i-lucide-clock",
            color: "warning",
            duration: 0
          });
          warned.set(site, warning.id);
        }
        return true;
      })
      .catch((cause) => {
        if (String(cause) === "client_session_expired") expire(site);
        return false;
      })
      .finally(() => pending.delete(site));
    pending.set(site, request);
    return request;
  }

  async function retry() {
    const site = store.currentSite;
    const valid = !site || await check(site);
    if (stopped || site !== store.currentSite) return;
    ready.value = valid;
    error.value = !valid;
  }

  watch(() => store.currentSite, () => {
    if (!started) return;
    ready.value = false;
    error.value = false;
    void retry();
  });

  onMounted(async () => {
    if (!desktop || useTauriWindowGetCurrentWindow().label !== "main") {
      ready.value = true;
      return;
    }
    unlisten = await useTauriEventListen<{ site: string }>("client-session-expired", ({ payload }) => expire(payload.site));
    unlistenLogin = await useTauriEventListen<{ resolved_site: string }>("login-success-detected", ({ payload }) => {
      clearWarning(payload.resolved_site);
    });
    if (stopped) {
      unlisten();
      unlistenLogin();
      return;
    }
    started = true;
    store.$persist();
    const poll = () => {
      void retry();
      for (const site of Object.keys(store.userMap)) {
        if (site !== store.currentSite) void check(site);
      }
    };
    poll();
    // Checks never extend the absolute expiry, including for background sites.
    timer = setInterval(poll, 30_000);
    countdownTimer = setInterval(() => {
      now.value = Date.now();
    }, 1000);
  });

  onBeforeUnmount(() => {
    stopped = true;
    if (timer) clearInterval(timer);
    if (countdownTimer) clearInterval(countdownTimer);
    unlisten?.();
    unlistenLogin?.();
  });

  return { sessionReady: ready, sessionError: error, retrySession: retry };
}
