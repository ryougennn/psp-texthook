/*
 * The popup dictionaries: lookups in the .bin files in
 * ms0:/SEPLUGINS/texthook/dicts/ (tools/gen_dict.py, tools/yomitan2dict.py),
 * deinflection, and longest-match tokenizing.
 *
 * Every dictionary there is searched, in file name order (1-jmdict.bin
 * before 2-vndb-names.bin), and its entries are listed in that order. They
 * stay on the memory stick (JMdict is 28 MB): a lookup is two or three small
 * reads. Only their key filters (up to 256 KB each) are held in memory, and
 * only while the popup is open; they answer most misses, which is most of
 * what tokenizing asks, without touching the stick.
 *
 * Deinflection works like rikaichan's: rules turn an inflected ending back
 * into a plainer one (読んだ -> 読む, 食べなかった -> 食べない -> 食べる), each
 * result tagged with the word class it must have. A candidate only counts
 * if a word of that class exists under it.
 *
 * Popup thread only.
 */

#include <pspkernel.h>
#include <pspsysmem.h>

#include "popup.h"
#include "mini.h"

#define DICT_DIR        "ms0:/SEPLUGINS/texthook/dicts"
#define DICT_VERSION    1
#define MAX_DICTS       4

#define MAX_MATCH       12          /* code points in the longest word looked up */
#define MAX_POSTINGS    2048        /* bytes of one bucket's postings */
#define MAX_WORD        48          /* bytes of a candidate word */
#define MAX_CANDIDATES  48

/* Word classes. The first five are the part-of-speech bits in the files. */
#define T_V1            0x0001      /* ichidan verb */
#define T_V5            0x0002      /* godan verb */
#define T_VK            0x0004      /* 来る */
#define T_VS            0x0008      /* する */
#define T_ADJ           0x0010      /* i-adjective; also anything ending in ない or たい */
#define T_MASU          0x0020      /* a ます form */
#define T_TE            0x0040      /* a て form */
#define T_TA            0x0080      /* a た form */
#define T_ANY           0xFFFF      /* the text itself */

#define T_WORDS         (T_V1 | T_V5 | T_VK | T_VS | T_ADJ)

/*
 * Refs: in the files (tools/gen_dict.py), a record's offset, its
 * part-of-speech bits and a reading flag. Once looked up, the
 * part-of-speech bits are replaced by the dictionary's index.
 */
#define REF_OFFSET(ref)     ((ref) & 0x01FFFFFF)
#define REF_POS(ref)        (((ref) >> 25) & 0x1F)  /* in the files */
#define REF_INDEX(ref)      (((ref) >> 25) & 0x1F)  /* once looked up */
#define REF_POS_BITS        0x3E000000u
#define REF_READING(ref)    ((ref) >> 31)           /* matched one of its readings */
#define REF_SAME(a, b)      (((a) & 0x3FFFFFFF) == ((b) & 0x3FFFFFFF))

typedef struct {
    char magic[4];
    u32  version, nbuckets, filter_log2;
    u32  off_filter, off_buckets, off_postings, off_records;
} dict_header;

typedef struct {
    SceUID      fd, filter_block;
    const u8    *filter;
    dict_header hdr;
    char        title[64];          /* from the file, else its name */
} dict;

static dict dicts[MAX_DICTS];
static int  ndicts;

/* --- files --------------------------------------------------------------- */

static int read_at(dict *d, u32 off, void *buf, int len)
{
    if (sceIoLseek32(d->fd, off, PSP_SEEK_SET) != (int)off)
        return -1;
    return sceIoRead(d->fd, buf, len);
}

static void close_one(dict *d)
{
    if (d->filter_block >= 0)
        sceKernelFreePartitionMemory(d->filter_block);
    if (d->fd >= 0)
        sceIoClose(d->fd);
    d->filter_block = -1;
    d->filter       = NULL;
    d->fd           = -1;
}

