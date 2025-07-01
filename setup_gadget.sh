#!/bin/bash

CONFIG_FILE=".config"
BACKUP_FILE=".config.bak"

# Lista wymaganych opcji (symbol=wartość)
declare -A REQUIRED_CONFIGS=(
  [CONFIG_USB_GADGET]="y"
  [CONFIG_USB_CONFIGFS]="y"
  [CONFIG_USB_G_SERIAL]="m"
  [CONFIG_U_SERIAL_CONSOLE]="y"
  [CONFIG_USB_CONFIGFS_SERIAL]="y"
  [CONFIG_USB_F_SERIAL]="y"
  [CONFIG_USB_CONFIGFS_MASS_STORAGE]="y"
  [CONFIG_USB_F_MASS_STORAGE]="y"
  [CONFIG_USB_CONFIGFS_RNDIS]="y"
  [CONFIG_USB_F_RNDIS]="y"
  [CONFIG_USB_CONFIGFS_ECM]="y"
  [CONFIG_USB_F_ECM]="y"
  [CONFIG_USB_CONFIGFS_HID]="y"
  [CONFIG_USB_F_HID]="y"
  [CONFIG_USB_CONFIGFS_F_FS]="y"
  [CONFIG_USB_F_FS]="y"
  [CONFIG_ISO9660_FS]="y"
  [CONFIG_BLK_DEV_LOOP]="y"
  [CONFIG_UDF_FS]="y"
)

# Sprawdź czy plik .config istnieje
if [[ ! -f "$CONFIG_FILE" ]]; then
  echo "❌ Nie znaleziono $CONFIG_FILE w bieżącym katalogu."
  exit 1
fi

# Utwórz kopię zapasową
cp "$CONFIG_FILE" "$BACKUP_FILE"
echo "📦 Utworzono kopię zapasową: $BACKUP_FILE"

CHANGED=0

for SYMBOL in "${!REQUIRED_CONFIGS[@]}"; do
  VALUE="${REQUIRED_CONFIGS[$SYMBOL]}"
  
  # Usuń istniejący wpis
  sed -i "/^$SYMBOL[ =]/d" "$CONFIG_FILE"
  sed -i "/^# $SYMBOL is not set/d" "$CONFIG_FILE"
  
  # Dodaj poprawny wpis na końcu pliku
  echo "$SYMBOL=$VALUE" >> "$CONFIG_FILE"
  echo "✅ Ustawiono: $SYMBOL=$VALUE"
  CHANGED=1
done

# Pokaż różnice
if [[ "$CHANGED" == "1" ]]; then
  echo -e "\n📄 Różnice względem oryginalnego pliku:"
  diff -u "$BACKUP_FILE" "$CONFIG_FILE" || true
else
  echo "ℹ️ Wszystkie wymagane opcje były już poprawnie ustawione."
fi
