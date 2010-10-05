#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <linux/tegra_audio.h>

#define FAILIF(x, ...) do if (x) { \
    fprintf(stderr, __VA_ARGS__);  \
    exit(EXIT_FAILURE);            \
} while (0)

int
main(int argc, char *argv[])
{
    int ifd, ofd, ofd_c;
    int nr, nw;
    struct tegra_audio_out_preload preload;
    struct tegra_audio_buf_config config;
    char *buffer;
    int len;
    struct tegra_audio_error_counts errors, errors_tot;

    argc--, argv++;
    FAILIF(!argc, "Expecting a file to play!\n");

    printf("file to play: [%s]\n", *argv);

    ifd = open(*argv, O_RDONLY);
    FAILIF(ifd < 0, "could not open %s: %s\n", *argv, strerror(errno));

    ofd = open("/dev/audio0_out", O_RDWR);
    FAILIF(ofd < 0, "could not open output: %s\n", strerror(errno));

    ofd_c = open("/dev/audio0_out_ctl", O_RDWR);
    FAILIF(ofd_c < 0, "could not open output control: %s\n", strerror(errno));

    FAILIF(ioctl(ofd_c, TEGRA_AUDIO_OUT_GET_BUF_CONFIG, &config) < 0,
           "Could not get output config: %s\n", strerror(errno));

    len = 1 << config.size;
    buffer = malloc(len);
    FAILIF(!buffer, "Could not allocate %d bytes!\n", len);

    memset(&errors_tot, 0, sizeof(errors_tot));
    do {
        nr = read(ifd, buffer, len);
        if (!nr) {
            printf("EOF\n");
            break;
        }
        FAILIF(nr < 0, "Could not read from %s: %s\n", *argv, strerror(errno));
        nw = write(ofd, buffer, nr);
        FAILIF(nw < 0, "Could not copy to output: %s\n", strerror(errno));
        FAILIF(nw != nr, "Mismatch nw = %d nr = %d\n", nw, nr);

        FAILIF(ioctl(ofd_c, TEGRA_AUDIO_OUT_GET_ERROR_COUNT, &errors) < 0,
               "Could not get error count: %s\n", strerror(errno));

        if (errors.late_dma || errors.full_empty) {
            printf("out %d (%d late, %d underrun errors)\n", nw,
                   errors.late_dma, errors.full_empty);
            errors_tot.late_dma += errors.late_dma;
            errors_tot.full_empty += errors.full_empty;
        }
        else
            printf("out %d\n", nw);

    } while (1);

    printf("played with %d late, %d underflow errors\n",
           errors.late_dma, errors.full_empty);
    return 0;
}

