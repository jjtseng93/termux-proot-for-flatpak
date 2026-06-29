/* LD_PRELOAD shim for Android-specific bwrap/flatpak issues.
 *
 * Fix 1: openat(O_TMPFILE) → EPERM
 *   Android kernels return EPERM for O_TMPFILE on unsupported filesystems
 *   instead of EOPNOTSUPP/ENOSYS.  libglnx only falls through to its mkstemp
 *   path for those two errnos, so we intercept EPERM and emulate O_TMPFILE
 *   with mkstemp+unlink.
 *
 * Fix 2: openat(proc_fd, "self", O_PATH) → ENOENT  ("open /proc/self failed")
 *   After bwrap's child does pivot_root, the HOST proc fd (opened before
 *   pivot_root) can no longer resolve the "self" magic symlink on Android.
 *   We cache the fd from the first successful "self" open and return a dup()
 *   of it on subsequent failures.
 *
 * Fix 3: openat(proc_self_fd, "uid_map"|"gid_map"|"setgroups", O_RDWR) → ENOENT
 *   After the second unshare(CLONE_NEWUSER) (needed for devpts setup), Android's
 *   kernel procfs does not expose uid_map/gid_map/setgroups for the nested user
 *   namespace via the HOST proc fd.  We first try the sandbox proc (/proc/self/X),
 *   then fall back to a memfd so bwrap can at least write() without aborting.
 *
 * Build:
 *   cc -shared -fPIC -O2 -o libotmpfile-preload.so tools/otmpfile-preload.c -ldl
 * Use:
 *   LD_PRELOAD=/path/to/libotmpfile-preload.so flatpak run ...
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef __O_TMPFILE
#define __O_TMPFILE 020000000  /* 0x400000 — generic/aarch64 */
#endif

static int (*real_openat)  (int, const char *, int, ...);
static int (*real_openat64)(int, const char *, int, ...);
static int (*real_linkat)  (int, const char *, int, const char *, int);
static int (*real_unshare) (int);

/* ── Debug tracing (FPSHIM_DEBUG=1) ─────────────────────────────────────── */

/* Check FPSHIM_DEBUG once and cache the result. */
static int fpshim_debug_val = -1;

static int
fpshim_debug (void)
{
  if (fpshim_debug_val < 0)
    fpshim_debug_val = getenv ("FPSHIM_DEBUG") ? 1 : 0;
  return fpshim_debug_val;
}

/* fplog_fd is opened early (in the constructor) from the parent process so that
 * clone()'d children inherit it even after they set up a private mount namespace
 * with their own /tmp.  NOT O_CLOEXEC so children keep it across clone(). */
static int fplog_fd = -1;

static void __attribute__((constructor))
fpshim_init (void)
{
  if (!fpshim_debug ())
    return;
  /* Open WITHOUT O_CLOEXEC so clone()'d children inherit this fd.
   * Use O_APPEND so concurrent writes from parent/child don't interleave badly. */
  fplog_fd = syscall (__NR_openat, AT_FDCWD, "/tmp/fpshim.log",
                      O_WRONLY | O_CREAT | O_APPEND, 0644);
}

static void
fplog_write (const char *buf, size_t n)
{
  /* Lazy fallback if constructor somehow didn't run. */
  if (fplog_fd < 0)
    fplog_fd = syscall (__NR_openat, AT_FDCWD, "/tmp/fpshim.log",
                        O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fplog_fd >= 0)
    write (fplog_fd, buf, n);
}