static int open_one(dict *d, const char *name)
{
    char path[128];

    mini_snprintf(path, sizeof(path), "%s/%s", DICT_DIR, name);
    d->filter_block = -1;
    d->fd           = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (d->fd < 0) {
        th_log("dict: can't open %s (%08x)", path, d->fd);
        return -1;
    }

    dict_header *h = &d->hdr;

    if (read_at(d, 0, h, sizeof(*h)) != sizeof(*h) || memcmp(h->magic, "THKD", 4) ||
        h->version != DICT_VERSION || h->filter_log2 < 16 || h->filter_log2 > 24 ||
        h->off_filter < sizeof(*h)) {
        th_log("dict: %s isn't a version %d dictionary; rebuild it", name, DICT_VERSION);
        close_one(d);
        return -1;
    }

    /* the title, if the file has one between the header and the filter */
    int tlen = h->off_filter - sizeof(*h);

    if (tlen > (int)sizeof(d->title) - 1)
        tlen = sizeof(d->title) - 1;
    memset(d->title, 0, sizeof(d->title));
    if (!tlen || read_at(d, sizeof(*h), d->title, tlen) != tlen || !d->title[0])
        strncpy0(d->title, name, sizeof(d->title));

    u32 size = 1u << (h->filter_log2 - 3);

    d->filter_block = sceKernelAllocPartitionMemory(TH_PARTITION, "thk_dict", PSP_SMEM_High,
                                                    size, NULL);
    if (d->filter_block < 0) {
        th_log("dict: no memory for the key filter of %s (%08x)", name, d->filter_block);
        close_one(d);
        return -1;
    }
    d->filter = sceKernelGetBlockHeadAddr(d->filter_block);

    int n = read_at(d, h->off_filter, (void *)d->filter, size);
    if (n != (int)size) {
        th_log("dict: %s: key filter: read %d of %lu bytes", name, n, size);
        close_one(d);
        return -1;
    }
    th_log("dict: %s (%s), %lu KB filter", name, d->title, size / 1024);
    return 0;
}

int th_dict_open(void)
{
    static SceIoDirent ent;
    char               names[MAX_DICTS][64];
    int                n = 0;

    ndicts = 0;
    SceUID dir = sceIoDopen(DICT_DIR);
    if (dir < 0) {
        th_log("dict: no %s folder (%08x)", DICT_DIR, dir);
        return -1;
    }

    /* the *.bin files, sorted by name */
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(dir, &ent) <= 0)
            break;

        int len = strlen(ent.d_name);
        if (len < 5 || len >= (int)sizeof(names[0]) ||
            strncasecmp(ent.d_name + len - 4, ".bin", 4))
            continue;
        if (n == MAX_DICTS) {
            th_log("dict: more than %d dictionaries; ignoring %s", MAX_DICTS, ent.d_name);
            continue;
        }

        int at = n++;
        while (at > 0 && strcmp(names[at - 1], ent.d_name) > 0) {
            memcpy(names[at], names[at - 1], sizeof(names[0]));
            at--;
        }
        strncpy0(names[at], ent.d_name, sizeof(names[0]));
    }
    sceIoDclose(dir);

    for (int i = 0; i < n; i++)
        if (open_one(&dicts[ndicts], names[i]) == 0)
            ndicts++;
    if (!ndicts)
        th_log("dict: no dictionaries in %s", DICT_DIR);
    return ndicts ? 0 : -1;
}

/* Partition blocks outlive the module: always free them. */
void th_dict_close(void)
{
    for (int i = 0; i < ndicts; i++)
        close_one(&dicts[i]);
    ndicts = 0;
}

/* The title of the dictionary a looked-up ref is from. */
static const char *dict_title(u32 ref)
{
    return REF_INDEX(ref) < (u32)ndicts ? dicts[REF_INDEX(ref)].title : "";
}

/* --- lookup -------------------------------------------------------------- */

/*
 * Records for the (already folded) key in dictionary `di`, most common
 * first. `types` limits them to words of those classes (T_ANY: all).
 * Returns how many were put in refs, with the dictionary's index in place
 * of the part-of-speech bits (see REF_INDEX).
 */
static int lookup(int di, const char *key, int len, u32 types, u32 *refs, int max)
{
    static u8 postings[MAX_POSTINGS];
    dict      *d  = &dicts[di];
    u32       h   = th_hash(key, len);
    u32       bit = (h >> 11) & ((1u << d->hdr.filter_log2) - 1);

    if (!d->filter || !(d->filter[bit >> 3] & (1 << (bit & 7))))
        return 0;

    u32 range[2];
    if (read_at(d, d->hdr.off_buckets + 4 * (h & (d->hdr.nbuckets - 1)), range, 8) != 8)
        return 0;

    int size = range[1] - range[0];
    if (size <= 0)
        return 0;
    if (size > MAX_POSTINGS)
        size = MAX_POSTINGS;
    if (read_at(d, d->hdr.off_postings + range[0], postings, size) != size)
        return 0;

    for (int p = 0; p + 2 <= size;) {
        int klen  = postings[p];
        int count = p + 1 + klen < size ? postings[p + 1 + klen] : 0;
        u8  *r    = postings + p + 2 + klen;

        if (p + 2 + klen + 4 * count > size)
            break;
        if (klen == len && !memcmp(postings + p + 1, key, len)) {
            int n = 0;

            for (int i = 0; i < count && n < max; i++) {
                u32 ref = r[4 * i] | r[4 * i + 1] << 8 | r[4 * i + 2] << 16 |
                          (u32)r[4 * i + 3] << 24;

                if (types == T_ANY || (REF_POS(ref) & types))
                    refs[n++] = (ref & ~REF_POS_BITS) | (u32)di << 25;
            }
            return n;
        }
        p += 2 + klen + 4 * count;
    }
    return 0;
}

