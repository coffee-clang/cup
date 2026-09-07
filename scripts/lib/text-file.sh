
# Owns the byte-level policy shared by internal text-file validators.

cup_text_file_is_nul_cr_free() (
    file=${1-}
    [ -n "$file" ] || return 1
    od -An -v -t x1 "$file" 2>/dev/null | awk '
        {
            for (i = 1; i <= NF; ++i) {
                if ($i == "00" || $i == "0d") exit 1
            }
        }
    '
)
