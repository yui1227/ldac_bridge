#!/bin/sh
# Fetch pinned third-party sources and Intel firmware, apply local patches.
# Run once after cloning (Git Bash / MSYS2):  sh setup.sh
set -e
cd "$(dirname "$0")"

BTSTACK_COMMIT=0f4dd2ac1240a503edc4ec7750a32704f85448fc
LIBLDAC_COMMIT=eeee1a3f5f8df1282e3a6d297085885fd886737b
FIRMWARE_URL=https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/intel
FIRMWARE_FILES="ibt-18-16-1.sfi ibt-18-16-1.ddc"   # Intel ThunderPeak (USB 8087:0025)

mkdir -p third_party

if [ ! -d third_party/btstack ]; then
    echo "Downloading BTstack $BTSTACK_COMMIT..."
    mkdir -p third_party/btstack
    curl -sSfL "https://github.com/bluekitchen/btstack/archive/$BTSTACK_COMMIT.tar.gz" \
        | tar -xz -C third_party/btstack --strip-components=1
    for p in patches/btstack-*.patch; do
        echo "Applying $p"
        patch -d third_party/btstack -p1 < "$p"
    done
fi

if [ ! -d third_party/libldac ]; then
    echo "Downloading libldac $LIBLDAC_COMMIT..."
    mkdir -p third_party/libldac
    curl -sSfL "https://android.googlesource.com/platform/external/libldac/+archive/$LIBLDAC_COMMIT.tar.gz" \
        | tar -xz -C third_party/libldac
fi

mkdir -p ldac_bridge/firmware
for f in $FIRMWARE_FILES; do
    if [ ! -f "ldac_bridge/firmware/$f" ]; then
        echo "Downloading firmware $f..."
        curl -sSfL -o "ldac_bridge/firmware/$f" "$FIRMWARE_URL/$f"
    fi
done

echo "Done. Build with: sh ldac_bridge/build.sh"
