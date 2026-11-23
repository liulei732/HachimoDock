/* Rasterize the requested codepoints from the official MiSans Medium OTF.
 * Stream u32 codepoints on stdin and tightly packed 8-bit coverage on stdout. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <stdio.h>
int main(int argc, char **argv) {
    FT_Library library;
    FT_Face face;
    if (argc != 2 || FT_Init_FreeType(&library) ||
        FT_New_Face(library, argv[1], 0, &face) || FT_Set_Pixel_Sizes(face, 0, 32)) return 1;
    uint32_t cp;
    while (fread(&cp, sizeof(cp), 1, stdin) == 1) {
        unsigned id = FT_Get_Char_Index(face, cp);
        if (!id || FT_Load_Glyph(face, id, FT_LOAD_FORCE_AUTOHINT | FT_LOAD_TARGET_LIGHT) ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return 1;
        FT_GlyphSlot glyph = face->glyph;
        FT_Bitmap *b = &glyph->bitmap;
        if (b->width > 127 || b->rows > 127 || (b->width && b->pixel_mode != FT_PIXEL_MODE_GRAY)) return 1;
        int8_t metrics[] = {(int8_t)b->width, (int8_t)b->rows,
                            (int8_t)glyph->bitmap_left, (int8_t)(glyph->bitmap_top-(int)b->rows)};
        if (fwrite(metrics, 1, sizeof(metrics), stdout) != sizeof(metrics)) return 1;
        for (unsigned y = 0; b->width && y < b->rows; y++)
            if (fwrite(b->buffer+y*b->pitch, 1, b->width, stdout) != b->width) return 1;
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return ferror(stdin) || fflush(stdout);
}
