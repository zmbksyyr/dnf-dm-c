// Read-only resource probe: call the production PVF and NPK parsers without a GUI/DB.
#include "pvf.h"
#include "npk.h"

static void hex_text(FILE *f, const char *s) {
    for (const u8 *p = (const u8 *)s; *p; p++) fprintf(f, "%02x", *p);
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    Pvf p = {0};
    PvfProg prog = {0};
    Err err = {0};
    u64 start = now_ms();
    if (!pvf_open(&p, argv[2], prog, err)) {
        fprintf(stderr, "%s\n", err);
        pvf_close(&p);
        return 1;
    }
    u64 load_ms = now_ms() - start;
    int ok = 0;
    if (!strcmp(argv[1], "extract") && argc == 5) {
        Buf b = {0};
        const Leaf *lf = pvf_find(&p, argv[3]);
        ok = lf && pvf_read(&p, lf, &b) && file_write(argv[4], b.p, b.n);
        buf_free(&b);
    } else if (!strcmp(argv[1], "inspect") && (argc == 4 || argc == 5)) {
        FILE *f = fopen(argv[3], "wb");
        if (!f) { pvf_close(&p); return 1; }
        fprintf(f, "META\t%d\t%d\t%u\t%llu\n", p.nleaf, p.items.n, p.str_cp,
                (unsigned long long)load_ms);
        for (int i = 0; i < p.nexp; i++) fprintf(f, "EXP\t%d\t%u\n", i, p.exp[i]);
        for (int i = 0; i < p.nleaf; i++) {
            const Leaf *lf = &p.leafs[i];
            fprintf(f, "LEAF\t%u\t%u\t%u\t", lf->len_raw, lf->crc, lf->off);
            hex_text(f, lf->path);
            fputc('\n', f);
            int stride = p.nleaf / 128;
            if (stride < 1) stride = 1;
            if (i % stride == 0 || !strcmp(lf->path, "character/exptable.tbl") ||
                !strcmp(lf->path, "stringtable.bin")) {
                Buf data = {0};
                if (!pvf_read(&p, lf, &data)) { fclose(f); pvf_close(&p); return 1; }
                char md5[33];
                hash_md5(data.p, data.n, md5);
                fprintf(f, "HASH\t%s\t", md5);
                hex_text(f, lf->path);
                fputc('\n', f);
                buf_free(&data);
            }
        }
        Npk npk = {0};
        if (argc == 5) {
            start = now_ms();
            ok = npk_build(&npk, argv[4], NULL, NULL, 0);
            fprintf(f, "NPK\t%d\t%d\t%d\t%llu\n", ok, npk.nfiles, npk.nents,
                    (unsigned long long)(now_ms() - start));
        }
        int samples[PK_CREATURE_EQUIP + 1] = {0};
        for (int i = 0; i < p.items.n; i++) {
            u32 id = p.items.key[i];
            int kind = pvf_item_kind(&p, id);
            char path[512];
            pvf_item_path(&p, id, path, sizeof(path));
            fprintf(f, "ITEM\t%u\t%d\t%u\t", id, kind, pvf_item_stack_limit(&p, id));
            hex_text(f, pvf_item_name(&p, id));
            fputc('\t', f);
            hex_text(f, path);
            fputc('\n', f);
            if (kind < 0 || kind > PK_CREATURE_EQUIP || samples[kind] >= 12 || !*pvf_item_name(&p, id)) continue;
            char img[512] = "";
            int frame = 0, w = 0, h = 0;
            int has_icon = pvf_item_icon(&p, id, img, sizeof(img), &frame);
            HBITMAP bmp = has_icon && npk.ok ? npk_icon(&npk, img, frame, &w, &h) : NULL;
            Str detail = {0};
            int has_detail = pvf_item_detail(&p, id, &detail);
            fprintf(f, "SAMPLE\t%u\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\n",
                    id, kind, has_icon, img, frame, bmp != NULL, w, h, has_detail, detail.n);
            if (bmp) DeleteObject(bmp);
            str_free(&detail);
            samples[kind]++;
        }
        npk_free(&npk);
        ok = !ferror(f);
        if (fclose(f) != 0) ok = 0;
    }
    printf("%s leaves=%d items=%d exp_entries=%d load_ms=%llu\n", ok ? "OK" : "FAIL",
           p.nleaf, p.items.n, p.nexp, (unsigned long long)load_ms);
    pvf_close(&p);
    return ok ? 0 : 1;
}
