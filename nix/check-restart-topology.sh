# Verify Root's restart topology in a synthesised image.
#
# Usage: check-restart-topology <report.txt> <system.sdf> <microkit.h> \
#          <runtime-abi.json> production|restart
#
# The generated SDF says what we asked for; the Microkit tool's report.txt says
# what it built. This checks the second against runtime-abi.json and the first:
# which PDs fault to Root, at what entry and priority, which TCB caps Root
# holds, and which notification caps connect Root to its peers.
#
# The Microkit manual calls report.txt human-readable only, with no stable
# format. This parser is written against the Microkit 2.3.0 layout and fails
# closed: a missing section, block or field is reported as a layout change
# rather than skipped, so a format drift breaks the build instead of passing
# vacuously. Cap slot bases come from the SDK's own microkit.h.

usage() {
  echo "usage: check-restart-topology <report.txt> <system.sdf> <microkit.h> <runtime-abi.json> production|restart" >&2
  exit 2
}

[ "$#" -eq 5 ] || usage
report=$1
sdf=$2
header=$3
abi=$4
mode=$5
case $mode in
production | restart) ;;
*) usage ;;
esac

fail() {
  echo "restart-topology ($mode): $*" >&2
  exit 1
}

layout_changed() {
  fail "report.txt layout changed (expected the Microkit 2.3.0 format): $*"
}

for heading in '# TCB Details' '# CNode Details'; do
  grep -qxF "$heading" "$report" || layout_changed "no '$heading' section"
done

