#include "extension/extension.h"
#include "path/path.h"           /* translate_path,  */
#include "path/binding.h"        /* Binding, bindings */
#include "path/temp.h"           /* create_temp_file,  */
#include <limits.h>              /* INT_MAX,  */
#include <linux/limits.h>        /* PATH_MAX,  */
#include <string.h>              /* strlen, strcmp */
#include <stdio.h>               /* FILE, getline, fprintf */
#include <stdlib.h>              /* free,  */
#include <sys/queue.h>           /* CIRCLEQ_*,  */
#include <sys/stat.h>            /* stat(2), struct stat,  */
#include <sys/sysmacros.h>       /* major(3), minor(3),  */

/* mountinfo path fields use octal escapes for characters that would
 * otherwise be separators or control characters. */
static void write_mountinfo_path(FILE *fp, const char *path)
{
	const unsigned char *cursor = (const unsigned char *) path;

	for (; *cursor != '\0'; cursor++) {
		switch (*cursor) {
		case ' ':
			fputs("\\040", fp);
			break;
		case '\t':
			fputs("\\011", fp);
			break;
		case '\n':
			fputs("\\012", fp);
			break;
		case '\\':
			fputs("\\134", fp);
			break;
		default:
			fputc(*cursor, fp);
			break;
		}
	}
}

static void write_runtime_binding_line(FILE *fp, int id, int parent_id,
				       const Binding *binding,
				       const char *mountpoint,
				       const struct stat *statbuf)
{
	fprintf(fp, "%d %d %u:%u ",
		id, parent_id,
		major(statbuf->st_dev), minor(statbuf->st_dev));
	write_mountinfo_path(fp, binding->host.path);
	fputc(' ', fp);
	write_mountinfo_path(fp, mountpoint);
	fputs(" rw,relatime - bind ", fp);
	write_mountinfo_path(fp, binding->host.path);
	fputs(" rw,relatime\n", fp);
}

static void write_runtime_binding_alias(FILE *fp, int id, int parent_id,
					const Binding *binding,
					const char *alias,
					const struct stat *statbuf)
{
	char alias_mountpoint[PATH_MAX];

	if ((size_t) snprintf(alias_mountpoint, sizeof(alias_mountpoint),
			      "/newroot%s", alias) >= sizeof(alias_mountpoint))
		return;

	write_runtime_binding_line(fp, id, parent_id, binding,
				   alias_mountpoint, statbuf);
}

/**
 * Append a synthesized mount-table line to @fp for each runtime
 * binding (i.e. one that wasn't part of the static -r/-b set).  This
 * is what lets sandbox helpers like bubblewrap find the mount they
 * just asked PRoot to create via emulate_mount().
 */
static void append_runtime_binding_lines(Tracee *target_tracee, FILE *fp)
{
	Binding *binding;
	int next_id = 1000000;
	int parent_id = 1;

	if (target_tracee->fs->bindings.guest == NULL)
		return;

	for (binding = CIRCLEQ_FIRST(target_tracee->fs->bindings.guest);
	     binding != (void *) target_tracee->fs->bindings.guest;
	     binding = CIRCLEQ_NEXT(binding, link.guest)) {
		char fd_visible_path[PATH_MAX];
		struct stat statbuf;

		/* Skip the root binding "/" — already present as the kernel root.  */
		if (strcmp(binding->guest.path, "/") == 0)
			continue;
		/* Internal alias for a pivot_root(".", ".") old-root fd.
		 * It is not a pathname-visible mount in the tracee. */
		if (strncmp(binding->guest.path, "/.proot-pivot-oldroot",
			    strlen("/.proot-pivot-oldroot")) == 0
		    && (binding->guest.path[strlen("/.proot-pivot-oldroot")] == '\0'
			|| binding->guest.path[strlen("/.proot-pivot-oldroot")] == '/'))
			continue;

		if (stat(binding->host.path, &statbuf) < 0)
			continue;

		write_runtime_binding_line(fp, next_id++, parent_id, binding,
					   binding->guest.path, &statbuf);

		/* PRoot does not track which guest mountpoint was used to
		 * obtain an open fd.  readlink(/proc/self/fd/N) therefore
		 * detranslates the backing host path and can report the
		 * source-side name (for example /bindfileXXXXXX) instead
		 * of the bind target (/newroot/payload).  bubblewrap uses
		 * that readlink result as the exact root_mount passed to
		 * parse_mountinfo(), so expose the same fd-visible alias
		 * as another view of this emulated mount. */
		strncpy(fd_visible_path, binding->host.path, PATH_MAX - 1);
		fd_visible_path[PATH_MAX - 1] = '\0';
		if (detranslate_path(target_tracee, fd_visible_path, NULL) >= 0
		    && strcmp(fd_visible_path, binding->guest.path) != 0)
			write_runtime_binding_line(fp, next_id++, parent_id, binding,
						   fd_visible_path, &statbuf);

		/* bubblewrap's --dev setup looks up the device nodes through
		 * their pre-pivot /newroot/... names while it is still building
		 * the sandbox. Keep those aliases visible as well so helper
		 * bind mounts for /dev/full, /dev/null, etc. can be resolved. */
		if (strncmp(binding->guest.path, "/dev/", 5) == 0 ||
		    strcmp(binding->guest.path, "/dev") == 0)
			write_runtime_binding_alias(fp, next_id++, parent_id, binding,
						    binding->guest.path, &statbuf);
	}
}

