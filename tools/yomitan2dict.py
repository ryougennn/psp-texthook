#!/usr/bin/env python3
"""Converts a Yomitan dictionary (a .zip) into a popup dictionary for
texthook.prx, in the same format as gen_dict.py writes:

    yomitan2dict.py "VNDB Names.zip" 2-vndb-names.bin

Copy the result to ms0:/SEPLUGINS/texthook/dicts/. The PSP searches every
dictionary there, in file name order, and lists their entries in that order.

Term banks become words: rows with the same sequence number are one entry
(their terms are its spellings, their glosses its senses), and Yomitan's
deinflection rules (v1, v5, vk, vs, adj-i) become word classes, so their
conjugated forms are found too. Higher scores come first. Kanji banks
become kanji meanings. Frequency and tag banks are ignored.

The result keeps the source dictionary's licence: add its notice to
tools/DICT-LICENSES.txt (VNDB Names: MIT, kaanium; names from VNDB, ODbL).
"""
import json
import re
import sys
import zipfile

from gen_dict import (MAX_GLOSS, MAX_KANJI, MAX_SENSES, POS_ADJ, POS_V1, POS_V5,
                      POS_VK, POS_VS, fold, write_dict)

BLOCKS = {"div", "li", "p", "tr", "br", "ol", "ul", "summary", "details"}


def is_kana(s):
    return all(0x3041 <= ord(c) <= 0x30FF for c in s)


def text_of(node):
    """The plain text of a gloss: a string, or Yomitan structured content."""
    if isinstance(node, str):
        return node
    if isinstance(node, list):
        return "".join(text_of(n) for n in node)
    if not isinstance(node, dict):
        return ""
    kind = node.get("type")
    if kind == "text":
        return node.get("text", "")
    if kind == "image" or node.get("tag") == "img":
        return ""
    inner = text_of(node.get("content", ""))
    return "; " + inner + "; " if node.get("tag") in BLOCKS else inner


def gloss_text(glosses):
    parts = []
    for g in glosses:
        t = re.sub(r"\s+", " ", text_of(g))
        t = re.sub(r"(\s*;\s*)+", "; ", t).strip("; ")
        if t:
            parts.append(t)
    return "; ".join(parts)


def pos_bits(rules, term):
    bits = 0
    for r in rules.split():
        if r == "v1":
            bits |= POS_V1
        elif r == "v5":
            bits |= POS_V5
        elif r == "vk":
            bits |= POS_VK
        elif r == "vs" and term.endswith("する"):
            bits |= POS_VS
        elif r == "adj-i":
            bits |= POS_ADJ
    return bits


def banks(z, prefix):
    names = sorted((n for n in z.namelist() if re.fullmatch(prefix + r"_\d+\.json", n)),
                   key=lambda n: int(re.search(r"(\d+)\.json$", n).group(1)))
    for n in names:
        yield from json.loads(z.read(n))


def main():
    src, out = sys.argv[1:3]
    z = zipfile.ZipFile(src)
    index = json.loads(z.read("index.json"))
    title = index.get("title", src)

    # group rows into entries: by sequence number, else one entry per row
    entries, by_seq = [], {}
    for row in banks(z, "term_bank"):
        term, reading, _, rules, score, glosses, seq = row[:7]
        key = seq if seq else None
        e = by_seq.get(key) if key is not None else None
        if e is None:
            e = {"forms": [], "senses": [], "pos": 0, "score": score}
            entries.append(e)
            if key is not None:
                by_seq[key] = e
        e["forms"].append((term, reading or term))
        e["pos"] |= pos_bits(rules, term)
        e["score"] = max(e["score"], score)
        g = gloss_text(glosses)
        if g and g not in e["senses"]:
            e["senses"].append(g)

    records, keys, pos_of = [], {}, []
    for e in entries:
        if not e["senses"]:
            continue
        kanji, kana = [], []
        for term, reading in e["forms"]:
            if not is_kana(term) and term not in kanji:
                kanji.append(term)
            if reading not in kana:
                kana.append(reading)

        senses = e["senses"][:MAX_SENSES]
        gloss = senses[0] if len(senses) == 1 else \
            " ".join("(%d) %s" % (i + 1, g) for i, g in enumerate(senses))
        gloss = gloss.encode()[:MAX_GLOSS].decode("utf-8", "ignore")

        idx = len(records)
        records.append(bytes([e["pos"]]) + "\x1f".join(kanji).encode() + b"\0" +
                       "\x1f".join(kana).encode() + b"\0" + gloss.encode() + b"\0")
        pos_of.append(e["pos"])

        for i, form in enumerate(kanji):
            keys.setdefault(fold(form), []).append(((-e["score"], i), idx, 0))
        for i, form in enumerate(kana):
            keys.setdefault(fold(form), []).append(((-e["score"], i), idx, 1))

    for row in banks(z, "kanji_bank"):
        char, meanings = row[0], row[4]
        if not meanings:
            continue
        text = ", ".join(meanings[:4]).encode()[:MAX_KANJI].decode("utf-8", "ignore")
        keys.setdefault("\x01" + char, []).append(((0, 0), len(records), 0))
        records.append(text.encode() + b"\0")
        pos_of.append(0)

    write_dict(out, records, keys, pos_of, title)


if __name__ == "__main__":
    main()
