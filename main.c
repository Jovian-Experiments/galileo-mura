#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <sys/types.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MURA_BLOB_SIZE (2048 * 1024)

#define PANEL_UNIQUE_ID_BASE (0x0360)
#define PANEL_TDM            (0x0370)

struct buffer {
    union {
        char *s8;
        unsigned char *u8;
    } data;

    size_t size;
};

/*
 * No need to free anything on error paths.
 * The kernel will do it for us when we close.
 */

static struct buffer read_file(const char *path, const char *mode, off_t offset, size_t size) {
    FILE *file = fopen(path, mode);
    if (!file) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return (struct buffer) { { NULL }, 0 };
    }

    if (!size) {
        if (fseeko(file, 0, SEEK_END) != 0) {
            fprintf(stderr, "Failed to seek in %s: %s\n", path, strerror(errno));
            return (struct buffer) { { NULL }, 0 };
        }

        off_t fsize = ftello(file);
        off_t remaining_size = fsize - offset;
        if (remaining_size <= 0) {
            fprintf(stderr, "Tried to seek to an invalid offset in %s.\n", path);
            return (struct buffer) { { NULL }, 0 };
        }

        size = (size_t)remaining_size;
    }

    if (fseeko(file, offset, SEEK_SET) != 0) {
        fprintf(stderr, "Failed to seek in %s: %s\n", path, strerror(errno));
        return (struct buffer) { { NULL }, 0 };
    }

    struct buffer buf = { { (char *)malloc(size + 1) }, size };
    if ((size = fread(buf.data.s8, 1, buf.size, file)) <= 0) {
        if (!feof(file)) {
            if (ferror(file))
                fprintf(stderr, "Error reading %s: %s\n", path, strerror(errno));
            else
                fprintf(stderr, "Error reading %s: Unknown error\n", path);
            return (struct buffer) { { NULL }, 0 };
        }
    }
    buf.size = size;
    /* Always null terminate incase we treat this as a string at some point */
    buf.data.s8[size] = '\0';

    if (fclose(file) != 0) {
        fprintf(stderr, "Failed to close file %s: %s\n", path, strerror(errno));
        return (struct buffer) { { NULL }, 0 };
    }

    return buf;
}