/*
 * A word record: a part-of-speech byte, then NUL-terminated kanji forms,
 * readings ('\x1f' between forms) and glosses. Returns the bytes read or -1.
 */
static int read_record(u32 ref, char *buf, int size)
{
    u32 di = REF_INDEX(ref);
    int n  = di < (u32)ndicts ? read_at(&dicts[di], dicts[di].hdr.off_records +
                                        REF_OFFSET(ref), buf, size - 1) : -1;

    if (n <= 0)
        return -1;
    buf[n] = 0;
    return n;
}

/* A kanji's meanings, from the first dictionary that has them. */
static int read_kanji(u32 cp, char *buf, int size)
{
    char key[8];
    u32  ref;
    int  len = 1;

    key[0] = 1;
    len += th_put_utf8(key + 1, cp);
    for (int di = 0; di < ndicts; di++)
        if (lookup(di, key, len, T_ANY, &ref, 1) == 1)
            return read_record(ref, buf, size);
    return -1;
}

/* --- deinflection -------------------------------------------------------- */

typedef struct {
    const char *from, *to;
    u16        in, out;    /* rule applies to words of class `in`; the result is `out` */
} rule;

/* Godan rows, in the order of the dictionary endings. */
static const char *const g_u[9]  = { "う", "く", "ぐ", "す", "つ", "ぬ", "ぶ", "む", "る" };
static const char *const g_a[9]  = { "わ", "か", "が", "さ", "た", "な", "ば", "ま", "ら" };
static const char *const g_i[9]  = { "い", "き", "ぎ", "し", "ち", "に", "び", "み", "り" };
static const char *const g_e[9]  = { "え", "け", "げ", "せ", "て", "ね", "べ", "め", "れ" };
static const char *const g_o[9]  = { "お", "こ", "ご", "そ", "と", "の", "ぼ", "も", "ろ" };
static const char *const g_te[9] = { "って", "いて", "いで", "して", "って",
                                     "んで", "んで", "んで", "って" };
static const char *const g_ta[9] = { "った", "いた", "いだ", "した", "った",
                                     "んだ", "んだ", "んだ", "った" };

/* Godan rules: {row, suffix, in, out}, expanded for all nine endings. */
static const struct {
    const char *const *row;
    const char        *suffix;
    u16               in;
} godan[] = {
    { g_a,  "ない",   T_ADJ },      /* 読まない */
    { g_a,  "ず",     T_ANY },
    { g_a,  "ずに",   T_ANY },
    { g_a,  "れる",   T_V1 },       /* passive 読まれる */
    { g_a,  "せる",   T_V1 },       /* causative 読ませる */
    { g_a,  "される", T_V1 },       /* 読まされる */
    { g_i,  "ます",   T_MASU },     /* 読みます */
    { g_e,  "る",     T_V1 },       /* potential 読める */
    { g_e,  "ば",     T_ANY },      /* 読めば */
    { g_e,  "",       T_ANY },      /* imperative 読め */
    { g_o,  "う",     T_ANY },      /* volitional 読もう */
    { g_te, "",       T_TE },
    { g_ta, "",       T_TA },
};

