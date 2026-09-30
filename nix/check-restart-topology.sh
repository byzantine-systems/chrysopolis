# Verify Root's restart topology in a synthesised image.
#
# Usage: check-restart-topology <report.txt> <system.sdf> <microkit.h> \
#          <system-abi.json> production|restart
#
# The generated SDF says what we asked for; the Microkit tool's report.txt says
# what it built. This checks the second against system-abi.json and the first:
# which PDs fault to Root, at what entry and priority, which TCB caps Root
# holds, which notification caps connect Root to its peers, and the control
# plane: the one BEAM -> Root PPC cap (slot, object and badge in beam_server's
# CNode), the absence of any signaling or PPC cap in Root's CNode, and the
# one-writer rights of the root_status and orchestrator_spec maps in the SDF.
#
# The Microkit manual calls report.txt human-readable only, with no stable
# format. This parser is written against the Microkit 2.3.0 layout and fails
# closed: a missing section, block or field is reported as a layout change
# rather than skipped, so a format drift breaks the build instead of passing
# vacuously. Cap slot bases come from the SDK's own microkit.h.

usage() {
  echo "usage: check-restart-topology <report.txt> <system.sdf> <microkit.h> <system-abi.json> production|restart" >&2
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
base_endpoint=$(define_of BASE_ENDPOINT_CAP)

# Badge classes the Microkit 2.3.0 tool mints. microkit.h does not name them:
# bit 62 marks a child fault delivered on the parent's ep_root, bit 63 marks a
# protected procedure call, and the low bits carry the child id (fault)
# or the callee's channel (PPC). The timer client PPC already in this image
# (badge 0x8000000000000001) pins the layout these constants parse. Bash hex
# literals at bit 63 wrap negative; every comparison below is bitwise, so the
# wrap is harmless.
fault_badge_bit=0x4000000000000000
ppc_badge_bit=0x8000000000000000

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

# The size="0x..." of one <memory_region> in the SDF.
mr_size() {
  awk -v name="name=\"$1\" " '
    index($0, "<memory_region ") && index($0, name) && !found {
      found = 1
      if (match($0, /size="0x[0-9a-f]+"/)) {
        print substr($0, RSTART + 6, RLENGTH - 7)
        matched = 1
      }
    }
    END { exit(matched ? 0 : 1) }
  ' "$sdf"
}

# The <map> lines belonging DIRECTLY to one protection_domain element. PD
# elements nest (beam_server sits inside root), so this counts real depth and
# keeps a child's maps out of its parent's block; production-sdf-gate uses the
# same technique for the nesting check.
pd_maps() {
  awk -v name="$1" '
    /<protection_domain / {
      depth++
      if (!inside && index($0, "name=\"" name "\"")) { inside = depth; next }
    }
    inside && depth == inside && /<map / { print }
    /<\/protection_domain>/ {
      if (inside && depth == inside) exit
      depth--
    }
  ' "$sdf"
}

# One attribute of the <map mr="..."> line of one PD. Fails when the PD maps
# the region without the attribute at all.
map_attr() {
  pd_maps "$1" | awk -v mr="mr=\"$2\" " -v attr="$3" '
    index($0, "<map ") && index($0, mr) && !found {
      found = 1
      if (match($0, " " attr "=\"[^\"]*\"")) {
        print substr($0, RSTART + length(attr) + 3, RLENGTH - length(attr) - 4)
        matched = 1
      }
    }
    END { exit(matched ? 0 : 1) }
  '
}

# Root's children, by PD name, from system-abi.json. The crasher exists only
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
  # beam_server holds exactly one more ep_root cap: the control PPC cap, verified
  # in the control-plane section below. Every other child holds exactly one.
  fault_caps=$(cnode_table "cnode_$name" | awk '$2 == "ep_root"')
  expected_eps=1
  [ "$name" = beam_server ] && expected_eps=2
  [ "$(wc -l <<<"$fault_caps")" -eq "$expected_eps" ] && [ -n "$fault_caps" ] ||
    fail "cnode_$name does not hold exactly $expected_eps ep_root cap(s)"
  fault_badge_found=0
  while read -r slot _ badge; do
    [[ $badge =~ ^0x[0-9a-fA-F]+$ ]] || layout_changed "cnode_$name ep_root badge is '$badge'"
    if ((slot == 2 && badge == (fault_badge_bit | id))); then
      fault_badge_found=1
    fi
  done <<<"$fault_caps"
  [ "$fault_badge_found" -eq 1 ] ||
    fail "cnode_$name holds no fault-badged ep_root cap at slot 2 carrying id $id"
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

# --- Root control plane -----------------------------------------------------
# The transport is asymmetric by design: one PPC channel (beam_server calls,
# Root serves on the ep_root it already owns) and two one-writer memory
# regions. The PPC endpoint cap lives only in the CALLER's CNode, so every
# control-plane capability Root gains here must be zero; report.txt proves
# that, and the SDF proves the map rights report.txt does not carry.
pp_root=$(jq -r '.control.pp_channel.root' "$abi")
pp_beam=$(jq -r '.control.pp_channel.beam' "$abi")
status_size=$(jq -r '.control.status.size' "$abi")
spec_size=$(jq -r '.control.spec.size' "$abi")

# beam_server holds the PPC cap at the endpoint slot for its channel id,
# badged with the PPC bit plus Root's channel id.
ppc_slot=$((base_endpoint + pp_beam))
read -r object badge <<<"$(cnode_slot cnode_beam_server "$ppc_slot")"
[ "${object:-}" = ep_root ] ||
  fail "cnode_beam_server slot $ppc_slot holds '${object:-nothing}', not the PPC endpoint cap"
[[ ${badge:-} =~ ^0x[0-9a-fA-F]+$ ]] || layout_changed "cnode_beam_server PPC badge is '${badge:-}'"
((badge == (ppc_badge_bit | pp_root))) ||
  fail "cnode_beam_server PPC badge $badge is not PPC|$pp_root"

# No other PD may call Root through a PPC endpoint. Fault caps also name
# ep_root, but use the distinct fault badge bit; the only PPC-badged ep_root
# across every CNode must be the BEAM cap checked above.
ppc_caps=0
while read -r cnode; do
  while read -r slot cap cap_badge; do
    [ "$cap" = ep_root ] || continue
    [[ $cap_badge =~ ^0x[0-9a-fA-F]+$ ]] || layout_changed "$cnode slot $slot ep_root badge is '$cap_badge'"
    if ((cap_badge & ppc_badge_bit)); then
      if [ "$cnode" != cnode_beam_server ] ||
         ((slot != ppc_slot || cap_badge != (ppc_badge_bit | pp_root))); then
        fail "$cnode slot $slot holds an unauthorized PPC cap to Root"
      fi
      ppc_caps=$((ppc_caps + 1))
    fi
  done < <(cnode_table "$cnode")
done < <(awk -F"'" '/^\t- CNode: / { print $2 }' "$report")
[ "$ppc_caps" -eq 1 ] || fail "found $ppc_caps PPC caps to Root, expected exactly one"

# Root gains no capability for the control plane: no signaling cap to
# beam_server of any kind, and no PPC-badged cap anywhere in its CNode.
root_leaks=$(cnode_table cnode_root | awk '$2 == "ntfn_beam_server" || $2 == "ep_beam_server"')
[ -z "$root_leaks" ] ||
  fail "cnode_root holds a signaling cap to beam_server: $root_leaks"
while read -r slot _ rbadge; do
  [ "$rbadge" = "-" ] && continue
  [[ $rbadge =~ ^0x[0-9a-fA-F]+$ ]] || layout_changed "cnode_root slot $slot badge is '$rbadge'"
  (((rbadge & ppc_badge_bit) == 0)) ||
    fail "cnode_root slot $slot carries the PPC badge $rbadge; the cap belongs in the caller's CNode"
done < <(cnode_table cnode_root)

# In this pinned topology, the only Root-held slots besides its own entry,
# VSpace and reply caps are the give-up notification and one TCB per declared
# child. Check the complete set so an unexpected cap cannot escape the
# narrower "no cap to beam_server" predicate above.
while read -r slot object _; do
  case $slot in
    1) expected_object=ep_root ;;
    2) expected_object=ep_fault_monitor ;;
    3) expected_object=pud_root ;;
    4) expected_object=reply_root ;;
    "$((base_notify + gone_channel))") expected_object=ntfn_blk_virt ;;
    *)
      expected_object=
      for name in "${!children[@]}"; do
        if ((slot == base_tcb + children[$name])); then
          expected_object=tcb_$name
          break
        fi
      done
      ;;
  esac
  [ -n "$expected_object" ] && [ "$object" = "$expected_object" ] ||
    fail "cnode_root slot $slot holds unexpected cap $object"
