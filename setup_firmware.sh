#!/bin/bash
# setup_firmware.sh – dodaje ath9k_htc firmware do wbudowania w jądro

set -e

FIRMWARE_PATH="ath9k_htc/htc_7010-1.4.0.fw"
KERNEL_TREE="$(pwd)"
FWMK="$KERNEL_TREE/firmware/Makefile"
CONFIG="$KERNEL_TREE/.config"

echo "[+] Sprawdzanie katalogu źródeł kernela: $KERNEL_TREE"

# 1. Upewnij się, że plik firmware istnieje
if [ ! -f "/lib/firmware/$FIRMWARE_PATH" ]; then
    echo "[!] Brak pliku /lib/firmware/$FIRMWARE_PATH"
    echo "    Pobieranie z GitHub (QCA)..."
    mkdir -p /lib/firmware/ath9k_htc/
    wget -O "/lib/firmware/$FIRMWARE_PATH" https://github.com/qca/open-ath9k-htc-firmware/raw/master/htc_7010.fw
fi

# 2. Skopiuj firmware do katalogu roboczego (opcjonalnie)
mkdir -p "$KERNEL_TREE/firmware/ath9k_htc"
cp "/lib/firmware/$FIRMWARE_PATH" "$KERNEL_TREE/firmware/$FIRMWARE_PATH"

# 3. Ustawienia w .config (zostaną nadpisane)
echo "[+] Modyfikuję .config"
grep -q CONFIG_ATH9K "$CONFIG" || {
    echo "CONFIG_WLAN=y" >> "$CONFIG"
    echo "CONFIG_MAC80211=y" >> "$CONFIG"
    echo "CONFIG_CFG80211=y" >> "$CONFIG"
    echo "CONFIG_ATH_COMMON=y" >> "$CONFIG"
    echo "CONFIG_ATH9K=y" >> "$CONFIG"
    echo "CONFIG_ATH9K_HTC=y" >> "$CONFIG"
}

sed -i '/^CONFIG_EXTRA_FIRMWARE=/d' "$CONFIG"
sed -i '/^CONFIG_EXTRA_FIRMWARE_DIR=/d' "$CONFIG"
echo "CONFIG_EXTRA_FIRMWARE=\"$FIRMWARE_PATH\"" >> "$CONFIG"
echo "CONFIG_EXTRA_FIRMWARE_DIR=\"/lib/firmware\"" >> "$CONFIG"
echo "CONFIG_PREVENT_FIRMWARE_BUILD=n" >> "$CONFIG"

# 4. Dodanie wpisu do firmware/Makefile
echo "[+] Dodaję wpis do firmware/Makefile (fw-shipped-y)"
if ! grep -q "$FIRMWARE_PATH" "$FWMK"; then
    echo "fw-shipped-y += $FIRMWARE_PATH" >> "$FWMK"
fi

# 5. Gotowe
echo "[✔] Gotowe. Teraz wykonaj:"
echo
echo "   make olddefconfig"
echo "   make -j\$(nproc)"
echo
echo "[ℹ] Firmware zostanie wbudowany do jądra."
