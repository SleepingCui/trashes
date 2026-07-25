#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <stdint.h>

#define CHUNK_SIZE 16384
#define MAGIC_SIZE 4
#define MAX_STRING_LEN 1024

int read_string(char* buffer, int max_len, FILE* fp) {
    int length = 0;
    int shift = 0;
    uint8_t b;
    
    do {
        if (fread(&b, 1, 1, fp) != 1) return -1;
        length |= (b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    
    if (length >= max_len) {
        fprintf(stderr, "String too long: %d\n", length);
        return -1;
    }
    
    if (length > 0) {
        if (fread(buffer, 1, length, fp) != (size_t)length) return -1;
    }
    buffer[length] = '\0';
    
    return length;
}

double read_double(FILE* fp) {
    uint64_t val;
    if (fread(&val, 8, 1, fp) != 1) {
        fprintf(stderr, "Failed to read double\n");
        exit(1);
    }
    return *(double*)&val;
}

int32_t read_int32(FILE* fp) {
    int32_t val;
    if (fread(&val, 4, 1, fp) != 1) {
        fprintf(stderr, "Failed to read int32\n");
        exit(1);
    }
    return val;
}

int64_t read_int64(FILE* fp) {
    int64_t val;
    if (fread(&val, 8, 1, fp) != 1) {
        fprintf(stderr, "Failed to read int64\n");
        exit(1);
    }
    return val;
}

void json_escape(const char* src, char* dst, int max_len) {
    int j = 0;
    for (int i = 0; src[i] && j < max_len - 2; i++) {
        switch (src[i]) {
            case '"':  j += snprintf(dst + j, max_len - j, "\\\""); break;
            case '\\': j += snprintf(dst + j, max_len - j, "\\\\"); break;
            case '\n': j += snprintf(dst + j, max_len - j, "\\n"); break;
            case '\r': j += snprintf(dst + j, max_len - j, "\\r"); break;
            case '\t': j += snprintf(dst + j, max_len - j, "\\t"); break;
            default:   dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.bin.gz> [output.json]\n", argv[0]);
        return 1;
    }

    const char* input_file = argv[1];
    const char* output_file = (argc >= 3) ? argv[2] : NULL;

    gzFile gz = gzopen(input_file, "rb");
    if (!gz) {
        fprintf(stderr, "Failed to open file: %s\n", input_file);
        return 1;
    }

    FILE* tmp = tmpfile();
    if (!tmp) {
        fprintf(stderr, "Failed to create temp file\n");
        gzclose(gz);
        return 1;
    }

    char buf[CHUNK_SIZE];
    int bytes_read;
    while ((bytes_read = gzread(gz, buf, CHUNK_SIZE)) > 0) {
        fwrite(buf, 1, bytes_read, tmp);
    }
    gzclose(gz);
    rewind(tmp);

    char magic[MAGIC_SIZE];
    if (fread(magic, 1, MAGIC_SIZE, tmp) != MAGIC_SIZE || 
        memcmp(magic, "TSGZ", MAGIC_SIZE) != 0) {
        fprintf(stderr, "Invalid file format (magic mismatch)\n");
        fclose(tmp);
        return 1;
    }

    uint8_t version;
    if (fread(&version, 1, 1, tmp) != 1) {
        fprintf(stderr, "Failed to read version\n");
        fclose(tmp);
        return 1;
    }
    fprintf(stderr, "Format version: %d\n", version);

    int64_t timestamp = read_int64(tmp);
    char song_name[MAX_STRING_LEN];
    char level_path[MAX_STRING_LEN];
    if (read_string(song_name, MAX_STRING_LEN, tmp) < 0 ||
        read_string(level_path, MAX_STRING_LEN, tmp) < 0) {
        fprintf(stderr, "Failed to read strings\n");
        fclose(tmp);
        return 1;
    }

    FILE* out = output_file ? fopen(output_file, "w") : stdout;
    if (!out) {
        fprintf(stderr, "Failed to open output file: %s\n", output_file);
        fclose(tmp);
        return 1;
    }

    char escaped_song[MAX_STRING_LEN * 2];
    char escaped_path[MAX_STRING_LEN * 2];
    json_escape(song_name, escaped_song, sizeof(escaped_song));
    json_escape(level_path, escaped_path, sizeof(escaped_path));

    fprintf(out, "{\n");
    fprintf(out, "  \"songName\": \"%s\",\n", escaped_song);
    fprintf(out, "  \"levelPath\": \"%s\",\n", escaped_path);
    fprintf(out, "  \"timestamp\": %ld,\n", (long)timestamp);
    fprintf(out, "  \"offsets\": [");

    int first = 1;
    long record_count = 0;
    while (1) {
        double timing;
        int32_t margin_code;

        if (fread(&timing, 8, 1, tmp) != 1) break;
        if (fread(&margin_code, 4, 1, tmp) != 1) break;

        if (!first) {
            fprintf(out, ",");
        }
        first = 0;

        fprintf(out, "[%.4f,%d]", timing, margin_code);
        record_count++;

        if (record_count % 10 == 0) {
            fprintf(out, "\n    ");
        }
    }

    fprintf(out, "]\n");
    fprintf(out, "}\n");

    fprintf(stderr, "Converted %ld records\n", record_count);

    if (output_file) fclose(out);
    fclose(tmp);

    return 0;
}