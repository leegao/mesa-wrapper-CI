sudo docker run --rm -v "$(pwd):/workspace" wrapper-compiler build

TARGET_UID="${SUDO_UID:-$(id -u)}"
TARGET_GID="${SUDO_GID:-$(id -g)}"

sudo docker run --rm -v "$(pwd):/workspace" --entrypoint chown wrapper-compiler -R "${TARGET_UID}:${TARGET_GID}" build

cd build/
rm libvulkan_wrapper.so.zip 2>/dev/null
zip libvulkan_wrapper.so.zip libvulkan_wrapper.so
