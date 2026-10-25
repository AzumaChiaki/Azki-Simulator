"""The one bubblewrap argv prefix that every supervised process shares.

bwrap builds its sandbox on an empty tmpfs root, so the interpreter path baked
into binaries (/lib64/ld-linux-x86-64.so.2) is not in view and every exec fails
with ENOENT until the usr-merge links are created; binding /usr alone is not
enough. The pid namespace is deliberately shared: SO_PEERCRED and process-group
reaping both need host-visible identities, and because bwrap has no
"--share-pid", the namespaces are enumerated instead of using "--unshare-all".
"""

LAUNCHER = "/usr/bin/bwrap"

PREAMBLE = [LAUNCHER, "--die-with-parent", "--new-session",
            "--unshare-user", "--unshare-ipc", "--unshare-uts", "--unshare-cgroup", "--unshare-net",
            "--tmpfs", "/", "--dev", "/dev", "--proc", "/proc",
            "--symlink", "usr/lib64", "/lib64", "--symlink", "usr/lib", "/lib",
            "--symlink", "usr/bin", "/bin",
            "--ro-bind", "/usr", "/usr", "--ro-bind", "/etc", "/etc", "--tmpfs", "/tmp"]
