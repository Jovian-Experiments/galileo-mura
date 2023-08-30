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

struct buffer {
    char *data;
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
        return (struct buffer) { NULL, 0 };
    }

    if (!size) {
        if (fseeko(file, 0, SEEK_END) != 0) {
            fprintf(stderr, "Failed to seek in %s: %s\n", path, strerror(errno));
            return (struct buffer) { NULL, 0 };
        }

        off_t fsize = ftello(file);
        off_t remaining_size = fsize - offset;
        if (remaining_size <= 0) {
            fprintf(stderr, "Tried to seek to an invalid offset in %s.\n", path);
            return (struct buffer) { NULL, 0 };
        }

        size = (size_t)remaining_size;
    }

    if (fseeko(file, offset, SEEK_SET) != 0) {
        fprintf(stderr, "Failed to seek in %s: %s\n", path, strerror(errno));
        return (struct buffer) { NULL, 0 };
    }

    struct buffer buf = { (char *)malloc(size + 1), size };
    if ((size = fread(buf.data, 1, buf.size, file)) <= 0) {
        if (!feof(file)) {
            if (ferror(file))
                fprintf(stderr, "Error reading %s: %s\n", path, strerror(errno));
            else
                fprintf(stderr, "Error reading %s: Unknown error\n", path);
            return (struct buffer) { NULL, 0 };
        }
    }
    buf.size = size;
    /* Always null terminate incase we treat this as a string at some point */
    buf.data[size] = '\0';

    if (fclose(file) != 0) {
        fprintf(stderr, "Failed to close file %s: %s\n", path, strerror(errno));
        return (struct buffer) { NULL, 0 };
    }

    return buf;
}

static int write_file(const char *path, const char *mode, struct buffer data) {
    FILE *file = fopen(path, mode);
    if (!file) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (fwrite(data.data, 1, data.size, file) != data.size) {
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

    if (strncmp(vendor.data, "Valve\n", vendor.size)) {
        fprintf(stderr, "Vendor didn't match. Was: %.*s Expected: %s\n", (int)vendor.size, vendor.data, "Valve\n");
        return 1;
    }

    if (strncmp(product.data, "Galileo\n", product.size)) {
        fprintf(stderr, "Product didn't match. Was: %.*s Expected: %s\n", (int)product.size, product.data, "Galileo\n");
        return 1;
    }

    /*
     * Grab display serial from dp aux.
     * We only do mura correction on SDC screens, which have a serial of length 5.
     */
    struct buffer display_serial = read_file("/dev/drm_dp_aux0", "rb", 864, 5);
    if (display_serial.size < 5) {
        fprintf(stderr, "Failed to get display serial.\n");
        return 1;
    }

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
    printf("Vendor: %.*s", (int)vendor.size, vendor.data);
    printf("Product: %.*s", (int)product.size, product.data);
    printf("Display Serial: %1X%1X%1X%1X%1X\n",
        (unsigned int)display_serial.data[0] & 0xFFu,
        (unsigned int)display_serial.data[1] & 0xFFu,
        (unsigned int)display_serial.data[2] & 0xFFu,
        (unsigned int)display_serial.data[3] & 0xFFu,
        (unsigned int)display_serial.data[4] & 0xFFu);
    return 0;
}