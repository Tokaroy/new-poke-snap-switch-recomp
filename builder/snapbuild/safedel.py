"""Junction-safe recursive delete.

Why this exists: on Windows a recursive delete that walks into a junction (or a directory symlink) deletes what the link points to.
On 2026-11-16 a cleanup of a scratch tree that held junctions into the repository emptied tools\\ for the whole project.

Rules enforced here:
  * A reparse point (junction, directory symlink, file symlink, any other reparse tag) is unlinked, never entered.
  * Only real directories are walked, and only real files are deleted.
  * The root itself is treated the same way: if it is a link, only the link goes.

Use `rmtree` instead of `shutil.rmtree` everywhere in the builder. tests/test_safedel.py fails if a bare `shutil.rmtree` comes back.
"""
import os
import stat
import sys
from pathlib import Path

_REPARSE = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)


def is_link(path):
    """True for a symlink, a junction or any other reparse point (does not follow it)."""
    p = os.fspath(path)
    try:
        st = os.lstat(p)
    except OSError:
        return False
    if stat.S_ISLNK(st.st_mode):
        return True
    if getattr(st, 'st_file_attributes', 0) & _REPARSE:
        return True
    isj = getattr(os.path, 'isjunction', None)
    return bool(isj and isj(p))


def unlink_link(path):
    """Remove a link without touching its target (rmdir for a directory link, unlink for a file link)."""
    p = os.fspath(path)
    try:
        os.rmdir(p)          # a junction or directory symlink: removes the link only
    except (NotADirectoryError, PermissionError, OSError):
        os.unlink(p)         # a file symlink


def _clear_readonly(p):
    try:
        os.chmod(p, stat.S_IWRITE | stat.S_IREAD)
    except OSError:
        pass


def _remove_file(p):
    try:
        os.unlink(p)
    except PermissionError:
        _clear_readonly(p)
        os.unlink(p)


def _walk_delete(root, ignore_errors):
    try:
        with os.scandir(root) as it:
            entries = list(it)
    except OSError:
        if ignore_errors:
            return
        raise
    for e in entries:
        p = e.path
        try:
            if is_link(p):
                unlink_link(p)
            elif e.is_dir(follow_symlinks=False):
                _walk_delete(p, ignore_errors)
                try:
                    os.rmdir(p)
                except PermissionError:
                    _clear_readonly(p)
                    os.rmdir(p)
            else:
                _remove_file(p)
        except OSError:
            if not ignore_errors:
                raise


def unlink_links(root):
    """Unlink every link under root (not following any) and return their paths. Use before a delete that is not ours."""
    found = []
    root = os.fspath(root)
    if is_link(root):
        return found
    stack = [root]
    while stack:
        d = stack.pop()
        try:
            with os.scandir(d) as it:
                entries = list(it)
        except OSError:
            continue
        for e in entries:
            if is_link(e.path):
                unlink_link(e.path)
                found.append(e.path)
            elif e.is_dir(follow_symlinks=False):
                stack.append(e.path)
    return found


def rmtree(path, ignore_errors=False):
    """Delete a folder tree, never following a link. A missing path is not an error when ignore_errors is set."""
    p = os.fspath(path)
    if not os.path.lexists(p):
        if ignore_errors:
            return
        raise FileNotFoundError(p)
    if is_link(p):
        try:
            unlink_link(p)
        except OSError:
            if not ignore_errors:
                raise
        return
    if not os.path.isdir(p):
        raise NotADirectoryError(p)
    _walk_delete(p, ignore_errors)
    try:
        os.rmdir(p)
    except PermissionError:
        _clear_readonly(p)
        try:
            os.rmdir(p)
        except OSError:
            if not ignore_errors:
                raise
    except OSError:
        if not ignore_errors:
            raise


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('usage: safedel.py <folder>...   (deletes each tree, unlinking links instead of following them)')
        sys.exit(2)
    for a in sys.argv[1:]:
        rmtree(Path(a))
        print('removed', a)
