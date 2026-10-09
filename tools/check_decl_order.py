"""粗查"用了但没定义/定义在调用之后"（C/C++ 单文件自查）。

用途：本仓库的编译验证由人来做，改完大文件先跑一遍这个 + check_balance.py，
能挡住"删函数时漏删调用点""新函数定义在调用之后"这类低级错。
它只做**取名+行号**的启发式检查，不是编译器。
"""
import re
import sys

# 定义：行首（容忍 static / 返回类型）到 名字(参数) 后跟 { 或 行尾
DEF_RE = re.compile(
    r'^\s*(?:static\s+)?(?:inline\s+)?(?:const\s+)?'
    r'(?:void|bool|int|size_t|uint\w+|int\w+|char|float|double|esp_err_t|lv_\w+|cJSON|DIR|FILE)'
    r'\s*\**\s*(\w+)\s*\(')


def strip_code(text: str) -> str:
    """把注释与字符串换掉（保留行号），避免误判里面的括号/名字。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                out.append(' ')
                i += 1
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            i += 2
        elif c in '"\'':
            quote = c
            out.append(' ')
            i += 1
            while i < n and text[i] != quote:
                out.append('\n' if text[i] == '\n' else ' ')
                if text[i] == '\\':
                    out.append(' ')
                    i += 1
                i += 1
            out.append(' ')
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def main(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        raw = f.read()
    code = strip_code(raw)
    lines = code.split('\n')

    defines = {}     # name -> line of definition
    protos = set()   # names with a forward declaration
    for idx, line in enumerate(lines, 1):
        m = DEF_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        if name in ('if', 'for', 'while', 'switch', 'return', 'sizeof', 'else',
                    'catch', 'defined', 'static_assert'):
            continue
        # 定义 = 这一行以 '{' 结尾，或下一行是 '{'
        stripped = line.rstrip()
        nxt = lines[idx].strip() if idx < len(lines) else ''
        if stripped.endswith('{') or nxt == '{':
            defines.setdefault(name, idx)
        elif stripped.endswith(');'):
            protos.add(name)

    bad = []
    for name, dline in sorted(defines.items(), key=lambda kv: kv[1]):
        if name in protos:
            continue                      # 有前置声明：随便用
        call_re = re.compile(r'\b' + re.escape(name) + r'\s*\(')
        for idx, line in enumerate(lines, 1):
            if idx >= dline:
                break
            if call_re.search(line):
                bad.append((name, idx, dline))
                break

    if bad:
        for name, cline, dline in bad:
            print(f"{path}: '{name}' 在第 {cline} 行被调用，但定义在 {dline} 行（且无前置声明）")
        return 1
    print(f"{path}: OK（{len(defines)} 个定义，{len(protos)} 个前置声明）")
    return 0


if __name__ == '__main__':
    rc = 0
    for p in sys.argv[1:]:
        rc |= main(p)
    sys.exit(rc)