# A numeric #define from microkit.h.
define_of() {
  local value
  value=$(awk -v name="$1" '
    $1 == "#define" && $2 == name && !found { print $3; found = 1 }
    END { exit(found ? 0 : 1) }
  ' "$header") || fail "$header does not define $1"
  [[ $value =~ ^[0-9]+$ ]] || fail "$header defines $1 as '$value', not a number"
  echo "$value"
}
base_tcb=$(define_of BASE_TCB_CAP)
base_notify=$(define_of BASE_OUTPUT_NOTIFICATION_CAP)

# The lines of one report block: "TCB tcb_x" or "CNode cnode_x". A block ends
# at the next block or section heading.
block() {
  awk -v head=$'\t'"- $1: '$2'" '
    $0 == head { inside = 1; found = 1; next }
    inside && (/^\t- / || /^#/) { exit }
    inside { print }
    END { exit(found ? 0 : 1) }
  ' "$report"
}

# One "* Field: value" or "-> Field: value" line of a TCB block.
tcb_field() {
  local lines
  lines=$(block TCB "$1") || layout_changed "no TCB block for $1"
  awk -v field="$2" '
    { line = $0; sub(/^[ \t]*(\* |-> )/, "", line) }
    index(line, field ": ") == 1 && !found {
      print substr(line, length(field) + 3)
      found = 1
    }
    END { exit(found ? 0 : 1) }
  ' <<<"$lines"
}

# A CNode block as "slot object badge" rows, with "-" for a missing badge.
cnode_table() {
  local lines table
  lines=$(block CNode "$1") || layout_changed "no CNode block for $1"
  table=$(awk '
    function flush() {
      if (slot != "") print slot, (object == "" ? "-" : object), (badge == "" ? "-" : badge)
    }
    { line = $0; sub(/^[ \t]*(\* |-> )/, "", line) }
    line ~ /^Slot: / { flush(); slot = substr(line, 7); object = ""; badge = ""; next }
    line ~ /^Object: / { object = substr(line, 9); gsub(/\047/, "", object) }
    line ~ /^Badge: / { badge = substr(line, 8) }
    END { flush() }
  ' <<<"$lines")
  [ -n "$table" ] || layout_changed "CNode $1 lists no slots"
  echo "$table"
}

# "object badge" for one slot of a CNode, or nothing when the slot is empty.
cnode_slot() {
  cnode_table "$1" | awk -v slot="$2" '$1 == slot { print $2, $3 }'
}

# One attribute of a <protection_domain> element in the SDF.
sdf_attr() {
  awk -v name="$1" -v attr="$2" '
    index($0, "<protection_domain name=\"" name "\"") && !found {
      found = 1
      if (match($0, " " attr "=\"[^\"]*\"")) {
        print substr($0, RSTART + length(attr) + 3, RLENGTH - length(attr) - 4)
        matched = 1
      }
    }
    END { exit(matched ? 0 : 1) }
  ' "$sdf"
}

# Root's children, by PD name, from runtime-abi.json. The crasher exists only
# in the restart image.
declare -A children=()
while read -r name id; do
  children[$name]=$id
done < <(jq -r '.drivers[] | "\(.name)_driver \(.child)"' "$abi")
children[beam_server]=$(jq -r '.children.beam' "$abi")
if [ "$mode" = restart ]; then
  children[crasher]=$(jq -r '.children.crasher' "$abi")
fi
entry=$(jq -r '.restart.entry_fallback' "$abi")

root_priority=$(tcb_field tcb_root Priority) || layout_changed "tcb_root has no Priority"
[[ $root_priority =~ ^[0-9]+$ ]] || layout_changed "tcb_root Priority is '$root_priority'"
sdf_root_priority=$(sdf_attr root priority) || fail "the SDF gives root no priority"
[ "$sdf_root_priority" = "$root_priority" ] ||
  fail "root has priority $root_priority in report.txt but $sdf_root_priority in the SDF"

for name in "${!children[@]}"; do
  id=${children[$name]}
  tcb=tcb_$name

  sdf_id=$(sdf_attr "$name" id) || fail "the SDF has no child PD $name"
  [ "$sdf_id" = "$id" ] || fail "$name has id $sdf_id in the SDF, $id in the ABI"

  # Root must be able to preempt a faulting child to handle it.
  priority=$(tcb_field "$tcb" Priority) || layout_changed "$tcb has no Priority"
  [[ $priority =~ ^[0-9]+$ ]] || layout_changed "$tcb Priority is '$priority'"
  [ "$(sdf_attr "$name" priority)" = "$priority" ] ||
    fail "$name priority in report.txt ($priority) differs from the SDF"
  ((priority < root_priority)) ||
    fail "$name priority $priority is not below root's $root_priority"

  # Microkit delivers a child's faults to its parent's endpoint.
  fault_ep=$(tcb_field "$tcb" "Fault Endpoint") || fail "$tcb has no fault endpoint"
  [ "$fault_ep" = "'ep_root'" ] || fail "$tcb faults to $fault_ep, not ep_root"

  # Every child boots at the shared ELF entry Root restarts drivers at.
  ip=$(tcb_field "$tcb" IP) || layout_changed "$tcb has no IP"
  [[ $ip =~ ^0x[0-9a-fA-F]+$ ]] || layout_changed "$tcb IP is '$ip'"
  ((ip == entry)) || fail "$tcb starts at $ip, not the ABI entry $entry"

  # microkit_pd_restart and microkit_pd_stop invoke BASE_TCB_CAP + id.
  read -r object _ <<<"$(cnode_slot cnode_root $((base_tcb + id)))"
  [ "${object:-}" = "$tcb" ] ||
    fail "cnode_root slot $((base_tcb + id)) holds '${object:-nothing}', not $tcb"

  # The child's fault endpoint cap carries its id in the badge's low byte.
  fault_caps=$(cnode_table "cnode_$name" | awk '$2 == "ep_root"')
  [ "$(wc -l <<<"$fault_caps")" -eq 1 ] && [ -n "$fault_caps" ] ||
    fail "cnode_$name does not hold exactly one ep_root cap"
  read -r _ _ badge <<<"$fault_caps"
  [[ $badge =~ ^0x[0-9a-fA-F]+$ ]] || layout_changed "cnode_$name ep_root badge is '$badge'"
  (((badge & 0xff) == id)) || fail "cnode_$name ep_root badge $badge does not carry id $id"
done

# No other PD faults to Root.
while read -r tcb; do
  name=${tcb#tcb_}
  [ "$name" = root ] && continue
  [ -n "${children[$name]+set}" ] && continue
  fault_ep=$(tcb_field "$tcb" "Fault Endpoint" || true)
  [ "$fault_ep" != "'ep_root'" ] || fail "$tcb faults to ep_root but is not a Root child"
done < <(awk -F"'" '/^\t- TCB: / { print $2 }' "$report")

# Root holds TCB caps for exactly its children.
tcb_caps=$(cnode_table cnode_root | awk '$2 ~ /^tcb_/' | wc -l)
[ "$tcb_caps" -eq "${#children[@]}" ] ||
  fail "cnode_root holds $tcb_caps TCB caps for ${#children[@]} children"

# The production give-up channel: root -> blk_virt.
gone_channel=$(jq -r '.giveup.root_blk_channel' "$abi")
read -r object _ <<<"$(cnode_slot cnode_root $((base_notify + gone_channel)))"
[ "${object:-}" = ntfn_blk_virt ] ||
  fail "cnode_root slot $((base_notify + gone_channel)) holds '${object:-nothing}', not ntfn_blk_virt"

# beam_server -> root restart channels: every one in the restart image, none
# in production.
root_caps=$(cnode_table cnode_beam_server | awk '$2 == "ntfn_root"')
if [ "$mode" = production ]; then
  [ -z "$root_caps" ] || fail "production beam_server holds notification caps to root"
else
  expected=0
  while read -r from to; do
    slot=$((base_notify + from))
    read -r object badge <<<"$(cnode_slot cnode_beam_server "$slot")"
    [ "${object:-}" = ntfn_root ] ||
      fail "cnode_beam_server slot $slot holds '${object:-nothing}', not ntfn_root"
    [[ ${badge:-} =~ ^0x[0-9a-fA-F]+$ ]] || layout_changed "cnode_beam_server slot $slot badge is '${badge:-}'"
    ((badge == 1 << to)) || fail "beam_server channel $from signals root with badge $badge, not channel $to"
    expected=$((expected + 1))
  done < <(jq -r '.drivers[] | "\(.beam_debug_channel) \(.root_debug_channel)", "\(.beam_fault_channel) \(.root_fault_channel)"' "$abi")
  [ "$(wc -l <<<"$root_caps")" -eq "$expected" ] ||
    fail "beam_server holds $(wc -l <<<"$root_caps") notification caps to root, expected $expected"
fi

echo "restart-topology ($mode): ${#children[@]} children, entries, priorities, cap slots and channels verified"
