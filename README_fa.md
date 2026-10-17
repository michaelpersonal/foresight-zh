<div dir="rtl">

# foresight-zh — نسخه‌ی فارسی

[English](README.md) · **فارسی**

رونویسی زنده‌ی فارسی برای Google AI Edge Foresight روی macOS، بدون تغییر دادن خود برنامه.
این شاخه روی پروژه‌ی [michaelpersonal/foresight-zh](https://github.com/michaelpersonal/foresight-zh) ساخته شده و فقط یک موتور فارسی کنار موتور چینی اضافه می‌کند.

غیررسمی است و هیچ ارتباطی با گوگل ندارد. هیچ کد یا مدلی از گوگل در آن نیست.

## نصب

مسیر ریپو باید دقیقاً `~/tools/foresight-zh` باشد، چون shim و لانچر این مسیر را انتظار دارند.

۱. Foresight را نصب کنید و یک بار بازش کنید.

۲. موتور فارسی را دانلود کنید (حدود ۱۵۰ مگابایت): کتابخانه‌ی sherpa-onnx، مدل
[Shenava Rizeh v1.0](https://huggingface.co/Reza2kn/Shenava-Rizeh-v1.0-sherpa-onnx)
(FastConformer CTC فارسی، لایسنس Apache-2.0) و Silero VAD. فارسی موتور پیش‌فرض این فورک است.

</div>

```bash
git clone -b persian https://github.com/hadi-buddy/foresight-zh.git ~/tools/foresight-zh
cd ~/tools/foresight-zh
./setup-persian.sh
```

<div dir="rtl">

۳. shim را بیلد کنید (روی مک با Xcode Command Line Tools):

</div>

```bash
shim/build.sh
```

<div dir="rtl">

۴. اختیاری: یک لانچر برای Dock و Spotlight بسازید:

</div>

```bash
osacompile -o ~/Applications/"Foresight فارسی.app" -e 'do shell script "$HOME/tools/foresight-zh/launch.sh"'
```

<div dir="rtl">

۵. اختیاری: watcher را نصب کنید تا اگر Foresight از Dock یا نوتیفیکیشن (بدون shim) باز شد، خودکار با shim دوباره اجرا شود: `./install-watcher.sh`

## استفاده

- Foresight را از لانچر بالا یا با `~/tools/foresight-zh/launch.sh` باز کنید. اگر از راه دیگری باز شود، فقط انگلیسی رونویسی می‌کند.
- متن بعد از هر مکث، عبارت به عبارت ظاهر می‌شود، نه کلمه به کلمه.
- لاگ: `~/Library/Logs/foresight-zh/shim.log` — باید خط `config: engine=persian` و بعد `asr (persian): ready` را ببینید.
- برای جلسه‌ی انگلیسی کافی است Foresight را معمولی (بدون لانچر) باز کنید؛ موتور انگلیسی خود برنامه کار می‌کند.

## چه چیزی فرق می‌کند

- **فقط فارسی:** مدل فارسی است. کلمه‌های انگلیسی وسط جمله با حروف فارسی و تقریبی نوشته می‌شوند، و جلسه‌ی تمام‌انگلیسی را خوب رونویسی نمی‌کند. برای جلسه‌ی انگلیسی موتور را برگردانید.
- **اعداد:** مدل اعداد را با حروف می‌نویسد. `shim/persian_itn.c` عددهای درست‌ساخت را به رقم فارسی تبدیل می‌کند: «بیست و پنج» ← «۲۵»، «هزار و چهارصد و سه» ← «۱۴۰۳». «یک» تنها به شکل کلمه می‌ماند، چون معمولاً معنی «یک روز» دارد نه عدد.
- **خلاصه‌ی جلسه:** اگر چت را به یک سرور مدل محلی وصل کرده باشید (بخش Chat on a local model در README انگلیسی)، برای رونوشت فارسی از مدل خواسته می‌شود خلاصه را فارسی بنویسد.
- **راست‌چین نیست:** حروف درست به هم می‌چسبند، اما رابط Foresight متن را چپ‌چین نشان می‌دهد و ترکیب فارسی و انگلیسی ممکن است جابه‌جا دیده شود. درست کردنش نیاز به دست بردن در لایه‌ی Dart دارد.
- اگر فایل‌های مدل فارسی نباشند، shim در لاگ می‌نویسد و به SenseVoice برمی‌گردد.

## فایل‌های اضافه‌شده

- `setup-persian.sh`: دانلود موتور فارسی و انتخابش
- `shim/persian_itn.c`: تبدیل اعداد گفتاری به رقم؛ تست: `shim/test/itn_test.sh`
- تغییر در `shim/sensevoice_engine.c`، `shim/foresight_zh.c` و `shim/chat_bridge.c` برای حالت فارسی

## وقتی Foresight آپدیت می‌شود

همان نکات README انگلیسی برقرار است: اگر regex فیلتر پیدا نشود، متن غیرانگلیسی دوباره حذف می‌شود و باید `patch_ascii_filter` در `shim/foresight_zh.c` به‌روز شود.

</div>
