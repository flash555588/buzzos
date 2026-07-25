#!/usr/bin/env python3
"""Generate src/user/bin/pinyin_data.h for the BuzzOS desktop IME.

Best-scheme data pipeline (offline-friendly after the first fetch):

1. Prefer assets/pinyin/pinyin.txt (mozillazg/pinyin-data, Unihan-derived).
2. Fall back to pypinyin over the GB2312 repertoire if the table is missing
   but pypinyin is installed.
3. Merge a curated phrase list so continuous input like "nihao" / "zhongguo"
   resolves without requiring the user to pick syllable-by-syllable.

Output is a sorted table of {key, space-separated UTF-8 candidates} that the
desktop binary-searches at runtime. Do not hand-edit the generated header.
"""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT = ROOT / "src" / "user" / "bin" / "pinyin_data.h"
DEFAULT_PINYIN_TXT = ROOT / "assets" / "pinyin" / "pinyin.txt"
DEFAULT_WORD_DICT = ROOT / "assets" / "pinyin" / "jieba_dict.small.txt"

# Max single-char candidates kept per syllable (frequency / GB order).
MAX_CHARS_PER_KEY = 28
# Max phrase candidates sharing one multi-syllable key (homophones).
MAX_PHRASES_PER_KEY = 16
# How many multi-character words to pull from the frequency dictionary.
MAX_LEXICON_WORDS = 12000
# Accept 2–4 Han characters per lexicon word (sweet spot for IME continuous input).
MIN_WORD_CHARS = 2
MAX_WORD_CHARS = 4