static int write_file(const char *path, const char *mode, struct buffer data) {
    FILE *file = fopen(path, mode);
    if (!file) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (fwrite(data.data.s8, 1, data.size, file) != data.size) {
        fprintf(stderr, "Failed to write to file %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (fclose(file) != 0) {
        fprintf(stderr, "Failed to close file %s: %s\n", path, strerror(errno));
        return 1;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    /*
     * Don't take in an arg to where this is written,
     * as this is run as root, so could write anywhere based on
     * user input... Ough.
     */
    const char *out_file_path = "/tmp/mura/blob.tar";

    /* Check euid is root */
    if (geteuid() != 0) {
        fprintf(stderr, "Must be ran as root via suid.\n");
        return 1;
    }

    /* Get real uid and gid to get ownership of mura tar. */
    uid_t real_uid = getuid();
    gid_t real_gid = getgid();

    /* Check we are on Galileo */
    struct buffer vendor = read_file("/sys/devices/virtual/dmi/id/sys_vendor", "r", 0, 0);
    if (!vendor.size)
        return 1;
    struct buffer product = read_file("/sys/devices/virtual/dmi/id/product_name", "r", 0, 0);
    if (!product.size)
        return 1;
    struct buffer deck_serial = read_file("/sys/devices/virtual/dmi/id/product_serial", "r", 0, 0);
    if (!product.size)
        return 1;

    if (strncmp(vendor.data.s8, "Valve\n", vendor.size)) {
        fprintf(stderr, "Vendor didn't match. Was: %.*s Expected: %s\n", (int)vendor.size, vendor.data.s8, "Valve\n");
        return 1;
    }

    if (strncmp(product.data.s8, "Galileo\n", product.size)) {
        fprintf(stderr, "Product didn't match. Was: %.*s Expected: %s\n", (int)product.size, product.data.s8, "Galileo\n");
        return 1;
    }

    /* Grab display serial from dp aux. */
    struct buffer display_serial = read_file("/dev/drm_dp_aux0", "rb", PANEL_UNIQUE_ID_BASE, 12);
    if (display_serial.size < 12) {
        fprintf(stderr, "Failed to get display serial.\n");
        return 1;
    }

    /*
     * SDC serials are only 5 characters long, HEX coded.
     * BOE serials start with SED and are 12 characters, ASCII.
     */
    bool is_boe = display_serial.data.s8[0] == 'S'
               && display_serial.data.s8[1] == 'E'
               && display_serial.data.s8[2] == 'D'
               && (display_serial.data.s8[5]  != '\0' || display_serial.data.s8[6]  != '\0'
                || display_serial.data.s8[7]  != '\0' || display_serial.data.s8[8]  != '\0'
                || display_serial.data.s8[9]  != '\0' || display_serial.data.s8[10] != '\0'
                || display_serial.data.s8[11] != '\0');

    int is_antiglare = -1;
    if (is_boe)
        is_antiglare = display_serial.data.s8[3] == 'S' ? 1 : 0;

#ifdef DEBUG_LOG_TDM
    struct buffer tdm_blob = read_file("/dev/drm_dp_aux0", "rb", PANEL_TDM, 2);
    if (tdm_blob.size < 2) {
        fprintf(stderr, "Failed to get display tdm.\n");
        return 1;
    }

    int gamma_cal_pts = 1;
    if (is_boe) {
        /* BOE */
        if (tdm_blob.data.u8[0] == 0xDAu && tdm_blob.data.u8[1] == 0x14u)
            gamma_cal_pts = 2;
    } else {
        /* SDC */
        if (tdm_blob.data.u8[0] == 0x04u && tdm_blob.data.u8[1] == 0x75u)
            gamma_cal_pts = 2;
    }
#endif

    /* Grab the mura blob from our mapped bios region. */
    struct buffer mura_blob = read_file("/dev/mem", "rb", 0xFFAA0000, MURA_BLOB_SIZE);
    if (mura_blob.size != MURA_BLOB_SIZE) {
        fprintf(stderr, "Failed to get mura blob.\n");
        return 1;
    }

    /* Write that out to the blob location. */
    if (write_file(out_file_path, "wb", mura_blob)) {
        fprintf(stderr, "Failed to write mura blob.\n");
        return 1;
    }

    if (chown(out_file_path, real_uid, real_gid) != 0) {
        fprintf(stderr, "Failed to set permissions for mura blob.\n");
        return 1;
    }

    /* Hooray! */
    printf("Success! My relief is almost palpable...\n");
    printf("Mura Blob Path: %s\n", out_file_path);
    /* Valve and Galileo already have newlines in the dmi vendor/product. Heh... */
    printf("Vendor: %.*s", (int)vendor.size, vendor.data.s8);
    printf("Product: %.*s", (int)product.size, product.data.s8);
    printf("Deck Serial: %.*s", (int)deck_serial.size, deck_serial.data.s8);
    printf("Manufacturer: %s\n", is_boe ? "BOE" : "SDC");
#ifdef DEBUG_LOG_TDM
    printf("TDM: %02hhX%02hhX\n", tdm_blob.data.u8[0], tdm_blob.data.u8[1]);
    printf("Gamma Calibration Points: %d\n", gamma_cal_pts);
#endif
    printf("Anti-glare: %s\n", is_antiglare == -1 ? "Unknown" : is_antiglare ? "Yes" : "No");
    if (is_boe) {
        printf("Display Serial: %.*s\n", 12, (const char *)display_serial.data.s8);
    } else {
        printf("Display Serial: %02hhX%02hhX%02hhX%02hhX%02hhX\n",
            display_serial.data.u8[0],
            display_serial.data.u8[1],
            display_serial.data.u8[2],
            display_serial.data.u8[3],
            display_serial.data.u8[4]);
    }
    return 0;
}