#define FPLOG(fmt, ...) \
  do { if (fpshim_debug()) { \
    char _buf[512]; \
    int _n = snprintf (_buf, sizeof (_buf), "[fpshim %d] " fmt "\n", getpid (), ##__VA_ARGS__); \
    if (_n > 0) fplog_write (_buf, (size_t)(_n < (int)sizeof (_buf) ? _n : (int)sizeof(_buf)-1)); \
  } } while (0)


static void
resolve_symbols (void)
{
  if (real_openat)
    return;
  real_openat   = dlsym (RTLD_NEXT, "openat");
  real_openat64 = dlsym (RTLD_NEXT, "openat64");
  real_linkat   = dlsym (RTLD_NEXT, "linkat");
  real_unshare  = dlsym (RTLD_NEXT, "unshare");
}

/* ── Fix 1: O_TMPFILE fallback ─────────────────────────────────────────── */

static int
anonymous_tmpfile (const char *dir, int flags, mode_t mode)
{
  char tmpl[1024];
  int n = snprintf (tmpl, sizeof (tmpl), "%s/otmp-XXXXXX", dir);
  if (n <= 0 || (size_t)n >= sizeof (tmpl))
    { errno = ENAMETOOLONG; return -1; }

  int fd = mkstemp (tmpl);
  if (fd < 0) return -1;

  unlink (tmpl);

  if (fchmod (fd, mode ? mode : 0600) < 0)
    { int e = errno; close (fd); errno = e; return -1; }

  if (flags & O_CLOEXEC)
    fcntl (fd, F_SETFD, FD_CLOEXEC);

  return fd;
}

static int
dirfd_to_path (int dirfd, char *buf, size_t size)
{
  if (dirfd == AT_FDCWD)
    return getcwd (buf, size) ? 0 : -1;

  char proc[64];
  snprintf (proc, sizeof (proc), "/proc/self/fd/%d", dirfd);
  ssize_t n = readlink (proc, buf, size - 1);
  if (n < 0) return -1;
  buf[n] = '\0';
  return 0;
}

static int
otmpfile_fallback (int dirfd, int flags, mode_t mode)
{
  char dir[1024];

  if (dirfd_to_path (dirfd, dir, sizeof (dir)) == 0)
    {
      int fd = anonymous_tmpfile (dir, flags, mode);
      if (fd >= 0) return fd;
    }

  const char *tmpdir = getenv ("TMPDIR");
  if (tmpdir)
    {
      int fd = anonymous_tmpfile (tmpdir, flags, mode);
      if (fd >= 0) return fd;
    }

  return anonymous_tmpfile ("/tmp", flags, mode);
}

/* ── Fix 2: /proc/self caching ──────────────────────────────────────────── */

/* fd kept open across calls; always dup()d before returning so the caller
 * can close its copy without affecting ours. */
static int cached_proc_self_fd = -1;

/* Track whether we have already needed the fallback (i.e. we are past the
 * first pivot_root).  Used to gate Fix 3. */
static int proc_pivot_seen = 0;

/* Fix 5: bwrap writes "1" to max_user_namespaces to disable user namespace
 * creation inside the sandbox, then verifies it by calling unshare(NEWUSER).
 * In proot we can't actually set the sysctl, so we count unshare(NEWUSER)
 * calls after the fake write: the first must succeed (creates 2nd user ns),
 * the second (verification) must fail with EPERM. */
static int userns_sysctl_faked = 0;
static int userns_unshare_count = 0;

static void
cache_proc_self (int fd)
{
  if (fd < 0) return;
  int copy = dup (fd);
  if (copy < 0) return;
  if (cached_proc_self_fd >= 0)
    close (cached_proc_self_fd);
  cached_proc_self_fd = copy;
}

static int
proc_self_fallback (int flags)
{
  proc_pivot_seen = 1;

  if (cached_proc_self_fd >= 0)
    {
      int fd = dup (cached_proc_self_fd);
      if (fd >= 0) return fd;
    }

  /* No cache yet – try the sandbox proc. */
  return real_openat (AT_FDCWD, "/proc/self", flags & ~O_NOFOLLOW);
}

/* ── Fix 3: uid_map / gid_map / setgroups fallback ─────────────────────── */

static int
is_ns_map_path (const char *path)
{
  return path &&
         (strcmp (path, "uid_map") == 0 ||
          strcmp (path, "gid_map") == 0 ||
          strcmp (path, "setgroups") == 0);
}

/* Return a writable memfd so bwrap can write() the content without error.
 * The data goes nowhere (second user namespace uid_map stays empty), but
 * bwrap can proceed and we can see if there are further issues. */
static int
fake_writable_fd (int flags)
{
  int fd = memfd_create ("uid_map_fake", MFD_CLOEXEC);
  if (fd >= 0) return fd;

  /* memfd_create not available – fall back to /dev/null */
  int devnull_flags = O_RDWR;
  if (flags & O_CLOEXEC) devnull_flags |= O_CLOEXEC;
  return real_openat (AT_FDCWD, "/dev/null", devnull_flags);
}

static int
ns_map_fallback (const char *path, int flags, mode_t mode)
{
  /* Try the sandbox proc first (mounted at /proc in the new rootfs). */
  char sandbox_path[64];
  snprintf (sandbox_path, sizeof (sandbox_path), "/proc/self/%s", path);
  int fd = real_openat (AT_FDCWD, sandbox_path, flags, mode);
  if (fd >= 0) return fd;

  /* Sandbox proc also can't provide it; give a fake writable fd. */
  return fake_writable_fd (flags);
}

/* ── Central dispatch ───────────────────────────────────────────────────── */

static int
handle_openat (int dirfd, const char *path, int flags, mode_t mode,
               int (*fn)(int, const char *, int, ...))
{
  int fd = fn (dirfd, path, flags, mode);

  if (fd >= 0)
    {
      /* Cache successful openat(non-FDCWD, "self", O_PATH) for Fix 2. */
      if (dirfd != AT_FDCWD && path && (flags & O_PATH)
          && strcmp (path, "self") == 0)
        cache_proc_self (fd);
      return fd;
    }

  int saved = errno;

  /* Fix 1 */
  if ((flags & __O_TMPFILE) && saved == EPERM)
    return otmpfile_fallback (dirfd, flags, mode);

  /* Fix 2 */
  if (dirfd != AT_FDCWD && path && saved == ENOENT
      && (flags & O_PATH) && strcmp (path, "self") == 0)
    return proc_self_fallback (flags);

  /* Fix 3: only after we have seen the first proc_self failure (post-pivot). */
  if (proc_pivot_seen && dirfd != AT_FDCWD && saved == ENOENT
      && is_ns_map_path (path))
    return ns_map_fallback (path, flags, mode);

  /* Fix 5: openat(proc_fd, "sys/user/max_user_namespaces", O_WRONLY) → EACCES
   * bwrap sets this sysctl to 1 when --disable-userns is used, to prevent
   * the sandbox from creating more user namespaces.  In proot the file
   * exists but is not writable.  Return a memfd so bwrap's write succeeds. */
  if ((saved == EACCES || saved == EPERM || saved == ENOENT)
      && path && strcmp (path, "sys/user/max_user_namespaces") == 0
      && (flags & O_WRONLY))
    {
      userns_sysctl_faked = 1;
      return fake_writable_fd (flags);
    }

  errno = saved;
  return -1;
}

/* ── Fix 4: linkat(AT_EMPTY_PATH) → EPERM ───────────────────────────────
 *
 * libglnx calls linkat(src_fd, "", dst_dfd, dst_path, AT_EMPTY_PATH) to
 * "name" an O_TMPFILE (or our mkstemp+unlink stand-in).  Android denies
 * this without CAP_DAC_READ_SEARCH.  We fall back to copying the fd
 * contents to a sibling temp file and renaming it into place.
 */
static int
linkat_copy_fallback (int src_fd, int dst_dfd, const char *dst_path)
{
  /* src_fd may be an O_PATH fd (used purely for inode reference), which
   * can't be read.  Open a readable copy via /proc/self/fd/<N>. */
  char src_proc[64];
  snprintf (src_proc, sizeof (src_proc), "/proc/self/fd/%d", src_fd);
  int read_fd = open (src_proc, O_RDONLY | O_CLOEXEC);
  if (read_fd < 0) return -1;

  /* Resolve the destination directory path for mkstemp. */
  char dirpath[PATH_MAX];
  if (dst_dfd == AT_FDCWD)
    {
      if (getcwd (dirpath, sizeof (dirpath)) == NULL)
        { close (read_fd); return -1; }
    }
  else
    {
      char fdlink[64];
      snprintf (fdlink, sizeof (fdlink), "/proc/self/fd/%d", dst_dfd);
      ssize_t n = readlink (fdlink, dirpath, sizeof (dirpath) - 1);
      if (n < 0) { close (read_fd); return -1; }
      dirpath[n] = '\0';
    }

  char tmpl[PATH_MAX];
  int n = snprintf (tmpl, sizeof (tmpl), "%s/.lnk-XXXXXX", dirpath);
  if (n <= 0 || (size_t)n >= sizeof (tmpl))
    { close (read_fd); errno = ENAMETOOLONG; return -1; }

  int tmp_fd = mkstemp (tmpl);
  if (tmp_fd < 0) { close (read_fd); return -1; }

  struct stat st;
  if (fstat (read_fd, &st) < 0)
    { int e = errno; close (tmp_fd); close (read_fd); unlink (tmpl); errno = e; return -1; }

  off_t remaining = st.st_size;
  while (remaining > 0)
    {
      ssize_t sent = sendfile (tmp_fd, read_fd, NULL,
                               (size_t)(remaining > (off_t)1048576 ? 1048576 : remaining));
      if (sent < 0)
        { int e = errno; close (tmp_fd); close (read_fd); unlink (tmpl); errno = e; return -1; }
      remaining -= sent;
    }

  fchmod (tmp_fd, st.st_mode & 0777);
  close (tmp_fd);
  close (read_fd);

  if (renameat (AT_FDCWD, tmpl, dst_dfd, dst_path) < 0)
    { int e = errno; unlink (tmpl); errno = e; return -1; }

  return 0;
}

int
linkat (int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
  resolve_symbols ();
  int r = real_linkat (olddirfd, oldpath, newdirfd, newpath, flags);
  if (r < 0 && (errno == EPERM || errno == EACCES))
    {
      int src_fd = -1;

      /* Case A: linkat(fd, "", ..., AT_EMPTY_PATH) — olddirfd is the file. */
      if ((flags & AT_EMPTY_PATH) && oldpath && oldpath[0] == '\0')
        src_fd = olddirfd;

      /* Case B: linkat(AT_FDCWD, "/proc/self/fd/<N>", ..., AT_SYMLINK_FOLLOW)
       * — caller opened the file as fd N and uses the procfs magic link as
       * the source path so the kernel can find the inode. */
      if (src_fd < 0 && oldpath)
        {
          int n = -1;
          if (sscanf (oldpath, "/proc/self/fd/%d", &n) == 1 ||
              sscanf (oldpath, "/proc/%*d/fd/%d", &n) == 1)
            src_fd = n;
        }

      if (src_fd >= 0)
        r = linkat_copy_fallback (src_fd, newdirfd, newpath);
    }
  return r;
}

/* ── Fix 5 cont.: unshare(CLONE_NEWUSER) verification spoofing ───────── */

int
unshare (int flags)
{
  resolve_symbols ();
  if (userns_sysctl_faked && (flags & CLONE_NEWUSER))
    {
      userns_unshare_count++;
      if (userns_unshare_count > 1)
        {
          /* This is the verification call — pretend user ns creation failed. */
          errno = EPERM;
          return -1;
        }
    }
  return real_unshare (flags);
}

/* ── mount hook: fix /.flatpak-info bind mount ───────────────────────────
 *
 * bwrap creates /.flatpak-info via:
 *   1. --file FD /.flatpak-info   → creates file in newroot tmpfs (OK)
 *   2. --ro-bind-data FD /.flatpak-info → bind-mounts a PRIVATE tmpfs file
 *      over it (source path like /bindfileXXX only exists in bwrap's own
 *      private mount namespace).
 *
 * proot cannot translate this private-tmpfs source path, so it fakes the
 * mount (returns 0) but never actually sets up the bind.  After pivot_root
 * the file appears absent inside the sandbox.
 *
 * Fix: intercept the bind-mount call for /.flatpak-info and instead copy
 * the content directly into the target file.  The target already exists as
 * a regular file (from the --file step) so we just overwrite it.
 */

#include <sys/mount.h>

int
mount (const char *source, const char *target, const char *filesystemtype,
       unsigned long mountflags, const void *data)
{
  static int (*real_mount)(const char *, const char *, const char *,
                           unsigned long, const void *);
  if (!real_mount) real_mount = dlsym (RTLD_NEXT, "mount");

  /* bwrap's --ro-bind-data /.flatpak-info uses source "/bindfileXXX" which only
   * exists in bwrap's private tmpfs.  proot cannot translate this path so the
   * bind-mount is either faked or broken.  Fix: copy content directly into the
   * target (created as a regular file by bwrap's --file step), then self-bind so
   * bwrap's mountinfo check passes. */
  if (target && strstr (target, "flatpak-info")
      && (mountflags & MS_BIND)
      && !(mountflags & MS_REMOUNT)
      && source && source[0] == '/' && strncmp (source, "/bindfile", 9) == 0)
    {
      int sfd = syscall (__NR_openat, AT_FDCWD, source, O_RDONLY | O_CLOEXEC, 0);
      int ok = 0;
      if (sfd >= 0)
        {
          int wfd = syscall (__NR_openat, AT_FDCWD, target,
                             O_WRONLY | O_TRUNC | O_CLOEXEC, 0);
          if (wfd >= 0)
            {
              char buf[4096];
              ssize_t n;
              while ((n = read (sfd, buf, sizeof (buf))) > 0)
                write (wfd, buf, (size_t) n);
              syscall (__NR_close, wfd);
              ok = 1;
            }
          else
            FPLOG ("flatpak-info write FAIL tgt=%s errno=%d (%s)",
                   target, errno, strerror (errno));
          syscall (__NR_close, sfd);
        }
      else
        FPLOG ("flatpak-info open FAIL src=%s errno=%d (%s)", source, errno, strerror (errno));

      if (ok)
        {
          int r = real_mount (target, target, filesystemtype, mountflags, data);
          if (r < 0)
            FPLOG ("flatpak-info self-bind FAIL tgt=%s errno=%d (%s)",
                   target, errno, strerror (errno));
          return r;
        }

      FPLOG ("flatpak-info FAILED src=%s tgt=%s", source, target);
      /* Fall through so bwrap reports its own error. */
    }

  return real_mount (source, target, filesystemtype, mountflags, data);
}

/* ── Public hooks ───────────────────────────────────────────────────────── */

int
openat (int dirfd, const char *path, int flags, ...)
{
  mode_t mode = 0;
  va_list ap;
  resolve_symbols ();
  if (flags & (O_CREAT | __O_TMPFILE))
    { va_start (ap, flags); mode = (mode_t) va_arg (ap, int); va_end (ap); }
  return handle_openat (dirfd, path, flags, mode, real_openat);
}

int
openat64 (int dirfd, const char *path, int flags, ...)
{
  mode_t mode = 0;
  va_list ap;
  resolve_symbols ();
  if (flags & (O_CREAT | __O_TMPFILE))
    { va_start (ap, flags); mode = (mode_t) va_arg (ap, int); va_end (ap); }
  return handle_openat (dirfd, path, flags, mode, real_openat64);
}