# High-value phrases for continuous input. Keys are pure a-z pinyin without
# separators; ü is written as v (nv/lv) to match the IME buffer.
COMMON_PHRASES: list[tuple[str, str]] = [
    # greetings / politeness
    ("nihao", "你好"),
    ("ninhao", "您好"),
    ("xiexie", "谢谢"),
    ("buhao", "不好"),
    ("duibuqi", "对不起"),
    ("meiguanxi", "没关系"),
    ("zaijian", "再见"),
    ("zaoshanghao", "早上好"),
    ("wanshanghao", "晚上好"),
    ("wanan", "晚安"),
    ("qingwen", "请问"),
    ("qing", "请"),
    ("buhao", "不好"),
    # daily
    ("shijie", "世界"),
    ("zhongguo", "中国"),
    ("beijing", "北京"),
    ("shanghai", "上海"),
    ("women", "我们"),
    ("nimen", "你们"),
    ("tamen", "他们 它们 她们"),
    ("ziji", "自己"),
    ("shenme", "什么"),
    ("zenme", "怎么"),
    ("weishenme", "为什么"),
    ("yinwei", "因为"),
    ("suoyi", "所以"),
    ("danshi", "但是"),
    ("ranhou", "然后"),
    ("xianzai", "现在"),
    ("yihou", "以后"),
    ("yiqian", "以前"),
    ("jintian", "今天"),
    ("mingtian", "明天"),
    ("zuotian", "昨天"),
    ("shijian", "时间 事件"),
    ("difang", "地方"),
    ("dongxi", "东西"),
    ("pengyou", "朋友"),
    ("laoshi", "老师"),
    ("xuesheng", "学生"),
    ("gongzuo", "工作"),
    ("xuexi", "学习"),
    ("shenghuo", "生活"),
    ("wenti", "问题"),
    ("fangfa", "方法"),
    ("keyi", "可以"),
    ("buxing", "不行"),
    ("zhidao", "知道"),
    ("xihuan", "喜欢"),
    ("xiangyao", "想要"),
    ("xuyao", "需要"),
    ("kaishi", "开始"),
    ("jieshu", "结束"),
    ("wancheng", "完成"),
    ("chenggong", "成功"),
    ("shibai", "失败"),
    ("zhongyao", "重要"),
    ("jiandan", "简单"),
    ("kunnan", "困难"),
    ("kaixin", "开心"),
    ("gaoxing", "高兴"),
    ("nanguo", "难过"),
    ("yueliang", "月亮"),
    ("taiyang", "太阳"),
    ("diannao", "电脑"),
    ("shouji", "手机"),
    ("wangluo", "网络"),
    ("hulianwang", "互联网"),
    # computing / BuzzOS
    ("shuru", "输入"),
    ("shurufa", "输入法"),
    ("pinyin", "拼音"),
    ("xitong", "系统"),
    ("wenjian", "文件"),
    ("wenjianjia", "文件夹"),
    ("bianji", "编辑"),
    ("bianjiqi", "编辑器"),
    ("liulanqi", "浏览器"),
    ("jisuanji", "计算机"),
    ("caozuoxitong", "操作系统"),
    ("zhuomian", "桌面"),
    ("chuangkou", "窗口"),
    ("yingyong", "应用"),
    ("chengxu", "程序"),
    ("daima", "代码"),
    ("biaoqing", "表情"),
    ("jianpan", "键盘"),
    ("shubiao", "鼠标"),
    ("xianshiqi", "显示器"),
    ("fenbianlv", "分辨率"),
    ("neicun", "内存"),
    ("yingpan", "硬盘"),
    ("wangye", "网页"),
    ("wangzhan", "网站"),
    ("lianjie", "连接"),
    ("duankai", "断开"),
    ("baocun", "保存"),
    ("dakai", "打开"),
    ("guanbi", "关闭"),
    ("tuichu", "退出"),
    ("denglu", "登录"),
    ("mima", "密码"),
    ("yonghu", "用户"),
    ("shezhi", "设置"),
    ("bangzhu", "帮助"),
    ("guanyu", "关于"),
    ("sousuo", "搜索"),
    ("xiazai", "下载"),
    ("shangchuan", "上传"),
    ("fuzhi", "复制"),
    ("niantie", "粘贴"),
    ("jianqie", "剪切"),
    ("chexiao", "撤销"),
    ("zhongzuo", "重做"),
    ("quanbu", "全部"),
    ("xuanze", "选择"),
    ("queding", "确定"),
    ("quxiao", "取消"),
    ("shanchu", "删除"),
    ("xinjian", "新建"),
    ("chongmingming", "重命名"),
    ("shuaxin", "刷新"),
    ("dayin", "打印"),
    ("dayinji", "打印机"),
    ("yinpin", "音频"),
    ("shipin", "视频"),
    ("tupian", "图片"),
    ("yinyue", "音乐"),
    ("youxi", "游戏"),
    ("zhongduan", "终端"),
    ("mingling", "命令"),
    ("jincheng", "进程"),
    ("xiancheng", "线程"),
    ("neihe", "内核"),
    ("qudong", "驱动"),
    ("wenben", "文本"),
    ("ziti", "字体"),
    ("yuyan", "语言"),
    ("zhongwen", "中文"),
    ("yingwen", "英文"),
    ("hanyu", "汉语"),
    ("putonghua", "普通话"),
    ("hanzi", "汉字"),
    ("zimu", "字母"),
    ("shuzi", "数字"),
    ("fuhao", "符号"),
    ("biaoji", "标记"),
    ("rizhi", "日志"),
    ("cuowu", "错误"),
    ("jinggao", "警告"),
    ("xinxi", "信息"),
    ("tongzhi", "通知"),
    ("xiaoxi", "消息"),
    ("liaotian", "聊天"),
    ("youxiang", "邮箱"),
    ("dizhi", "地址"),
    ("mingzi", "名字"),
    ("xingming", "姓名"),
    ("nianling", "年龄"),
    ("xingbie", "性别"),
    ("guojia", "国家"),
    ("chengshi", "城市"),
    ("jiedao", "街道"),
    ("gongsi", "公司"),
    ("xuexiao", "学校"),
    ("yiyuan", "医院"),
    ("yinhang", "银行"),
    ("shichang", "市场"),
    ("jiage", "价格"),
    ("qian", "钱"),
    ("renminbi", "人民币"),
    ("gupiao", "股票"),
    ("jingji", "经济"),
    ("zhengzhi", "政治"),
    ("wenhua", "文化"),
    ("lishi", "历史"),
    ("dili", "地理"),
    ("kexue", "科学"),
    ("jishu", "技术"),
    ("yishu", "艺术"),
    ("yinyue", "音乐"),
    ("dianying", "电影"),
    ("dianshi", "电视"),
    ("xinwen", "新闻"),
    ("tiyu", "体育"),
    ("zuqiu", "足球"),
    ("lanqiu", "篮球"),
    ("pingpang", "乒乓"),
    ("yumaoqiu", "羽毛球"),
    ("youyong", "游泳 有用"),
    ("paobu", "跑步"),
    ("lvxing", "旅行"),
    ("jiudian", "酒店"),
    ("canting", "餐厅"),
    ("kafei", "咖啡"),
    ("niunai", "牛奶"),
    ("shuiguo", "水果"),
    ("shucai", "蔬菜"),
    ("miantiao", "面条"),
    ("mifan", "米饭"),
    ("jiaozi", "饺子"),
    ("baozi", "包子"),
    ("mantou", "馒头"),
    ("shuijiao", "睡觉 水饺"),
    ("chifan", "吃饭"),
    ("heshui", "喝水"),
    ("xiuxi", "休息"),
    ("gongzuozhong", "工作中"),
    ("zaijian", "再见"),
]


