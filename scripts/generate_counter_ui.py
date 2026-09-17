"""Generate a fixed demo layout, subset LVGL fonts and matching visual previews.

Requires Pillow and an OFL-licensed Noto Sans SC TTF supplied with --font.
No market data, microphone access or transaction backend is used.
"""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BG = 0x101716
CARD = 0x192320
LINE = 0x33403A
GOLD = 0xE4C58A
WHITE = 0xF2F0E8
MUTED = 0x94A49B
INK = 0x19221B


def box(x, y, w, h, color=CARD, radius=18):
    return dict(x=x, y=y, w=w, h=h, color=color, radius=radius)


def text(x, y, w, value, size=20, color=WHITE, align="left"):
    return dict(x=x, y=y, w=w, h=0, text=value, size=size, ink=color, align=align)


def button(x, y, w, h, value, action, fill=GOLD, ink=INK, size=24):
    return dict(**box(x, y, w, h, fill, 12), text=value, size=size, ink=ink, action=action)


def common(active):
    nodes = [box(32, 31, 8, 32, GOLD, 3), text(56, 22, 420, "星豚 · 智慧柜台", 28),
             text(879, 30, 170, "柜台 01", 20, MUTED, "right"),
             button(1084, 25, 164, 44, "设备设置", 4, 0x29352D, GOLD, 20),
             box(32, 137, 1216, 1, LINE, 0), box(32, 654, 1216, 1, LINE, 0),
             box(34, 684, 8, 8, MUTED, 4), text(53, 669, 215, "录音未开启", 18, MUTED),
             text(300, 669, 720, "演示模式 · 未连接电子秤和交易系统", 18, MUTED),
             text(1020, 669, 228, "透明计价  安心选购", 18, GOLD, "right")]
    for index, title in enumerate(["金价与计价", "订单预览", "服务说明"]):
        x = 32 + index * 200
        nodes.append(button(x, 87, 178, 48, title, index, BG,
                            GOLD if index == active else MUTED, 24))
        if index == active:
            nodes.append(box(x + 25, 135, 128, 3, GOLD, 1))
    return nodes


def pages():
    # Integer-cent arithmetic: 12.680 g at 898 yuan/g, plus 35 yuan/g labor.
    material = (12680 * 89800 + 500) // 1000
    labor = (12680 * 3500 + 500) // 1000
    money = lambda cents: f"¥{cents // 100:,}.{cents % 100:02d}"
    total = money(material + labor)
    home = common(0) + [
        box(32, 158, 640, 402), text(60, 180, 310, "今日金价", 28),
        text(490, 188, 154, "演示报价", 18, GOLD, "right"),
        text(60, 243, 300, "足金 999", 24, GOLD),
        text(55, 275, 500, "¥898.00", 76, GOLD), text(545, 337, 100, "元/克", 20, MUTED),
        box(60, 404, 584, 1, LINE, 0),
        text(60, 423, 250, "回收参考", 18, MUTED), text(60, 454, 280, "¥826.00", 32),
        box(328, 431, 1, 65, LINE, 0),
        text(359, 423, 275, "铂金 Pt950", 18, MUTED), text(359, 454, 280, "¥368.00", 32),
        text(60, 522, 580, "价格为演示，不作为成交依据", 16, MUTED),
        box(32, 577, 640, 58, 0x202D25, 12),
        text(55, 588, 590, "每一克有依据，每一项都清晰。", 20, GOLD),
        box(696, 158, 552, 477), text(724, 180, 285, "本次计价", 28),
        text(1060, 187, 160, "示例商品", 18, MUTED, "right"),
        text(724, 235, 480, "足金素圈手镯", 24),
        text(724, 294, 260, "商品克重（手动）", 18, MUTED),
        text(984, 283, 236, "12.680 g", 28, WHITE, "right"),
        text(724, 340, 270, "金料金额", 20, MUTED),
        text(984, 336, 236, money(material), 24, WHITE, "right"),
        text(724, 385, 270, "工费 · ¥35/克", 20, MUTED),
        text(984, 381, 236, money(labor), 24, WHITE, "right"),
        box(724, 437, 496, 1, LINE, 0), text(724, 454, 240, "预计合计", 18, MUTED),
        text(719, 478, 510, total, 44, GOLD),
        button(724, 558, 496, 58, "核对明细", 1),
    ]
    order = common(1) + [
        box(32, 158, 778, 477), text(64, 181, 650, "请核对本次商品与费用", 28),
        text(64, 235, 650, "订单演示  /  仅供界面体验", 18, MUTED),
        box(64, 288, 714, 1, LINE, 0),
        text(64, 311, 370, "商品", 20, MUTED), text(414, 305, 364, "足金素圈手镯", 24, WHITE, "right"),
        text(64, 369, 370, "克重（手动输入）", 20, MUTED), text(414, 363, 364, "12.680 g", 24, WHITE, "right"),
        text(64, 427, 370, "金料金额", 20, MUTED), text(414, 421, 364, money(material), 24, WHITE, "right"),
        text(64, 485, 370, "工费", 20, MUTED), text(414, 479, 364, money(labor), 24, WHITE, "right"),
        text(64, 571, 700, "没有连接收银系统，不会创建订单或扣款。", 18, MUTED),
        box(832, 158, 416, 477), text(862, 186, 356, "预计合计", 24, MUTED),
        text(857, 263, 378, total, 44, GOLD), text(862, 357, 356, "明细清楚，再安心确认。", 20),
        button(862, 462, 356, 66, "确认（演示）", 3),
        button(862, 548, 356, 58, "返回计价", 0, 0x29352D, WHITE, 22),
    ]
    info = common(2) + [
        text(40, 175, 1100, "看得见的价格，听得见的服务。", 32),
        text(40, 235, 1100, "柜台客显原型 · 当前所有内容均为离线演示", 20, MUTED),
        box(32, 303, 389, 234), box(444, 303, 389, 234), box(856, 303, 392, 234),
        text(60, 325, 340, "01  价格透明", 24, GOLD),
        text(60, 390, 334, "金价、克重与工费分项展示。\n本机未接入实时行情，\n示例报价不可用于成交。", 20),
        text(472, 325, 340, "02  记录明示", 24, GOLD),
        text(472, 390, 334, "当前录音未开启。\n正式服务开启记录前，\n应清楚告知顾客用途。", 20),
        text(884, 325, 338, "03  双方核对", 24, GOLD),
        text(884, 390, 334, "确认之前，逐项核对明细。\n当前确认仅演示交互，\n不会下单，也不会扣款。", 20),
        button(462, 565, 356, 62, "返回金价与计价", 0),
    ]
    done = common(1) + [
        box(220, 170, 840, 454), text(260, 211, 760, "明细已确认", 44, GOLD, "center"),
        text(260, 290, 760, "演示操作完成", 24, WHITE, "center"),
        text(260, 352, 760, "本次未创建订单、未扣款，也未开启录音。", 20, MUTED, "center"),
        text(260, 405, 760, "感谢核对，每一项费用都清楚。", 24, WHITE, "center"),
        button(462, 520, 356, 64, "返回首页", 0),
    ]
    return [home, order, info, done]


