import re

c_path = r"D:\Project\Embedded\ESP32\ai-wireless-debugger-main\main\net\http_status.c"
html_path = r"D:\Project\Embedded\ESP32\ai-wireless-debugger-main\new_index.html"

src = open(c_path, "r", encoding="utf-8").read()
# Normalize all line endings to LF so no bare CR ends up inside a C literal.
html = open(html_path, "r", encoding="utf-8").read()
html = html.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n")

# Split HTML into C string lines: each physical line becomes "...\n".
# The trailing \n escape is REQUIRED: C concatenates adjacent string literals
# back to back, so without it every newline vanishes. That is harmless between
# HTML tags but fatal inside <script>, where a "//" comment then swallows every
# following statement (they all end up on the same physical line).
lines = html.split("\n")
c_lines = []
for i, line in enumerate(lines):
    # Escape backslash and double-quote for C
    esc = line.replace("\\", "\\\\").replace('"', '\\"')
    if i == len(lines) - 1:
        c_lines.append('"' + esc + '";')
    else:
        c_lines.append('"' + esc + '\\n"')
c_str = "\n".join(c_lines)

# Replace the old s_index_html array. Anchor the end on the comment that
# always follows it, so a trailing escaped quote in the old block cannot
# cause a silent no-match (re.sub would otherwise write the file unchanged).
pattern = re.compile(
    r'static const char s_index_html\[\]\s*=.*?(?=/\* GET / - serve HTML page \*/)',
    re.DOTALL,
)
matches = pattern.findall(src)
if len(matches) != 1:
    raise SystemExit("ERROR: expected exactly 1 s_index_html block, found %d" % len(matches))

# Use a lambda: a plain replacement string goes through re's own backslash
# processing and would turn our doubled backslashes ("\\s") back into one.
replacement = "static const char s_index_html[] = \n" + c_str + "\n\n"
new_src = pattern.sub(lambda m: replacement, src, count=1)

# Preserve the C file's original CRLF line endings.
crlf = "\r\n" in src
with open(c_path, "w", encoding="utf-8", newline="") as f:
    f.write(new_src.replace("\r\n", "\n").replace("\n", "\r\n") if crlf else new_src)

print("OK, replaced. new size:", len(new_src.encode("utf-8")), "bytes,",
      len(c_lines), "C string lines")
