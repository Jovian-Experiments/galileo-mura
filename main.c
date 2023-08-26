#include <stdio.h>
#include <errno.h>
#include <string.h>

#define MURA_BLOB_SIZE (2048 * 1024)

/*
 * No need to free anything on error paths.
 * The kernel will do it for us when we close.
 */
int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <mura out tar>\n", argv[0]);
        return 1;
    }

    const char *out_file_path = argv[1];

    /* Open the output file */
    FILE *out_file = fopen(out_file_path, "wb");
    if (!out_file) {
        fprintf(stderr, "Failed to open %s: %s\n", out_file_path, strerror(errno));
        return 1;
    }

    /* Open /dev/mem to see all of RAM (scary!) */
    FILE *mem_file = fopen("/dev/mem", "rb");
    if (!mem_file) {
        fprintf(stderr, "Failed to open /dev/mem: %s\n", strerror(errno));
        return 1;
    }

    /* Seek to where the mura blob is */
    if (fseek(mem_file, 0xFFAA0000, SEEK_SET) != 0) {
        fprintf(stderr, "Failed to seek memory: %s\n", strerror(errno));
        return 1;
    }

    /* Readback the mura blob */
    char mura_blob_buffer[MURA_BLOB_SIZE];
    if (fread(mura_blob_buffer, 1, MURA_BLOB_SIZE, mem_file) != MURA_BLOB_SIZE) {
        if (feof(mem_file))
            fprintf(stderr, "Error reading memory: Unexpected end of file\n");
        else if (ferror(mem_file))
            fprintf(stderr, "Error reading memory: %s\n", strerror(errno));
        return 1;
    }

    /* Close the file to memory (phew!) */
    if (fclose(mem_file) != 0) {
        fprintf(stderr, "Failed to close memory file: %s\n", strerror(errno));
        return 1;
    }
    mem_file = NULL;

    /* Write out our mura blob to the right file. */
    if (fwrite(mura_blob_buffer, 1, MURA_BLOB_SIZE, out_file) != MURA_BLOB_SIZE) {
        fprintf(stderr, "Failed to write to output file: %s\n", strerror(errno));
        return 1;
    }

    /* Close everything. */
    if (fclose(out_file) != 0) {
        fprintf(stderr, "Failed to close output file: %s\n", strerror(errno));
        return 1;
    }

    /* Hooray! */
    printf("Successfully written mura blob to: %s. My relief is almost palpable...\n", out_file_path);
    return 0;
}