#!/usr/bin/env bash
# test-services.sh — drive a field-bus service end to end against a real board, over zenoh.
#
# Provisions a device and its points, reads the config back, reads the values, writes one, and
# listens for the notify stream — in that order, because each step depends on the last. Every
# packet is printed both as bytes and as JSON, so a failure shows what was actually sent.
#
#   ./scripts/test-services.sh modbus --device hvac
#   ./scripts/test-services.sh modbus --device zcd --endpoint tls/192.168.10.29:7447
#   ./scripts/test-services.sh modbus --device ddm18sd
#   ./scripts/test-services.sh bacnet --device io-card --mac 11
#   ./scripts/test-services.sh lora
#
# The device profiles are the ones on the bench, taken from ACB-M/docs/*-Module/*-test. They
# exist so the addresses are not invented: getting a Modbus address wrong reads a real register
# that means something else, and the reply looks perfectly healthy.
#
#   ADDRESSES ARE PDU (0-BASED) HERE, WHICH IS THE CONSOLE NUMBER MINUS ONE.
#   The board adds 1 on the way to its Modbus master (zenoh_modbus.c: address + 1). The console
#   subtracts 1 from what you type. So a register typed as `mb_read_ao 1 11 1000` is
#   `address=999` here. Verified against the ZC-Damper alive register, which must read 53521.
#
# Not idempotent-by-cleanup on purpose: what it creates is left on the board so you can keep
# poking at it. Pass --clean to remove it at the end.
set -u

PROTO="${1:-modbus}"
shift || true

ENDPOINT=""
PASSWORD="${QZ_PASSWORD:-acbm-fabric-2026}"
PROFILE=""
CLEAN=0
MAC=11
SUB_SECS=20
# Empty until a profile picks its own, so two profiles cannot overwrite each other's device.
# A device id is the key of the upsert: reusing it silently repoints every point that
# references it, which is how the HVAC points ended up addressing the damper.
DEVICE_ID=""

while [ $# -gt 0 ]; do
    case "$1" in
        --endpoint)  ENDPOINT="$2"; shift 2 ;;
        --password)  PASSWORD="$2"; shift 2 ;;
        --device)    PROFILE="$2"; shift 2 ;;
        --device-id) DEVICE_ID="$2"; shift 2 ;;
        --mac)       MAC="$2"; shift 2 ;;
        --sub-secs)  SUB_SECS="$2"; shift 2 ;;
        --clean)     CLEAN=1; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

APP="$(dirname "$0")/../build/q_zenoh_testapp"
[ -x "$APP" ] || { echo "build it first: cmake --build build -j" >&2; exit 1; }

ARGS=()
[ -n "$ENDPOINT" ] && ARGS=(--endpoint "$ENDPOINT")

DEVICE=""
POINTS=()
WRITE=""          # "point_id=N value=X", empty when the device has nothing safe to write
NOTE=""

case "$PROTO:${PROFILE:-default}" in

  # --- Modbus: board support carrying the Daikin HVAC handler -----------------------------
  # ACB-M/docs/Modbus-Module/hvac-control-test — slave 1 on RS485-1 at 38400-8-N-1.
  # Register map is 0-based internal, so console = internal + 1 and PDU = internal.
  modbus:hvac)
    : "${DEVICE_ID:=1}"
    DEVICE="device_id=$DEVICE_ID unit_id=1 interface=IF_RS485_1 enabled=true baud=38400 parity=PAR_NONE"
    POINTS=(
      "point_id=204 device_ref=$DEVICE_ID reg_type=REG_HOLDING address=40000 data_type=DT_U16 scale=1 writable=true  poll_class=POLL_NORMAL name=hvac-power"
      "point_id=202 device_ref=$DEVICE_ID reg_type=REG_HOLDING address=40001 data_type=DT_U16 scale=1 writable=true  poll_class=POLL_FAST   name=hvac-setpoint"
      "point_id=205 device_ref=$DEVICE_ID reg_type=REG_HOLDING address=40002 data_type=DT_U16 scale=1 writable=false poll_class=POLL_NORMAL name=hvac-room-temp"
      "point_id=203 device_ref=$DEVICE_ID reg_type=REG_HOLDING address=40007 data_type=DT_U16 scale=1 writable=false poll_class=POLL_NORMAL name=hvac-conn"
    )
    WRITE="point_id=202 value=220"          # 22.0 °C — the register is in 0.1 °C steps
    NOTE="Setpoint is 0.1 °C: 220 means 22.0. When hvac-conn reads 0 the AC is not attached and
