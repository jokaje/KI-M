#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "../firmware/main/pixel_avatar.h"
int main(int argc, char **argv) {
    assert(argc == 4);
    FILE *pack = fopen(argv[1], "rb"), *masks = fopen(argv[2], "rb");
    assert(pack && masks);
    static uint16_t src[4096], dst[192 * 192];
    static uint8_t mask[512];
    assert(fseek(pack, 45 * 8192, SEEK_SET) == 0);
    assert(fseek(masks, 45 * 512, SEEK_SET) == 0);
    assert(fread(src, 1, sizeof(src), pack) == sizeof(src));
    assert(fread(mask, 1, sizeof(mask), masks) == sizeof(mask));
    fclose(pack); fclose(masks);
    kim_pixel_compose(dst, src, mask, 0x10c5);
    FILE *out = fopen(argv[3], "wb"); assert(out);
    assert(fwrite(dst, 1, sizeof(dst), out) == sizeof(dst));
    fclose(out);
    return 0;
}
