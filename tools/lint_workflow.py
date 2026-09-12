"""从 workflow yml 中提取各步骤的 run 脚本块，供后续做语法检查。

用法：python tools/lint_workflow.py
产出：obj/lint/<job>-<序号>-<步骤名>.<ps1|cmd|sh>
"""
import os
import re
import sys
import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
YML = os.path.join(ROOT, '.github', 'workflows', 'build.yml')
OUT = os.path.join(ROOT, 'obj', 'lint')

EXT = {'pwsh': 'ps1', 'powershell': 'ps1', 'cmd': 'cmd', 'bash': 'sh', 'sh': 'sh'}


def main():
    raw = open(YML, encoding='utf-8').read()

    print('== 1. GitHub 表达式核对 ==')
    exprs = re.findall(r'\$\{\{[^}]*\}\}', raw)
    for e in exprs:
        print('   ', e)
    ok = raw.count('${{') == len(exprs)
    print('    共 %d 个，全部闭合: %s' % (len(exprs), ok))

    print('\n== 2. 提取脚本块 ==')
    doc = yaml.safe_load(raw)
    if os.path.isdir(OUT):
        for f in os.listdir(OUT):
            os.remove(os.path.join(OUT, f))
    else:
        os.makedirs(OUT)

    n = 0
    for job, cfg in doc['jobs'].items():
        for i, step in enumerate(cfg.get('steps', []), 1):
            if 'run' not in step:
                continue
            shell = step.get('shell', '')
            ext = EXT.get(shell, 'sh')
            title = step.get('name', 'step')
            title = re.split(r'[（(]', title)[0].strip().replace(' ', '_')
            name = '%s-%d-%s.%s' % (job, i, title, ext)
            data = step['run']
            if not data.endswith('\n'):
                data += '\n'
            # .ps1 必须带 UTF-8 BOM：Windows PowerShell 5.1 在没有 BOM 时会按
            # 系统 ANSI 代码页（简中 = GBK）读取，中文会变乱码并连带把引号
            # 配错，解析出成片的假语法错误。GitHub 上的 pwsh 7 默认 UTF-8，
            # 不受影响，但本地校验要按最严格的那个来。
            codec = 'utf-8-sig' if ext == 'ps1' else 'utf-8'
            with open(os.path.join(OUT, name), 'w', encoding=codec,
                      newline='\r\n' if ext == 'cmd' else '\n') as fh:
                fh.write(data)
            n += 1
            print('    %-50s shell=%s' % (name, shell or '(默认)'))
    print('    共 %d 个脚本块，已写入 obj/lint/' % n)


if __name__ == '__main__':
    sys.exit(main())