def get_font(path, size):
    font = ImageFont.truetype(str(path), size)
    try:
        font.set_variation_by_axes([500 if size >= 28 else 400])
    except (OSError, AttributeError):
        pass
    return font


def generate_font(path, output, size, chars):
    font = get_font(path, size)
    ascent, descent = font.getmetrics()
    bitmap, descriptors, offsets = [], ["{0}"], []
    chars = sorted(set(chars) - {"\n"}, key=ord)
    start = ord(chars[0])
    for char in chars:
        left, top, right, bottom = font.getbbox(char, anchor="ls")
        width, height = right - left, bottom - top
        pixels = Image.new("L", (max(1, width), max(1, height)))
        ImageDraw.Draw(pixels).text((-left, -top), char, font=font, fill=255, anchor="ls")
        values = [min(15, (v + 8) // 17) for v in pixels.getdata()] if width * height else []
        if len(values) % 2:
            values.append(0)
        index = len(bitmap)
        bitmap.extend((values[i] << 4) | values[i + 1] for i in range(0, len(values), 2))
        descriptors.append("{.bitmap_index=%d,.adv_w=%d,.box_w=%d,.box_h=%d,.ofs_x=%d,.ofs_y=%d}" %
                           (index, round(font.getlength(char) * 16), width, height, left, -bottom))
        offsets.append(str(ord(char) - start))
    name = f"counter_font_{size}"
    data = "\n".join(",".join(f"0x{x:02x}" for x in bitmap[i:i + 24]) + "," for i in range(0, len(bitmap), 24))
    output.write_text(
        '// Generated by scripts/generate_counter_ui.py from Noto Sans SC (OFL-1.1).\n#include <lvgl.h>\n'
        f'static const uint8_t bitmap[]={{\n{data}\n}};\n'
        'static const lv_font_fmt_txt_glyph_dsc_t glyphs[]={\n' + ',\n'.join(descriptors) + '\n};\n'
        'static const uint16_t unicode_list[]={' + ','.join(offsets) + '};\n'
        'static const lv_font_fmt_txt_cmap_t cmaps[]={{'
        f'.range_start={start},.range_length={ord(chars[-1])-start+1},.glyph_id_start=1,'
        f'.unicode_list=unicode_list,.list_length={len(chars)},.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY'
        '}};\nstatic lv_font_fmt_txt_dsc_t dsc={'
        '.glyph_bitmap=bitmap,.glyph_dsc=glyphs,.cmaps=cmaps,.cmap_num=1,.bpp=4,.bitmap_format=0};\n'
        f'const lv_font_t {name}={{.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,'
        '.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,'
        f'.line_height={ascent+descent},.base_line={descent},.dsc=&dsc}};\n', encoding="utf-8")
    return len(bitmap)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--font', type=Path, required=True)
    parser.add_argument('--preview-dir', type=Path, required=True)
    args = parser.parse_args()
    board = Path(__file__).resolve().parents[1] / 'main/boards/waveshare/esp32-p4-wifi6-dev-kit-b'
    layouts = pages()
    args.preview_dir.mkdir(parents=True, exist_ok=True)
    charsets = {}
    for layout in layouts:
        for item in layout:
            if 'text' in item:
                charsets.setdefault(item['size'], set()).update(item['text'])
    # Settings labels and driver status strings are rendered dynamically.
    # Include their Chinese characters and printable ASCII in the 20px font.
    charsets.setdefault(20, set()).update(chr(i) for i in range(32, 127))
    for name in ('counter_ui.cc', 'counter_settings.cc', 'board_peripherals.cc', 'board_peripherals.h'):
        source = (board / name).read_text(encoding='utf-8')
        charsets[20].update(c for c in source if ord(c) >= 0x3000)
    total = sum(generate_font(args.font, board / f'counter_font_{size}.c', size, chars)
                for size, chars in sorted(charsets.items()))
    declarations = '\n'.join(f'LV_FONT_DECLARE(counter_font_{size});' for size in sorted(charsets))
    header = '// Generated by scripts/generate_counter_ui.py.\n#pragma once\n#include <lvgl.h>\n' + declarations
    header += '\nstruct CounterUiItem {int x,y,w,h; uint32_t color,ink; int radius,action; const char* text; const lv_font_t* font; lv_text_align_t align;};\n'
    fonts = {size: get_font(args.font, size) for size in charsets}
    for page, layout in enumerate(layouts):
        image = Image.new('RGB', (1280, 720), '#101716')
        draw = ImageDraw.Draw(image)
        header += f'static const CounterUiItem kCounterPage{page}[]={{\n'
        for item in layout:
            x,y,w,h = (item[k] for k in ('x','y','w','h'))
            text_value = item.get('text')
            if text_value is None or 'action' in item:
                draw.rounded_rectangle((x,y,x+w-1,y+h-1), radius=item.get('radius',0), fill=f"#{item['color']:06x}")
            if text_value is not None:
                font = fonts[item['size']]
                asc, desc = font.getmetrics()
                lines = text_value.split('\n')
                ty = y + (h - (asc+desc)) / 2 if 'action' in item else y
                for line in lines:
                    tw = font.getlength(line)
                    align = item.get('align','left')
                    tx = x + (w-tw)/2 if 'action' in item or align == 'center' else (x+w-tw if align == 'right' else x)
                    if tw > w:
                        raise ValueError(f'Text exceeds bounds: {line!r}, {tw} > {w}')
                    draw.text((tx,ty+asc), line, font=font, fill=f"#{item['ink']:06x}", anchor='ls')
                    ty += asc+desc
            literal = json.dumps(text_value, ensure_ascii=False) if text_value is not None else 'nullptr'
            font_expr = f"&counter_font_{item['size']}" if text_value is not None else 'nullptr'
            header += ('{%d,%d,%d,%d,0x%06x,0x%06x,%d,%d,%s,%s,LV_TEXT_ALIGN_%s},\n' %
                       (x,y,w,h,item.get('color',BG),item.get('ink',WHITE),item.get('radius',0),
                        item.get('action',-1),literal,font_expr,item.get('align','left').upper()))
        header += '};\n'
        image.save(args.preview_dir / f'counter-ui-{page}.png')
    header += 'struct CounterUiPage {const CounterUiItem* items; size_t count;};\nstatic const CounterUiPage kCounterPages[]={\n'
    header += ',\n'.join(f'{{kCounterPage{i},sizeof(kCounterPage{i})/sizeof(kCounterPage{i}[0])}}' for i in range(len(layouts)))
    header += '\n};\n'
    (board / 'counter_ui_layout.h').write_text(header,encoding='utf-8')
    (args.preview_dir / 'layout.json').write_text(json.dumps(layouts,ensure_ascii=False,indent=2),encoding='utf-8')
    print(f'{len(layouts)} pages; {sum(len(c) for c in charsets.values())} glyphs across sizes; {total} bitmap bytes')


if __name__ == '__main__':
    main()
