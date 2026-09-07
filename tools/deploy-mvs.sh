#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

DRY_RUN=0
if [ "${1:-}" = "--dry-run" ] || [ "${1:-}" = "-n" ]; then
	DRY_RUN=1
fi

ENV_FILE="${DBASE_ENV_FILE:-${ROOT_DIR}/.env}"
if [ ! -f "$ENV_FILE" ]; then
	echo "ERROR: ${ENV_FILE} not found. Copy .env.example to .env and edit it." >&2
	exit 1
fi

set -a
. "$ENV_FILE"
set +a

MVS_HOST="${MBT_MVS_HOST:-hercules}"
MVS_PORT="${MBT_MVS_PORT:-1080}"
MVS_USER="${MBT_MVS_USER:-RVEZ001}"
MVS_PASS="${MBT_MVS_PASS:-}"
MVS_PROTOCOL="${MBT_MVS_PROTOCOL:-http}"
MVS_REJECT_UNAUTHORIZED="${MBT_MVS_REJECT_UNAUTHORIZED:-false}"
MVS_HLQ="${MBT_MVS_HLQ:-IBMUSER}"

DBASE_PDS="${DBASE_PDS:-${MVS_HLQ}.DBASE}"
DBASE_LOADLIB="${DBASE_LOADLIB:-${DBASE_PDS}.LOAD}"
DBASE_STORE="${DBASE_STORE:-${DBASE_PDS}.KV}"
DBASE_XMIT_IN="${DBASE_XMIT_IN:-${MVS_HLQ}.MBT.XMIT.IN}"
DBASE_LOAD_VOLUME="${DBASE_LOAD_VOLUME:-TSO003}"
DBASE_CMDPROC="${DBASE_CMDPROC:-SYS2.CMDPROC}"

if [ -n "${DBASE_TOOLCHAIN_BIN:-}" ]; then
	export PATH="${DBASE_TOOLCHAIN_BIN}:${PATH}"
fi

if [ -z "$MVS_HOST" ]; then
	echo "ERROR: set MBT_MVS_HOST in .env" >&2
	exit 1
fi
if [ "$DRY_RUN" != "1" ] && [ -z "$MVS_PASS" ]; then
	echo "ERROR: set MBT_MVS_PASS in .env" >&2
	exit 1
fi

ZOWE_CONN=(--host "$MVS_HOST" --port "$MVS_PORT" --user "$MVS_USER"
	--password "$MVS_PASS" --protocol "$MVS_PROTOCOL"
	--reject-unauthorized "$MVS_REJECT_UNAUTHORIZED")

run_cmd() {
	echo "+ $*"
	if [ "$DRY_RUN" = "1" ]; then
		return 0
	fi
	"$@"
}

try_cmd() {
	echo "+ $*"
	if [ "$DRY_RUN" = "1" ]; then
		return 0
	fi
	"$@" || true
}

upload_member() {
	local src="$1"
	local dsn="$2"
	local member="$3"
	echo "+ upload ${src} -> ${dsn}(${member})"
	if [ "$DRY_RUN" = "1" ]; then
		return 0
	fi
	zowe zos-files upload stdin-to-data-set "${dsn}(${member})" \
		"${ZOWE_CONN[@]}" < "$src"
}

receive_loadlib() {
	local jcl

	echo "+ upload dist/dbase-tso-0.1.0-load.xmit -> ${DBASE_XMIT_IN}"
	if [ "$DRY_RUN" = "1" ]; then
		echo "+ submit RECEIVE ${DBASE_XMIT_IN} -> ${DBASE_LOADLIB}"
		return 0
	fi
	zowe zos-files upload file-to-data-set dist/dbase-tso-0.1.0-load.xmit \
		"$DBASE_XMIT_IN" --binary "${ZOWE_CONN[@]}"

	jcl="$(mktemp /tmp/dbase-receive.XXXXXX.jcl)"
	sed \
		-e "s/IBMUSER.MBT.XMIT.IN/${DBASE_XMIT_IN}/g" \
		-e "s/IBMUSER.DBASE.LOAD/${DBASE_LOADLIB}/g" \
		-e "s/TSO003/${DBASE_LOAD_VOLUME}/g" \
		jcl/RECEIVE.jcl > "$jcl"
	echo "+ submit ${jcl}"
	zowe zos-jobs submit local-file "$jcl" --wait-for-output "${ZOWE_CONN[@]}"
	rm -f "$jcl"
}

alloc_store() {
	local jcl

	jcl="$(mktemp /tmp/dbase-alloc.XXXXXX.jcl)"
	sed \
		-e "s/IBMUSER.DBASE.KV/${DBASE_STORE}/g" \
		-e "s/TSO003/${DBASE_LOAD_VOLUME}/g" \
		jcl/ALLOCVS.jcl > "$jcl"
	echo "+ submit ${jcl}"
	if [ "$DRY_RUN" != "1" ]; then
		zowe zos-jobs submit local-file "$jcl" --wait-for-output "${ZOWE_CONN[@]}"
	fi
	rm -f "$jcl"
}

echo "Deploy host: ${MVS_PROTOCOL}://${MVS_HOST}:${MVS_PORT} as ${MVS_USER}"
echo "Loadlib:     ${DBASE_LOADLIB}"
echo "Store:       ${DBASE_STORE}"
echo "Source PDS:  ${DBASE_PDS}"
echo "CLIST:       ${DBASE_CMDPROC}(DBASE)"
[ "$DRY_RUN" = "1" ] && echo "(dry run: no MVS changes)"

echo "+ make package"
make package

try_cmd zowe zos-files create data-set-partitioned "$DBASE_PDS" --size 60TRK "${ZOWE_CONN[@]}"
try_cmd zowe zos-files create data-set-partitioned "$DBASE_LOADLIB" --size 15TRK --record-format U --record-length 0 --block-size 6144 "${ZOWE_CONN[@]}"
try_cmd zowe zos-files create data-set-sequential "$DBASE_XMIT_IN" --size 20TRK --record-format FB --record-length 80 --block-size 3120 "${ZOWE_CONN[@]}"
alloc_store
receive_loadlib

upload_member src/dbase.c "$DBASE_PDS" DBASEC
upload_member src/dbasetso.c "$DBASE_PDS" DBASETC
upload_member asm/dbtget.asm "$DBASE_PDS" DBTGET
upload_member asm/dbtput.asm "$DBASE_PDS" DBTPUT
upload_member project.toml "$DBASE_PDS" PROJTOML
upload_member jcl/ALLOCVS.jcl "$DBASE_PDS" ALLOCVS
upload_member jcl/DBASE.jcl "$DBASE_PDS" RUNJCL
upload_member jcl/RECEIVE.jcl "$DBASE_PDS" RECEIVE
upload_member scripts/TESTDO.txt "$DBASE_PDS" SCRIPT
upload_member README.md "$DBASE_PDS" README
upload_member clist/DBASE.clist "$DBASE_CMDPROC" DBASE

echo "Deploy complete."
