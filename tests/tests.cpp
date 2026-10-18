#include "l0/codec.hpp"
#include "l0/query.hpp"
#include "l0/ranking.hpp"
#include "l0/tokenizer.hpp"
#include <iostream>
#include <random>
#include <unistd.h>

using namespace l0;
namespace {
size_t checks = 0;
/// @brief Учитывает проверку и выбрасывает исключение с пояснением при провале.
void check(bool ok, const std::string& why) {
    ++checks;
    if (!ok)
        throw std::runtime_error("TEST: " + why);
}
/// @brief Проверяет, что вызов f завершается исключением, производным от std::exception.
template <class F> void fails(F f, const std::string& why) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, why);
}
struct Temporary {
    fs::path root = fs::temp_directory_path() / ("l0-tests-" + std::to_string(getpid()));
    /// @brief Создаёт уникальный для процесса каталог тестовых данных, не перезаписывая
    /// существующий.
    Temporary() {
        require(!fs::exists(root), "test directory already exists");
        fs::create_directories(root);
    }
    /// @brief Удаляет собственный каталог тестов, не выбрасывая исключений при очистке.
    ~Temporary() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};
/// @brief Проверяет PForDelta на случайных данных, границах фреймов и повреждённых потоках.
void test_codec() {
    // Фиксированный seed делает случайные ошибки воспроизводимыми.
    std::mt19937 rng(38291);
    for (unsigned round = 0; round < 8000; ++round) {
        size_t n = 1 + rng() % 128;
        std::vector<uint32_t> values(n);
        unsigned mode = round % 6;
        for (auto& v : values) {
            v = mode == 0   ? 0
                : mode == 1 ? UINT32_MAX
                : mode == 2 ? rng()
                : mode == 3 ? rng() % 8
                : mode == 4 ? uint32_t(1) << (rng() % 32)
                            : rng() % 1000;
            if (mode == 3 && rng() % 20 == 0)
                v = rng();
        }
        Bytes b;
        p4::encode(values.data(), values.size(), b);
        Cursor c{b.data(), b.size()};
        auto got = p4::decode(c);
        check(got == values && c.left == 0, "PForDelta roundtrip");
    }
    std::vector<uint32_t> a = {0, 1, 2, 3, UINT32_MAX, 3, 7, 0};
    Bytes b;
    p4::encode_stream(a, b);
    for (size_t i = 0; i < b.size(); ++i)
        fails(
            [&] {
                Cursor c{b.data(), i};
                p4::decode(c);
            },
            "truncated codec");
    for (size_t n : {size_t(0), size_t(128), size_t(129), size_t(2048)}) {
        std::vector<uint32_t> v(n, 17);
        Bytes bytes;
        p4::encode_stream(v, bytes);
        check(p4::decode_stream({bytes.data(), bytes.size()}, n) == v, "stream boundary");
    }
    Bytes malformed = {1, 33, 0};
    fails(
        [&] {
            Cursor c{malformed.data(), malformed.size()};
            p4::decode(c);
        },
        "invalid width");
}
struct Eval {
    bool match = false;
    std::vector<Span> spans;
};
// Наивный перебор пар O(n*m) не использует код соединения интервалов движка.
/// @brief Наивно вычисляет совпадение и интервалы AST на токенах одного документа.
/// Перебор всех пар позиций служит независимым эталоном для оптимизированных итераторов.
Eval evaluate(const Query& q, const std::vector<std::string>& doc) {
    Eval out;
    if (q.op == Op::Term) {
        for (size_t i = 0; i < doc.size(); ++i)
            if (doc[i] == q.term)
                out.spans.push_back({uint32_t(i), uint32_t(i)});
        out.match = !out.spans.empty();
        return out;
    }
    auto a = evaluate(*q.left, doc), b = evaluate(*q.right, doc);
    if (q.op == Op::And) {
        out.match = a.match && b.match;
        return out;
    }
    if (q.op == Op::AndNot) {
        out.match = a.match && !b.match;
        return out;
    }
    if (q.op == Op::Or) {
        out.match = a.match || b.match;
        out.spans = a.spans;
        out.spans.insert(out.spans.end(), b.spans.begin(), b.spans.end());
    } else {
        for (auto x : a.spans)
            for (auto y : b.spans) {
                if (x.end < y.begin && y.begin - x.end <= q.distance)
                    out.spans.push_back({x.begin, y.end});
                else if (q.op == Op::Near && y.end < x.begin && x.begin - y.end <= q.distance)
                    out.spans.push_back({y.begin, x.end});
            }
        out.match = !out.spans.empty();
    }
    std::sort(out.spans.begin(), out.spans.end());
    out.spans.erase(std::unique(out.spans.begin(), out.spans.end()), out.spans.end());
    return out;
}
/// @brief Генерирует воспроизводимый запрос с не более depth уровнями булевых комбинаций.
std::string random_query(std::mt19937& rng, int depth) {
    static const std::vector<std::string> atoms = {"a",
                                                   "ab",
                                                   "abc",
                                                   "b",
                                                   "c",
                                                   "missing",
                                                   "ёж",
                                                   "STRASSE",
                                                   "\"a b\"",
                                                   "\"b a b\"",
                                                   "a ADJ a",
                                                   "a NEAR/3 b",
                                                   "(a OR b) ADJ c",
                                                   "(a ADJ b) NEAR/2 c"};
    if (depth == 0 || rng() % 3 == 0)
        return atoms[rng() % atoms.size()];
    static const std::vector<std::string> ops = {" AND ", " OR ", " AND NOT "};
    return "(" + random_query(rng, depth - 1) + ops[rng() % ops.size()] +
           random_query(rng, depth - 1) + ")";
}
/// @brief Проверяет токенизацию, парсер и многосегментный поиск сравнением с наивным эталоном.
void test_queries(const fs::path& root) {
    check(tokens("Hello, WORLD! Straße ЁЖ 42") ==
              std::vector<std::string>({"hello", "world", "strass", "ёж", "42"}),
          "Unicode normalization");
    for (auto bad :
         {"", "a b", "NOT a", "a AND", "a OR OR b", "(a", "a)", "\"a", "a NEAR/0 b", "a ADJ/-1 b",
          "a NEAR/4294967295 b", "(a AND b) ADJ c", "a AND NOT NOT b", "a-b"})
        fails([&] { parse_query(bad); }, std::string("invalid query: ") + bad);
    auto precedence = parse_query("a OR b AND NOT c ADJ d");
    check(precedence->op == Op::Or && precedence->right->op == Op::AndNot &&
              precedence->right->right->op == Op::Adj,
          "precedence");
    std::vector<std::string> texts = {"a b c", "b a c",    "a a b",     "a x b",     "a",
                                      "",      "abc ab a", "Ёж Straße", "b a b a b", "a x x x x b"};
    std::mt19937 rng(725);
    std::vector<std::string> vocabulary = {"a", "ab", "abc", "b", "c", "x", "ЁЖ", "Straße"};
    for (size_t i = 0; i < 450; ++i) {
        std::string text;
        for (size_t j = 0, n = rng() % 90; j < n; ++j)
            text += vocabulary[rng() % vocabulary.size()] + " ";
        texts.push_back(text);
    }
    std::vector<std::vector<std::string>> documents;
    {
        // Малый порог намеренно создаёт несколько сегментов даже на коротком корпусе.
        Builder builder(root, 25000);
        for (size_t i = 0; i < texts.size(); ++i) {
            builder.add({"id-" + std::to_string(i), "https://example.test/" + std::to_string(i),
                         texts[i], "source.parquet", i});
            documents.push_back(tokens(texts[i]));
        }
        builder.finish();
    }
    Index index(root);
    check(index.documents == texts.size() && index.segments.size() > 1, "multi-segment index");
    auto info = index.document(3);
    check(info.id == "id-3" && info.row == 3 && info.source == "source.parquet",
          "document metadata");
    for (unsigned i = 0; i < 350; ++i) {
        std::string text = i == 0 ? "a NEAR/1 a" : random_query(rng, 3);
        auto query = parse_query(text);
        std::vector<uint32_t> expected;
        for (size_t d = 0; d < documents.size(); ++d)
            if (evaluate(*query, documents[d]).match)
                expected.push_back(uint32_t(d));
        auto actual = search(index, *query, 100000, true);
        check(actual.hits == expected && actual.matches == expected.size() && actual.complete,
              "oracle query " + text);
        // Проверяем точные множества интервалов, а не только совпадение документов.
        if (query->op == Op::Term || query->op == Op::Near || query->op == Op::Adj) {
            for (auto& segment : index.segments) {
                auto it = compile_query(*query, *segment);
                for (; it->doc() != END; it->next())
                    check(it->spans() == evaluate(*query, documents[it->doc()]).spans,
                          "span oracle " + text);
            }
        }
    }
    auto q = parse_query("a OR b");
    auto limited = search(index, *q, 2, false);
    check(limited.hits.size() == 2 && !limited.complete, "limit short circuit");
    auto no_positions = search(index, *parse_query("a AND b AND NOT missing"), 10000, true);
    check(no_positions.stats.position_blocks_decoded == 0,
          "Boolean queries do not decode positions");
    auto zero = search(index, *q, 0, true);
    check(zero.hits.empty() && zero.matches > 0 && zero.complete, "count-only search");
    fails([&] { Builder duplicate(root); }, "do not overwrite index");
}
/// @brief Проверяет дальние и случайные ADVANCE, счётчики распаковки и устойчивость END.
void test_skips(const fs::path& root) {
    constexpr uint32_t n = 18000;
    {
        Builder b(root, 64 * 1024 * 1024);
        for (uint32_t i = 0; i < n; ++i)
            b.add({std::to_string(i), "", i % 3 == 0 ? "common rare common" : "common", "", i});
        b.finish();
    }
    Index index(root);
    check(index.segments.size() == 1, "skip fixture one segment");
    SearchStats stats;
    auto it = index.segments[0]->iterator("common", &stats);
    check(it->doc() == 0, "initial doc");
    check(it->advance(17001) == 17001, "long skip");
    // При дальнем скачке достаточно исходного и целевого блока; промежуточные не читаются.
    check(stats.blocks_decoded == 2 && stats.skip_jumps > 0, "ADVANCE skips compressed blocks");
    check(it->advance(3) == 17001, "ADVANCE is monotonic");
    check(it->spans() == std::vector<Span>({{0, 0}, {2, 2}}), "positions after long skip");
    check(it->advance(n) == END && it->next() == END && it->advance(0) == END, "exhaustion stable");
    std::mt19937 rng(9);
    for (int repeat = 0; repeat < 60; ++repeat) {
        auto rare = index.segments[0]->iterator("rare", nullptr);
        uint32_t d = 0;
        while (d < n + 1000) {
            d += 1 + rng() % 1000;
            // rare встречается ровно в документах, чьи номера кратны трём.
            uint32_t expected = ((d + 2) / 3) * 3;
            if (expected >= n)
                expected = END;
            check(rare->advance(d) == expected, "random sparse advance");
        }
    }
    auto both = search(index, *parse_query("rare AND common"), 0, true);
    check(both.matches == 6000, "multi-block intersection");
}
/// @brief Проверяет пустой корпус, пустой словарь и отказ открытия незавершённого индекса.
void test_empty(const fs::path& root) {
    {
        Builder b(root / "zero");
        b.finish();
    }
    Index empty(root / "zero");
    check(empty.documents == 0 && search(empty, *parse_query("x")).hits.empty(), "empty corpus");
    {
        Builder b(root / "blank");
        b.add({"a", "", "!!!", "", 0});
        b.finish();
    }
    Index blank(root / "blank");
    check(blank.documents == 1 && search(blank, *parse_query("x")).hits.empty(),
          "empty vocabulary");
    {
        Builder unfinished(root / "unfinished");
        unfinished.add({"a", "", "word", "", 0});
    }
    fails([&] { Index bad(root / "unfinished"); }, "unpublished index rejected");
}
/// @brief Проверяет известные основы Porter2 и сохранение координат после объединения словоформ.
void test_stemming(const fs::path& root) {
    check(tokens("running runs runner runners run") ==
              std::vector<std::string>({"run", "run", "runner", "runner", "run"}),
          "verb stemming");
    check(tokens("studies studying studied study") ==
              std::vector<std::string>({"studi", "studi", "studi", "studi"}),
          "study stemming");
    check(tokens("connecting connected connections connect") ==
              std::vector<std::string>({"connect", "connect", "connect", "connect"}),
          "suffix stemming");
    check(tokens("skies dying lying tying") ==
              std::vector<std::string>({"sky", "die", "lie", "tie"}),
          "Snowball exceptions");
    check(tokens("42 abc123 КОШКИ café") ==
              std::vector<std::string>({"42", "abc123", "кошки", "café"}),
          "non-English tokens preserved");
    {
        Builder builder(root);
        builder.add({"0", "", "Running dogs run", "", 0, 2.0});
        builder.add({"1", "", "a runner studies", "", 1, 1.0});
        builder.finish();
    }
    Index index(root);
    check(search(index, *parse_query("runs"), 10, true).hits == std::vector<uint32_t>({0}),
          "query/document stems match");
    check(search(index, *parse_query("\"run dog\""), 10, true).hits == std::vector<uint32_t>({0}),
          "stemmed phrase");
    check(search(index, *parse_query("run NEAR/2 running"), 10, true).hits ==
              std::vector<uint32_t>({0}),
          "stem coordinates retained");
    check(search(index, *parse_query("run ADJ runs"), 10, true).hits.empty(),
          "stemming does not collapse positions");
    check(search(index, *parse_query("studying"), 10, true).hits == std::vector<uint32_t>({1}),
          "query inflection");
}
/// @brief Проверяет внешнюю сортировку, несколько проходов слияния и ранговые docID между
/// сегментами.
void test_ranking(const fs::path& root) {
    fs::create_directory(root);
    std::vector<Document> docs;
    for (uint32_t i = 0; i < 95; ++i)
        docs.push_back({std::to_string(i), "url-" + std::to_string(i),
                        i % 2 ? "common runs" : "common walking", "source.parquet", i,
                        double((i * 13) % 17) / 3.0 - 2.0});
    auto expected = docs;
    std::stable_sort(expected.begin(), expected.end(),
                     [](const Document& a, const Document& b) { return a.score > b.score; });
    {
        Builder builder(root / "index", 4000);
        // Каждая запись — отдельная порция; fan-in=3 заставляет пройти несколько уровней слияния.
        RankedDocuments ranked(root / "scratch", 1, 3);
        for (const auto& d : docs)
            ranked.add(d);
        check(builder.document_count() == 0, "no indexing before global score sorting");
        ranked.index_into(builder);
        fails([&] { ranked.add(docs[0]); }, "cannot append after ranking");
        builder.finish();
    }
    check(!fs::exists(root / "scratch"), "ranking scratch cleaned");
    Index index(root / "index");
    check(index.segments.size() > 1 && index.documents == docs.size(), "ranked segments");
    std::vector<uint32_t> expected_runs;
    for (uint32_t i = 0; i < expected.size(); ++i) {
        auto actual = index.document(i);
        check(actual.id == expected[i].id && actual.score == expected[i].score &&
                  actual.row == expected[i].row && actual.url == expected[i].url,
              "stable score order and source metadata");
        if (expected[i].row % 2)
            expected_runs.push_back(i);
    }
    for (auto query :
         {"running", "common AND runs", "common AND NOT walking", "common ADJ running"})
        check(search(index, *parse_query(query), 1000, true).hits == expected_runs,
              "query-independent score ranking");
    auto top = search(index, *parse_query("running"), 3, false);
    check(top.hits == std::vector<uint32_t>(expected_runs.begin(), expected_runs.begin() + 3),
          "early limit returns highest scores");
    {
        Builder builder(root / "unsorted");
        builder.add({"0", "", "word", "", 0, 1.0});
        fails([&] { builder.add({"1", "", "word", "", 1, 2.0}); },
              "builder rejects unsorted score input");
    }
    {
        RankedDocuments ranked(root / "invalid", 1);
        auto bad = docs[0];
        bad.score = std::numeric_limits<double>::quiet_NaN();
        fails([&] { ranked.add(bad); }, "NaN score rejected");
        bad.score = std::numeric_limits<double>::infinity();
        fails([&] { ranked.add(bad); }, "infinite score rejected");
    }
    {
        Builder builder(root / "empty");
        RankedDocuments ranked(root / "empty-scratch", 1);
        ranked.index_into(builder);
        builder.finish();
    }
    check(Index(root / "empty").documents == 0, "empty ranked corpus");
    // Старый формат не должен открываться новым токенизатором и правилами ранжирования.
    {
        std::ofstream manifest(root / "empty" / "manifest");
        manifest << "L0INDEX 1\n";
    }
    fails([&] { Index old(root / "empty"); }, "old format requires rebuild");
}
} // namespace
/// @brief Запускает C++ проверки во временном каталоге и возвращает код их успешности.
int main() {
    try {
        Temporary temp;
        test_codec();
        test_queries(temp.root / "queries");
        test_skips(temp.root / "skips");
        test_empty(temp.root);
        test_stemming(temp.root / "stemming");
        test_ranking(temp.root / "ranking");
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
