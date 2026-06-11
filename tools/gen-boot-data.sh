#!/usr/bin/env bash
# Generate boot_data.c with embedded boot files
set -euo pipefail

OUTPUT="${1:-boot_data.c}"
OTP_DIR="${2:?OTP_DIR required}"
BOOT_SCRIPT="${3:-$OTP_DIR/releases/28/start_clean.boot}"

if [[ ! -f "$BOOT_SCRIPT" ]]; then
  echo "ERROR: Boot script not found: $BOOT_SCRIPT" >&2
  exit 1
fi

# Find kernel and stdlib versions
KERNEL_DIR=$(find "$OTP_DIR/lib" -maxdepth 1 -name "kernel-*" -type d | head -1)
STDLIB_DIR=$(find "$OTP_DIR/lib" -maxdepth 1 -name "stdlib-*" -type d | head -1)

if [[ -z "$KERNEL_DIR" || -z "$STDLIB_DIR" ]]; then
  echo "ERROR: Could not find kernel or stdlib in $OTP_DIR/lib" >&2
  exit 1
fi

echo "Found kernel: $KERNEL_DIR" >&2
echo "Found stdlib: $STDLIB_DIR" >&2

# Generate C file with embedded data
cat > "$OUTPUT" << 'EOF'
/*
 * Auto-generated boot data for Chrysopolis in-memory filesystem.
 * DO NOT EDIT - regenerated at build time.
 */
#include <stddef.h>

EOF

# Function to embed a file as a C array
embed_file() {
  local file="$1"
  local varname="$2"
  
  if [[ ! -f "$file" ]]; then
    echo "static const unsigned char ${varname}[] = {0};" >> "$OUTPUT"
    echo "static const size_t ${varname}_size = 0;" >> "$OUTPUT"
    return
  fi
  
  echo "static const unsigned char ${varname}[] = {" >> "$OUTPUT"
  
  # Use od to dump file as octal bytes, then format as hex
  # Remove trailing comma from the last line
  od -An -v -tx1 "$file" | \
    sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g' | \
    sed 's/^/ /' | \
    sed '$ s/,$//' >> "$OUTPUT"
  
  echo "};" >> "$OUTPUT"
  echo "static const size_t ${varname}_size = sizeof(${varname});" >> "$OUTPUT"
  echo >> "$OUTPUT"
}

# Embed the boot script
embed_file "$BOOT_SCRIPT" "boot_script_data"

# Collect and embed .beam files
declare -a BEAM_FILES
declare -a BEAM_VARS
declare -a BEAM_PATHS

# Function to process .beam files from a directory
process_beam_dir() {
  local lib_dir="$1"
  local lib_name=$(basename "$lib_dir")
  
  if [[ ! -d "$lib_dir/ebin" ]]; then
    return
  fi
  
  while IFS= read -r -d '' beam_file; do
    local beam_name=$(basename "$beam_file" .beam)
    # Sanitize variable name: replace all non-alphanumeric chars with underscore
    local var_name="${lib_name//[^a-zA-Z0-9]/_}_${beam_name//[^a-zA-Z0-9]/_}"
    local virt_path="/lib/$lib_name/ebin/$beam_name.beam"
    
    embed_file "$beam_file" "${var_name}_data"
    
    BEAM_FILES+=("$beam_file")
    BEAM_VARS+=("${var_name}_data")
    BEAM_PATHS+=("$virt_path")
  done < <(find "$lib_dir/ebin" -maxdepth 1 -name "*.beam" -type f -print0)
}

echo "Embedding kernel modules..." >&2
process_beam_dir "$KERNEL_DIR"

echo "Embedding stdlib modules..." >&2
process_beam_dir "$STDLIB_DIR"

# Generate file table entries
cat >> "$OUTPUT" << 'EOF'
/* File table - paths to embedded data */
typedef struct {
  const char *path;
  const unsigned char *data;
  size_t size;
} boot_file_entry_t;

const boot_file_entry_t boot_files[] = {
  {"/releases/28/start.boot", boot_script_data, sizeof(boot_script_data)},
EOF

# Add all .beam file entries
for i in "${!BEAM_PATHS[@]}"; do
  echo "  {\"${BEAM_PATHS[$i]}\", ${BEAM_VARS[$i]}, sizeof(${BEAM_VARS[$i]})}," >> "$OUTPUT"
done

# Add device files and sentinel
cat >> "$OUTPUT" << 'EOF'
  {"/dev/null", NULL, 0},
  {"/dev/zero", NULL, 0},
  {"/dev/urandom", NULL, 0},
  {NULL, NULL, 0}  /* sentinel */
};
EOF

echo "Generated $OUTPUT with boot data (${#BEAM_FILES[@]} .beam files)" >&2