done < <(cnode_table cnode_root)

# One-writer map rights, read from the SDF the tool consumed (report.txt has
# no mapping section). Each map must sit at the ABI vaddr with the ABI setvar
# symbol, and neither may set a cache attribute: the ABI requires both aliases
# cacheable, and one side pinning cached="false" while the other keeps the
# default would alias one region under two memory attributes.
check_control_map() {
  local pd=$1 mr=$2 perms=$3 vaddr=$4 setvar=$5
  local mapped
  mapped=$(pd_maps "$pd" | awk -v name="mr=\"$mr\" " 'index($0, "<map ") && index($0, name) { n++ } END { print n+0 }')
  [ "$mapped" -eq 1 ] || fail "$pd has $mapped maps of $mr, expected exactly one"
  [ "$(map_attr "$pd" "$mr" perms)" = "$perms" ] ||
    fail "$pd maps $mr with perms '$(map_attr "$pd" "$mr" perms)', expected $perms"
  [ "$(map_attr "$pd" "$mr" vaddr)" = "$vaddr" ] ||
    fail "$pd maps $mr at '$(map_attr "$pd" "$mr" vaddr)', expected $vaddr"
  [ "$(map_attr "$pd" "$mr" setvar_vaddr)" = "$setvar" ] ||
    fail "$pd maps $mr with setvar '$(map_attr "$pd" "$mr" setvar_vaddr)', expected $setvar"
  if map_attr "$pd" "$mr" cached >/dev/null; then
    fail "$pd's $mr map sets a cache attribute; both aliases must keep the default"
  fi
}