static const rule rules_fixed[] = {
    /* ichidan: 食べ + ... -> 食べる */
    { "ない",       "る",   T_ADJ,  T_V1 },
    { "ず",         "る",   T_ANY,  T_V1 },
    { "ます",       "る",   T_MASU, T_V1 },
    { "た",         "る",   T_TA,   T_V1 },
    { "て",         "る",   T_TE,   T_V1 },
    { "られる",     "る",   T_V1,   T_V1 },
    { "れる",       "る",   T_V1,   T_V1 },     /* ら抜き 見れる */
    { "させる",     "る",   T_V1,   T_V1 },
    { "よう",       "る",   T_ANY,  T_V1 },
    { "ろ",         "る",   T_ANY,  T_V1 },
    { "れば",       "る",   T_ANY,  T_V1 },

    /* 来る, in kana and with the kanji */
    { "こない",     "くる", T_ADJ,  T_VK },
    { "きます",     "くる", T_MASU, T_VK },
    { "きた",       "くる", T_TA,   T_VK },
    { "きて",       "くる", T_TE,   T_VK },
    { "こられる",   "くる", T_V1,   T_VK },
    { "こさせる",   "くる", T_V1,   T_VK },
    { "こよう",     "くる", T_ANY,  T_VK },
    { "こい",       "くる", T_ANY,  T_VK },
    { "くれば",     "くる", T_ANY,  T_VK },
    { "来ない",     "来る", T_ADJ,  T_VK },
    { "来ます",     "来る", T_MASU, T_VK },
    { "来た",       "来る", T_TA,   T_VK },
    { "来て",       "来る", T_TE,   T_VK },
    { "来られる",   "来る", T_V1,   T_VK },
    { "来させる",   "来る", T_V1,   T_VK },
    { "来よう",     "来る", T_ANY,  T_VK },
    { "来い",       "来る", T_ANY,  T_VK },
    { "来れば",     "来る", T_ANY,  T_VK },

    /* する */
    { "しない",     "する", T_ADJ,  T_VS },
    { "せず",       "する", T_ANY,  T_VS },
    { "します",     "する", T_MASU, T_VS },
    { "した",       "する", T_TA,   T_VS },
    { "して",       "する", T_TE,   T_VS },
    { "される",     "する", T_V1,   T_VS },
    { "させる",     "する", T_V1,   T_VS },
    { "しよう",     "する", T_ANY,  T_VS },
    { "しろ",       "する", T_ANY,  T_VS },
    { "せよ",       "する", T_ANY,  T_VS },
    { "すれば",     "する", T_ANY,  T_VS },

    /* 行く: って, not いて */
    { "行って",     "行く", T_TE,   T_V5 },
    { "行った",     "行く", T_TA,   T_V5 },
    { "いって",     "いく", T_TE,   T_V5 },
    { "いった",     "いく", T_TA,   T_V5 },

    /* i-adjectives (and ない, たい) */
    { "かった",     "い",   T_TA,   T_ADJ },
    { "くて",       "い",   T_TE,   T_ADJ },
    { "く",         "い",   T_ANY,  T_ADJ },
    { "くない",     "い",   T_ADJ,  T_ADJ },
    { "ければ",     "い",   T_ANY,  T_ADJ },
    { "かろう",     "い",   T_ANY,  T_ADJ },
    { "さ",         "い",   T_ANY,  T_ADJ },
    { "そう",       "い",   T_ANY,  T_ADJ },
    { "すぎる",     "い",   T_V1,   T_ADJ },
    { "なきゃ",     "ない", T_ANY,  T_ADJ },
    { "ないで",     "ない", T_ANY,  T_ADJ },

    /* polite forms back to ます */
    { "ました",     "ます", T_TA,   T_MASU },
    { "まして",     "ます", T_TE,   T_MASU },
    { "ません",     "ます", T_ANY,  T_MASU },
    { "ませんでした", "ます", T_ANY, T_MASU },
    { "ましょう",   "ます", T_ANY,  T_MASU },

    /* endings on the ます stem: 読みたい -> 読みます -> 読む */
    { "たい",       "ます", T_ADJ,  T_MASU },
    { "ながら",     "ます", T_ANY,  T_MASU },
    { "なさい",     "ます", T_ANY,  T_MASU },
    { "そう",       "ます", T_ANY,  T_MASU },
    { "すぎる",     "ます", T_V1,   T_MASU },
    { "やすい",     "ます", T_ADJ,  T_MASU },
    { "にくい",     "ます", T_ADJ,  T_MASU },
    { "たがる",     "ます", T_V5,   T_MASU },

    /* て/た forms with something after them */
    { "ている",     "て",   T_V1,   T_TE },
    { "でいる",     "で",   T_V1,   T_TE },
    { "てる",       "て",   T_V1,   T_TE },
    { "でる",       "で",   T_V1,   T_TE },
    { "ておく",     "て",   T_V5,   T_TE },
    { "でおく",     "で",   T_V5,   T_TE },
    { "とく",       "て",   T_V5,   T_TE },
    { "どく",       "で",   T_V5,   T_TE },
    { "てしまう",   "て",   T_V5,   T_TE },
    { "でしまう",   "で",   T_V5,   T_TE },
    { "ちゃう",     "て",   T_V5,   T_TE },
    { "じゃう",     "で",   T_V5,   T_TE },
    { "たら",       "た",   T_ANY,  T_TA },
    { "だら",       "だ",   T_ANY,  T_TA },
    { "たり",       "た",   T_ANY,  T_TA },
    { "だり",       "だ",   T_ANY,  T_TA },
};

