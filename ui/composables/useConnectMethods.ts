import { useUserInfoStore } from "~/store/modules/userInfo";

export interface ConnectMethod {
  value: string
  label: string
  type: string
  icon: string
  disabled: boolean
  listen: string
  component: string
}

interface ConnectMethodsResponse {
  [protocol: string]: ConnectMethod[]
}

export const useConnectMethods = () => {
  const { currentSite, currentUser, orgId } = storeToRefs(useUserInfoStore());
  const getSessionKey = () => JSON.stringify([
    currentSite.value,
    orgId.value,
    currentUser.value?.bearerToken
  ]);

  const fetchConnectMethods = async (): Promise<ConnectMethodsResponse> => {
    // 连接方式属于实时权限，不复用之前登录或规则变更前的结果。
    const sessionKey = getSessionKey();
    const data = await useTauriCoreInvoke<string>("get_connect_methods", {});
    if (sessionKey !== getSessionKey()) {
      throw new Error("Session changed while fetching connect methods");
    }
    const methods = JSON.parse(data) as ConnectMethodsResponse;
    if (!methods || typeof methods !== "object" || Array.isArray(methods)) {
      throw new Error("Invalid connect methods response");
    }
    return methods;
  };

  const getMethodsForProtocol = async (protocol: string): Promise<ConnectMethod[]> => {
    const allMethods = await fetchConnectMethods();
    const protocolMethods = allMethods[protocol] || [];
    return protocolMethods.filter((method) => {
      if (method.disabled) return false;
      // 远程应用也支持 orig_app 等自定义协议，按连接方式类型判断。
      if (method.type === "applet") return true;
      return protocol !== "http" && method.type === "native" && !method.value.endsWith("_guide");
    });
  };

  const getDefaultMethodForProtocol = async (protocol: string): Promise<string> => {
    const methods = await getMethodsForProtocol(protocol);
    return methods[0]?.value || "";
  };

  const getMethodDisplayName = async (protocol: string, methodValue: string): Promise<string> => {
    const methods = await getMethodsForProtocol(protocol);
    const method = methods.find((m) => m.value === methodValue);
    return method?.label ?? methodValue;
  };

  return {
    fetchConnectMethods,
    getMethodsForProtocol,
    getDefaultMethodForProtocol,
    getMethodDisplayName
  };
};