status_root_vaddr=$(printf '0x%x' "$(jq -r '.control.status.root_vaddr' "$abi")")
status_beam_vaddr=$(printf '0x%x' "$(jq -r '.control.status.beam_vaddr' "$abi")")
spec_root_vaddr=$(printf '0x%x' "$(jq -r '.control.spec.root_vaddr' "$abi")")
spec_beam_vaddr=$(printf '0x%x' "$(jq -r '.control.spec.beam_vaddr' "$abi")")
check_control_map root root_status rw "$status_root_vaddr" "$(jq -r '.control.status.root_setvar' "$abi")"
check_control_map beam_server root_status r "$status_beam_vaddr" "$(jq -r '.control.status.beam_setvar' "$abi")"
check_control_map beam_server orchestrator_spec rw "$spec_beam_vaddr" "$(jq -r '.control.spec.beam_setvar' "$abi")"
check_control_map root orchestrator_spec r "$spec_root_vaddr" "$(jq -r '.control.spec.root_setvar' "$abi")"
for mr in root_status orchestrator_spec; do
  [ "$(grep -Fc "<memory_region name=\"$mr\" " "$sdf")" -eq 1 ] ||
    fail "the SDF must declare exactly one $mr memory region"
  [ "$(grep -Fc "<map mr=\"$mr\" " "$sdf")" -eq 2 ] ||
    fail "$mr must have exactly the Root and BEAM mappings"
done

status_size_hex=$(printf '0x%x' "$status_size")
spec_size_hex=$(printf '0x%x' "$spec_size")
[ "$(mr_size root_status)" = "$status_size_hex" ] ||
  fail "root_status is $(mr_size root_status) bytes in the SDF, expected $status_size_hex"
[ "$(mr_size orchestrator_spec)" = "$spec_size_hex" ] ||
  fail "orchestrator_spec is $(mr_size orchestrator_spec) bytes in the SDF, expected $spec_size_hex"

# The PPC channel itself: both ends pinned from the ABI, pp on the beam end
# only, and notify off on both ends.
ppc_channel=$(awk '
  /<channel>/   { inside = 1; block = "" }
  inside        { block = block $0 "\n" }
  /<\/channel>/ {
    inside = 0
    if (index(block, "pd=\"beam_server\"") && index(block, "pd=\"root\"") && index(block, "pp=\"true\""))
      { printf "%s", block; found = 1 }
  }
  END           { exit(found ? 0 : 1) }
' "$sdf") || fail "the SDF has no beam_server -> root pp channel"
# Attribute order on a channel end line is the renderer's choice, so match
# each end as a line that carries all three of its required attributes.
awk -v id="$pp_beam" '
  /pd="beam_server"/ {
    ok = index($0, "id=\"" id "\"") && index($0, "pp=\"true\"") && index($0, "notify=\"false\"")
    seen = 1
    exit
  }
  END { exit(seen && ok ? 0 : 1) }
' <<<"$ppc_channel" || fail "the PPC channel's beam end is not id $pp_beam with pp and no notify"
awk -v id="$pp_root" '
  /pd="root"/ {
    ok = index($0, "id=\"" id "\"") && index($0, "notify=\"false\"")
    seen = 1
    exit
  }
  END { exit(seen && ok ? 0 : 1) }
' <<<"$ppc_channel" || fail "the PPC channel's root end is not id $pp_root with notify=false"

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