every other HVAC register reads 0 as well — that is the slave's state, not a transport fault."
    ;;

  # --- Modbus: ZC-Damper, 10 damper actuators ---------------------------------------------
  # ACB-M/docs/Modbus-Module/zc-damper-control-test — slave 11, 38400-8-N-1.
  # Its callback compares internal numbers directly, so console = internal and PDU = internal-1.
  modbus:zcd)
    : "${DEVICE_ID:=11}"
    DEVICE="device_id=$DEVICE_ID unit_id=11 interface=IF_RS485_1 enabled=true baud=38400 parity=PAR_NONE"
    POINTS=(
      "point_id=201 device_ref=$DEVICE_ID reg_type=REG_HOLDING address=999   data_type=DT_U16 scale=1 writable=false poll_class=POLL_FAST   name=zcd-alive"
      "point_id=210 device_ref=$DEVICE_ID reg_type=REG_INPUT   address=999   data_type=DT_U16 scale=1 writable=false poll_class=POLL_NORMAL name=zcd-pos-0"
      "point_id=211 device_ref=$DEVICE_ID reg_type=REG_INPUT   address=1099  data_type=DT_U16 scale=1 writable=false poll_class=POLL_NORMAL name=zcd-state-0"
      "point_id=212 device_ref=$DEVICE_ID reg_type=REG_HOLDING address=42000 data_type=DT_U16 scale=1 writable=true  poll_class=POLL_NORMAL name=zcd-target-0"
    )
    WRITE="point_id=212 value=50"
    NOTE="zcd-alive must read 53521 (0xD111); anything else means the address convention is wrong,
not that the damper is broken. A damper takes ~15 s for full stroke, so read the position back
after a wait — and after a board reboot it homes all ten sequentially for 2.5-5 minutes first."
    ;;

  # --- Modbus: DDM18SD energy meter -------------------------------------------------------
  # ACB-M/docs/Modbus-Module/ddm18sd-energy-meter-test — 9600-8-E-1, EVEN parity is mandatory:
  # wrong parity is total silence. Values are IEEE-754 floats across two registers, high word
  # first, addressed 0-based on the PDU.
  modbus:ddm18sd)
    : "${DEVICE_ID:=2}"
    DEVICE="device_id=$DEVICE_ID unit_id=1 interface=IF_RS485_1 enabled=true baud=9600 parity=PAR_EVEN"
    POINTS=(
      "point_id=220 device_ref=$DEVICE_ID reg_type=REG_INPUT address=0    data_type=DT_F32 scale=1 writable=false poll_class=POLL_NORMAL name=ddm-voltage"
      "point_id=221 device_ref=$DEVICE_ID reg_type=REG_INPUT address=6    data_type=DT_F32 scale=1 writable=false poll_class=POLL_NORMAL name=ddm-current"
      "point_id=222 device_ref=$DEVICE_ID reg_type=REG_INPUT address=12   data_type=DT_F32 scale=1 writable=false poll_class=POLL_NORMAL name=ddm-power"
      "point_id=223 device_ref=$DEVICE_ID reg_type=REG_INPUT address=70   data_type=DT_F32 scale=1 writable=false poll_class=POLL_NORMAL name=ddm-frequency"
    )
    WRITE=""    # the meter needs FC 0x10 to change anything; this path is read-only
    NOTE="This meter is the reason per-device baud exists. Over the console the whole RS485 port
had to be switched to 9600-8-E-1 and back, so the meter could not share the bus with the
ZC-Damper at 38400-8-N-1. Here baud and parity travel WITH the device and the board applies
them before each transaction, so both can sit on one bus, time-multiplexed. Note the meter and
the HVAC board support both answer to unit 1 — they cannot be wired at the same time."
    ;;

  # --- BACnet MS/TP IO card ---------------------------------------------------------------
  # ACB-M/docs/BACnet-Module — the MAC is set by DIP switch on the card; pass --mac.
  bacnet:*)
    : "${DEVICE_ID:=1}"
    DEVICE="device_id=$DEVICE_ID mac_addr=$MAC enabled=true"
    POINTS=(
      "point_id=301 device_ref=$DEVICE_ID obj_type=OBJ_AV obj_instance=1 scale=1 writable=true  poll_class=POLL_NORMAL name=bac-av-1"
      "point_id=302 device_ref=$DEVICE_ID obj_type=OBJ_AI obj_instance=1 scale=1 writable=false poll_class=POLL_NORMAL name=bac-ai-1"
      "point_id=303 device_ref=$DEVICE_ID obj_type=OBJ_BV obj_instance=1 scale=1 writable=true  poll_class=POLL_NORMAL name=bac-bv-1"
    )
    WRITE="point_id=301 value=50"
    NOTE="There is no discovery — the master never sends Who-Is and synthesises an address-cache
