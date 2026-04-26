#!/usr/bin/awk -f
# Parse keysymdef.h into an emittable C table: { "Name", 0xHEX },
# Skip XK_VoidSymbol (0xffffff sentinel -- not a real key). Skip aliases
# to Unicode keysyms (0x1000xxxx range is the "U+xxxx" mapping form;
# ISO 10646 keysyms are already reachable via the direct codepoint, and
# including them would balloon the table with duplicates).
BEGIN {
    print "/* Auto-generated from X11/keysymdef.h by gen_keysyms.awk."
    print " * Do not edit by hand. */"
    print ""
    print "#include \"keysym_table.h\""
    print ""
    print "const struct KeysymEntry g_keysym_table[] = {"
}
/^#define XK_[A-Za-z0-9_]+[ \t]+0x[0-9a-fA-F]+/ {
    name = $2
    sub(/^XK_/, "", name)
    val = $3
    if (name == "VoidSymbol") next
    # Unicode-range keysyms (0x1000xxxx) are duplicates of the plain
    # codepoint for XStringToKeysym purposes; skip to keep the table
    # focused on the symbolic names Tk binding specs actually use.
    if (val ~ /^0x01/) next
    printf "    {\"%s\", %s},\n", name, val
}
END {
    print "    {(const char *)0, 0}"
    print "};"
}
