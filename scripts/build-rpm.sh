#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="0.2.0"
rpm_root="${project_root}/build/rpmbuild"

if ! command -v rpmbuild >/dev/null; then
    echo "rpmbuild is missing. Install it with: sudo dnf install rpm-build" >&2
    exit 1
fi

mkdir -p "${rpm_root}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
# Avoid mixing packages from previous versions in wildcard-based installs.
find "${rpm_root}/RPMS" "${rpm_root}/SRPMS" -type f -name '*.rpm' -delete
tar -C "${project_root}" \
    --exclude=.git \
    --exclude=build \
    --transform="s,^\./,xtream-player-${version}/," \
    -czf "${rpm_root}/SOURCES/xtream-player-${version}.tar.gz" .
cp "${project_root}/packaging/xtream-player.spec" "${rpm_root}/SPECS/"

rpmbuild -ba "${rpm_root}/SPECS/xtream-player.spec" \
    --define "_topdir ${rpm_root}" \
    --define "_sourcedir ${rpm_root}/SOURCES"

main_rpm="$(find "${rpm_root}/RPMS" -type f -name "xtream-player-${version}-*.rpm" \
    ! -name '*debuginfo*' ! -name '*debugsource*' -print -quit)"
echo "Packages created under ${rpm_root}/RPMS and ${rpm_root}/SRPMS"
echo "Install with: sudo dnf upgrade '${main_rpm}'"
