# TextFabric

Кросс-платформенная C++20-библиотека для заполнения DOCX-шаблонов: подстановка текста, клонирование строк таблиц, обновление встроенных диаграмм и вставка изображений — по именованным закладкам Word, без промежуточных форматов. Опционально конвертирует результат в PDF/HTML через Microsoft Word или LibreOffice.

**Подтверждённые платформы (v0.1.0):** Linux x86_64 (GCC + Nix) — `ctest` **69/69 ✅**, включая графики, работу с изображениями (PNG/JPEG/BMP/TIFF), PDF/HTML-конвертеры и 3 integration-теста с `find_package(TextFabric)` из install-префикса. Windows x64 (MSVC 2022 + vcpkg) — собирается и проходит основной набор тестов; ветки JPEG/BMP/TIFF ждут отдельного прогона на Windows-хосте. Windows x86 и macOS — CMake-presets готовы, не прогонялись.

## Содержание

- [Быстрый старт](#быстрый-старт)
- [Авторинг шаблона `.docx`](#авторинг-шаблона-docx)
- [API](#api)
- [Полный пример](#полный-пример)
- [Runtime-зависимости](#runtime-зависимости)
- [Коды ошибок](#коды-ошибок)
- [Лицензия](#лицензия)

---

## Быстрый старт

Сборка из репозитория:

```bash
# Linux (Nix dev shell):
nix develop
cmake --preset linux-x64
cmake --build --preset linux-x64
ctest --preset linux-x64

# Windows (MSVC 2022 + vcpkg) — подробно в docs/build-windows.md:
cmake --preset win-x64-vcpkg
cmake --build --preset win-x64-vcpkg
ctest   --preset win-x64-vcpkg
```

Подключение в чужом CMake-проекте:

```cmake
find_package(TextFabric 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE TextFabric::textfabric)
```

Или через vcpkg-манифест (`vcpkg.json`):

```json
{ "dependencies": ["textfabric"] }
```

Минимальный вызов:

```cpp
#include <textfabric/merger.hpp>

auto m = textfabric::make_docx_merger();
m->setCodePage("UTF-8");
m->load("template.docx");
m->setClipboardValue("Body.Greeting", "{{name}}", "Alice");
m->save("report.docx");
```

---

## Авторинг шаблона `.docx`

Шаблон — обычный DOCX, созданный в MS Word или LibreOffice Writer. Никакого промежуточного формата не требуется.

### 1. Закладки (`<w:bookmarkStart w:name="..."/>`)

Все запросы адресуются **по имени закладки Word**. Вставьте закладку поверх текста, который должен быть заменён (Word: *Insert → Bookmark → Add*).

| Тип закладки | Назначение | Пример |
|---|---|---|
| `_Header` / `_Footer` | **Псевдо-закладки**, зарезервированные под `word/header*.xml` / `word/footer*.xml`. Реальной `<w:bookmarkStart>` не требуется и не ищется — движок перебирает все части заголовка/футера в архиве и подставляет `value` в первую, где встретится `field`. Удобно, когда заголовок/футер шаблона содержит текстовые плейсхолдеры без закладок. | `setClipboardValue("_Header", "{User}", "Alice")` |
| Обычная закладка | Любое имя внутри `word/document.xml`, например `Body.*` | `Body.Greeting`, `Body.Scores` |

### 2. Плейсхолдеры

Внутри закладки размещайте токены вида `{{name}}` либо `kNAME`. Библиотека ищет **первое вхождение** токена в склеенном тексте закладки и заменяет его; форматирование первого рана (`<w:rPr>`: шрифт, жирный, кегль) сохраняется.

Токен может быть разорван на несколько `<w:r>`/`<w:t>` (частый артефакт редактирования в Word) — движок корректно склеит текст и очистит «хвостовые» `<w:t>`.

### 3. Таблицы

Для динамически клонируемых строк:

1. Нарисуйте таблицу: одна **заголовочная** строка (не трогается) + одна **шаблонная** строка.
2. В шаблонной строке поставьте **одну закладку**, перекрывающую всю строку (от первой ячейки до последней).
3. В ячейках — **уникальный плейсхолдер на ячейку** (`{{col1}}`, `{{col2}}`, …).

Пример фрагмента `word/document.xml`:

```xml
<w:tr>  <!-- template row -->
  <w:tc><w:p>
    <w:bookmarkStart w:id="5" w:name="Body.Scores"/>
    <w:r><w:t>{{rowName}}</w:t></w:r>
  </w:p></w:tc>
  <w:tc><w:p>
    <w:r><w:t>{{rowScore}}</w:t></w:r>
    <w:bookmarkEnd w:id="5"/>
  </w:p></w:tc>
</w:tr>
```

Ограничения v1:
- Вложенные таблицы не тестировались, но алгоритм клонирования их не ломает.
- `<w:bookmarkStart>` в клонах автоматически удаляются, чтобы избежать дубликатов `w:id` (Word иначе откажется открывать документ).

### 4. Картинки (`setImage`)

Закладка (например `Body.Logo`) должна располагаться **внутри одного `<w:p>`**; `setImage` удалит все `<w:r>` между `bookmarkStart`/`bookmarkEnd` и вставит `<w:r><w:drawing>…</w:drawing></w:r>` между ними. Плейсхолдер вроде `{{logo}}` замещается картинкой целиком.

Размер в документе (`<wp:extent cx cy>`) по умолчанию равен пиксельному размеру исходного файла при 96 DPI (9525 EMU/px). Через параметр `ImageSize{min_width_px, min_height_px, max_width_px, max_height_px}` можно задать bounds — крохотная иконка растянется до `min`, гигантская фотография сожмётся до `max`, aspect ratio сохраняется единым скаляром на обе оси. Конфликтующие bounds (например `min_width=400, max_height=200` на квадратной картинке) → `ReportError::InvalidField` **до** мутации архива — документ остаётся нетронутым.

---

## API

Полный заголовочный файл: [include/textfabric/merger.hpp](include/textfabric/merger.hpp). Фабрика `textfabric::make_docx_merger()` возвращает `std::unique_ptr<IReportMerger>`. Все строки — UTF-8.

| Метод | Что делает |
|---|---|
| `setCodePage(cp)` | Устанавливает кодировку. v1 принимает только `"UTF-8"`, остальное → `NotImplemented`. |
| `load(path)` | Читает `.docx`-архив, парсит `word/document.xml`. |
| `save(path.docx)` | Round-trip `.docx`-архива. |
| `save(path.pdf/html)` | Конвертация через внешний процесс (Microsoft Word или LibreOffice — см. [Runtime-зависимости](#runtime-зависимости)). `NoConverter`, если ни один не найден. |
| `setClipboardValue(bookmark, field, value)` | Подставляет `value` на место `field` внутри `bookmark`. Поддержка split-run. `bookmark` = `"_Header"`/`"_Footer"` — особый случай: перебирает все `word/header*.xml`/`word/footer*.xml` части архива вместо поиска закладки. |
| `hasBookmark(bookmark)` | Проверяет, есть ли в загруженном шаблоне закладка с таким именем. Возвращает `false` и для отсутствующей закладки, и для незагруженного шаблона — никогда не бросает исключение. Не резолвит псевдо-закладки `_Header`/`_Footer`. |
| `clearBookmark(bookmark)` | Стирает текст плейсхолдера внутри закладки, оставляя её пустой — полезно, когда закладку решили не заполнять на этом прогоне. Не резолвит псевдо-закладки `_Header`/`_Footer`. |
| `setTableRow(bookmark, fields, rows)` | Клонирует `<w:tr>`-шаблон по числу `rows`, подставляет `rows[i][j]` в ячейку с `fields[j]`, удаляет шаблонную строку. |
| `paste(bookmark)` | Валидирует существование закладки, логирует активацию. В v1 — no-op, оставлен для будущей семантики «раскрытия секции». |
| `setChartValue(bookmark, field, series, category, value)` | Переписывает `<c:val>/<c:numCache>/<c:pt idx="N">/<c:v>` для точки `(series, category)` в чарте, на который указывает `bookmark`. Поддерживает bar/line/pie/area/doughnut/radar/3D варианты. Scatter/bubble/stock/surface → `NotImplemented`. `field` принимается для симметрии API, в матчинге не используется. **Ограничение:** точечно обновляет только уже существующую пару `(series, category)` — не может добавить или убрать категорию/серию. Ось категорий и набор серий графика зафиксированы шаблоном; `category`, которой нет в шаблоне, даёт `InvalidField`. |
| `setImage(bookmark, image_path, bounds={})` | Вставляет картинку в параграф закладки. PNG — как есть; JPEG/BMP — через stb_image; TIFF — через libtiff. Всё транскодируется в PNG перед вкладкой. Необязательный `ImageSize{min_w, min_h, max_w, max_h}` в пикселях клампит отображаемый размер с сохранением aspect ratio. |

---

## Полный пример

См. работающий код в [examples/basic_report.cpp](examples/basic_report.cpp) и шаблонную фабрику в [examples/generate_template.cpp](examples/generate_template.cpp). Сокращённый скелет:

```cpp
#include <textfabric/error.hpp>
#include <textfabric/merger.hpp>

try {
    auto m = textfabric::make_docx_merger();
    m->setCodePage("UTF-8");
    m->load("template.docx");

    // --- Header fields (пробегает word/header*.xml, закладка не нужна) ---
    m->setClipboardValue("_Header", "{ReportName}", "Q1 2026 Summary");
    m->setClipboardValue("_Header", "{User}",       "Иван Петров");
    m->setClipboardValue("_Header", "{UKDate}",     "2026-04-22");

    // --- Mixed-field bookmark ---
    m->setClipboardValue("Body.Greeting", "{{name}}", "Alice");
    m->setClipboardValue("Body.Greeting", "{{app}}",  "Acme Corp");

    // --- Table rows ---
    m->setTableRow("Body.Scores",
        {"{{rowName}}", "{{rowScore}}"},
        {
            {"Alice",    "95"},
            {"Борис",    "82"},
            {"Carol Жу", "77"},
        });

    // --- Image ---
    // Без bounds → отображаемый размер = пиксельному. С bounds → aspect-
    // preserving clamp (апскейлит крохотные иконки, даунскейлит большие фото).
    m->setImage("Body.Logo", "logo.png",
                textfabric::ImageSize{/*min*/64, 64, /*max*/512, 512});

    m->paste("Body.Greeting");
    m->save("report.docx");
}
catch (const textfabric::ReportException& e) {
    std::cerr << textfabric::to_string(e.code()) << ": " << e.what() << "\n";
}
```

---

## Runtime-зависимости

Для `load`, `save(".docx")`, `setClipboardValue`, `setTableRow`, `paste` и `setImage(PNG)` никаких внешних зависимостей нет — всё в самой библиотеке (PNG-парсер hand-rolled).

### Картинки (setImage)

| Формат входа | Что происходит | Требуется |
|---|---|---|
| **PNG** | Байты проходят насквозь, размеры читаются из IHDR-чанка | — (hand-rolled парсер в `src/docx/image.cpp`) |
| **JPEG**, **BMP** | Декод в RGBA → re-encode в PNG → вкладка | `stb_image` + `stb_image_write` (vcpkg `stb` / FetchContent с github/nothings/stb). Без них — `NotImplemented`. |
| **TIFF** | `libtiff TIFFClientOpen` → `TIFFReadRGBAImageOriented` → repack → PNG | `libtiff` (vcpkg `tiff` / nix `libtiff`). Без libtiff — `NotImplemented`. |

Детект формата — по magic bytes входного файла, не по расширению. Архив всегда хранит только `word/media/image{N}.png` — в `[Content_Types].xml` ничего кроме `<Default Extension="png" ContentType="image/png"/>` на выходе не появляется.

### PDF / HTML экспорт

`save("*.pdf")` / `save("*.html")` идёт через внешний конвертер. Приоритет детекта (сверху вниз):

| Шаг | Что проверяем | Как |
|---|---|---|
| 1 | **Microsoft Word** (Windows only) | `reg query HKCR\Word.Application` — есть ли COM-класс. Конвертация — через генерируемый в scratch-каталоге `.vbs` + `cscript //B //Nologo` → `Documents.Open` → `SaveAs2`. Запуск в headless-режиме (`word.Visible=False`, `word.DisplayAlerts=0`). |
| 2 | **LibreOffice `soffice`** (все платформы) | `TEXTFABRIC_SOFFICE` env (абсолютный путь; пустое значение отключает эту ветку) → Windows `C:\Program Files[(x86)]\LibreOffice\program\soffice.exe` → PATH-probe (`soffice --version`). Конвертация — `soffice --headless --convert-to pdf/html --outdir <scratch> <input>`. |
| — | если ни то, ни другое | `ReportError::NoConverter` с подсказкой «install Microsoft Word or LibreOffice». |

Env-переменные:

| Имя | Эффект |
|---|---|
| `TEXTFABRIC_DISABLE_CONVERTERS=1` | Мастер-выключатель. `find_converter` сразу возвращает None, `save("*.pdf")` → `NoConverter`. Нужен для тестов и force-fallback. |
| `TEXTFABRIC_NO_MSWORD=1` | Windows-only. Пропускает ветку Word и сразу идёт в LibreOffice. Полезно когда Word установлен, но нужен LO-совместимый рендер. |
| `TEXTFABRIC_SOFFICE=<path>` | Если задано непусто — использовать только этот бинарь как LibreOffice, без PATH/Program Files. Если задано пустое — полностью отключить LO-ветку (но Word остаётся, если доступен). |

Ветка MSWord — осознанный компромисс: на Windows-рабочих станциях с установленным Office она даёт рендер, идентичный тому, что пользователь видит в Word интерактивно, и не требует ставить LibreOffice рядом. Цена — возможное расхождение вывода между Windows (через Word) и Linux/macOS (через LibreOffice); для сценариев, где важен паритет рендера между платформами, используйте `TEXTFABRIC_NO_MSWORD=1` и деплойте LibreOffice везде.

## Коды ошибок

Все исключения — [`textfabric::ReportException`](include/textfabric/error.hpp) с полем `code()` типа `ReportError`:

| Код | Когда бросается |
|---|---|
| `CantOpenTemplate`     | Файл не найден или не является zip-архивом |
| `CantCopyDocxTemplate` | В архиве нет `word/document.xml` либо XML не парсится |
| `InvalidBookmark`      | Закладка не найдена; либо `setTableRow` адресует закладку вне `<w:tr>` |
| `InvalidField`         | Плейсхолдер отсутствует в закладке; либо `rows[i].size() != fields.size()` |
| `SaveFailed`           | Неизвестное расширение на `save()` или ошибка записи zip |
| `NoConverter`          | Ни Microsoft Word, ни LibreOffice не найдены при `save("*.pdf")` / `save("*.html")`. Починить: установить MS Word (Windows), установить LibreOffice, задать `TEXTFABRIC_SOFFICE=/abs/path/to/soffice`, или убрать `TEXTFABRIC_DISABLE_CONVERTERS`. |
| `NotImplemented`       | Сценарий не поддержан текущей реализацией: `setCodePage(≠ "UTF-8")`; `setChartValue` на scatter/bubble/stock/surface графике; JPEG/BMP/TIFF в `setImage`, когда библиотека собрана без stb_image/libtiff. |

`textfabric::to_string(ReportError)` возвращает имя кода — удобно для логов и пользовательских сообщений.

---

## Зависимости сборки

pugixml, libzip, nlohmann-json, fmt, inja, stb, libtiff — через vcpkg-манифест (Windows/macOS) или Nix flake (Linux). Подробности в [vcpkg.json](vcpkg.json) и [flake.nix](flake.nix). Тесты используют Catch2.

## Лицензия

[MIT](LICENSE).