#define N_GODAN (sizeof(godan) / sizeof(godan[0]) * 9)
#define N_FIXED (sizeof(rules_fixed) / sizeof(rules_fixed[0]))

static rule rules[N_GODAN + N_FIXED];
static char godan_from[N_GODAN][16];
static int  nrules;

/* The godan rules are spelled out once, on first use. */
static void build_rules(void)
{
    int n = 0;

    for (unsigned g = 0; g < sizeof(godan) / sizeof(godan[0]); g++) {
        for (int k = 0; k < 9; k++, n++) {
            mini_snprintf(godan_from[n], sizeof(godan_from[n]), "%s%s",
                          godan[g].row[k], godan[g].suffix);
            rules[n].from = godan_from[n];
            rules[n].to   = g_u[k];
            rules[n].in   = godan[g].in;
            rules[n].out  = T_V5;
        }
    }
    for (unsigned i = 0; i < N_FIXED; i++)
        rules[n++] = rules_fixed[i];
    nrules = n;
}

typedef struct {
    char word[MAX_WORD];
    u16  len, types;
} candidate;

/*
 * Every form `word` could be inflected from, itself first (T_ANY), breadth
 * first. Returns the count.
 */
static int deinflect(const char *word, int len, candidate *out, int max)
{
    if (!nrules)
        build_rules();

    memcpy(out[0].word, word, len);
    out[0].len   = len;
    out[0].types = T_ANY;
    int n = 1;

    for (int c = 0; c < n; c++) {
        for (int r = 0; r < nrules && n < max; r++) {
            const rule *ru = &rules[r];
            int        fl  = strlen(ru->from), tl = strlen(ru->to);
            int        base = out[c].len - fl;

            /* the stem may be empty (して -> する), the result not */
            if (!(out[c].types & ru->in) || base < 0 || base + tl == 0 || base + tl > MAX_WORD ||
                memcmp(out[c].word + base, ru->from, fl))
                continue;

            candidate *d = &out[n];
            memcpy(d->word, out[c].word, base);
            memcpy(d->word + base, ru->to, tl);
            d->len   = base + tl;
            d->types = ru->out;

            int dup = 0;
            for (int k = 0; k < n && !dup; k++)
                dup = out[k].len == d->len && out[k].types == d->types &&
                      !memcmp(out[k].word, d->word, d->len);
            if (!dup)
                n++;
        }
    }
    return n;
}

/* --- matching ------------------------------------------------------------ */

/* Kana or kanji: where words can start. */
static int is_japanese(u32 cp)
{
    return (cp >= 0x3041 && cp <= 0x30FF) ||    /* kana, ー */
           (cp >= 0x3400 && cp <= 0x9FFF) ||    /* kanji */
           cp == 0x3005;                        /* 々 */
}

/* Katakana to hiragana, as gen_dict.py folds its keys. */
static u32 fold(u32 cp)
{
    return cp >= 0x30A1 && cp <= 0x30F6 ? cp - 0x60 : cp;
}

/*
 * The longest word at the start of text[0..n): its length in code points
 * (0 = none), with up to `max` record refs, most common first, in refs.
 */
static int match_word(const u32 *text, int n, u32 *refs, int max, int *count)
{
    static candidate cands[MAX_CANDIDATES];
    char             key[MAX_WORD];

    *count = 0;
    if (n > MAX_MATCH)
        n = MAX_MATCH;

    for (int len = n; len > 0; len--) {
        int klen = 0;

        for (int i = 0; i < len && klen + 4 < MAX_WORD; i++)
            klen += th_put_utf8(key + klen, fold(text[i]));

        int nc    = deinflect(key, klen, cands, MAX_CANDIDATES);
        int found = 0;

        for (int c = 0; c < nc && found < max; c++) {
            u32 types = cands[c].types == T_ANY ? T_ANY : cands[c].types & T_WORDS;

            if (!types)
                continue;

            for (int di = 0; di < ndicts && found < max; di++) {
                /* the same word can come back through several candidates */
                int got = lookup(di, cands[c].word, cands[c].len, types, refs + found,
                                 max - found);
                for (int i = found; i < found + got; i++) {
                    int dup = 0;

                    for (int k = 0; k < found && !dup; k++)
                        dup = REF_SAME(refs[k], refs[i]);
                    if (dup)
                        refs[i--] = refs[--got + found];
                }
                found += got;
            }
        }
        if (found) {
            *count = found;
            return len;
        }
    }
    return 0;
}

