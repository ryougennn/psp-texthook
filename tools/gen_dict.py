#!/usr/bin/env python3
"""Writes a popup dictionary for texthook.prx (ms0:/SEPLUGINS/texthook/dicts/),
from EDRDG's JMdict_e and KANJIDIC2:

    gen_dict.py JMdict_e.gz kanjidic2.xml.gz 1-jmdict.bin

Both are at http://ftp.edrdg.org/pub/Nihongo/ (© James William Breen and
EDRDG, CC BY-SA 4.0; the result is too, see tools/DICT-LICENSES.txt). tools/yomitan2dict.py writes the same format from a Yomitan
dictionary, with write_dict() below. The file is read on demand, a few small
reads per lookup, so only its key filter is ever held in memory.

Layout (little-endian):

    header   32 bytes: "THKD", version, bucket count, log2 of the filter's
             bits, then the offsets of the four parts below
    title    optional, up to the filter: the dictionary's name, NUL-terminated
    filter   one bit per (hash >> 11) of every key, to skip most misses
             without touching the stick; about 8 bits per key, 8-256 KB
    buckets  bucket count + 1 u32 offsets into the postings, by hash & (count - 1)
    postings per key: u8 length, the key (UTF-8), u8 count, count u32 refs:
             bits 0-24 the record's offset, 25-29 its part-of-speech bits
             (so deinflected matches are checked without reading it), 31
             set = the key is one of the word's readings
    records  words: u8 part-of-speech bits (POS_* below), then three
             NUL-terminated strings: kanji forms and readings ('\\x1f'
             between forms), and the glosses, "(1) ...; ... (2) ..."
             kanji: the meanings, NUL-terminated

Keys are the words' kanji forms and readings, katakana folded to hiragana,
and "\\x01" + a kanji for kanji meanings. The hash is FNV-1a over the key's
UTF-8, the same as th_hash() in shared/text.c.
"""
import gzip
import re
import struct
import sys
import xml.etree.ElementTree as ET

VERSION = 1
FILTER_LOG2 = (16, 21)  # smallest and largest filter, as log2 of its bits

# Part-of-speech bits, the same as T_V1 ... T_ADJ in user/dict.c.
POS_V1, POS_V5, POS_VK, POS_VS, POS_ADJ = 1, 2, 4, 8, 16

MAX_GLOSS = 600         # bytes of glosses per word
MAX_SENSES = 8
MAX_PER_KEY = 12        # records per key, most common first
MAX_KANJI = 64          # bytes of meanings per kanji

COMMON = {"news1", "ichi1", "spec1", "spec2", "gai1"}


def fold(s):
    """Katakana to hiragana, as dict_fold() does on the PSP."""
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in s)


def fnv1a(b):
    h = 0x811C9DC5
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def pos_bits(tags):
    bits = 0
    for t in tags:
        if t.startswith("v1"):
            bits |= POS_V1
        elif t.startswith("v5"):
            bits |= POS_V5
        elif t == "vk":
            bits |= POS_VK
        elif t in ("vs-i", "vs-s"):
            bits |= POS_VS
        elif t in ("adj-i", "adj-ix"):
            bits |= POS_ADJ
    return bits


def rank(pri, common, written, grammar, index):
    """
    Lower is more common: common words first (particles count as common,
    since JMdict rarely tags them), then those written this way (に the
    particle before 二 read に), then grammar words (の the particle before
    野, 居る before 要る), then by news frequency, then main forms first (本
    is ほん's main form, but only another way to write もと).
    """
    nf = [int(p[2:]) for p in pri if p.startswith("nf")]
    return (0 if COMMON & set(pri) or common else 1, 0 if written else 1,
            0 if grammar else 1, nf[0] if nf else 99, index)


def load_jmdict(path):
    text = gzip.open(path, "rt", encoding="utf-8").read()
    # Keep entity names ("v5r") rather than their expansions, and drop the DTD.
    text = re.sub(r"<!DOCTYPE.*?\]>", "", text, flags=re.S)
    text = re.sub(r"&([\w-]+);", r"\1", text)
    return ET.fromstring(text)


def glosses(entry):
    senses, last_pos = [], []
    for s in entry.iter("sense"):
        pos = [p.text for p in s.findall("pos")] or last_pos
        last_pos = pos
        g = [x.text for x in s.findall("gloss") if x.text]
        if g:
            senses.append((pos, "; ".join(g)))
    return senses


