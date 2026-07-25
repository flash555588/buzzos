# Pinyin IME source data

| File | Role |
|------|------|
| `pinyin.txt` | [pinyin-data](https://github.com/mozillazg/pinyin-data) Unihan readings → single-char table |
| `jieba_dict.small.txt` | jieba small frequency lexicon → multi-char words/phrases |

`tools/gen_pinyin_data.py` merges both (plus a small curated desktop vocab)
into `src/user/bin/pinyin_data.h`.

Regenerate:

```sh
python tools/gen_pinyin_data.py
# optional: python tools/gen_pinyin_data.py --max-words 20000
```

Requires `pypinyin` for lexicon → pinyin conversion. Singles can also be built
from GB2312 via pypinyin if `pinyin.txt` is missing.
