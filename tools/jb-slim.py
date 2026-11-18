#!/usr/bin/env python3
"""jb-slim.py PREFIX OUT PATH...: a part of the Procursus prefix (tools/mk-jb.sh) -- the paths given, relative to the
prefix, with the links they go through and every library of the prefix they load (@rpath, /var/jb/...), followed
until nothing new is needed. build.sh JB=ssh: sshd, a shell and a few commands instead of the whole 132 MB."""
import os, shutil, subprocess, sys

prefix, out, paths = sys.argv[1], sys.argv[2], sys.argv[3:]
done, todo = set(), list(paths)


def deps(path):
    try:
        text = subprocess.run(['llvm-otool', '-L', path], capture_output=True, text=True).stdout
    except FileNotFoundError:
        sys.exit('jb-slim.py: llvm-otool is needed')
    for line in text.splitlines()[1:]:
        name = line.strip().split(' (')[0]
        if name.startswith('@rpath/'):
            yield 'usr/lib/' + name[len('@rpath/'):]
        elif name.startswith('/var/jb/'):
            yield name[len('/var/jb/'):]


while todo:
    rel = os.path.normpath(todo.pop())
    if rel in done:
        continue
    done.add(rel)
    src, dst = os.path.join(prefix, rel), os.path.join(out, rel)
    if not os.path.lexists(src):
        print('jb-slim.py: not in the prefix:', rel, file=sys.stderr)
        continue
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.islink(src):
        target = os.readlink(src)
        if os.path.lexists(dst):
            os.remove(dst)
        os.symlink(target, dst)
        todo.append(target[len('/var/jb/'):] if target.startswith('/var/jb/') else
                    os.path.join(os.path.dirname(rel), target))
    elif os.path.isdir(src):
        shutil.copytree(src, dst, symlinks=True, dirs_exist_ok=True)
    else:
        shutil.copy2(src, dst)
        todo.extend(deps(src))