def build_words(root):
    records, keys = [], {}    # keys: folded key -> [(rank, record index, is_reading)]
    pos_of = []
    for entry in root.iter("entry"):
        kebs = [(k.findtext("keb"), [p.text for p in k.findall("ke_pri")])
                for k in entry.findall("k_ele")]
        rebs = [(r.findtext("reb"), [p.text for p in r.findall("re_pri")])
                for r in entry.findall("r_ele")]
        senses = glosses(entry)
        if not rebs or not senses:
            continue

        pos = 0
        for p, _ in senses:
            pos |= pos_bits(p)

        if len(senses) == 1:
            gloss = senses[0][1]
        else:
            gloss = " ".join("(%d) %s" % (i + 1, g)
                             for i, (_, g) in enumerate(senses[:MAX_SENSES]))
        gloss = gloss.encode()[:MAX_GLOSS].decode("utf-8", "ignore")

        idx = len(records)
        pos_of.append(pos)
        records.append(struct.pack("B", pos) +
                       "\x1f".join(k for k, _ in kebs).encode() + b"\0" +
                       "\x1f".join(r for r, _ in rebs).encode() + b"\0" +
                       gloss.encode() + b"\0")

        # Each form ranks by its own tags: 本 is common as ほん, not as もと.
        tags = {p for p, _ in senses for p in p}
        particle = bool(tags & {"prt", "conj", "aux", "aux-v", "cop"})
        kana = not kebs or "uk" in {m.text for m in entry.iter("misc")}
        for i, (form, pri) in enumerate(kebs):
            keys.setdefault(fold(form), []).append((rank(pri, False, True, particle, i), idx, 0))
        # a word usually written in kana is as common as its kanji forms say
        common = particle or (kana and any(COMMON & set(p) for _, p in kebs + rebs))
        for i, (form, pri) in enumerate(rebs):
            keys.setdefault(fold(form), []).append((rank(pri, common, kana, particle, i), idx, 1))
    return records, keys, pos_of


def build_kanji(path, records, keys, pos_of):
    root = ET.fromstring(gzip.open(path, "rb").read())
    for c in root.iter("character"):
        lit = c.findtext("literal")
        rm = c.find("reading_meaning")
        if rm is None:
            continue
        m = [x.text for x in rm.iter("meaning") if not x.get("m_lang") and x.text]
        if not m:
            continue
        text = ", ".join(m[:4]).encode()[:MAX_KANJI].decode("utf-8", "ignore")
        keys.setdefault("\x01" + lit, []).append(((0,), len(records), 0))
        records.append(text.encode() + b"\0")
        pos_of.append(0)


def write_dict(out, records, keys, pos_of, title):
    """
    records: the record bytes; keys: folded key -> [(rank, record index,
    is_reading)], lowest rank first; pos_of: each record's part-of-speech bits.
    """
    offsets, pos = [], 0
    for r in records:
        offsets.append(pos)
        pos += len(r)
    record_blob = b"".join(records)
    assert len(record_blob) < 1 << 25, "record offsets no longer fit in 25 bits"

    nbuckets = 1
    while nbuckets < len(keys) // 2:
        nbuckets *= 2
    filter_log2 = FILTER_LOG2[0]
    while filter_log2 < FILTER_LOG2[1] and 1 << filter_log2 < 8 * len(keys):
        filter_log2 += 1

    filt = bytearray(1 << (filter_log2 - 3))
    buckets = [[] for _ in range(nbuckets)]
    for key, hits in keys.items():
        kb = key.encode()
        if len(kb) > 255:
            continue
        h = fnv1a(kb)
        bit = (h >> 11) & ((1 << filter_log2) - 1)
        filt[bit >> 3] |= 1 << (bit & 7)

        seen, refs = set(), []
        for _, idx, reading in sorted(hits):
            if idx not in seen:
                seen.add(idx)
                refs.append(offsets[idx] | (pos_of[idx] << 25) | (reading << 31))
        refs = refs[:MAX_PER_KEY]
        buckets[h & (nbuckets - 1)].append(
            struct.pack("B", len(kb)) + kb + struct.pack("<B%dI" % len(refs), len(refs), *refs))

    table, postings = [], bytearray()
    for b in buckets:
        table.append(len(postings))
        for p in b:
            postings += p
    table.append(len(postings))

    name = title.encode()[:63].decode("utf-8", "ignore").encode() + b"\0"
    name += b"\0" * (-len(name) % 4)

    off_filter = 32 + len(name)
    off_buckets = off_filter + len(filt)
    off_postings = off_buckets + 4 * len(table)
    off_records = off_postings + len(postings)
    header = struct.pack("<4s7I", b"THKD", VERSION, nbuckets, filter_log2,
                         off_filter, off_buckets, off_postings, off_records)

    with open(out, "wb") as f:
        f.write(header)
        f.write(name)
        f.write(filt)
        f.write(struct.pack("<%dI" % len(table), *table))
        f.write(postings)
        f.write(record_blob)

    print("%s: \"%s\", %d keys, %d records, %d KB filter, %.1f MB" %
          (out, title, len(keys), len(records), len(filt) // 1024,
           (off_records + len(record_blob)) / 1e6))


def main():
    jmdict, kanjidic, out = sys.argv[1:4]
    records, keys, pos_of = build_words(load_jmdict(jmdict))
    build_kanji(kanjidic, records, keys, pos_of)
    write_dict(out, records, keys, pos_of, "JMdict")


if __name__ == "__main__":
    main()
