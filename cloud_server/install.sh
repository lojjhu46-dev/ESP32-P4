#!/usr/bin/env bash
# facecheck installer (run as root from the extracted cloud_server directory)
#   tar xzf facecheck.tgz -C /opt && bash /opt/cloud_server/install.sh
set -euo pipefail

SRC="$(cd "$(dirname "$0")" && pwd)"
MODE=prod
if [ "${1:-}" = "--test" ]; then
  MODE=test
fi
if [ "$MODE" = "test" ]; then
  PREFIX=/opt/facecheck_test
  DATA=/var/lib/facecheck_test
  CONF=/etc/facecheck_test.json
  UNIT=/etc/systemd/system/facecheck-test.service
  SVC=facecheck-test.service
  PORT=18080
  BIND_HOST=127.0.0.1
  DEDUP=10            # short window so the PC-image regression can re-send frames
else
  PREFIX=/opt/facecheck
  DATA=/var/lib/facecheck
  CONF=/etc/facecheck.json
  UNIT=/etc/systemd/system/facecheck.service
  SVC=facecheck.service
  PORT=8080
  BIND_HOST=0.0.0.0
  DEDUP=1800          # one check-in per person+device per 30 min
fi
USER=facecheck

if [ "$(id -u)" != "0" ]; then
  echo "install.sh must run as root" >&2
  exit 1
fi
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }

# 1) system user + directories
if ! id "$USER" >/dev/null 2>&1; then
  useradd -r -s /sbin/nologin -d "$DATA" "$USER"
  echo "created user: $USER"
fi
install -d -m 0755 -o root -g root "$PREFIX"
cp -a "$SRC"/. "$PREFIX"/
rm -rf "$PREFIX"/__pycache__
chmod 0755 "$PREFIX"/install.sh 2>/dev/null || true
install -d -m 0750 -o "$USER" -g "$USER" "$DATA" "$DATA/photos"

# 2) config (keep existing token on reinstall)
if [ ! -f "$CONF" ]; then
  TOKEN="$(python3 - <<'PY'
import secrets
print(secrets.token_urlsafe(24))
PY
)"
  SECRET="$(python3 - <<'PY'
import secrets
print(secrets.token_hex(32))
PY
)"
  cat > "$CONF" <<EOF
{
  "host": "$BIND_HOST",
  "port": $PORT,
  "token": "$TOKEN",
  "cookie_secret": "$SECRET",
  "data_dir": "$DATA",
  "dedup_window_s": $DEDUP,
  "snapshots_keep": 5,
  "photo_max_bytes": 2097152,
  "max_body_bytes": 8388608,
  "photo_keep_days": 90,
  "session_hours": 12,
  "max_concurrent": 8
}
EOF
  chown root:"$USER" "$CONF"
  chmod 0640 "$CONF"
  echo
  echo "=============================================="
  echo " TOKEN: $TOKEN"
  echo " (stored in $CONF, also needed by qt_host)"
  echo "=============================================="
  echo
else
  echo "config exists, keeping token: $CONF"
fi

# 3) timezone
timedatectl set-timezone Asia/Shanghai 2>/dev/null || \
  ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime

# 4) systemd unit
cat > "$UNIT" <<EOF
[Unit]
Description=FaceCheck lightweight sign-in service ($MODE)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$USER
Group=$USER
Environment=TZ=Asia/Shanghai
Environment=PYTHONUNBUFFERED=1
Environment=PYTHONDONTWRITEBYTECODE=1
WorkingDirectory=$PREFIX
ExecStart=/usr/bin/python3 $PREFIX/app.py --config $CONF
Restart=always
RestartSec=3
MemoryMax=256M
CPUQuota=80%
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$DATA
UMask=0077

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable "$UNIT" >/dev/null 2>&1 || true
systemctl restart "$SVC"
sleep 1

# 5) firewall only for the production instance; test stays loopback-only
if [ "$MODE" = "test" ]; then
  echo "test instance binds $BIND_HOST:$PORT (no firewall change; use an SSH tunnel)"
elif command -v firewall-cmd >/dev/null 2>&1 && systemctl is-active --quiet firewalld; then
  firewall-cmd --permanent --add-port=$PORT/tcp >/dev/null || true
  firewall-cmd --reload >/dev/null || true
  echo "firewalld: opened tcp/$PORT"
else
  echo "firewalld not active; open tcp/$PORT in the Alibaba Cloud security group"
fi

cat <<EOF

facecheck installed ($MODE).
  service : systemctl status $SVC
  logs    : journalctl -u $SVC -f
  health  : curl http://127.0.0.1:$PORT/api/health
  prefix  : $PREFIX
  data    : $DATA
  config  : $CONF

Optional hardening (not applied automatically):
  * add 1G swap:  fallocate -l 1G /swapfile && chmod 600 /swapfile &&
                  mkswap /swapfile && swapon /swapfile
                  echo '/swapfile none swap sw 0 0' >> /etc/fstab
  * disable rpcbind if NFS is not used: systemctl disable --now rpcbind
EOF
if [ "$MODE" = "prod" ]; then
  cat <<EOF
  * restrict inbound tcp/$PORT to your office egress IP in the security group
EOF
else
  cat <<EOF
  * test access from a PC: ssh -L $PORT:127.0.0.1:$PORT root@<server> -N
EOF
fi
