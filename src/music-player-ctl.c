/* music-player-ctl: control a running music-player from anywhere (window
 * manager keys, scripts). See ctl.c for the commands. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void usage(void)
{
	fputs("usage: music-player-ctl play|pause|toggle|stop|next|prev|vol+|vol-|vol N|\n"
	      "                        seek+|seek-|seek N|order NAME|stop-after|status|show|fullscreen|search TEXT|rescan|\n"
	      "                        add PATH|open PATH|quit\n",
	      stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))
		usage();
	char line[4096] = "";
	for (int i = 1; i < argc; i++) {
		if (i > 1)
			strncat(line, " ", sizeof line - strlen(line) - 1);
		/* Paths are made absolute: the player has another working directory. */
		if (i == 2 && (!strcmp(argv[1], "add") || !strcmp(argv[1], "open"))) {
			char *abs = realpath(argv[i], NULL);
			strncat(line, abs ? abs : argv[i], sizeof line - strlen(line) - 1);
			free(abs);
		} else {
			strncat(line, argv[i], sizeof line - strlen(line) - 1);
		}
	}
	strncat(line, "\n", sizeof line - strlen(line) - 1);

	const char *rt = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s/music-player.sock", rt && *rt ? rt : "/tmp");
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		fputs("music-player is not running\n", stderr);
		return 1;
	}
	if (write(fd, line, strlen(line)) < 0) {
		perror("music-player-ctl");
		return 1;
	}
	char buf[1024];
	ssize_t n;
	int status = 0;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		if (!strncmp(buf, "error", 5))
			status = 1;
		fwrite(buf, 1, (size_t)n, stdout);
	}
	close(fd);
	return status;
}
