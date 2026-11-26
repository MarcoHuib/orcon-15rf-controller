# The QEMU memory test's image (tools/qemu_memtest.sh): the pinned ESP-Matter / ESP-IDF 5.5.5 image of tools/build_idf.sh
# plus Espressif QEMU exactly as ESP-IDF v5.5.5 pins it in tools/tools.json (idf_tools.py checks its SHA-256).
# Built locally or in CI for the test only; never pushed anywhere.
FROM espressif/esp-matter@sha256:94ed6a211d5bc6b55c44a36802bdb0e663a92d9aac7ece3944e6a1bec10a7f17
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      libsdl2-2.0-0 libslirp0 libpixman-1-0 libgcrypt20 libglib2.0-0t64 && rm -rf /var/lib/apt/lists/* && \
    python3 $IDF_PATH/tools/idf_tools.py install qemu-xtensa && \
    ln -s /opt/espressif/tools/tools/qemu-xtensa/esp_develop_9.2.2_20260417/qemu/bin/qemu-system-xtensa /usr/local/bin/qemu-system-xtensa
