// SPDX-License-Identifier: GPL-2.0
// comwifi-attach: put a serial port into raw 115200 8N1 and bind the comwifi
// line discipline to it.  Stays in the foreground holding the tty open; the
// kernel module takes it from there (including baud negotiation).
//
//   comwifi-attach [-f] /dev/ttyUSB0
//     -f   enable RTS/CTS hardware flow control (wire it, or the link stalls)
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <linux/serial.h>

#ifndef N_DEVELOPMENT
#define N_DEVELOPMENT 29
#endif

int main(int argc, char **argv)
{
	int flow = 0, c;

	while ((c = getopt(argc, argv, "f")) != -1) {
		if (c == 'f')
			flow = 1;
		else
			goto usage;
	}
	if (optind + 1 != argc) {
usage:
		fprintf(stderr, "usage: %s [-f] /dev/ttyX\n", argv[0]);
		return 2;
	}

	int fd = open(argv[optind], O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	struct termios t;
	if (tcgetattr(fd, &t)) {
		perror("tcgetattr");
		return 1;
	}
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD | CS8;
	t.c_cflag &= ~(PARENB | CSTOPB);
	if (flow)
		t.c_cflag |= CRTSCTS;
	else
		t.c_cflag &= ~CRTSCTS;
	cfsetspeed(&t, B115200);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &t)) {
		perror("tcsetattr");
		return 1;
	}

	// USB-UART bridges buffer RX for up to 16 ms by default
	struct serial_struct ss;
	if (!ioctl(fd, TIOCGSERIAL, &ss)) {
		ss.flags |= ASYNC_LOW_LATENCY;
		ioctl(fd, TIOCSSERIAL, &ss);
	}

	tcflush(fd, TCIOFLUSH);
	int ldisc = N_DEVELOPMENT;
	if (ioctl(fd, TIOCSETD, &ldisc)) {
		perror("TIOCSETD (is comwifi.ko loaded? are you root?)");
		return 1;
	}
	fprintf(stderr, "comwifi attached to %s\n", argv[optind]);

	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_BLOCK, &set, NULL);
	int sig;
	sigwait(&set, &sig);

	ldisc = 0;	// N_TTY
	ioctl(fd, TIOCSETD, &ldisc);
	return 0;
}
