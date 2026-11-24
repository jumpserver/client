#!/bin/sh
set -eu

sed -i 's|^mirrorlist=|#mirrorlist=|;s|^#baseurl=http://dl.rockylinux.org/|baseurl=https://dl.rockylinux.org/|' /etc/yum.repos.d/Rocky-*.repo
cat > /etc/yum.repos.d/centos-qt511.repo <<'EOF'
[centos-qt511-base]
name=CentOS 8.0 BaseOS
baseurl=https://vault.centos.org/8.0.1905/BaseOS/x86_64/os/
enabled=0
gpgcheck=1
gpgkey=https://www.centos.org/keys/RPM-GPG-KEY-CentOS-Official
[centos-qt511-app]
name=CentOS 8.0 AppStream
baseurl=https://vault.centos.org/8.0.1905/AppStream/x86_64/os/
enabled=0
gpgcheck=1
gpgkey=https://www.centos.org/keys/RPM-GPG-KEY-CentOS-Official
EOF

dnf install -y gcc-c++ cmake make python39 zlib-devel libsecret-devel rpm-build desktop-file-utils xdg-utils pkgconf-pkg-config file
dnf install -y --disablerepo='*' --enablerepo=centos-qt511-base --enablerepo=centos-qt511-app \
  qt5-qtbase-devel-5.11.1-5.el8 qt5-qtsvg-devel-5.11.1-2.el8 qt5-qtmultimedia-devel-5.11.1
test "$(pkg-config --modversion Qt5Core)" = 5.11.1
