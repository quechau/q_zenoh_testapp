#!/usr/bin/env bash
# test-services.sh — drive one field-bus service end to end against a real board.
#
# Provisions a device and a point, reads the config back, reads the point value, writes it,
# and listens for the notify stream — in that order, because each step depends on the last.
# Every packet is printed both as bytes and as JSON, so a failure shows what was actually sent.
#
#   ./scripts/test-services.sh modbus
#   ./scripts/test-services.sh bacnet --endpoint tls/192.168.10.29:7447
#   ./scripts/test-services.sh lora   --password acbm-fabric-2026
#
# It is deliberately NOT idempotent-by-cleanup: the device and point it creates are left on the
# board so you can keep poking at them. Pass --clean to remove them at the end.
set -u

PROTO="${1:-modbus}"
shift || true

ENDPOINT=""
PASSWORD="${QZ_PASSWORD:-acbm-fabric-2026}"
CLEAN=0
DEVICE_ID=1
POINT_ID=101
SUB_SECS=20

while [ $# -gt 0 ]; do
    case "$1" in
        --endpoint)  ENDPOINT="$2"; shift 2 ;;
        --password)  PASSWORD="$2"; shift 2 ;;
        --device-id) DEVICE_ID="$2"; shift 2 ;;
        --point-id)  POINT_ID="$2"; shift 2 ;;
        --sub-secs)  SUB_SECS="$2"; shift 2 ;;
        --clean)     CLEAN=1; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

APP="$(dirname "$0")/../build/q_zenoh_testapp"
[ -x "$APP" ] || { echo "build it first: cmake --build build -j" >&2; exit 1; }

# With no --endpoint the app finds the board itself (mDNS for the name, a TCP sweep for the
# address). That is slower but works on a bench where the address moves.
ARGS=()
[ -n "$ENDPOINT" ] && ARGS=(--endpoint "$ENDPOINT")

# The per-protocol difference is only in how a device is addressed and what a point selects.
# Everything else — the envelope, the sequence, the verdicts — is identical by design.
case "$PROTO" in
    modbus)
        DEVICE="device_id=$DEVICE_ID unit_id=11 interface=IF_RS485_1 enabled=true baud=9600 parity=PAR_NONE"
        POINT="point_id=$POINT_ID device_ref=$DEVICE_ID reg_type=REG_HOLDING address=0 data_type=DT_U16 scale=1 writable=true poll_class=POLL_NORMAL name=test-$PROTO"
        ;;
    bacnet)
        # mac_addr is the MS/TP MAC on the local segment. There is no discovery: a wrong MAC
        # shows up as a permanently faulted point, not as an error.
        DEVICE="device_id=$DEVICE_ID mac_addr=11 enabled=true"
        POINT="point_id=$POINT_ID device_ref=$DEVICE_ID obj_type=OBJ_AV obj_instance=1 scale=1 writable=true poll_class=POLL_NORMAL name=test-$PROTO"
        ;;
    lora)
        # dev_addr is the 32-bit LoRaRaw address printed by the sensor at boot. The bus is
        # PUSH: nothing is polled, so a point stays faulted until the sensor transmits.
        DEVICE="device_id=$DEVICE_ID dev_addr=0x6CC06351 enabled=true"
        POINT="point_id=$POINT_ID device_ref=$DEVICE_ID field=FIELD_TEMPERATURE scale=1 writable=false name=test-$PROTO"
        ;;
    *) echo "protocol must be modbus, bacnet or lora" >&2; exit 2 ;;
esac

say() { printf '\n\033[1m=== %s\033[0m\n' "$*"; }

say "$PROTO — provisioning, reading, writing"

{
    echo "login $PASSWORD"

    echo "config $PROTO read"            # before: what is already there
    echo "config $PROTO add-device $DEVICE"
    echo "config $PROTO add-point $POINT"
    echo "config $PROTO read"            # after: the board's own snapshot

    echo "points $PROTO read"            # every point
    echo "points $PROTO read $POINT_ID"  # just this one

    # LoRa is receive-only — the board rejects every write by design, so asking for one is
    # part of the test rather than a mistake: the expected result is WR_NOT_WRITABLE.
    echo "points $PROTO write point_id=$POINT_ID value=42"

    echo "points $PROTO sub $SUB_SECS"   # the COV stream, if anything changes

    if [ "$CLEAN" = "1" ]; then
        echo "config $PROTO del-point $POINT_ID"
        echo "config $PROTO del-device $DEVICE_ID"
    fi
    echo "quit"
} | "$APP" "${ARGS[@]}"

cat <<EOF

=== how to read the result

  applied_ids: [$DEVICE_ID]     the board accepted the device
  rejected: [{id, reason}]      it did not — the reason names which rule (REJ_BAD_ID,
                                REJ_UNKNOWN_REF, REJ_POOL_FULL, REJ_IFACE_ROLE, …)
  quality: "Q_GOOD"             the field bus answered
  quality: "Q_FAULT"            the point is provisioned but the device is not answering:
                                wrong unit id / MAC / dev_addr, wrong register, or a line
                                rate that does not match the slave
  error: { code: ... }          the request never reached a handler; ERROR_PERMISSION here
                                usually means the login lapsed, not that the data is wrong

An empty payload on a points READ is not an error either: PointValues with no entries encodes
to zero bytes, so a board with nothing provisioned answers with no payload at all.
EOF