entry from mac_addr alone, so a wrong DIP setting is a permanently faulted point, not an error.
WRITES ARE PERMANENT: WriteProperty goes out at priority 1 and is never relinquished, so the
first value written seizes that slot on the target until it reboots. Writes to AI and BI are
rejected whatever `writable` says, because the master decides from the object type."
    ;;

  # --- LoRa Droplet -----------------------------------------------------------------------
  lora:*)
    : "${DEVICE_ID:=1}"
    DEVICE="device_id=$DEVICE_ID dev_addr=0x6CC06351 enabled=true"
    POINTS=(
      "point_id=401 device_ref=$DEVICE_ID field=FIELD_TEMPERATURE scale=1 writable=false name=lora-temp"
      "point_id=402 device_ref=$DEVICE_ID field=FIELD_HUMIDITY    scale=1 writable=false name=lora-rh"
      "point_id=403 device_ref=$DEVICE_ID field=FIELD_BATTERY     scale=1 writable=false name=lora-batt"
      "point_id=404 device_ref=$DEVICE_ID field=FIELD_RSSI        scale=1 writable=false name=lora-rssi"
    )
    WRITE="point_id=401 value=1"    # expected to be refused: the bus is receive-only
    NOTE="The LoRa bus is PUSH, not poll: a Droplet broadcasts every few minutes and sleeps, so a
freshly provisioned point stays faulted until it transmits — use a long --sub-secs. dev_addr is
printed by the sensor at boot; a wrong one never faults visibly, it simply never updates. Every
write is refused WR_NOT_WRITABLE until actuator channels land, which is what this one tests."
    ;;

  modbus:default)
    echo "modbus needs a device profile: --device hvac | zcd | ddm18sd" >&2
    exit 2 ;;
  *)
    echo "protocol must be modbus, bacnet or lora" >&2
    exit 2 ;;
esac

say() { printf '\n\033[1m=== %s\033[0m\n' "$*"; }
say "$PROTO / ${PROFILE:-default} — provisioning, reading, writing"
[ -n "$NOTE" ] && printf '%s\n' "$NOTE"

{
    echo "login $PASSWORD"

    echo "config $PROTO read"                       # before: what is already there
    echo "config $PROTO add-device $DEVICE"
    for p in "${POINTS[@]}"; do echo "config $PROTO add-point $p"; done
    echo "config $PROTO read"                       # after: the board's own snapshot

    echo "points $PROTO read"                       # every point
    [ -n "$WRITE" ] && echo "points $PROTO write $WRITE"
    echo "points $PROTO read"                       # read-back after the write

    echo "points $PROTO sub $SUB_SECS"              # the COV stream

    if [ "$CLEAN" = "1" ]; then
        for p in "${POINTS[@]}"; do
            id="${p#point_id=}"; id="${id%% *}"
            echo "config $PROTO del-point $id"
        done
        echo "config $PROTO del-device $DEVICE_ID"
    fi
    echo "quit"
} | "$APP" "${ARGS[@]}"

cat <<'EOF'

=== how to read the result

  applied_ids: [N]        the board accepted that device or point
  rejected: [{id,reason}] it did not — REJ_BAD_ID (outside the middleware id range),
                          REJ_UNKNOWN_REF (point names a device that does not exist),
                          REJ_POOL_FULL, REJ_IFACE_ROLE (the port is not a master),
                          REJ_DUPLICATE, REJ_INVALID
  quality: "Q_GOOD"       the field bus answered
  quality: "Q_FAULT"      provisioned, but nothing answered: wrong unit id / MAC / dev_addr,
                          wrong register, or a line rate the slave does not use
  accepted: true          the write reached the device cache. NOT bus confirmation — the
                          authoritative confirmation is the value reading back
  reason: WR_NOT_WRITABLE the point was provisioned without writable=true, or the protocol
                          refuses writes to that kind of point at all
  no "value" field        the value is zero. proto3 does not transmit defaults, so a register
                          reading 0 shows as an absent field, NOT as a missing reply
  no payload at all       an empty result set, which is a valid answer
EOF
