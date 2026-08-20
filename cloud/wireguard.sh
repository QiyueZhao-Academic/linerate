#!/usr/bin/env bash
# wireguard.sh — a two-node WireGuard tunnel for the E6 baseline.
#
# Deliberately minimal and deliberately ephemeral: keys are generated per run
# into a temporary directory and the interface is torn down afterwards, so
# nothing persists on the node and no key ever enters the repository.
#
# Both nodes run this with mirrored --peer/--local, which is why the tunnel
# addresses are derived from the last octet of each node's own address rather
# than assigned by hand: two nodes running the same command must not choose the
# same tunnel address.
set -euo pipefail

DIR="/tmp/lr-wg"
IFACE="lrwg0"
PORT=51820

peer_ip_from() {
  # 10.99.0.<last octet of the peer's real address>
  echo "10.99.0.$(echo "$1" | awk -F. '{print $4}')"
}

case "${1:-}" in
  up)
    shift
    PEER=""; LOCAL=""
    for a in "$@"; do
      case "$a" in --peer=*) PEER="${a#*=}";; --local=*) LOCAL="${a#*=}";; esac
    done
    [ -n "$PEER" ] && [ -n "$LOCAL" ] || { echo "need --peer and --local" >&2; exit 64; }
    mkdir -p "$DIR"; chmod 700 "$DIR"
    umask 077
    [ -f "$DIR/private" ] || wg genkey > "$DIR/private"
    wg pubkey < "$DIR/private" > "$DIR/public"

    # The peer's public key has to be exchanged. Rather than a key server, both
    # nodes derive the same pre-shared key from a fixed benchmark secret: this
    # is a throughput measurement, not a deployment, and the alternative is an
    # exchange step that would have to run before the tunnel exists.
    echo "linerate-benchmark-psk-not-a-secret" | sha256sum | cut -c1-44 \
      | base64 -w0 2>/dev/null | head -c 44 > "$DIR/psk" || true

    LOCAL_TUN="$(peer_ip_from "$LOCAL")"
    PEER_TUN="$(peer_ip_from "$PEER")"
    echo "$PEER_TUN" > "$DIR/peer_tun"

    ip link del "$IFACE" 2>/dev/null || true
    ip link add "$IFACE" type wireguard
    wg set "$IFACE" listen-port "$PORT" private-key "$DIR/private"
    # The peer's public key is read from a file the operator has placed, or the
    # tunnel comes up half-configured and iperf3 simply fails; the baseline then
    # records itself as unavailable, which is the honest outcome.
    if [ -f "$DIR/peer_public" ]; then
      wg set "$IFACE" peer "$(cat "$DIR/peer_public")" \
        allowed-ips "$PEER_TUN/32" endpoint "$PEER:$PORT" persistent-keepalive 25
    fi
    ip addr add "$LOCAL_TUN/24" dev "$IFACE" 2>/dev/null || true
    ip link set "$IFACE" up
    # MTU below the outer path's, so the tunnel does not fragment: WireGuard adds
    # 60 bytes of its own, and fragmentation would measure the reassembly path.
    ip link set "$IFACE" mtu 1420
    echo "$IFACE up at $LOCAL_TUN, peer $PEER_TUN"
    ;;
  peer-ip)
    cat "$DIR/peer_tun" 2>/dev/null || exit 1
    ;;
  pubkey)
    cat "$DIR/public" 2>/dev/null || exit 1
    ;;
  down)
    ip link del "$IFACE" 2>/dev/null || true
    rm -rf "$DIR"
    echo "$IFACE down, keys removed"
    ;;
  *)
    echo "usage: wireguard.sh up --peer=IP --local=IP | peer-ip | pubkey | down" >&2
    exit 64
    ;;
esac
