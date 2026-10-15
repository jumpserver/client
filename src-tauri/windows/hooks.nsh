!macro NSIS_HOOK_POSTINSTALL
  ; 删除 Windows 对 jms2 协议的用户选择缓存。
  ; 如果旧版本曾经被记录为默认处理程序，这个缓存可能会继续盖过新注册项。
  DeleteRegKey HKCU "Software\Microsoft\Windows\Shell\Associations\UrlAssociations\jms2\UserChoice"

  ; 显式写入 HKCU\Software\Classes\jms2。
  ; HKCU 的协议注册优先级高于 HKLM，所以可以覆盖旧版本可能留下的机器级注册。
  WriteRegStr HKCU "Software\Classes\jms2" "URL Protocol" ""
  WriteRegStr HKCU "Software\Classes\jms2" "" "URL:jms2"
  WriteRegStr HKCU "Software\Classes\jms2" "FriendlyTypeName" "JumpServer Client URL"

  ; 系统 UI 中显示的协议图标。
  WriteRegStr HKCU "Software\Classes\jms2\DefaultIcon" "" "$\"$INSTDIR\${MAINBINARYNAME}.exe$\",0"

  ; 打开 jms2:// 链接时执行的命令。%1 是浏览器或系统传进来的原始链接。
  WriteRegStr HKCU "Software\Classes\jms2\shell\open\command" "" "$\"$INSTDIR\${MAINBINARYNAME}.exe$\" $\"%1$\""

  ; 服务端 v5 起只下发 jms2:// 链接，旧的 jms:// 协议已不再被使用。
  ; 这里在升级时顺手清掉旧注册项和它的 UserChoice 缓存，避免系统继续把
  ; jms:// 链接交给一个已经不存在或已失效的处理程序。
  ReadRegStr $R7 HKCU "Software\Classes\jms\shell\open\command" ""
  StrCmp $R7 "$\"$INSTDIR\${MAINBINARYNAME}.exe$\" $\"%1$\"" 0 +2
    DeleteRegKey HKCU "Software\Classes\jms"
  DeleteRegKey HKCU "Software\Microsoft\Windows\Shell\Associations\UrlAssociations\jms\UserChoice"

  ; 通知 Windows shell 协议关联已经变化，避免系统继续使用旧缓存。
  System::Call 'shell32::SHChangeNotify(i, i, p, p) (0x08000000, 0x1000, 0, 0)'
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  ; 只在当前 HKCU jms2 协议确实指向本安装目录时删除，避免误删其他应用新接管的注册。
  ReadRegStr $R7 HKCU "Software\Classes\jms2\shell\open\command" ""
  StrCmp $R7 "$\"$INSTDIR\${MAINBINARYNAME}.exe$\" $\"%1$\"" 0 +2
    DeleteRegKey HKCU "Software\Classes\jms2"

  ; 通知 Windows shell 协议关联已经变化，避免卸载后继续使用旧缓存。
  System::Call 'shell32::SHChangeNotify(i, i, p, p) (0x08000000, 0x1000, 0, 0)'
!macroend

!macro NSIS_HOOK_POSTUNINSTALL
  ; Tauri 的更新器会以 /UPDATE 调用旧版卸载器，此时同样会执行本 hook。
  ; 只在用户真正卸载时删除数据，避免升级过程丢失配置、日志和插件。
  ${If} $UpdateMode <> 1
    ; installMode="both" 可能会将 shell 上下文切到 all，用户数据始终属于当前用户。
    SetShellVarContext current

    ; Tauri 自身的 app data/store/video 目录。
    RMDir /r "$APPDATA\${BUNDLEID}"
    RMDir /r "$LOCALAPPDATA\${BUNDLEID}"

    ; JumpServer 配置、日志和用户插件使用的自定义目录。
    RMDir /r "$APPDATA\jumpserver-client"
    RMDir /r "$LOCALAPPDATA\jumpserver-client"

    ; 兼容旧版本使用产品名创建的用户数据目录。
    RMDir /r "$APPDATA\JumpServerClient"
    RMDir /r "$LOCALAPPDATA\JumpServerClient"
  ${EndIf}
!macroend