_TONE_MAP = str.maketrans({
    "ā": "a", "á": "a", "ǎ": "a", "à": "a",
    "ē": "e", "é": "e", "ě": "e", "è": "e",
    "ī": "i", "í": "i", "ǐ": "i", "ì": "i",
    "ō": "o", "ó": "o", "ǒ": "o", "ò": "o",
    "ū": "u", "ú": "u", "ǔ": "u", "ù": "u",
    "ǖ": "v", "ǘ": "v", "ǚ": "v", "ǜ": "v", "ü": "v",
    "ń": "n", "ň": "n", "ǹ": "n",
    "ḿ": "m",
})


def strip_tones(pinyin: str) -> str:
    s = unicodedata.normalize("NFC", pinyin.strip().lower())
    s = s.translate(_TONE_MAP)
    s = re.sub(r"[^a-z]", "", s)
    # Standard IME convention: ü → v already; also accept u: forms.
    return s.replace("u:", "v")


def is_cjk(ch: str) -> bool:
    if len(ch) != 1:
        return False
    o = ord(ch)
    return 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF


def gb2312_level(ch: str) -> int:
    """0 = GB2312 level-1 (common), 1 = level-2, 2 = outside."""
    try:
        raw = ch.encode("gb2312")
    except UnicodeEncodeError:
        return 2
    if len(raw) != 2:
        return 2
    lead = raw[0]
    if 0xB0 <= lead <= 0xD7:
        return 0
    if 0xD8 <= lead <= 0xF7:
        return 1
    return 2


def load_from_pinyin_txt(path: Path) -> dict[str, list[str]]:
    """Parse mozillazg/pinyin-data pinyin.txt into syllable -> [chars]."""
    table: dict[str, list[str]] = defaultdict(list)
    seen: dict[str, set[str]] = defaultdict(set)
    line_re = re.compile(r"^U\+([0-9A-Fa-f]+):\s*(\S+)")
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = line_re.match(line)
        if not m:
            continue
        cp = int(m.group(1), 16)
        if not (0x4E00 <= cp <= 0x9FFF or 0x3400 <= cp <= 0x4DBF):
            continue
        ch = chr(cp)
        if gb2312_level(ch) > 1:
            # Keep the font + IME aligned: BuzzOS CJK glyphs are GB2312-based.
            continue
        readings = m.group(2).split(",")
        # Primary reading only.  Extra Unihan alts (童 also listed under
        # zhōng, etc.) pollute everyday IME candidate lists; phrases cover
        # the multi-syllable cases users actually need.
        if not readings:
            continue
        key = strip_tones(readings[0])
        if not key or len(key) > 6:
            continue
        if ch not in seen[key]:
            seen[key].add(ch)
            table[key].append(ch)
    return table


