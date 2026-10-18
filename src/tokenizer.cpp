#include "l0/tokenizer.hpp"
#include "runtime/api.h"
#include "src_c/stem_UTF_8_english.h"
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>
#include <unicode/uversion.h>
namespace l0 {
/// @brief Нормализует английский токен официальным Snowball English (Porter2) 3.0.1.
/// Числа, смешанные токены и слова вне a-z сохраняются после Unicode case folding.
static std::string stem_english(std::string_view word) {
    if (!std::all_of(word.begin(), word.end(),
                     [](unsigned char c) { return c >= 'a' && c <= 'z'; }))
        return std::string(word);
    // Собственное состояние на поток исключает гонки при параллельных запросах.
    static thread_local std::unique_ptr<SN_env, decltype(&english_UTF_8_close_env)> stemmer(
        english_UTF_8_create_env(), english_UTF_8_close_env);
    require(bool(stemmer), "cannot allocate Snowball stemmer");
    require(word.size() <= INT32_MAX, "term exceeds Snowball size limit");
    require(SN_set_current(stemmer.get(), int(word.size()),
                           reinterpret_cast<const symbol*>(word.data())) == 0,
            "cannot copy term into Snowball stemmer");
    require(english_UTF_8_stem(stemmer.get()) >= 0, "Snowball stemming failed");
    return std::string(reinterpret_cast<const char*>(stemmer->p), size_t(stemmer->l));
}
/// @brief Дописывает корректную кодовую точку Unicode в строку в кодировке UTF-8.
static void utf8(std::string& s, UChar32 cp) {
    if (cp < 128)
        s.push_back(char(cp));
    else if (cp < 2048) {
        s.push_back(char(192 | (cp >> 6)));
        s.push_back(char(128 | (cp & 63)));
    } else if (cp < 65536) {
        s.push_back(char(224 | (cp >> 12)));
        s.push_back(char(128 | ((cp >> 6) & 63)));
        s.push_back(char(128 | (cp & 63)));
    } else {
        s.push_back(char(240 | (cp >> 18)));
        s.push_back(char(128 | ((cp >> 12) & 63)));
        s.push_back(char(128 | ((cp >> 6) & 63)));
        s.push_back(char(128 | (cp & 63)));
    }
}
/// @brief Нормализует регистр через ICU, применяет английский стемминг и выдаёт токены с позициями.
/// Некорректный UTF-8 заменяется U+FFFD; слишком большой для ICU текст отклоняется.
void tokenize(std::string_view text, const std::function<void(std::string_view, uint32_t)>& emit) {
    require(text.size() < INT32_MAX, "document exceeds ICU size limit");
    UErrorCode err = U_ZERO_ERROR;
    int32_t n = 0;
    // ICU принимает UTF-16; ошибочные байты UTF-8 заменяются разделителем U+FFFD.
    std::vector<UChar> src(text.size() + 1);
    u_strFromUTF8WithSub(src.data(), int32_t(src.size()), &n, text.data(), int32_t(text.size()),
                         0xfffd, nullptr, &err);
    require(U_SUCCESS(err), "UTF-8 conversion failed");
    require(uint64_t(n) * 3 + 1 <= INT32_MAX, "normalized document exceeds ICU size limit");
    // Полное сведение регистра может расширять символ, например ß превращается в ss.
    std::vector<UChar> folded(size_t(n) * 3 + 1);
    err = U_ZERO_ERROR;
    int32_t count = u_strFoldCase(folded.data(), int32_t(folded.size()), src.data(), n,
                                  U_FOLD_CASE_DEFAULT, &err);
    require(U_SUCCESS(err), "Unicode case folding failed");
    std::string word;
    uint64_t pos = 0;
    /// @brief Передаёт накопленный токен обработчику и увеличивает координату один раз.
    auto flush = [&] {
        if (!word.empty()) {
            require(pos < END, "too many tokens in document");
            // Стемминг заменяет ровно один токен одним термом, сохраняя координаты NEAR/ADJ.
            word = stem_english(word);
            emit(word, uint32_t(pos++));
            word.clear();
        }
    };
    for (int32_t i = 0; i < count;) {
        UChar32 cp;
        U16_NEXT(folded.data(), i, count, cp);
        // Комбинируемые знаки продолжают уже начатое слово, но не начинают новое.
        int8_t cat = u_charType(cp);
        bool mark =
            cat == U_NON_SPACING_MARK || cat == U_COMBINING_SPACING_MARK || cat == U_ENCLOSING_MARK;
        if (u_isalnum(cp) || (mark && !word.empty()))
            utf8(word, cp);
        else
            flush();
    }
    flush();
}
/// @brief Собирает выход потокового токенизатора в вектор собственных строк.
std::vector<std::string> tokens(std::string_view text) {
    std::vector<std::string> v;
    // Копируем временное представление: буфер токенизатора переиспользуется.
    tokenize(text, [&](std::string_view t, uint32_t) { v.emplace_back(t); });
    return v;
}
/// @brief Формирует версию токенизатора с версией Unicode используемой ICU.
std::string tokenizer_version() {
    UVersionInfo v;
    u_getUnicodeVersion(v);
    return "unicode-casefold-alnum-snowball-english-3.0.1-v2-" + std::to_string(v[0]) + "." +
           std::to_string(v[1]);
}
} // namespace l0
