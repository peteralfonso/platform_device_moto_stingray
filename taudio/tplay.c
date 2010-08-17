#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/ioctl.h>

#define FAILIF(x, ...) do if (x) { \
    fprintf(stderr, __VA_ARGS__);  \
    exit(EXIT_FAILURE);            \
} while (0)

static char buffer[4096];

int
main(int argc, char *argv[])
{
    int ifd, ofd;
    int nr, nw;

    argc--, argv++;
    FAILIF(!argc, "Expecting a file to play!\n");

    printf("file to play: [%s]\n", *argv);

    ifd = open(*argv, O_RDONLY);
    FAILIF(ifd < 0, "could not open %s: %s\n", *argv, strerror(errno));

    ofd = open("/dev/audio0_out", O_RDWR);
    FAILIF(ifd < 0, "could not open output: %s\n", strerror(errno));

    do {
        nr = read(ifd, buffer, sizeof(buffer));
        if (!nr) {
            printf("EOF\n");
            break;
        }
        FAILIF(nr < 0, "Could not read from %s: %s\n", *argv, strerror(errno));
        nw = write(ofd, buffer, nr);
        FAILIF(nw < 0, "Could not copy to output: %s\n", strerror(errno));
        FAILIF(nw != nr, "Mismatch nw = %d nr = %d\n", nw, nr);
    } while (1);

    return 0;
}