def load_from_pypinyin() -> dict[str, list[str]]:
    try:
        from pypinyin import Style, pinyin as py_pinyin  # type: ignore
    except ImportError as exc:
        raise SystemExit(
            "assets/pinyin/pinyin.txt missing and pypinyin is not installed. "
            "Install pypinyin or fetch pinyin-data into assets/pinyin/pinyin.txt."
        ) from exc

    table: dict[str, list[str]] = defaultdict(list)
    seen: dict[str, set[str]] = defaultdict(set)
    # GB2312 Han repertoire (same footprint as the desktop Unicode font).
    for lead in range(0xB0, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                ch = bytes([lead, trail]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if not is_cjk(ch):
                continue
            readings = py_pinyin(ch, style=Style.NORMAL, heteronym=False)
            if not readings or not readings[0]:
                continue
            key = strip_tones(readings[0][0])
            if not key:
                continue
            if ch not in seen[key]:
                seen[key].add(ch)
                table[key].append(ch)
    return table


def sort_chars(chars: list[str]) -> list[str]:
    """Prefer GB2312 level-1, then codepoint order for stable output."""
    return sorted(chars, key=lambda c: (gb2312_level(c), ord(c)))


def word_all_gb2312(word: str) -> bool:
    return all(is_cjk(ch) and gb2312_level(ch) <= 1 for ch in word)


def word_to_pinyin_key(word: str) -> str | None:
    """Convert a Han word to tone-less a-z pinyin (ü → v)."""
    try:
        from pypinyin import Style, lazy_pinyin  # type: ignore
    except ImportError:
        return None
    parts = lazy_pinyin(word, style=Style.NORMAL, errors="ignore")
    if not parts or len(parts) != len(word):
        return None
    key = "".join(strip_tones(p) for p in parts)
    if not key or not re.fullmatch(r"[a-z]+", key):
        return None
    # Guard absurd keys from bad segmentation / non-Han mix-ins.
    if len(key) > 24:
        return None
    return key


def load_lexicon_phrases(
    path: Path, limit: int
) -> list[tuple[str, str, int]]:
    """Load (pinyin_key, word, freq) from a jieba-style `word freq [tag]` dict."""
    if not path.is_file():
        print(f"warning: lexicon not found at {path}, skipping bulk phrases")
        return []

    try:
        from pypinyin import Style, lazy_pinyin  # noqa: F401
    except ImportError:
        print("warning: pypinyin not installed; cannot expand lexicon phrases")
        return []

    rows: list[tuple[str, int]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        word = parts[0]
        try:
            freq = int(parts[1])
        except ValueError:
            continue
        n = len(word)
        if n < MIN_WORD_CHARS or n > MAX_WORD_CHARS:
            continue
        if not word_all_gb2312(word):
            continue
        rows.append((word, freq))

    rows.sort(key=lambda w: (-w[1], w[0]))
    rows = rows[:limit]

    out: list[tuple[str, str, int]] = []
    for word, freq in rows:
        key = word_to_pinyin_key(word)
        if not key:
            continue
        # Skip pure single-syllable multi-char anomalies (rare); keep 2+ letters.
        if len(key) < 2:
            continue
        out.append((key, word, freq))
    print(f"lexicon: kept {len(out)} phrases from {path.name} (cap {limit})")
    return out


def merge_phrases(
    table: dict[str, list[str]],
    phrase_freq: dict[tuple[str, str], int],
    lexicon: list[tuple[str, str, int]],
) -> None:
    """Attach multi-character words to the pinyin table.

    phrase_freq maps (key, word) -> score used later for ordering.
    Curated phrases get a huge boost so they always beat raw corpus noise.
    """
    curated_boost = 10**12

    for key, items in COMMON_PHRASES:
        key = strip_tones(key)
        for rank, phrase in enumerate(items.split()):
            if not phrase:
                continue
            # Preserve curated listing order among equal-frequency items.
            score = curated_boost + max(0, 1000 - rank)
            phrase_freq[(key, phrase)] = max(
                phrase_freq.get((key, phrase), 0), score
            )
            if phrase not in table[key]:
                table[key].insert(0, phrase)

    for key, word, freq in lexicon:
        phrase_freq[(key, word)] = max(phrase_freq.get((key, word), 0), freq)
        if word not in table[key]:
            table[key].append(word)


def render_header(
    table: dict[str, list[str]], phrase_freq: dict[tuple[str, str], int]
) -> str:
    keys = sorted(table.keys())
    lines: list[str] = []
    lines.append("/* Generated by tools/gen_pinyin_data.py. Do not edit by hand. */")
    lines.append("#ifndef BUZZOS_PINYIN_DATA_H")
    lines.append("#define BUZZOS_PINYIN_DATA_H")
    lines.append("")
    lines.append("struct pinyin_entry {")
    lines.append("    const char *key;")
    lines.append("    const char *items; /* space-separated UTF-8 candidates */")
    lines.append("};")
    lines.append("")
    lines.append("/* Sorted by key for binary search.")
    lines.append(" *  - single-syllable keys: GB2312 chars (primary reading)")
    lines.append(" *  - multi-syllable keys: frequency-ranked words/phrases")
    lines.append(" *    from jieba lexicon + curated desktop vocabulary")
    lines.append(" */")
    lines.append("static const struct pinyin_entry pinyin_entries[] = {")

    phrase_keys = 0
    phrase_items = 0

    for key in keys:
        items = table[key]
        singles = [x for x in items if len(x) == 1]
        phrases = [x for x in items if len(x) > 1]

        if phrases:
            phrases = sorted(
                phrases,
                key=lambda w: (-phrase_freq.get((key, w), 0), len(w), w),
            )[:MAX_PHRASES_PER_KEY]
            phrase_keys += 1
            phrase_items += len(phrases)

        if singles and not phrases and len(key) <= 6:
            # Pure syllable entry.
            ordered = sort_chars(singles)[:MAX_CHARS_PER_KEY]
        elif phrases and not singles:
            ordered = phrases
        elif phrases and singles:
            # Continuous input prefers words; keep a few singles as fallback
            # only for short keys that are also valid syllables.
            room = max(0, MAX_CHARS_PER_KEY - len(phrases))
            ordered = phrases + sort_chars(singles)[: min(room, 8)]
        else:
            ordered = sort_chars(singles)[:MAX_CHARS_PER_KEY]

        if not ordered:
            continue
        joined = " ".join(ordered)
        joined_c = joined.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'    {{"{key}", "{joined_c}"}},')

    lines.append("};")
    lines.append("")
    lines.append(
        "#define PINYIN_ENTRY_COUNT "
        "((int)(sizeof(pinyin_entries) / sizeof(pinyin_entries[0])))"
    )
    lines.append("")
    lines.append("#endif /* BUZZOS_PINYIN_DATA_H */")
    lines.append("")
    # Stash stats on the object for main() logging via attribute is awkward;
    # print here.
    print(f"phrase keys: {phrase_keys}, phrase items: {phrase_items}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--pinyin-txt", type=Path, default=DEFAULT_PINYIN_TXT)
    ap.add_argument("--word-dict", type=Path, default=DEFAULT_WORD_DICT)
    ap.add_argument(
        "--max-words",
        type=int,
        default=MAX_LEXICON_WORDS,
        help=f"max multi-char words from lexicon (default {MAX_LEXICON_WORDS})",
    )
    ap.add_argument(
        "--force-pypinyin",
        action="store_true",
        help="Ignore pinyin.txt and build singles from pypinyin + GB2312",
    )
    args = ap.parse_args()

    if not args.force_pypinyin and args.pinyin_txt.is_file():
        print(f"loading singles from {args.pinyin_txt}")
        table: dict[str, list[str]] = load_from_pinyin_txt(args.pinyin_txt)
    else:
        print("loading singles via pypinyin + GB2312")
        table = load_from_pypinyin()

    phrase_freq: dict[tuple[str, str], int] = {}
    lexicon = load_lexicon_phrases(args.word_dict, args.max_words)
    merge_phrases(table, phrase_freq, lexicon)

    text = render_header(table, phrase_freq)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8", newline="\n")

    n_keys = text.count('\n    {"')
    print(f"wrote {args.out} ({n_keys} keys, {args.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
