# Native DEB/RPM metadata. The generators inspect the libraries in the build
# distribution; package-linux.py refuses to generate RPMs on Debian hosts.
set(CPACK_PACKAGE_NAME "client-qt")
# Preserve the native package identity for upgrades; only the download name changes.
set(_package_arch "${CLIENT_GOARCH}")
if(NOT _package_arch)
  set(_package_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(CPACK_PACKAGE_FILE_NAME "OSMClient-${PROJECT_VERSION}-linux-${_package_arch}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_PACKAGE_RELOCATABLE FALSE)
set(CPACK_PACKAGE_DESCRIPTION "OSMClient desktop client with OAuth login, authorized assets and native protocol launchers.")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://www.jumpserver.org/")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES TRUE)

set(CPACK_DEBIAN_PACKAGE_NAME "client-qt")
set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.deb")
set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "FIT2CLOUD")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsecret-1-0, desktop-file-utils, xdg-utils, ca-certificates")
if(CLIENT_QT_MAJOR STREQUAL "6")
  string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", qt6-qpa-plugins")
endif()
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst"
  "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postrm")
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)

set(CPACK_RPM_PACKAGE_NAME "client-qt")
set(CPACK_RPM_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.rpm")
set(CPACK_RPM_PACKAGE_RELEASE "1")
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_REQUIRES "libsecret, desktop-file-utils, xdg-utils, ca-certificates")
if(CLIENT_QT_MAJOR STREQUAL "5")
  string(APPEND CPACK_RPM_PACKAGE_REQUIRES ", qt5-qtbase-gui, qt5-qtsvg")
else()
  string(APPEND CPACK_RPM_PACKAGE_REQUIRES ", qt6-qtbase-gui, qt6-qtsvg")
endif()
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/update-desktop-cache.sh")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/update-desktop-cache.sh")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
  "/usr/share/applications" "/usr/share/icons" "/usr/share/icons/hicolor"
  "/usr/share/icons/hicolor/512x512" "/usr/share/icons/hicolor/512x512/apps"
  "/usr/share/licenses")
set(CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none\n%define debug_package %{nil}")

# Fail packaging an incomplete development build instead of silently publishing
# a GUI without the protocol launcher or system credential storage.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/preflight.cmake.in"
               "${CMAKE_CURRENT_BINARY_DIR}/linux-package-preflight.cmake" @ONLY)
set(CPACK_INSTALL_SCRIPTS "${CMAKE_CURRENT_BINARY_DIR}/linux-package-preflight.cmake")
