"""粗略括号配平检查（跳过注释与字符串/字符字面量）。

用途：本仓库的编译验证由人来做，改完大文件（app_online.cc / online_service.cc）
先跑一遍这个，能挡住"括号/引号写漏"这类低级语法错。
不是编译器，只做配平与引号闭合检查。
"""
import sys


def strip(code: str) -> str:
    out = []
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == '/' and i + 1 < n and code[i + 1] == '/':
            while i < n and code[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and code[i + 1] == '*':
            i += 2
            while i + 1 < n and not (code[i] == '*' and code[i + 1] == '/'):
                i += 1
            i += 2
        elif c == '"':
            i += 1
            while i < n and code[i] != '"':
                i += 2 if code[i] == '\\' else 1
            i += 1
        elif c == "'":
            i += 1
            while i < n and code[i] != "'":
                i += 2 if code[i] == '\\' else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def main(paths):
    bad = 0
    for path in paths:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            code = strip(f.read())
        stack = []
        pairs = {')': '(', '}': '{', ']': '['}
        line = 1
        for ch in code:
            if ch == '\n':
                line += 1
            elif ch in '({[':
                stack.append((ch, line))
            elif ch in ')}]':
                if not stack or stack[-1][0] != pairs[ch]:
                    print(f"{path}:{line}: unbalanced '{ch}'")
                    bad += 1
                    break
                stack.pop()
        else:
            if stack:
                ch, ln = stack[-1]
                print(f"{path}:{ln}: unclosed '{ch}' (共 {len(stack)} 个未闭合)")
                bad += 1
            else:
                print(f"{path}: OK")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