/* --- the popup's line and entry ------------------------------------------ */

void th_dict_tokenize(th_popup *p)
{
    p->ntok = 0;
    for (int i = 0; i < p->n && p->ntok < TH_POPUP_TOKENS;) {
        if (!is_japanese(p->text[i])) {
            i++;
            continue;
        }

        th_token *t     = &p->tok[p->ntok++];
        int      count  = 0;
        int      len    = ndicts ? match_word(p->text + i, p->n - i, t->refs,
                                                 TH_POPUP_REFS, &count) : 0;

        t->start = i;
        t->len   = len ? len : 1;
        t->nrefs = count;
        i += t->len;
    }
}

/* Length of the common start of a and b, in bytes of a, kana folded. */
static int common_start(const char *a, const char *b)
{
    const char *s = a;

    while (*a && *b) {
        const char *na = a, *nb = b;

        if (fold(th_next_utf8(&na)) != fold(th_next_utf8(&nb)))
            break;
        a = na, b = nb;
    }
    return a - s;
}

/*
 * The form in `forms` ('\x1f' between them) that best matches the text,
 * copied to out: the one sharing the longest start with it (近づこう picks
 * 近づく, not 近付く).
 */
static void pick_form(const char *forms, const char *text, char *out, int size)
{
    const char *best = forms;
    int        best_len = 0, best_n = 0;

    for (const char *f = forms; *f;) {
        const char *e = f;

        while (*e && *e != '\x1f')
            e++;

        int n = common_start(f, text);
        if (n > best_n) {
            best     = f;
            best_len = e - f;
            best_n   = n;
        } else if (f == forms) {
            best_len = e - f;
        }
        f = *e ? e + 1 : e;
    }
    if (best_len >= size)
        best_len = size - 1;
    memcpy(out, best, best_len);
    out[best_len] = 0;
}

void th_dict_select(th_popup *p)
{
    static char rec[720];
    char        text[MAX_MATCH * 4 + 1];
    th_token    *t = &p->tok[p->cur];
    int         n  = 0;

    for (int i = 0; i < t->len; i++)
        n += th_put_utf8(text + n, p->text[t->start + i]);
    text[n] = 0;

    p->status     = NULL;
    p->source     = NULL;
    p->reading[0] = 0;
    p->nkanji     = 0;

    if (t->nrefs && read_record(t->refs[p->entry], rec, sizeof(rec)) > 0) {
        const char *kanji = rec + 1;
        const char *kana  = kanji + strlen(kanji) + 1;
        const char *gloss = kana + strlen(kana) + 1;

        /* matched a reading: show it, under the word's usual spelling */
        if (!*kanji) {
            pick_form(kana, text, p->head, sizeof(p->head));
        } else if (REF_READING(t->refs[p->entry])) {
            pick_form(kanji, "", p->head, sizeof(p->head));
            pick_form(kana, text, p->reading, sizeof(p->reading));
        } else {
            pick_form(kanji, text, p->head, sizeof(p->head));
            pick_form(kana, "", p->reading, sizeof(p->reading));
        }
        strncpy0(p->gloss, gloss, sizeof(p->gloss));
        if (REF_INDEX(t->refs[p->entry]))
            p->source = dict_title(t->refs[p->entry]);
    } else {
        strncpy0(p->head, text, sizeof(p->head));
        strncpy0(p->gloss, ndicts ? "(no entry)" : "", sizeof(p->gloss));
    }

    /* the kanji of the headword, each once */
    for (const char *s = p->head; *s && p->nkanji < TH_POPUP_KANJI;) {
        u32 cp  = th_next_utf8(&s);
        int dup = 0;

        for (int k = 0; k < p->nkanji; k++)
            dup |= p->kanji[k] == cp;
        if (cp < 0x3400 || cp > 0x9FFF || dup ||
            read_kanji(cp, p->meaning[p->nkanji], sizeof(p->meaning[0])) < 0)
            continue;
        p->kanji[p->nkanji++] = cp;
    }
}