static void mountinfo_check_open_path(Tracee *tracee, char path[PATH_MAX]) {
	/* Try matching "/proc/<PID>/mountinfo"  */
	size_t len = strlen(path);
	if (
			len > (6 + 10) &&
			0 == strncmp(path, "/proc/", 6) &&
			0 == strcmp(path + (len - 10), "/mountinfo")
	   ) {
		/* Check if current root is under /data and if so replace contents
		 * of /proc/<PID>/mountinfo to make it contain /data as / mountpoint.
		 * This is needed because on Android / is read only mount
		 *
		 * https://github.com/termux/proot/issues/294
		 */
		char *path_end = NULL;
		long target_pid = strtol(path + 6, &path_end, 10);
		if (path_end != path + (len - 10) || target_pid <= 0 || target_pid > INT_MAX) {
			return;
		}
		Tracee *target_tracee = get_tracee(tracee, target_pid, false);
		if (target_tracee == NULL) {
			return;
		}

		/* Check if our root is under "/data"  */
		char root_path[PATH_MAX]; // Host path to guest root
		translate_path(target_tracee, root_path, AT_FDCWD, "/", true);
		Comparison compare_result = compare_paths(root_path, "/data");
		bool is_android_data = (compare_result == PATH2_IS_PREFIX || compare_result == PATHS_ARE_EQUAL);

		/* Are there bindings to expose as fake mounts (mount(2)
		 * calls from sandbox helpers are converted into
		 * bindings — see emulate_mount).  Skip the root
		 * binding, which the real kernel mount table already
		 * covers.  */
		bool has_extra_bindings = false;
		if (target_tracee->fs->bindings.guest != NULL) {
			Binding *b;
			for (b = CIRCLEQ_FIRST(target_tracee->fs->bindings.guest);
			     b != (void *) target_tracee->fs->bindings.guest;
			     b = CIRCLEQ_NEXT(b, link.guest)) {
				if (strcmp(b->guest.path, "/") != 0) {
					has_extra_bindings = true;
					break;
				}
			}
		}

		if (!is_android_data && !has_extra_bindings)
			return;

		/* Open real /proc/<PID>/mountinfo  */
		FILE *real_mountinfo_fp = fopen(path, "r");
		if (real_mountinfo_fp == NULL) {
			return;
		}

		/* Prepare faked mountinfo  */
		const char *new_path = create_temp_file(tracee->ctx, "mountinfo");
		FILE *new_mountinfo_fp = fopen(new_path, "w");
		if (new_mountinfo_fp == NULL) {
			fclose(real_mountinfo_fp);
			return;
		}

		char *line = NULL;
		size_t line_buf_len = 0;
		ssize_t line_len = 0;
		bool found_line = false;

		if (is_android_data) {
			while ((line_len = getline(&line, &line_buf_len, real_mountinfo_fp)) > 0) {
				char *chunk = line;
				/* Skip columns before 'root'  */
				for (int i = 0; i < 4 && chunk - line < line_len; i++) {
					chunk = strchr(chunk, ' ');
					if (chunk == NULL) goto end_line_scan;
					chunk++;
				}

				/* Match path  */
				char *chunk_end = strchr(chunk, ' ');
				if (chunk_end == NULL) continue;

				if (chunk_end - chunk == 5 && 0 == memcmp(chunk, "/data", 5)) {
					/* Write line into new file keeping only "/" from root column  */
					fwrite(line, chunk - line + 1, 1, new_mountinfo_fp);
					fwrite(chunk_end, line_len - (chunk_end - line), 1, new_mountinfo_fp);
					found_line = true;
					break;
				}
end_line_scan: ;
			}

			/* Once root was added, rescan and add other standard mounts  */
			if (found_line) {
				fseek(real_mountinfo_fp, 0, SEEK_SET);
				while ((line_len = getline(&line, &line_buf_len, real_mountinfo_fp)) > 0) {
					char *chunk = line;
					/* Skip columns before 'root'  */
					for (int i = 0; i < 4 && chunk - line < line_len; i++) {
						chunk = strchr(chunk, ' ');
						if (chunk == NULL) goto end_line_scan2;
						chunk++;
					}

					/* Match path  */
					char *chunk_end = strchr(chunk, ' ');
					if (chunk_end == NULL) continue;

					size_t mount_len = chunk_end - chunk;
					if (
							(mount_len == 4 && 0 == memcmp(chunk, "/dev", 4)) ||
							(mount_len >= 5 && 0 == memcmp(chunk, "/dev/", 5)) ||
							(mount_len == 5 && 0 == memcmp(chunk, "/proc", 5)) ||
							(mount_len == 4 && 0 == memcmp(chunk, "/sys", 4)) ||
							(mount_len >= 5 && 0 == memcmp(chunk, "/sys/", 5)) ||
							(mount_len == 4 && 0 == memcmp(chunk, "/tmp", 4))
							) {
						/* Copy line into new file verbatim  */
						fwrite(line, line_len, 1, new_mountinfo_fp);
					}
end_line_scan2: ;
				}
			}
			else {
				/* Some Android/proot combinations already present the
				 * kernel root as "/" instead of exposing a distinct
				 * "/data" mountpoint.  The old implementation still
				 * appended synthetic bindings, but then discarded the
				 * generated file because found_line stayed false.  Keep
				 * the real table verbatim in this case and append the
				 * emulated namespace below. */
				fseek(real_mountinfo_fp, 0, SEEK_SET);
				while ((line_len = getline(&line, &line_buf_len,
							   real_mountinfo_fp)) > 0)
					fwrite(line, line_len, 1, new_mountinfo_fp);
				found_line = true;
			}
		} else {
			/* Non-Android case: copy real mountinfo verbatim.  */
			while ((line_len = getline(&line, &line_buf_len, real_mountinfo_fp)) > 0)
				fwrite(line, line_len, 1, new_mountinfo_fp);
			found_line = true;
		}

		/* Append synthesized entries for runtime bindings so
		 * helpers like bubblewrap find the mounts they think
		 * they just created.  */
		append_runtime_binding_lines(target_tracee, new_mountinfo_fp);

		free(line);
		fclose(new_mountinfo_fp);
		fclose(real_mountinfo_fp);

		/* Redirect open to our temp file  */
		if (found_line) {
			strncpy(path, new_path, PATH_MAX - 1);
			path[PATH_MAX - 1] = '\0';
		}
		return;
	}

}

int mountinfo_callback(Extension *extension, ExtensionEvent event,
        intptr_t data1 UNUSED, intptr_t data2 UNUSED)
{
    switch (event) {
    case TRANSLATED_PATH:
	{
		Tracee *tracee = TRACEE(extension);
		Sysnum num = get_sysnum(tracee, ORIGINAL);
		if (num == PR_open || num == PR_openat) {
			mountinfo_check_open_path(tracee, (char*) data1);
		}
        return 0;
	}

    default:
        return 0;
    }
}
