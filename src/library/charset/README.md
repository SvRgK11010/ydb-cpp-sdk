Здесь представлены функции и enum'ы для работы с кодировками.

Наиболее полезные конструкции этой библиотеки:
1. [`enum ECharset`](https://a.yandex-team.ru/arc/trunk/arcadia/src/library/charset/doccodes.h) - перечень кодировок, которые умеет определять [детектор кодировок](https://a.yandex-team.ru/arc/trunk/arcadia/kernel/recshell/recshell.h?rev=8268697#L56).
2. [Функция](https://a.yandex-team.ru/arc/trunk/arcadia/src/library/charset/recyr.hh?rev=r6888372#L137) `inline std::string Recode(ECharset from, ECharset to, const std::string& in)` для преобразования кодировок.
3. [Функция](https://a.yandex-team.ru/arc/trunk/arcadia/src/library/charset/wide.h?rev=r6888372#L277) `inline TUtf16String UTF8ToWide(const char* text, size_t len, const CodePage& cp)`, пытающаяся построить широкую строку из UTF-8, а если не получается - с помощью кодировки `cp`.

3. [Класс `TCiString`](https://a.yandex-team.ru/arc/trunk/arcadia/src/library/charset/ci_string.h) - аналог `std::string`, но использующий case-insensitive-компаратор и хеш и поддерживающий разные кодировки.

В комплекте есть ещё много функций для работы со старой однобайтной Yandex-кодировкой. Не рекомендуется к использованию. Для преобразования из UTF-8 в `TUtf16String` и для работы с Unicode используйте функции из [arcadia/src/util/charset](https://a.yandex-team.ru/arc/trunk/arcadia/src/util/charset).

Библиотека src/library/charset/lite - содержит часть функциональности, не зависящей от libiconv (e.g. пункты 2 и 3 из основной библиотеки)
