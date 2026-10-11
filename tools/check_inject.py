import re
s = open(r"D:\Project\Embedded\ESP32\ai-wireless-debugger-main\main\net\http_status.c", encoding="utf-8").read()
m = re.search(r"static const char s_index_html\[\][\s\S]*?;\n", s)
assert m, "s_index_html block not found"
blk = m.group(0).splitlines()
bad = []
for i, l in enumerate(blk):
    st = l.strip()
    if st.startswith('"'):
        r = st.rstrip()
        if not r.endswith('\\n"') and not r.endswith('";'):
            bad.append((i + 1, r[:80]))
print("block lines:", len(blk))
print("BAD string lines missing trailing \\n:", len(bad))
for b in bad[:20]:
    print(b)
