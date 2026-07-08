from PIL import Image
import os

MAX_COLORS = 256

def png_to_indexed_c_arrays(png_filename, array_name):
    img = Image.open(png_filename).convert('RGBA')
    width, height = img.size
    pixels = list(img.getdata())

    # Будуємо палітру за фактичними RGB565-значеннями (а не 8-біт RGB),
    # бо кілька вихідних кольорів можуть зливатись в один 565-колір -
    # це ще трохи зменшує палітру.
    palette = []          # list[int] -> color565
    color_to_index = {}   # color565 -> index
    indices = bytearray(width * height)

    for i, p in enumerate(pixels):
        r, g, b, a = p
        color565 = 0x0000 if a < 128 else (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

        idx = color_to_index.get(color565)
        if idx is None:
            if len(palette) >= MAX_COLORS:
                raise ValueError(
                    f"{png_filename}: більше {MAX_COLORS} унікальних кольорів (565) - "
                    f"індексований формат не підходить без додаткового квантування."
                )
            idx = len(palette)
            palette.append(color565)
            color_to_index[color565] = idx

        indices[i] = idx

    # Гарантуємо, що прозорий колір (0x0000) справді присутній у палітрі,
    # навіть якщо в зображенні взагалі немає прозорих пікселів - інакше
    # блітер у hal_retro_go.c не матиме на що спиратись при перевірці.
    if 0x0000 not in color_to_index:
        if len(palette) >= MAX_COLORS:
            raise ValueError(f"{png_filename}: немає вільного слота під color-key 0x0000")
        palette.append(0x0000)

    with open("assets.c", "a") as f:
        f.write(f"// Generated from {png_filename} (indexed 8-bit, {len(palette)} кольорів)\n")
        f.write(f"const unsigned short {array_name}_pal[{len(palette)}] = {{\n")
        for i, c in enumerate(palette):
            f.write(f"0x{c:04X}, ")
            if (i + 1) % 16 == 0:
                f.write("\n")
        f.write("\n};\n\n")

        f.write(f"const unsigned char {array_name}_idx[{width} * {height}] = {{\n")
        for i, idx in enumerate(indices):
            f.write(f"{idx}, ")
            if (i + 1) % 32 == 0:
                f.write("\n")
        f.write("\n};\n\n")

    raw_size = width * height * 2
    new_size = width * height * 1 + len(palette) * 2
    print(f"{png_filename}: {raw_size/1024:.1f} KB -> {new_size/1024:.1f} KB "
          f"(-{100*(1 - new_size/raw_size):.0f}%), палітра {len(palette)} кол.")


# Скидаємо старий файл, якщо є
if os.path.exists("assets.c"):
    os.remove("assets.c")

# Конвертуємо всі ассети в індексований формат (assets.c)
png_to_indexed_c_arrays("title.png", "asset_title")
png_to_indexed_c_arrays("cards.png", "asset_cards")
# asset_mainscreen прибрано: ніде в коді не використовується
# (main.c явно каже "завантаження mainscreen.png видалено для економії
# пам'яті"), а важив би 150 КБ - майже 40% усіх ваших ассетів.
png_to_indexed_c_arrays("font.png", "asset_font")
png_to_indexed_c_arrays("faces.png", "asset_faces")
print("Файл assets.c успішно згенеровано (індексований 8-bit формат)!")
